# `src/dictionary.cpp`

The run-time (heap-owned) dictionary: `dyn::dictionary` from `include/es/serreg/dictionary.hpp` and the
free function `dyn::to_rri()`, plus the two `tools::` loaders that need nothing but a file read —
`load_dictionary_rri()` and `load_dictionary_file()` (spec §6.7, §6.9; FR-W3, FR-W5, FR-D5). It implements
`add_entries()`, `add_rri()`, `merge()`, `find_type()`, `find_type_by_name()`, `find_literal()`,
`find_call()`, `record_size()`, `computed_program_hash()`, `to_json()`, `to_toml()`. Parsing is bounded:
every count read from the wire is checked against the remaining bytes before anything is reserved from it,
field geometry is validated before the decoder ever sees a descriptor, and unknown `kind` values are rejected
instead of guessed at. The file never keeps a pointer into the parsed bytes — every name and text is copied —
so a dictionary outlives its source (a `dlclose`d image, a freed capture buffer).

## Depends on / used by

Includes: `internal.hpp` (`append_quoted`, `dec`, `hex64`), `es/serreg/dictionary.hpp`, `es/serreg/hash.hpp`
(`fingerprint64`, `fingerprint64_u64s`), `es/serreg/tools.hpp` (`read_file`). From `format.hpp`: `fmt::cursor`,
`fmt::writer`, entry tags, `arg_kind`, call flags, `struct_flag_flat`, `rri_header_size`, `entry_header_size`,
`align8`. From `descriptor.hpp`: `kind`, `kind_size`, `to_string(kind)`.

Used by: `src/reader.cpp` (merging embedded and side dictionaries, `record_size`), `src/decoder.cpp`
(`find_type`), `src/formatters.cpp`, `src/diff.cpp`, `src/cli.cpp`, `src/wireshark.cpp` (the tables of the
generated dissector), `src/elf_reader.cpp`
(`extract_embedded_dictionary` → `add_entries`; `extract_registry_from_elf` is a wrapper), `src/registry.cpp` (`to_json`/`to_toml` go through
`add_rri(export_image())`), `deserialize.hpp` (deserialize-time validation), `tests/fuzz/fuzz_targets.cpp`
(`fuzz_rri`: `add_rri`, then `to_rri`, `to_json`, `to_toml`, then the same bytes through `add_entries`),
`tests/unit/test_tools.cpp` (cc F16 conflicts), `test_registry.cpp`, `runtime_selftest.cpp`,
`tests/canary/rri_canary.cpp` (independent decode of the same bytes).

## Structure

- **`computed_program_hash()`** (`dictionary.cpp:12-19`): `fingerprint64_u64s` over the `std::map` keys, already sorted.
- **Anonymous namespace — parsing** (`dictionary.cpp:21-271`): `max_field_depth = 32`; `read_name`; the
  `min_*_bytes` constants and `count_fits`; `parse_field` (recursive FIELD with the v2 kind arities and, since
  1.6, the map/set entry geometry); `parse_type` (struct/enum body; 1.6: the `transparent` and `fixed` flags);
  `parse_literal`; `arg_kind_name`; `parse_call` (ARG validation).
- **Anonymous namespace — encoding** (`dictionary.cpp:273-332`): `field_size`, `put_name`, `put_field`,
  `type_body_size`, `put_entry` (header + body + zero pad to 8).
- **Anonymous namespace — canonical text helpers** (`dictionary.cpp:334-381`): `indent`, `json_field`,
  `toml_field`.
- **Conflict helpers** (`dictionary.cpp:383-411`): `conflict_text_max` (256), `clipped`,
  `conflict_summary`, `room_for`.
- **`add_type`**, **`add_literal`**, **`add_call`** (`dictionary.cpp:468-474`): first definition wins; a different body is a `type_conflict` (types) or an `id_conflict` (literals, calls).
- **Parsing members** (`dictionary.cpp:449-586`): `add_entries`, `add_rri`, `merge`.
- **Lookups** (`dictionary.cpp:588-629`): the four `find_*` and `record_size`.
- **`const_value_text`** (`dictionary.cpp:633-676`, B7): a constant's value as the scalar or enumerator
  it stands for, for the JSON and TOML renderings.
- **Canonical JSON / TOML** (`dictionary.cpp:679-836`): `to_json`, `to_toml`.
- **`to_rri`** (`dictionary.cpp:838-921`).
- **`tools::load_dictionary_rri` / `load_dictionary_file`** (`dictionary.cpp:977-983`).

## How it works

### ENTRY stream: `add_entries`

```cpp
if (!c.read(total_len))
    return std::unexpected(error::corrupt_dictionary);
if (total_len == 0)
{ // trailing zero padding (e.g. section alignment) is tolerated
    for (const std::byte* p = c.p; p != c.end; ++p)
        if (*p != std::byte{0})
            return std::unexpected(error::corrupt_dictionary);
    return {};
}
if (total_len < fmt::entry_header_size || total_len % 8 != 0 ||
    total_len - sizeof(std::uint32_t) > c.remaining())
    return std::unexpected(error::corrupt_dictionary);
```

(`dictionary.cpp:487-498`.) Each ENTRY is `u32 total_len, u8 tag, u8 version, u16 rsv, body`. `total_len`
must be at least 8, a multiple of 8, and inside the stream; a zero length means the rest must be zero padding
(an ELF section is aligned that way). `version != 1` entries are skipped whole — forward compatibility. The
body is parsed by tag: `tag_struct`/`tag_enum` → `parse_type` → `add_type`; `tag_literal` → `try_emplace`
(first text wins); `tag_call` → `try_emplace`; any other tag (alias/image entries of §6.7, future tags) is
skipped. Any parse failure is `corrupt_dictionary` for the whole call — a partially applied stream is never
reported as success, although entries already inserted stay in the maps.

### `add_rri`: header, then bounded entry stream

`add_rri()` (`dictionary.cpp:538-585`) reads the 40-byte header and checks the `"ESRG"` magic,
`major == format_major` and `format_minor_oldest ≤ minor ≤ format_minor` (`bad_version` otherwise: 1.6 and 1.7 are read —
1.5 predates the `map`/`set` FIELD kinds and the flag bits, 1.4 has no constant arguments and its reader would
misparse their ENTRY, 1.3 may carry the canonical names from before format 1.4, formats ≤ 1.2 carry FNV-1a ids this
library no longer computes, a newer minor is not guessed at), `hash_algo == hash_algo_xxh3_64` (`unsupported`),
then (FR-D5) `n_entries ≤ decode_limits::dictionary_entries` (1 000 000; more is `corrupt_dictionary`,
`dictionary.cpp:560`), then
`count_fits(c, n_entries, min_entry_bytes)`. It walks `n_entries` headers only to find the end of the entry area,
so trailing bytes after the last entry are ignored — which is what lets the readers hand it a 1.7 dictionary
frame's payload whole, pad, forward link and CRC trailer included — and hands exactly that window to
`add_entries`. The header's `rri_flag_delta` (1.7) is not needed here: a delta is an RRI with fewer entries, and
merging it into what a reader already has (`merge`) is the same operation as merging a full image.
`program_hash` is taken from the header if the dictionary had none yet. The header's ABI tag becomes `abi` for the
first source (`abi_known`); a later source with a different tag sets `abi_mixed`, which the capture walker reports
as an `abi_mismatch` problem. `merge()` (`dictionary.cpp:587`) carries the three ABI fields over the same way.

### `count_fits` and the depth limit

```cpp
// Minimum on-wire size of each repeated element; an on-wire count must fit in the remaining bytes at this
// rate before anything is allocated from it (cc F17 / gk 1.15: an 80-byte ENTRY must not reserve 6 MiB).
constexpr std::size_t min_field_bytes = 32, min_enumerator_bytes = 10, arg_bytes = 16,
                      min_entry_bytes = 8;

bool count_fits(const fmt::cursor& c, const std::size_t count, const std::size_t min_bytes)
{
    return count <= c.remaining() / min_bytes;
}
```

(`dictionary.cpp:41-47`.) `n_elem`, `n` (fields or enumerators), `n_args` and `n_entries` are all checked
this way before `resize`/`reserve`. The checked-in `tests/fixtures/negative/entry_n_elem_65535.bin` is the
bomb this guards against. `parse_field` also refuses nesting deeper than `max_field_depth` (32), which bounds
recursion on a hostile stream independently of the byte check.

### FIELD parsing and the v2 kind arities

```cpp
else if (f.k == kind::string_ || f.k == kind::vector_ || f.k == kind::optional_ ||
         f.k == kind::variant_ || f.k == kind::map_ || f.k == kind::set_)
{ // v2: a reference slot {u32 off, u32 n}; element descriptors as the kind requires
    if (f.size != 8)
        return false;
    const std::size_t want = f.k == kind::string_  ? 0
                             : f.k == kind::variant_ ? f.extent
                             : f.k == kind::map_     ? 2
                                                     : 1;
    if (f.elem.size() != want || (f.k == kind::variant_ && f.extent == 0))
        return false;
    for (const field& e : f.elem)
        if (e.size == 0 &&
            e.k != kind::struct_) // an empty element image only for empty structs
            return false;
    if (f.k == kind::map_ || f.k == kind::set_)
    { // 1.6: `extent` is the entry stride: it must hold the key (and the value at its offset), be a
        // multiple of the entry alignment, and not be zero
        const field& key = f.elem[0];
        const std::uint64_t end =
            f.k == kind::map_
                ? std::max<std::uint64_t>(key.size, std::uint64_t{f.elem[1].offset} + f.elem[1].size)
                : key.size;
        std::uint32_t al = key.align ? key.align : 1;
        if (f.k == kind::map_ && f.elem[1].align > al)
            al = f.elem[1].align;
        if (f.extent == 0 || f.extent < end || f.extent % al != 0 || key.offset != 0 ||
            (f.k == kind::map_ && f.elem[1].offset < key.size))
            return false;
    }
}
```

(`dictionary.cpp:51-129`.) A FIELD is `u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16
since, u16 deprecated, u16 n_elem, u64 type_hash, u32 extent, name, FIELD × n_elem`. After the nested
elements are parsed: an `array_` must have exactly one element, a non-empty element when `extent > 0`, and
`extent × elem.size ≤ size` (computed in `uint64_t`); the six dynamic kinds must be 8-byte reference slots
with 0 / 1 / 1 / `extent` / 2 / 1 element descriptors (string / vector / optional / variant / map / set,
`extent ≥ 1` for a variant) and no zero-size element unless it is a struct; a `map_` or `set_` FIELD (1.6) must
also describe a usable entry — its `extent` is the entry stride, non-zero, a multiple of the larger element
alignment and at least the end of the last element; the key at entry offset 0; a map's value at an offset no
smaller than the key's size — so that `decoder.cpp` and the Lua dissector lay the block out from the FIELD alone;
`literal_` and every value outside the defined ranges (gaps between `byte_` and `enum_`, between `struct_` and
`string_`, above `set_`, and 0) are rejected. Sizes and alignments must also be ones a writer can produce (D-01):

```cpp
if (is_scalar_kind(f.k) && f.size != kind_size(f.k) &&
    !(f.k == kind::wchar && (f.size == 2 || f.size == 4)))
    return false;
if (f.k == kind::enum_ && f.size != 1 && f.size != 2 && f.size != 4 && f.size != 8)
    return false;
if (f.align > 4096 || (f.align & (f.align - 1)) != 0)
    return false;
```
(`dictionary.cpp:121-127`.) A scalar FIELD is exactly its kind's size (a `wchar` 2 or 4 bytes), an enum FIELD 1, 2,
4 or 8 bytes, and the alignment a power of two no larger than 4096 (0 passes the test and counts as 1). Otherwise
a forged size or alignment would reach the decoders' stride arithmetic and the visitors: a `u8` of `0xFFFFFFFF`
bytes with alignment `0xFFFE` makes `n × stride` wrap. `parse_type` (`dictionary.cpp:131-190`) then checks every direct field lies inside
`[0, size)`; the decoder re-checks against the actual payload. For format 1.6 it also reads the two flag bytes: for a
struct, bits other than `flat` and `transparent` are refused, and a `transparent` type must be non-flat with
exactly one field that is unnamed, at offset 0, of a slot kind other than `string_` (`vector_`, `optional_`,
`variant_`, `map_`, `set_`), the type's size being that slot (8); for an enum, the byte after the underlying kind
— reserved until 1.5 — is `flags`, and any bit other than `fixed` is refused (`type::enum_fixed`).

### Calls: `parse_call`

`parse_call()` (`dictionary.cpp:219-300`): `u64 hash, u16 n_args, u16 flags, u32 rsv`, then `n_args` ARGs of
`u8 arg_kind, u8 kind, u8 rsv[6], u64 hash`. Unknown flag bits (anything but `call_flag_print`, `call_flag_newline`
and (1.7) `call_flag_log`) and `arg_kind > arg_const` are rejected; a `log` call must also be a println
(`dictionary.cpp:225-230`). A scalar ARG must have a known `kind_size` and a hash equal to
`fingerprint64(to_string(k))`; a type, literal or string ARG must carry `kind == 0`; a string ARG's hash must be
`fingerprint64("string")`.

A constant ARG (`arg_const`, format 1.5) is followed by 8 more bytes, its `u64 value`
(`dictionary.cpp:246-259`). Its `kind` is either a known scalar kind, and then the hash must be that kind name's
hash, or 0 for an enum, whose hash is the enum's schema hash. For a scalar constant the value's bits above the
scalar's size must be zero, and a `bool` constant must be 0 or 1; anything else fails the parse
(`corrupt_dictionary`). An enum's size is not known here; the frame walker checks it (`src/reader.cpp`).
`arg_kind_name` (`dictionary.cpp:202-217`) names the category `"const"` for JSON and TOML. A print call must have at least one argument and its first must be a literal (the format
string). A log call (format 1.7) must instead have the envelope of `log.hpp`'s `log_layout`: argument 0 a
`u8` constant (the level), 1 a literal (the file), 2 a `u32` constant (the line), 3 a `u8` constant at most 15 (the
mask), 4 a `u64` scalar (the timestamp), then a `u32` scalar when the mask's bit 0 is set and a `u64` scalar when
bit 1 is, and then the format literal (`dictionary.cpp:269-294`); anything else fails the parse. JSON and TOML
write such a call as `"form": "log"` / `form = "log"`. `call_arg::is_literal` is derived from `ak` for the 1.0-era callers.

### `add_entries` conflicts and `merge`

```cpp
// First definition wins; a later, different body for the same schema hash is recorded (cc F16 / gk 1.10).
void add_type(dictionary& d, type&& t)
{
    const std::uint64_t h = t.schema_hash;
    const auto [it, inserted] = d.types.try_emplace(h, std::move(t));
    if (inserted)
        return;
    if (it->second != t && room_for(d, d.conflicts))
        d.conflicts.push_back({conflict_summary(it->second), conflict_summary(t)});
}
```

(`dictionary.cpp:413-421`.) `type::operator==` is defaulted and structural (name, hashes, size/align, flags, fields,
enumerators), so two identical ENTRYs from two images collapse silently and two different bodies under one
schema hash become a `type_conflict{kept, dropped}`. Literals and calls follow the same rule (review G2-10): `add_literal` and `add_call` (`dictionary.cpp:468-474`) keep the first body and record a different
second one as an `id_conflict` (texts for a literal; `call_text`, the flags and each argument's kind and hash,
for a call).

A conflict is bounded in size and number (D-03). Two whole `type` values per `type_conflict` would let a few
bytes of hostile input that redefine one large type many times make the dictionary copy it once per redefinition.
So:

- `conflict_summary(t)` keeps what the reports print: the name (clipped to `conflict_text_max`, 256 bytes, plus
  `"..."`), `schema_hash`, `layout_hash`, `size`, `align`, `is_enum`, `is_flat` and `underlying`. Fields and
  enumerators are left out. The kept definition in `types` stays whole.
- The texts of an `id_conflict` (a literal's text, a call's `call_text`) go through `clipped` too.
- `room_for(d, list)` admits an entry while the list holds fewer than `dictionary::max_conflicts` (64) and
  otherwise counts it in `d.conflicts_omitted`; the cap applies to `conflicts` and `id_conflicts` separately, and
  one counter holds what both dropped.

`merge()` (`dictionary.cpp:557-586`) reuses the three helpers, appends the other side's `conflicts` and
`id_conflicts` through `room_for` (so the cap holds after a merge), adds the other side's `conflicts_omitted`,
adopts the other's `program_hash` only if this one has none, and merges the ABI fields
(the first known tag wins; a different one sets `abi_mixed`).
(Note: after a moved-from `t` the comparison `it->second != t` in `add_type` compares against the argument
object; `try_emplace` only moves when it inserts, so `t` is intact in the comparison branch.)

### `record_size`

```cpp
result<std::uint32_t> dictionary::record_size(const call_arg& a) const noexcept
{
    if (a.is_literal || a.ak == fmt::arg_const)
        return 0; // FR-L6: a literal occupies no bytes in a compact frame; 1.5: nor does a constant
    if (a.ak == fmt::arg_scalar)
        return scalar_size(a.k);
    if (a.ak == fmt::arg_string)
        return std::unexpected(error::unsupported); // dynamic: u32 len + bytes, read by the walker
    const type* const t = find_type(a.hash);
    if (!t)
        return std::unexpected(error::unknown_type);
    if (!t->is_flat)
        return std::unexpected(error::unsupported); // dynamic members: v2
    return t->size;
}
```

(`dictionary.cpp:641-655`.) The compact-frame cutter's oracle for fixed-size records. Strings and dynamic
types answer `unsupported` on purpose: their length is on the wire and `reader.cpp` reads it. A compact literal
is 0 bytes (FR-L6), and so is a constant (1.5: its value is `call_arg::value`); a scalar is `scalar_size(k)`, which takes a `wchar`'s width from the writer's ABI tag.

### Canonical JSON and TOML

`to_json()` (`dictionary.cpp:679-771`) is hand-built with keys in sorted order and two-space indent:
`calls` (each `{args: [{hash, kind[, scalar][, value, bits]}], [form,] hash}`), `literals` (`{id, text}`), `program_hash`,
`types` (`align, deprecated, enumerators|fields, [fixed,] is_enum, is_flat, layout_hash, name, schema_hash, since,
size[, transparent][, underlying]`), with every field object's keys also sorted (`json_field`; a `map`/`set`
field's `elem` array holds the key and value descriptors, its `extent` the entry stride). `"fixed"` (1.6) is
written for every enum, `"transparent": true` only for a container argument's type — in the golden
`dynamic.json`, `std::vector<i32>` carries it and `corpus::Side` has `"fixed": true`. Arrays follow `std::map`
order — hash order — so two dictionaries with the same content render identically (FR-W5). `form` appears
only when `flags != 0`. A constant argument (1.5) has `"kind": "const"`,
`"scalar"` when it is a scalar (an enum constant has none; its `hash` is the enum's schema hash), and (B7)
two members: `"value"`, the value typed as what it stands for, and `"bits"`, the `u64` as 16 hex
digits — in the golden `orders.json`:
`{"hash": "0x5c23eefcac97a25f", "kind": "const", "scalar": "i32", "value": 42, "bits": "0x000000000000002a"}`,
`{"hash": "0x074e267cca5188e1", "kind": "const", "value": "sell", "bits": "0x0000000000000002"}`,
`… "scalar": "f64", "value": -1.5, "bits": "0xbff8000000000000"`. `const_value_text` (`dictionary.cpp:633-676`)
renders integers and finite floating-point values as numbers (a float without `.`, `e` or `E` gets `.0`), `bool`
as `true`/`false`, characters and `byte` as quoted strings (through `tools::value_text`), an enum constant as its
enumerator's name in quotes (looked up in the dictionary by the ARG hash; a negative enumerator is matched on the
underlying width's bits), and anything else — a non-finite float, a value no enumerator names, an enum the
dictionary lacks — as a quoted string, so every spelling is valid JSON and valid TOML and a consumer that needs
the exact value never parses the text.
`to_toml()` (`dictionary.cpp:774-836`)
emits the same content as arrays of tables (`[[types]]`, `[[types.fields]]`, `[[types.fields.elem]]`,
`[[types.enumerators]]`, `[[literals]]`, `[[calls]]`, `[[calls.args]]`; a constant ARG as `value = 42` and
`bits = "0x…"`; 1.6: `fixed = true|false` under an enum's `[[types]]`, `transparent = true` under a container
argument's). Both use `append_quoted` for
strings, which is valid JSON and a valid TOML basic string.

### `to_rri`

`to_rri()` (`dictionary.cpp:838-921`) writes the 40-byte RRI header (`"ESRG"`, `format_major`,
`format_minor`, `flags`, `hash_algo_xxh3_64`, `n_entries`, `program_hash`, `0`, the dictionary's `abi` — the
source's tag, not the tool's) and then one ENTRY per type,
literal and call via `put_entry`, which sizes the body first (`type_body_size` = 34 + name + fields/enumerators;
literal 16 + text; call 16 + 16 × args + 8 × constant args, each constant ARG being followed by its `value`
(`dictionary.cpp:898-917`, 1.5)), writes the 8-byte header with `version = 1`, and zero-pads to a
multiple of 8. The struct flags byte is `flat | transparent` and the enum's second byte `fixed` (1.6,
`dictionary.cpp:862-869`), so a re-encoded 1.6 dictionary carries both flags; `put_field` writes a map's two and a
set's one element descriptor after the name as `blob_bytes.hpp` does.

### `computed_program_hash`, `scalar_size` and the ABI

`computed_program_hash()` hashes the type schema hashes in key order with `fingerprint64_u64s`, matching
`registry::program_hash()` (FR-H3). It is what `srdump --program-hash` prints when the source had no header
(an ELF section). `record_size()` sizes a top-level scalar argument with `scalar_size(k)` (`dictionary.hpp`):
the fixed width of the kind, except `wchar`, whose width comes from the writer's ABI tag (2 on Windows, 4 on
Linux) — the one scalar kind whose canonical name does not carry a width. A compact literal record is 0 bytes
(FR-L6); there is no path for format 1.0's 8-byte literal records.

## Maintaining it

- **Tests:** `test_tools.cpp` ("cc F16 dyn::dictionary records same-hash different-body definitions as
  conflicts"), `test_roundtrip.cpp` ("FR-A5 constants through the dictionary: RRI round trip, verify, dump, JSON,
  print (1.5)": `to_rri(load(rri))` keeps every call, the JSON value, and a constant with bits above its scalar's
  size is refused), `runtime_selftest.cpp` `review_2_1_10_decoders()` (the D-01 FIELD refusals and the D-03
  201 redefinitions, which keep 64 summaries without fields and count the rest), `test_registry.cpp` ("FR-R9/FR-D3 export_image() parses back and lists the same entities",
  "FR-D2 to_json() names every type"), `runtime_selftest.cpp` (hand-built ENTRYs, the `entry_n_elem_65535`
  and `bomb_array_extent` bombs), `format_canary_rri` (`rri_canary.cpp` decodes
  `orders.rri` from literal offsets — it fails if the ENTRY layout drifts), the `golden_fixture_{rri,json,toml}_byte_exact`
  tests, `srdump_env_dump_parses`, and `fuzz_rri` with `tests/fixtures/corpus/rri/*`.
- **Wire format:** any change to `parse_field`/`parse_type`/`parse_literal`/`parse_call` layouts, to
  `to_rri`'s sizes, or to the accepted `kind` set is a format change: bump `format_minor`, update
  `tests/golden/blobs.cpp`, `tests/fixtures/golden/*`, `tests/canary/rri_canary.cpp`; the seed corpora
  under `tests/fixtures/corpus/` (named after the minor they carry, e.g. `-1.6`) are regenerated with them.
- **Canonical text is frozen too:** `to_json`/`to_toml` output equals `orders.json`/`orders.toml` and
  `dynamic.json`/`dynamic.toml`. New keys must be added in sorted position and, like `form` and `transparent`,
  only when non-default if old renderings must not change (`fixed` is written for every enum, so adding it
  changed every enum's rendering).
- **Every count → `count_fits` before allocation; every recursion → a depth argument.** A new repeated
  element needs a `min_*_bytes` constant. `n_entries` is also bounded by `decode_limits::dictionary_entries`
  (`test_limits.cpp` "2.1.16 A3 decode_limits::dictionary_entries bounds what a dictionary may
  declare"); the byte bound `decode_limits::dictionary_bytes` is applied by the readers before the bytes reach
  `add_rri` (`src/reader.cpp`, `src/file_io.cpp`).
- **Reject unknown kinds and flags.** The `parse_field` kind-range test must be extended, not relaxed, when a
  kind is added; `parse_call` must keep refusing unknown flag bits.
- **Geometry checks belong here and in the decoder.** `dictionary.cpp` validates the descriptor against
  itself (`offset + size ≤ type.size`, `extent × elem ≤ size`, slot size 8); `decoder.cpp` validates against
  the actual payload. Do not drop either.
- **Allocation:** this is the tools layer; it allocates freely (`std::map`, `std::string`). It must never be
  called from `serialize`'s hot path.
- **Threading:** a `dictionary` is a plain value; no internal synchronisation. `registry::to_json` builds a
  fresh one per call.
- **Fuzz** `fuzz_rri` under ASan/UBSan after any parser or encoder change.
