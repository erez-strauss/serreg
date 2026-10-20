# `include/es/serreg/detail/embed_none.hpp`

No embedded registry copy (review addendum A4). `<es/serreg/blob.hpp>` includes this header instead of
`detail/embed_elf.hpp` when `ES_SERREG_EMBED_NONE` is defined — PUBLIC on `es::serreg` when CMake is configured
with `ES_SERREG_IMAGE_FORMAT=none`, or defined by hand for a toolchain without named sections. It defines the
three hooks `embed_type_blob<T>()`, `embed_call_blob<Ts...>()`, `embed_literal_blob<S>()` as empty `consteval`
functions, so the program carries no `es_serreg_dict` section. Descriptors, hashes, blobs and the wire format are
identical to the ELF build; only the section copy is missing. It includes the same headers as `embed_elf.hpp` and
compiles on its own (include rules).

## Depends on / used by

Includes: `es/serreg/detail/blob_bytes.hpp` and `es/serreg/literal.hpp` (`fixed_string`).

Included by: `include/es/serreg/blob.hpp` (`#if defined(ES_SERREG_EMBED_NONE)`), and by its own generated
translation unit in `tests/headers` (`serreg_headers_public`, without `ES_SERREG_EMBED_NONE`). Built in the tree by the private
`serreg_image_none` library and `link_image_none` (`tests/link/CMakeLists.txt`, Debug builds only).

## Structure

- **`namespace es::serreg::detail`** (`embed_none.hpp:10-23`): `embed_type_blob<T>()` (`embed_none.hpp:13-15`),
  `embed_call_blob<Ts...>()` (`embed_none.hpp:16-18`), `embed_literal_blob<S>()` (`embed_none.hpp:19-21`).

## How it works

```cpp
template<class T> consteval void embed_type_blob()
{
}
```

(`embed_none.hpp:13-15`.) `make_type_desc<T>()`, `make_call_desc<Ts...>()` and `literal_blob_span<S>()` call the
hooks as in the ELF build; here they instantiate nothing, and `ES_SERREG_DICT_SECTION` and the `*_blob_copy_v`
templates do not exist. Offline tools then need another carrier of the same ENTRY bytes: an `.rri` from
`registry::export_image()` (or `ES_SERREG_DUMP_REGISTRY`, `srdump --rri-out`), or a capture, which embeds its
dictionary anyway. `tools::extract_embedded_dictionary` on such an ELF executable returns `not_found`.

## Maintaining it

- **Same signatures as `embed_elf.hpp`.** `blob.hpp` calls the hooks unconditionally; a hook added to one header
  must be added to the other.
- **Do not define `ES_SERREG_EMBED_NONE` in only some TUs** of a program built with the ELF backend: the result is a
  section that lacks the copies of those TUs (types instantiated only there are missing from `srdump --elf`), and
  nothing at build time reports it.
- **Tests:** `link_image_none_backend` (no `es_serreg_dict` section: `extract_embedded_dictionary("/proc/self/exe")`
  is `not_found`) and `link_image_none_program_hash_equal` (same program hash as `link_plain`, so the descriptors
  and blobs are unchanged).
- **Allocation/threading:** none; compile-time only.
