# `include/es/serreg/blob.hpp`

The completed constants `descriptor_v<T>`, `call_desc_v<Ts...>`, `call_hash_v<Ts...>`, `call_name_v<Ts...>`,
`literal_blob_v<S>`, and the hook that places the embedded registry copy (spec §6.7, §6.8, D6). One blob format,
three carriers: the section (read offline by `srdump --elf`), the RRI export, and the dictionary inside a capture. It
is the join between the **compile-time reflection layer** (it consumes `reflect.hpp`'s `type_desc` pieces) and the
**registry** (its constants are what `registrar*` nodes point at). It is **reflection-dependent** (includes
`reflect.hpp`, uses `std::define_static_string`).

The header is assembled from three parts. The ENTRY blob bytes of types and call signatures,
`call_arg_desc`/`call_desc`, the form tags and the call string are in `detail/blob_bytes.hpp`
(`files/include-es-serreg-detail-blob_bytes.hpp.md`; include rules). The ELF-specific part (review addendum A4) — the `es_serreg_dict` section copies and `ES_SERREG_DICT_SECTION` — lives in
`detail/embed_elf.hpp`, and `detail/embed_none.hpp` is the no-section alternative. `blob.hpp` includes
`blob_bytes.hpp` and the chosen backend at the top, then defines the factories that call the backend's `embed_*`
hooks and the public constants. Users include `<es/serreg/blob.hpp>` (or `<es/serreg/serreg.hpp>`), never the
`detail/` headers.

## Depends on / used by

Includes, package headers first (include rules): `es/serreg/descriptor.hpp`,
`es/serreg/detail/blob_bytes.hpp` (`type_blob_bytes_v`, `call_blob_bytes_v`, `call_args_v`, `call_string`,
`call_flags`), `es/serreg/hash.hpp` (`fingerprint64`), `es/serreg/literal.hpp` (`literal_blob_bytes_v`),
`es/serreg/reflect.hpp` (`make_type_desc_base`, `mapped_t`), and `es/serreg/detail/embed_none.hpp` when
`ES_SERREG_EMBED_NONE` is defined, else `es/serreg/detail/embed_elf.hpp`; then `<cstdint>`, `<meta>`, `<span>`,
`<string_view>`, `<type_traits>`. Each backend includes `blob_bytes.hpp` itself (the blob variables it copies), so
it can sit with the other includes.

Included by: `registry.hpp` (and through it `serialize.hpp`, `deserialize.hpp`, `capture.hpp`, `print.hpp`),
`serreg.hpp`; `tests/golden/blobs.cpp` reads `detail::type_blob_bytes_v<T>`, `literal_blob_bytes_v<S>`,
`call_blob_bytes_v<Ts...>` directly.

## What it defines

Public:

| name | kind | meaning |
|---|---|---|
| `descriptor_v<T>` | `ES_SERREG_HIDDEN inline constexpr type_desc` | the complete descriptor of `mapped_t<T>`, blob span filled |
| `call_hash_v<Ts...>` | `inline constexpr uint64_t` | `fingerprint64(call_string<remove_cvref_t<Ts>...>())` |
| `call_name_v<Ts...>` | `ES_SERREG_HIDDEN inline constexpr string_view` | the call string, `define_static_string`'d |
| `call_desc_v<Ts...>` | `ES_SERREG_HIDDEN inline constexpr call_desc` | hash, args, blob, flags |
| `literal_blob_v<S>` | `ES_SERREG_HIDDEN inline constexpr span<const byte>` | the literal ENTRY (COMDAT copy), after the `embed_literal_blob<S>()` hook |

`call_arg_desc`, `call_desc`, `print_tag` and `println_tag` are public names too, defined in
`detail/blob_bytes.hpp` and reached through this header. `ES_SERREG_DICT_SECTION` is in
`detail/embed_elf.hpp`.

`namespace detail`:

| name | kind | meaning |
|---|---|---|
| `make_type_desc<T>()`, `make_call_desc<Ts...>()`, `literal_blob_span<S>()` | `consteval` | call the `embed_*` hook, return the completed constant (`blob.hpp:29-46`) |

The blob builders (`put_field`, `type_blob_bytes`, `call_string`, `call_blob_bytes`, `type_blob_bytes_v`,
`call_blob_bytes_v`, …) are in `detail/blob_bytes.hpp`; the section copies and the `embed_*` hooks are in
`detail/embed_elf.hpp` / `embed_none.hpp`.

## How it works

### `descriptor_v<T>` completes the circle

```cpp
template<class T> consteval type_desc make_type_desc()
{
    embed_type_blob<T>(); // this TU's section copy (embed_elf.hpp), or nothing
    type_desc d = make_type_desc_base<T>();
    d.blob = type_blob_bytes_v<T>;
    return d;
}
template<class T>
ES_SERREG_HIDDEN inline constexpr type_desc descriptor_v = detail::make_type_desc<detail::mapped_t<T>>();
```
(`blob.hpp:29`, `:51-53`). `descriptor_v<T>` is defined over `mapped_t<T>`, so `descriptor_v<Stamp>` and
`descriptor_v<StampFlat>` are the same object when `as_flat<Stamp>` maps to `StampFlat`. The blob span points at
the COMDAT array, which the linker deduplicates across TUs of one image; with hidden visibility every image gets
its own.

### COMDAT blob vs TU-local section copy

Two copies of every blob exist per image in the default (ELF) build, on purpose. The second one is
defined outside this header, in the backend `blob.hpp` includes at the top:

```cpp
#if defined(ES_SERREG_EMBED_NONE)
#include <es/serreg/detail/embed_none.hpp>
#else
#include <es/serreg/detail/embed_elf.hpp>
#endif
```
(`blob.hpp:12-16`). The COMDAT `*_blob_bytes_v` is what runtime code references (descriptor spans,
`reg_node::blob`, `lit<S>::text()`); the `static constexpr *_blob_copy_v` in `detail/embed_elf.hpp` exists only so
that `srdump --elf` can read a complete dictionary out of a binary without running it. Each `make_*` function calls
its `embed_*` hook first, which takes the address of the copy inside a `consteval` function — enough to instantiate
the variable in the current TU, not an odr-use that survives to run time. The copies are internal-linkage because
GCC puts every COMDAT variable naming a custom section into one section with the first variable's group, which
breaks multi-TU links (`files/include-es-serreg-detail-embed_elf.hpp.md`). `[[gnu::retain]]` needs binutils ≥ 2.36
(see `CLAUDE.md`); `link_gc_sections` in `tests/link/` builds with `--gc-sections` and checks the section survives.
Cost: about 250 B per type per TU that instantiates it (`docs/deviations.md`, cc F8). With `ES_SERREG_EMBED_NONE`
(CMake `ES_SERREG_IMAGE_FORMAT=none`) the hooks are empty and there is no section; descriptors, blobs and hashes are
unchanged (`link_image_none_program_hash_equal`).

### What is emitted per type per image

| symbol | linkage / section | count | referenced by |
|---|---|---|---|
| `detail::type_blob_bytes_v<T>` | COMDAT `.rodata`, hidden | 1 per image | `descriptor_v<T>.blob`, `registrar<T>::node.blob` |
| `detail::type_blob_copy_v<T>` (`embed_elf.hpp`) | internal, `es_serreg_dict`, retained | 1 per TU that instantiates `descriptor_v<T>`; none with `ES_SERREG_EMBED_NONE` | nothing at run time (`srdump --elf`) |
| `descriptor_v<T>` | COMDAT `.rodata`, hidden | 1 per image | `registrar<T>::node.payload`, `put_object`/`scrub_range`, `view` |
| `detail::fields_v<T>`, `gaps_v<T>`, `bools_v<T>`, `enumerators_v<E>`, `element_field_v<E>`, `variant_alts_v<V>` (`reflect.hpp`) | COMDAT, hidden | 1 per image | `descriptor_v<T>` spans |
| `call_desc_v<Ts...>`, `call_args_v<Ts...>`, `call_blob_bytes_v<Ts...>`, `call_name_v<Ts...>` | COMDAT, hidden | 1 per image per signature | `registrar_call<Ts...>::node` |
| `call_blob_copy_v<Ts...>`, `literal_blob_copy_v<S>` | internal, section | 1 per TU | `srdump --elf` |

The "1 per image" column is the unit the registry counts (one reference per image on the heap entry,
`registry_entry::refs`), the "1 per TU" column is the measured ~250 B per type per TU cost.

### How `srdump --elf` consumes the section

`tools::extract_embedded_dictionary(path)` (`src/elf_reader.cpp`; `extract_registry_from_elf` is a wrapper
for it) reads the file, checks the ELF magic, finds the section header named `es_serreg_dict`, and hands its raw bytes to `dyn::dictionary::add_entries`, which walks ENTRY by ENTRY using
`total_len` — a stream of section copies from every TU, duplicates included, absorbed by hash. Because the copies
are `static constexpr` and `alignas(8)`, the linker concatenates them without gaps; the reader does not need
relocations. `ES_SERREG_DICT_SECTION` names the section without a leading dot on purpose (the reader comment says
some documents write `.es_serreg_dict`; the attribute string is authoritative).

### Adding a fourth carrier

The three carriers (section, RRI, capture dictionary) share one encoder, so a new one (an INDEX footer, a
delta dictionary — both listed as future work) should be a new *container* around the same ENTRY bytes, not a new
encoding: build it from `registry::snapshot().entries[i].blob` or `dyn::to_rri`, and let `add_entries` parse it.

## Maintaining it

- **The bytes are decided in `detail/blob_bytes.hpp`** (and `literal.hpp` for literals); its page lists the golden
  tests that freeze them and the rules for an encoding change. `call_hash_v` and `call_name_v` here are derived from
  `call_string`, so they change with it.
- **ODR/visibility.** Keep `ES_SERREG_HIDDEN` on every per-image constant here and keep the section copies in
  `detail/embed_elf.hpp` `static`. Making a section copy `inline` re-creates the COMDAT-group section bug the comment describes
  (multi-TU link failures); making a COMDAT blob default-visibility lets glibc bind it `STB_GNU_UNIQUE`, which
  turns `dlclose` into a no-op for that DSO, so a reload does not run its new code
  (`dlopen_I_plugin_without_flags_unloads` checks that the attributes alone prevent it; the `es::serreg` target adds
  `-fno-gnu-unique` PUBLIC for the same reason).
- `descriptor_v<T>.blob.data() == type_blob_bytes_v<T>.data()` is asserted in the golden test; do not introduce a
  copy between them (`reg_node::blob` is this span; in `push()` the registry views it in place for the main program and copies it for
  any other image).
- `alignas(8)` on the section copies keeps the section a contiguous chain of 8-multiple ENTRYs, which is what
  `extract_embedded_dictionary` → `add_entries` walks by `total_len` (the section is named without a leading dot;
  `src/elf_reader.cpp:138`).
- **The `embed_*` hooks are the only executable-format dependency of this header**: `make_type_desc`,
  `make_call_desc` and `literal_blob_span` must keep calling them, and `embed_elf.hpp`/`embed_none.hpp` must keep
  the same three signatures.
- **Include rules**: the backend include stays with the other package includes at the top; each backend
  includes `detail/blob_bytes.hpp` and `literal.hpp` itself and compiles alone (`serreg_headers_public` in
  `tests/headers`, ctest `headers_include_rules`).
- Tests: `tests/golden/blobs.cpp`, `tests/golden/hashes.cpp`, `tests/canary/`, `tests/headers/`, `tests/link/` (`link_gc_sections`,
  `link_lto`, `link_two_tu_dedup`, `link_no_pie_*`, and in Debug builds `link_image_none_*`), `tests/unit/test_capture.cpp` "FR-D6 extract_registry_from_elf
  (/proc/self/exe) matches the compile-time hashes", `srdump_elf_*` ctests (`srdump_elf_no_skipped_type` proves a
  skipped member's type has no section copy — `touch` never instantiates it).
