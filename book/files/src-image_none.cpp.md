# `src/image_none.cpp`

The null image backend of the registry (`src/image.hpp`; review addendum A4). The whole program is one
image and nothing is ever unloaded. It is compiled into
`libserreg` instead of `src/image_elf.cpp` when CMake is configured with `-DES_SERREG_IMAGE_FORMAT=none`, which is
accepted only together with `-DES_SERREG_BUILD_SHARED=OFF` (a shared `libserreg` serves images that can be
unloaded). The same setting defines `ES_SERREG_EMBED_NONE` PUBLIC, so the headers emit no `es_serreg_dict` section
either (`include/es/serreg/detail/embed_none.hpp`). Registration, the published index, captures and
`export_image()` work unchanged; hashes, descriptors and the wire format are those of the ELF build.

## Depends on / used by

Includes: `image.hpp` only.

Used by: `src/registry.cpp` through `image::*`; in the tree, the private static library `serreg_image_none` and
the executable `link_image_none` in `tests/link/CMakeLists.txt` (Debug builds only).

## Structure

- **`the_image`** (`image_none.cpp:11`): a `constinit const char` whose address is the one image id.
- **`current()`, `main_image()`** (`image_none.cpp:14-21`): both return `&the_image`.
- **`name()`** (`image_none.cpp:22-25`): `"<main program>"`.
- **`can_unload()`** (`image_none.cpp:26-29`): `false`.
- **`first_registry_marker()`** (`image_none.cpp:30-33`): `nullptr`.

The interface has no loader hooks (`files/src-image.hpp.md`), so there is nothing else to define.

## How it works

```cpp
constinit const char the_image = 0; // one id for every address
```

(`image_none.cpp:11`.) Every address maps to the same non-null image base, which is also the main image. What
follows in `src/registry.cpp`:

| registry behaviour | with the null backend |
|---|---|
| `registry_entry::image_base`, `images()` | one value, `&the_image`; `images().size() == 1` |
| `refs` | 1 per logical entry: TUs of one image share one COMDAT `registrar` node |
| `make_entry` | every registration is the main program's, so `payload` is `&descriptor_v<T>` / `&call_desc_v<Ts...>` and no deep copy is made; the ENTRY blob and the name are still copied |
| `on_guard_destroyed` | runs only at process exit; it drops the reference like any other guard destructor |
| `unload_guard_active(addr)` | always `false` |
| `warn_if_second_copy` | silent: `first_registry_marker()` is `nullptr` |
| `serreg_init` | reads the environment, blocks the dump signal and registers the exit hook as usual |

```cpp
const void* first_registry_marker() noexcept
{
    return nullptr; // one image, one copy
}
```

(`image_none.cpp:30-33`.) The Q10 check needs a global symbol lookup, which a format-neutral backend does not
have; a program built this way is one image with one copy of the library by construction.

The offline path of this backend is `tools::extract_embedded_dictionary`: with `ES_SERREG_EMBED_NONE` an ELF
executable has no `es_serreg_dict` section (`not_found`), and a non-ELF image is `unsupported`. An `.rri` from
`registry::export_image()` or `srdump --rri-out`, or the dictionary inside a capture, is the way to read such a
program's types without running it.

## Maintaining it

- **Keep it free of platform headers.** The point of this file is that it builds on any hosted C++26 toolchain;
  anything that needs `<dlfcn.h>` belongs in an ELF (or other format) backend.
- **The CMake rule is the safety net**: `none` with the shared library is a `FATAL_ERROR` in `CMakeLists.txt`.
  Do not relax it — with one image id for every address, the registry would treat a plugin's descriptors as the
  main program's and keep pointers into the plugin instead of deep copies, which dangle after its unload.
- **A program built this way must not `dlopen` images that serialise**: they would carry their own registry (no
  shared `libserreg.so`).
- **Tests:** `link_image_none_backend` (`tests/link/image_none_main.cpp`: 5 entries at `main()`, one non-null
  image, `refs == 1`, nothing unloadable, frames verify against `export_image()`, no `es_serreg_dict` section) and
  `link_image_none_program_hash_equal` (the program hash equals `link_plain`'s). Both exist in Debug builds only.
- **Allocation:** none. **Threading:** no state; every function is reentrant.
