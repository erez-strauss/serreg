# `src/diff.cpp`

`tools::diff()` and `diff_report::to_text()` from `include/es/serreg/tools.hpp` (spec FR-D3): compare two
dictionaries — types by canonical name, calls and literals by id — and classify each difference as
`identical`, `names-only`, `metadata-only`, `breaking`, `added`, `removed` or `duplicate-name` for types and
`call added/removed/changed`, `literal added/removed/changed` for the rest, and (decision E)
mark each entry that breaks the reader the caller named with a `diff_direction`: a layout change, a changed
call and a changed literal always; removals under `old_data` (the default: a reader built from the new side
replays old captures) and `both`; additions under `new_data` (an old reader on a live stream) and `both`; none
of those under `none`. `diff_report::breaking` is set when any entry is. The verdicts
describe **identity** — did the bytes or the names of a type move, is a call or literal still there — not
decodability. It never modifies its inputs, never resolves nested types (two types with equal
`layout_hash` are layout-equal by construction of the hash), and never drops a same-named type silently: a
second definition under one canonical name is reported as `duplicate-name` (gk 1.11, §6.2b). Several
versions of one name on both sides are paired by schema hash before anything is compared (T-09).

## Depends on / used by

Includes: `internal.hpp` (`dec`, `hex64`), `es/serreg/tools.hpp` (`diff_entry`, `diff_report`), `<algorithm>`
(`std::ranges::find_if`, `std::ranges::equal`). From `dictionary.hpp`: `dyn::dictionary`, `dyn::type`,
`dyn::field`, `dyn::call`, `dyn::call_arg::operator==`, `dyn::literal`.

Library: `es::serreg_tools` (`libserreg_tools`, decision G).

Used by: `src/cli.cpp` (`--diff <a> <b> [--direction d]`: exit 1 when `breaking`), `tests/unit/test_tools.cpp`
("gk 1.11 diff: enumerator value change is breaking, additions/renames are not, calls and literals are diffed";
"2.1.16 decision E: --diff --direction decides whether removals or additions are breaking"),
`test_registry.cpp` (FR-R9/FR-D3 export round trip).

## Structure

- **`same_metadata()`** (`diff.cpp:21-30`): recursive comparison of `since`/`deprecated` over two field
  lists (and their `elem` lists); used only after the layout hashes were found equal.
- **`enum_verdict()`** (`diff.cpp:34-56`): the enumerator-pair rules.
- **`by_name()`** (`diff.cpp:59-65`): `std::map<string_view, vector<const type*>>` grouping each side's types
  by canonical name; `dictionary::types` iterates in hash order, so `v[0]` — the "primary" — is deterministic.
- **`literal_label()`** (`diff.cpp:67-77`): `literal "text"` with quotes/backslashes escaped and control
  characters replaced by `?`.
- **`tools::diff()`** (`diff.cpp:81-241`): the lambdas `extras` (further versions of a name found on one side
  only) and `verdict_of` (the verdict ladder for one pair); types, then calls, then literals; then
  the direction pass that sets `diff_entry::breaking`/`note` and the three counts.
- **`diff_report::to_text()`** (`diff.cpp:243-284`): one line per entry plus a summary line.

## How it works

### Type verdicts

```cpp
const auto verdict_of = [&](const dyn::type* const ta, const dyn::type* const tb) {
    std::string verdict;
    if (ta->schema_hash == tb->schema_hash)
        verdict = "identical";
    else if (ta->layout_hash != tb->layout_hash || ta->is_enum != tb->is_enum)
        verdict = "breaking";
    else if (ta->is_enum)
        verdict = enum_verdict(*ta, *tb);
    else if (ta->since == tb->since && ta->deprecated == tb->deprecated &&
             same_metadata(ta->fields, tb->fields))
        verdict = "names-only";
    else
        verdict = "metadata-only";
    return verdict;
};
```

(`diff.cpp:92-106`.) The decision ladder relies on the two hashes' definitions: the **schema hash** covers
names, layout and metadata, so equal schema hashes mean nothing changed; the **layout hash** covers kinds,
offsets, sizes and alignments only, so a different layout hash (or struct↔enum) is **breaking** — old bytes
cannot be read with the new descriptor. Equal layout but different schema hash is then either a rename
(`names-only`, when `since`/`deprecated` agree on the type and on every field, recursively) or a
`metadata-only` change. Types
present on one side only are `added`/`removed`; whether that breaks anything is decided by the direction pass
below (decision E, review T-09).

### Enumerators (gk 1.11)

```cpp
for (const auto& [name, value] : a.enumerators)
{
    const auto by_name =
        std::ranges::find_if(b.enumerators, [&](const auto& e) { return e.first == name; });
    if (by_name != b.enumerators.end() && by_name->second != value)
        return "breaking";
    const auto by_value =
        std::ranges::find_if(b.enumerators, [&](const auto& e) { return e.second == value; });
    if (by_value == b.enumerators.end())
        return "breaking";
    if (by_name == b.enumerators.end())
        renamed = true;
}
```

(`diff.cpp:33-55`.) Enumerators are compared as `(name, value)` pairs from `a`'s side: a name whose value
changed, or a value that disappeared, is **breaking** (captured values would decode to another meaning or to
nothing); a value that kept its number under a new name is a rename (`names-only`); extra enumerators only in
`b` are a compatible addition — `identical` if counts and `since`/`deprecated` match, else `metadata-only`.
An enum's layout hash covers only its underlying type, which is why this separate rule exists.

### Duplicate names

```cpp
const auto extras = [&](const std::string_view name, const std::vector<const dyn::type*>& v,
                        const bool side_a) {
    for (std::size_t i = 1; i < v.size(); ++i) // gk 1.11: never drop a same-named type silently
        rep.entries.push_back({std::string(name), side_a ? v[i]->schema_hash : 0,
                               side_a ? 0 : v[i]->schema_hash, "duplicate-name"});
};
```

(`diff.cpp:86-91`.) A dictionary can legitimately hold two types with one canonical name (e.g. a flat and a
dynamic `Msg` from two builds merged with `--elf a --elf b`). For a name found on one side only, `v[0]` is the
`added` or `removed` entry and `extras` lists every further definition as `duplicate-name` on the side it came
from. It is not marked breaking.

For a name found on both sides, the versions are paired first:

```cpp
// 2.1.12 (review T-09): several versions of one name (a dictionary merged from several binaries) pair
// up by schema hash first, so a version present on both sides is "identical"; only the versions left
// over are compared with each other (first with first), and the rest are listed as duplicate names.
std::vector<const dyn::type*> ra, rb;
for (const dyn::type* const ta : va)
{
    if (std::ranges::any_of(it->second, [&](const dyn::type* const tb) {
            return tb->schema_hash == ta->schema_hash;
        }))
    {
        rep.entries.push_back(
            {std::string(name), ta->schema_hash, ta->schema_hash, "identical"});
    }
    else
        ra.push_back(ta);
}
for (const dyn::type* const tb : it->second)
    if (!std::ranges::any_of(va, [&](const dyn::type* const ta) {
            return ta->schema_hash == tb->schema_hash;
        }))
        rb.push_back(tb);
if (!ra.empty() && !rb.empty())
{
    std::string verdict = verdict_of(ra[0], rb[0]);
    rep.entries.push_back(
        {std::string(name), ra[0]->schema_hash, rb[0]->schema_hash, std::move(verdict)});
    ra.erase(ra.begin());
    rb.erase(rb.begin());
}
for (const dyn::type* const ta : ra)
    rep.entries.push_back({std::string(name), ta->schema_hash, 0, "duplicate-name"});
for (const dyn::type* const tb : rb)
    rep.entries.push_back({std::string(name), 0, tb->schema_hash, "duplicate-name"});
```

(`diff.cpp:116-148`.) Every version whose schema hash occurs on both sides (T-09)
is `identical`. Of the versions left over, the first of `a` is compared with the first of `b` through
`verdict_of`; the rest are `duplicate-name` on their side. Comparing only `v[0]` of each side would give a
dictionary merged from several binaries wrong answers both ways: with `a = {0x1111, 0x1113}` and
`b = {0x1113}` it would compare `0x1111` with `0x1113` and say BREAKING although `0x1113` is unchanged, and with
`b = {0x1111, 0x1114}` it would compare `0x1111` with itself and miss that `0x1113` and `0x1114` differ in layout
(`runtime_selftest.cpp` checks both cases).

### Calls and literals

Calls (`diff.cpp:157-178`) are matched by call hash. Since the hash is the XXH3-64 fingerprint of the canonical call string,
two calls with the same hash normally have the same arguments; the comparison of `flags` and of every
`call_arg` (category, scalar kind, hash — `dyn::call_arg::operator==`) exists for corrupted or hand-edited
dictionaries and yields `call changed` + breaking. Literals (`diff.cpp:180-196`) are matched by id; a changed
text under the same id is `literal changed` + breaking (the id is the fingerprint of the text, so this too signals a
damaged dictionary). Whether an addition or a removal is breaking depends on the direction (below).

### The direction pass (decision E)

```cpp
// 2.1.16 (decision E): what breaks depends on the direction the caller named
const bool strict_removals =
    direction == diff_direction::old_data || direction == diff_direction::both;
const bool strict_additions =
    direction == diff_direction::new_data || direction == diff_direction::both;
for (diff_entry& e : rep.entries)
{
    if (e.verdict == "breaking" || e.verdict == "call changed" ||
        e.verdict == "literal changed")
    {
        e.breaking = true;
        e.note =
            e.verdict == "breaking"
                ? "layout differs: frames of either side cannot be read by the other"
                : "changed on both sides: frames of either side cannot be read by the other";
        ++rep.breaking_changes;
    }
    else if (strict_removals && (e.verdict == "removed" || e.verdict == "call removed" ||
                                 e.verdict == "literal removed"))
```

(`diff.cpp:197-239`.) On top of the seven type verdicts and the six call/literal verdicts there is
one pass over the finished entries that answers "breaking for whom?". Whether a removal or an addition breaks
anything depends on who reads what, so the caller names it (`tools::diff(a, b, direction)`,
`diff_direction::old_data` by default):

| direction | breaking | the reader it stands for |
|---|---|---|
| `old_data` (default) | layout/call/literal changes, **removals** | a reader built from the new side (`b`) replays data written by the old side (`a`): a type, call or literal that is gone cannot be read typed |
| `new_data` | layout/call/literal changes, **additions** | a reader built from the old side meets a live stream of the new side: an unknown call, type or literal |
| `both` | all three | both deployments at once |
| `none` | layout/call/literal changes only | identity only |

A breaking entry gets `diff_entry::breaking = true` and a `note` saying why (`removed: a reader built from the new
side cannot read old frames of this call typed`, `added: a reader built from the old side meets an unknown type`,
`layout differs: …`); the report counts them in `breaking_changes`, `breaking_removals` and `breaking_additions`,
and `diff_report::breaking` is their disjunction. `duplicate-name` is never breaking by itself. What the pass does
*not* do is decide decodability: a removed type whose frames a dictionary-driven reader could still walk is
breaking under `old_data` all the same, because the question `--diff` answers is identity — the `--compat` of a
later release will answer the other one.

### `same_metadata`: what "names-only" checks

```cpp
bool same_metadata(const std::vector<dyn::field>& a, const std::vector<dyn::field>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].since != b[i].since || a[i].deprecated != b[i].deprecated ||
            !same_metadata(a[i].elem, b[i].elem))
            return false;
    return true;
}
```

(`diff.cpp:20-29`.) Reached only when the layout hashes agree, so field counts, kinds, offsets and sizes are
already known equal; the only remaining non-name inputs to the schema hash are `since`/`deprecated` on the
type and on every (nested) field. If those match too, the schema-hash difference can only come from names —
hence `names-only`. Field names themselves are not compared: they are part of the schema hash and a
difference there is exactly what `names-only` reports.

### The text report

`to_text()` (`diff.cpp:243-284`) prints `verdict` padded to 16 columns, the name, `0x… -> 0x…` (`-` for
a missing side) and `  breaking: <note>` on every breaking entry; then a summary: `N types: a
identical, b names-only, c metadata-only, d breaking, e added, f removed, g duplicate-name; M calls: … ; L
literals: …`, followed by `  BREAKING (direction old-data: C changed, R removed, A added)` when the report is
breaking, or `  (direction old-data)` when it is not — the direction is always named, so a report can be read
without knowing the command line.
`cli::execute` prints this and returns `rep.breaking ? 1 : 0`. A typical run with the default direction:

```
identical       corpus::Order  0x9a3c… -> 0x9a3c…
metadata-only   d::Venue  0x1f02… -> 0x77e1…
removed         d::Cancel  0x5b8a… -> -  breaking: removed: a reader built from the new side cannot read old frames of this type typed
call added      call 0x4d10…  - -> 0x4d10…
4 types: 1 identical, 0 names-only, 1 metadata-only, 0 breaking, 0 added, 1 removed, 0 duplicate-name; 1 calls: 1 added, 0 removed, 0 changed; 0 literals: 0 added, 0 removed, 0 changed  BREAKING (direction old-data: 0 changed, 1 removed, 0 added)
```

(Hashes abbreviated here; the tool prints all 16 digits.) The same two dictionaries under `--direction none` end
in `(direction none)` and exit 0, under `new-data` the call addition is the breaking line instead. Entries appear
in map order — types by name, then
calls and literals by id — so two runs over the same inputs produce identical text.

## Maintaining it

- **Tests:** `test_tools.cpp` "gk 1.11 diff: …" covers every verdict including `duplicate-name`, renamed and
  removed enumerators, changed call flags/args and literal text; "2.1.16 decision E: --diff --direction decides
  whether removals or additions are breaking" covers a removal and an addition under each of the four
  directions, the counts, the `breaking:` annotation and the summary line; `test_registry.cpp` exercises the
  `identical` path over an exported RRI.
- **Verdict strings are API** (`tools.hpp` documents them, `cli` users grep them). Adding a verdict means
  updating the summary loop in `to_text()` (the fixed list `identical … duplicate-name`) and the header comment
  in `tools.hpp`. The `breaking:` notes and the `BREAKING (direction …)` line are part of the same contract
 ; `diff_direction` and `to_string(diff_direction)` live in `tools.hpp`.
- **Breaking is a property of the pair (verdict, direction)**, computed once in the direction pass; do not set
  `rep.breaking` in the verdict code.
- **What counts as breaking must stay derived from the layout hash**; do not re-implement layout comparison
  here. If the layout-hash definition in `reflect.hpp` changes, this file's verdicts change with it — the
  golden `hashes.cpp` guards that.
- **`by_name` keys are `string_view`s into the input dictionaries**; the inputs must outlive the call (they
  do — `diff` returns owned `std::string`s in `diff_entry`).
- **Allocation:** tools layer; maps and strings are fine. Not on any hot path.
- **Threading:** pure function; both dictionaries are read-only.
- **Wire format:** none — this file reads `dyn::` structures only. It is unaffected by `format_minor`.
