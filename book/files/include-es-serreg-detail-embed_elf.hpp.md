# `include/es/serreg/detail/embed_elf.hpp`

The header-side half of the ELF image backend (spec §6.8, FR-D6; split out of `blob.hpp`, review addendum A4): a TU-local copy of every type, literal and call ENTRY blob in the read-only section `es_serreg_dict`, so that
`srdump --elf` and `tools::extract_embedded_dictionary` can read a program's complete dictionary without running
it. It defines the section macro `ES_SERREG_DICT_SECTION`, the three copy variable templates, and the three
`consteval` hooks `embed_type_blob<T>()`, `embed_call_blob<Ts...>()`, `embed_literal_blob<S>()` that `blob.hpp`'s
descriptor factories call. It is included only by `<es/serreg/blob.hpp>`, at the top with the other package headers,
and is the default unless `ES_SERREG_EMBED_NONE` is defined. It includes the blobs it copies itself and compiles on
its own (include rules). It is part of the **compile-time layer** and depends on
reflection through the blobs it copies.

## Depends on / used by

Includes: `es/serreg/detail/blob_bytes.hpp` (`detail::type_blob_bytes_v<T>`, `call_blob_bytes_v<Ts...>`) and
`es/serreg/literal.hpp` (`literal_blob_bytes_v<S>`, `fixed_string`).

Included by: `include/es/serreg/blob.hpp` (`#if !defined(ES_SERREG_EMBED_NONE)`), and by its own generated
translation unit in `tests/headers` (`serreg_headers_public`). Read back by
`src/elf_reader.cpp` (`extract_embedded_dictionary`, `extract_registry_from_elf`) and therefore by `srdump --elf`,
`srdump --diff` on a binary, and the `srdump_elf_*` ctests.

## Structure

- **`ES_SERREG_DICT_SECTION`** (`embed_elf.hpp:14`): `[[gnu::section("es_serreg_dict"), gnu::used, gnu::retain]]`.
- **Section copies** (`embed_elf.hpp:19-25`): `type_blob_copy_v<T>`, `literal_blob_copy_v<S>`,
  `call_blob_copy_v<Ts...>` — `static constexpr`, `alignas(8)`, in the section.
- **`embed_*` hooks** (`embed_elf.hpp:28-39`): `consteval` functions that take the address of the copy.

## How it works

### The copies

```cpp
#define ES_SERREG_DICT_SECTION [[gnu::section("es_serreg_dict"), gnu::used, gnu::retain]]

namespace es::serreg::detail
{

template<class T>
ES_SERREG_DICT_SECTION alignas(8) static constexpr auto type_blob_copy_v = type_blob_bytes_v<T>;
```

(`embed_elf.hpp:14-20`.) Two copies of every blob exist per image, on purpose. The COMDAT `*_blob_bytes_v` in
`detail/blob_bytes.hpp` (types, calls) and `literal.hpp` (literals) is what run-time code references (descriptor spans, `reg_node::blob`, `lit<S>::text()`); the
`static constexpr *_blob_copy_v` here exists only for offline readers. It has internal linkage because GCC emits
every COMDAT variable that names a custom section into *one* section carrying the group of whichever came first,
which breaks multi-TU links; TU-local copies avoid that. Duplicates across TUs are harmless — readers key entries
by hash. `[[gnu::used, gnu::retain]]` keep the copies under `--gc-sections` although nothing references them at
run time (`retain` needs binutils ≥ 2.36). `alignas(8)` keeps the section a contiguous chain of ENTRYs whose
lengths are multiples of 8, which `dyn::dictionary::add_entries` walks by `total_len`.

### The hooks

```cpp
template<class T> consteval void embed_type_blob()
{
    static_cast<void>(&type_blob_copy_v<T>);
}
```

(`embed_elf.hpp:28-31`.) `make_type_desc<T>()`, `make_call_desc<Ts...>()` and `literal_blob_span<S>()` in
`blob.hpp` call `embed_type_blob<T>()`, `embed_call_blob<Ts...>()` and `embed_literal_blob<S>()` first. Taking the
address inside a `consteval` function instantiates the copy in the current TU; it is not an odr-use that survives
to run time. Because `descriptor_v<T>`, `call_desc_v<Ts...>` and `literal_blob_v<S>` are initialised through these
factories, every TU that instantiates one of them gets its section copy — and a `[[=skip]]` member's type, never
instantiated, gets none (`srdump_elf_no_skipped_type`).

## Maintaining it

- **Keep the copies `static`.** Making a copy `inline` re-creates the COMDAT-group section bug above (multi-TU link
  failures).
- **The section name has no leading dot** — the attribute string is authoritative; `extract_section_from_image`
  retries with `.es_serreg_dict` only as a courtesy for other producers.
- **The embed hooks must stay `consteval` and side-effect free**; they exist to instantiate a variable, and the
  `embed_none.hpp` versions must keep the same signatures.
- **Self-contained**: keep the two includes; the header must compile alone (`tests/headers`).
- **Byte content is not decided here**: the copies are the blobs of `detail/blob_bytes.hpp`/`literal.hpp`, frozen by
  `tests/golden/blobs.cpp`. Changing the section layout (alignment, name) changes what offline readers accept:
  golden `blobs.cpp`, fixtures, canary, `format_minor`, CHANGELOG.
- **Cost:** about 250 B per type per TU that instantiates it (`docs/deviations.md`, cc F8).
- **Tests:** `tests/link` (`link_gc_sections`, `link_lto`, `link_two_tu_dedup`, `link_no_pie_*`),
  `test_capture.cpp` "FR-D6 extract_registry_from_elf(/proc/self/exe) matches the compile-time hashes",
  `golden_srdump_program_hash_elf_agrees`, the `srdump_elf_*` ctests; `link_image_none_backend` checks the
  opposite (no section with `ES_SERREG_EMBED_NONE`).
- **Allocation/threading:** none; compile-time only.
