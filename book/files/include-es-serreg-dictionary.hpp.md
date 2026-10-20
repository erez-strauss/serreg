# `include/es/serreg/dictionary.hpp`

The runtime, heap-owned view of a dictionary: `dyn::dictionary` is the decoded ENTRY stream (`format.hpp`) from a
registry snapshot, an RRI image, an ELF `es_serreg_dict` section, or the dictionary frames inside a capture. It is
what `srdump`, `tools::verify`/`dump`/`diff`, `render_frame`, the descriptor-driven visitor and the side-dictionary
path of `inspect`/`dispatch` work from when the type is not compiled into the reading program. This is the
**readers layer** (spec §6.4, §6.9); declarations only — the implementation is `src/dictionary.cpp` in
`libserreg`. **No reflection dependency**: it describes types by data, not by C++ types.

## Depends on / used by

Includes: `<cstdint>`, `<map>`, `<span>`, `<string>`, `<string_view>`, `<utility>`, `<vector>`,
`es/serreg/descriptor.hpp` (`kind`), `es/serreg/error.hpp`, `es/serreg/format.hpp` (`fmt::arg_kind`,
`call_flag_*`, `format_minor`).

Included by: `deserialize.hpp` (`record_view::dyn_desc`, the `resolver`, `inspect(bytes, const dyn::dictionary*)`),
`tools.hpp` (every service takes or returns one), `src/dictionary.cpp` (definitions), `src/internal.hpp`,
`src/registry.cpp` (`to_json`/`to_toml` render through a dictionary built from the snapshot),
`tests/fuzz/fuzz_targets.cpp` (`fuzz_rri`), `tests/unit/runtime_selftest.cpp`.

## What it defines

Everything is in `namespace es::serreg::dyn`.

| name | kind | meaning |
|---|---|---|
| `field` | struct | owned mirror of `field_desc`: `name, k, offset, size, align, since, deprecated, type_hash, extent, std::vector<field> elem`; `operator==` defaulted. 1.6: a `map` field's `elem` holds the key and the value (offsets inside one entry), a `set` field's its key, and `extent` is the entry stride |
| `type` | struct | owned mirror of `type_desc`: `name, schema_hash, layout_hash, size, align, is_enum, is_flat, transparent, enum_fixed, underlying, since, deprecated, fields, enumerators (vector<pair<string,int64_t>>)`; `operator==` defaulted (structural). 1.6: `transparent` — a container argument's type, one unnamed field at 0 (`struct_flag_transparent`); `enum_fixed` — the enum has a fixed underlying type (`enum_flag_fixed`) |
| `literal` | struct | `id, text` |
| `call_arg` | struct | `is_literal, hash, ak (fmt::arg_kind), k (scalar kind), value` (1.5: a constant's bytes as a `u64`, zero-extended); `operator==` |
| `call` | struct | `hash, flags, args`; `is_print()`, `newline()`, `is_log()` (`call_flag_log`, a logger's call; `dictionary.hpp:80-83`); `operator==` |
| `type_conflict` | struct | `kept, dropped` — two ENTRYs with one schema hash and different bodies; both are summaries (name, hashes, size, alignment; no fields or enumerators) |
| `id_conflict` | struct | `is_call, id, kept, dropped` — the same for a literal id or a call hash; `kept`/`dropped` describe the bodies (the literal's text; the call's flags and argument kinds and hashes) |
| `dictionary` | struct | the three maps, `program_hash`, the writer's ABI (`abi`, `abi_known`, `abi_mixed`), `conflicts`, `id_conflicts`, and the methods below |
| `to_rri(const dictionary&, uint32_t flags = 0)` | function | re-encodes a dictionary as an RRI byte image |

`dictionary` members:

| member | meaning |
|---|---|
| `std::map<uint64_t, type> types` | keyed by schema hash |
| `std::map<uint64_t, literal> literals` | keyed by id |
| `std::map<uint64_t, call> calls` | keyed by call hash |
| `uint64_t program_hash` | from the source header; 0 if unknown |
| `uint64_t abi`, `bool abi_known`, `bool abi_mixed`, `same_abi()`, `scalar_size(kind)` | 1.3 (FR-W4): the writer's ABI tag (the first source's), whether a later source had another one, and the writer's size of a top-level scalar (`wchar` from the tag) |
| `std::vector<type_conflict> conflicts` | filled by `add_entries`/`merge`; `tools::verify` reports each |
| `std::vector<id_conflict> id_conflicts` | review G2-10: the same for literals and calls; both walkers and `verify` report each as a `literal conflict:` / `call conflict:` problem |
| `static constexpr size_t max_conflicts` (64), `uint64_t conflicts_omitted` | D-03: each of the two lists keeps at most `max_conflicts` entries; the conflicts beyond that are only counted |
| `add_entries(span<const byte>)` | parse a bare ENTRY stream (ELF section content, RRI body) |
| `add_rri(span<const byte>)` | parse header + entries; sets the ABI fields and (if 0) `program_hash` |
| `merge(const dictionary&)` | union; second definitions of a hash are kept out and recorded as conflicts when they differ |
| `find_type(hash)`, `find_type_by_name(sv)`, `find_literal(id)`, `find_call(hash)` | lookups, `nullptr` when absent |
| `computed_program_hash()` | FR-H3 over the sorted keys of `types` |
| `record_size(const call_arg&)` | compact record size: flat type size, `scalar_size` for a scalar, 0 for a literal and (1.5) for a constant; `error::unsupported` for a string (per-record `u32 len`) |
| `std::uint64_t abi`, `abi_known`, `abi_mixed`, `same_abi()` | 1.3: the writer's ABI tag (first source), whether a source set it, whether two sources disagreed; `same_abi()` = this host's tag and not mixed |
| `scalar_size(kind)` | size of a top-level scalar as the writer laid it out: `kind_size`, except `wchar` from the ABI tag |
| `to_json()`, `to_toml()` | canonical, sorted renderings (FR-W5) |

## How it works

### One model for four sources

The same `dyn::dictionary` is produced by `tools::load_dictionary_rri` (an `.rri` file or `--dict`),
`tools::extract_registry_from_elf` (`--elf a.out`, the `es_serreg_dict` section concatenated across TUs),
`registry::instance().export_image()` parsed back (in-process), and the dictionary frames / FILE-embedded RRI
of a capture (`src/reader.cpp`). Because readers key everything by hash, duplicates across TUs or across a
FILE dictionary and a `--dict` side file collapse. `merge` is how `srdump` layers `--dict`/`--elf` over the
capture's embedded dictionary (`extra` in `tools::verify`/`dump`).

### Parsing is validating (FR-D5)

The header's comment on `add_entries` is the contract the fuzz target enforces:

```cpp
// Every count on the wire is checked against the remaining bytes before anything is allocated,
// and field geometry (offset + size within the type, extent × element size within the field) is validated
// here so the decoder never sees an inconsistent descriptor (FR-D5).
result<void> add_entries(std::span<const std::byte> entries);
```
(`dictionary.hpp:127-132`). In `src/dictionary.cpp` `parse_field` additionally enforces per-kind arity: an `array_`
has exactly one element descriptor with `extent × elem.size ≤ size`; the kinds `string_`/`vector_`/`optional_`/
`variant_` must have `size == 8` (a reference slot) and 0 / 1 / 1 / `extent` element descriptors; the 1.6 kinds
`map_`/`set_` likewise `size == 8`, 2 / 1 element descriptors, the key at entry offset 0, a map's value at an
offset ≥ the key's size, and `extent` (the entry stride) a non-zero multiple of the entry alignment that holds
both; a `transparent` type must be a non-flat struct of size 8 with exactly one unnamed dynamic field at 0; an
enum flags byte with a bit other than `fixed` is refused; unknown `kind`
values (the gaps in the `kind` numbering) are rejected rather than guessed at. `add_rri` refuses a `major` ≠ 1 or a
`minor` outside `fmt::format_minor_oldest` … `fmt::format_minor` (6 … 7) with `bad_version`, a `hash_algo` ≠ 2
with `unsupported`, an `n_entries` above `decode_limits::dictionary_entries` (1 000 000; FR-D5) with `corrupt_dictionary`, and pre-walks the
ENTRY chain by `total_len` so trailing bytes past `n_entries` are ignored. Failure is always `corrupt_dictionary`
(or `truncated`/`bad_magic`), never UB — `fuzz_rri` mutates RRIs into `add_rri` for exactly this.

### The writer's ABI: the one piece of reader state

```cpp
std::uint64_t abi{fmt::host_abi_tag};
bool abi_known = false;
bool abi_mixed = false;
bool same_abi() const noexcept { return abi == fmt::host_abi_tag && !abi_mixed; }
std::uint32_t scalar_size(const kind k) const noexcept
{
    return k == kind::wchar ? fmt::unpack_abi(abi).wchar_size : kind_size(k);
}
```
(`dictionary.hpp:109-121`). This is the one piece of *reader state* in the dictionary (format 1.3, FR-W4): the ABI tag
of the writer, taken from the first FILE or RRI header (`add_rri`, `src/reader.cpp`), or this process's tag for a
dictionary built in-process (registry snapshot, ELF section). Sizes and offsets of members are in every FIELD, so
decoding by the dictionary works across ABIs; the tag supplies what the FIELDs do not — the width of a top-level
`wchar` argument (`scalar_size`), and the fact that the writer differed (`srdump` prints an `abi` line; two
different tags in one capture are an `abi_mismatch` problem).

### Conflicts

`type_conflict{kept, dropped}` is FR-R4 seen from the tooling side: `add_entries`/`merge` keep the first
definition of a schema hash in `types`; a later ENTRY with the same hash and a *different* body (structural
`operator==` on `type`) goes to `conflicts`. `tools::verify` lists each as a problem, `srdump --elf` exits 1 on
them (`srdump_elf_conflicting_plugins_exit_1`), and `tests/unit/test_tools.cpp` "cc F16 dyn::dictionary records
same-hash different-body definitions as conflicts" pins the behaviour. An identical duplicate (the normal
multi-TU case) is silently absorbed. A second, different text under one literal id (or a second body for a call
hash) is not dropped without a trace: it is an `id_conflict`
(`test_dynamic.cpp` "G2-9/G2-10 a literal id with two bodies is a conflict in both walkers and in verify").

`kept` and `dropped` of a `type_conflict` are summaries (D-03): the name (at most
256 bytes, then `"..."`), the hashes, size and alignment (and the enum/flat flags), without fields or
enumerators; the whole kept definition is still in `types`. `id_conflict` texts are clipped the same way. Each list
holds at most `dictionary::max_conflicts` (64) entries; `conflicts_omitted` counts the ones left out, and
`tools::verify` reports them as one "N more conflicts not listed" problem. Otherwise a few bytes of hostile input
could make the dictionary copy a huge type once per redefinition. `kept.fields` of a conflict is an empty vector;
look the type up in `types` instead.

### `call_arg` and `type`

`call_arg` keeps the `is_literal` bool next to `ak` for readers that test it; `ak` and `k` come from the ARG bytes
`arg_kind` and `kind`. In format 1.5 `ak` can be `arg_const`: then `k` is
the scalar kind of the constant or `kind{}` for an enum, `hash` the scalar name's hash or the enum's schema hash,
and `value` the `u64` that follows the ARG (`dictionary.hpp:52-60`). `value` is part of `operator==`, so two
ENTRYs for one call hash that differ only in a constant's value are an `id_conflict`. In JSON and TOML such an
argument is `"kind": "const"` with, for a scalar, `"scalar"`, and (B7) `"value"` typed as what the
constant stands for (`42`, `true`, `"sell"`, `-1.5`) plus `"bits"` (the `u64` in hex, `files/src-dictionary.cpp.md`). `call::flags` carries `call_flag_print`/`call_flag_newline`
(`is_print()`, `newline()`), which `render_frame` and the text dump consult. In format 1.7
`call_flag_log` (`is_log()`) marks a logger's call: a print call whose first arguments are the log envelope —
`constant<u8 level>`, `lit<File>`, `constant<u32 Line>`, `constant<u8 mask>`, `u64` time, then by the mask a `u32`
thread id and a `u64` sequence, then the format literal (`log.hpp` `log_layout`). The ENTRY parser refuses a log
call whose envelope does not have that shape (`src/dictionary.cpp`); the text form labels it `log(...)` and JSON/TOML
`"form": "log"`. `type::is_flat` is the struct ENTRY's
flag bit; for a dynamic type (`is_flat == false`) `size`/`align` are the fixed part and `fields[i].offset` are
canonical image offsets — `resolver::is_dynamic` in `deserialize.hpp` reads `!dd->is_flat` to know that the record
carries a `u32 len, u32 0` prefix. In format 1.6 `type::transparent` marks the synthetic type of a container argument
(`std::vector<i32>`): a dynamic type like any other to the walkers, but one whose single unnamed field the
renderers print as the record's value; `type::enum_fixed` tells a dictionary-driven reader whether an enum carries
a value range (the range check itself is typed-only, `docs/deviations.md`). The text dump shows them as `transparent`
after `dynamic` and as `unfixed` after an enum's name; JSON as `"transparent": true` / `"fixed": true|false`; TOML
as `transparent = true` / `fixed = …` (chapter 6 §6.4).

### `record_size`: what the compiled walker needs per argument

```cpp
// Native size in bytes of one compact record of this argument (flat type size, 0 for a literal — FR-L6,
// the scalar's size). A string record is dynamic: error::unsupported — the walker reads its u32 length prefix.
result<std::uint32_t> record_size(const call_arg& a) const noexcept;
```
(`dictionary.hpp:145-147`). The compiled frame walker in `src/reader.cpp` is the dictionary-only twin of
`frame_view::compact_size`: for each ARG of the resolved `call` it asks `record_size`, and for the two
variable-length categories (a string, or a type whose `dyn::type::is_flat` is false) it reads the `u32 len`
itself with bounds checks. `unknown_type` when `find_type(a.hash)` is null for an `arg_type` ARG stops the frame.
A constant (1.5) answers 0 like a literal: the walker takes its value from `call_arg::value`, not from the frame.

### Life cycle inside `srdump orders.ser --dict extra.rri --json`

1. `cli::parse` fills `options{capture = "orders.ser", dicts = {"extra.rri"}, fmt = json}`.
2. `cli::execute` → `tools::read_file` both, `load_dictionary_rri(extra)` → a `dyn::dictionary` (its ABI tag
   from `extra.rri`'s header; a minor other than 1.6 or 1.7 is `bad_version`).
3. `tools::dump(capture, &extra, opt, out)` → `src/reader.cpp` parses the FILE header (sets `program_hash` and
   `abi` from the capture), `add_rri` on the embedded dictionary, `merge(*extra)` — conflicts recorded,
   duplicates absorbed.
4. Frame loop: `find_call(call_hash)` → `record_size`/length prefixes → `visit_record(payload, *find_type(hash),
   dict, visitor)` for each type record; `render_frame` when `call.is_print()` and `!opt.raw`.
5. Formatters render; `dump_dictionary` renders the merged dictionary once at the top.

### `to_rri`

The inverse of `add_rri`: writes a 40-byte header (`format_major`, current `format_minor`, `flags`, algo 2,
`n_entries`, the recorded `program_hash`, 0, the dictionary's `abi`) and re-encodes every `type`, `literal` and `call` as an
ENTRY (a call ENTRY is 16 bytes per ARG plus 8 per constant, 1.5; in 1.6 the struct flags carry `transparent`
and the enum flags byte `fixed`). Used by `srdump --elf … --rri-out` and by export paths that start from parsed data. Because the encoder is
the same byte rule as `blob.hpp`, a round trip through `add_rri` → `to_rri` reproduces the compile-time blobs;
`tests/unit/test_registry.cpp` "FR-R9/FR-D3 export_image() parses back and lists the same entities" and the
`golden_srdump_program_hash_*` tests rely on that.

## Maintaining it

- **Structural equality is semantic.** `field::operator==` and `type::operator==` are defaulted over all members;
  they decide what a "conflict" is and what `tools::diff` calls "identical". Adding a member to `field`/`type`
  that is not part of the ENTRY (a cache, say) would make identical ENTRYs compare unequal — do not.
- **Parser changes need fuzzing.** After touching `src/dictionary.cpp` run `fuzz_rri` (and `fuzz_capture`) under
  the `gcc16-asan-ubsan` preset for a few minutes; `tests/fixtures/negative/` holds the crafted bad inputs.
- **New kinds / new ARG categories** must be accepted by `parse_field`/`parse_call` with explicit arity and size
  rules, and *rejected* when unknown — that rejection is what makes an old reader refuse a newer dictionary
  cleanly. Adjust `record_size` too.
- The ABI fields must be set from the *source* (`add_rri` does; `add_entries` on a bare section cannot and leaves
  the host default) and survive `merge` — `to_rri` writes `abi`, so a re-encoded dictionary keeps its origin.
- `to_json`/`to_toml` output is compared byte-for-byte against `tests/fixtures/golden/*.json|.toml` by
  `golden_srdump_*_matches_fixture`;
  any rendering change regenerates the fixtures (via `make_fixtures`/`make_dynamic_fixtures`) and is a CHANGELOG
  item. Sorted, deterministic output is FR-W5.
- `find_type_by_name` is linear; `find_*` by hash are `std::map` lookups. This is the tooling path, not the hot
  path — `registry::find_*` in-process is the lock-free one.
- Tests: `tests/unit/test_tools.cpp` (cc F16 conflicts, gk 1.11 diff, gk 1.12 JSON i64/u64 quoting),
  `tests/unit/test_registry.cpp` (export/parse-back, `to_json` names every type), `tests/unit/test_capture.cpp`
  ("FR-D6 extract_registry_from_elf(/proc/self/exe) matches the compile-time hashes"), `tests/unit/test_print.cpp`
  and `test_dynamic.cpp` (minor-version handling), `tests/unit/test_roundtrip.cpp` ("FR-A5 constants through the
  dictionary: RRI round trip, verify, dump, JSON, print (1.5)"), `fuzz_rri`, and every `srdump_*`/`golden_srdump_*`/`compat*` ctest.
