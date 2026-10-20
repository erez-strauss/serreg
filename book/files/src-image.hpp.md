# `src/image.hpp`

The private interface between the registry and the executable-format backend (review addendum A4).
`src/registry.cpp` is portable C++: the heap entries, the published index, conflicts, export and the
environment hooks. Everything it needs to know about the *program image* — which loaded image an address belongs
to, which image is the main program, and whether an image can be unloaded — goes through the free functions
declared here. Exactly one backend defines them, chosen when `libserreg` is built (CMake `ES_SERREG_IMAGE_FORMAT`,
`files/build-system.md`): `src/image_elf.cpp` (default) or `src/image_none.cpp`. The header is not installed and
nothing in it is public API; it has no includes and no reflection dependency. Nothing here is used by
`serialize()`/`deserialize()`.

The interface has nothing that observes the loader: the registry makes every registration a heap entry when it is
pushed and treats an unload and process exit the same way (decision A), so it never needs to tell them apart (an
earlier design had `init()`, `unload_is_observable()` and `loader_events` hooks called from `dlopen()`/`dlclose()`
interposers).

## Depends on / used by

Includes: nothing (`#pragma once` and declarations only).

Included by: `src/registry.cpp` (calls `image::*`), `src/image_elf.cpp` and `src/image_none.cpp` (each defines
`image::*`).

## Structure

- **Header comment** (`image.hpp:1-13`): the split, the two backends, and the rule that CMake refuses `none` for
  the shared library.
- **`namespace es::serreg::image`** (`image.hpp:16-33`): five functions, all `noexcept`.
  - `current(addr)` (`image.hpp:20`): base address of the loaded image containing `addr`, or `nullptr`.
  - `main_image()` (`image.hpp:22`): base of the main program, computed once.
  - `name(base)` (`image.hpp:24`): path or name for diagnostics; `"<main program>"` when unknown.
  - `can_unload(addr)` (`image.hpp:27`): what `registry::unload_guard_active` returns.
  - `first_registry_marker()` (`image.hpp:31`): what the first `es_serreg_registry_marker_v3()` in the global
    scope returns — the address of the defining copy's private marker (review Q10; a function) — or
    the value of an older copy's `es_serreg_registry_marker_v2` object, or `nullptr` when the backend cannot look.

## How it works

### The image questions

```cpp
const void* current(const void* addr) noexcept;
// Base of the main program; computed once, never under the registry mutex (it may take a loader lock — G2-5).
const void* main_image() noexcept;
// Path or name of the image at `base`, for diagnostics; "<main program>" when unknown.
const char* name(const void* base) noexcept;
// registry::unload_guard_active: can the image containing `addr` really be unloaded (not the main program, not
// DF_1_NODELETE)? Always false for the null backend. Diagnostic only: the registry does not depend on it (2.1.13).
bool can_unload(const void* addr) noexcept;
```

(`image.hpp:20-27`.) The functions cover two things the registry needs from the executable format:

| operation | functions | registry use |
|---|---|---|
| **identity** | `current`, `main_image`, `name`, `first_registry_marker` | `push()` records `image_base = current(payload or blob)`; `main_image()` separates the main program's registrations (the entry points at the program's own descriptor) from all others (the entry holds a deep copy, G2-4); `name` fills the conflict diagnostic and the second-copy warning |
| **diagnostics** | `can_unload` | `registry::unload_guard_active()` only; nothing in the registry's behaviour depends on it |
| **embedding** | none at run time | the `es_serreg_dict` section is a header-side matter (`detail/embed_elf.hpp` / `embed_none.hpp`); its offline reader is `tools::extract_embedded_dictionary` (`src/elf_reader.cpp`) |

`first_registry_marker()` is used by `warn_if_second_copy()`, which compares it with the address of its own marker
and prints one line when another copy of libserreg defined the symbol first (chapter 9.1). The symbol is a
function, `es_serreg_registry_marker_v3()`, because two copies of the earlier data object
`es_serreg_registry_marker_v2` are an ODR violation under AddressSanitizer; the ELF backend still reads a `_v2`
object when no `_v3` function is found.

These are free functions, not a class with virtual members: one backend is linked into a given `libserreg`, the
calls are made at load/unload time, and there is nothing to choose at run time.

## Maintaining it

- **One backend per build.** A new executable format (PE/COFF, Mach-O) is a new `src/image_<fmt>.cpp` defining all
  five functions, a value in the `ES_SERREG_IMAGE_FORMAT` check in `CMakeLists.txt`, a header-side embedding
  (`include/es/serreg/detail/embed_<fmt>.hpp`, selected in `blob.hpp`) and a branch in
  `tools::extract_embedded_dictionary`. The registry does not change.
- **`main_image()` and `current()` may take a loader lock** (ELF: `dl_iterate_phdr`, `dladdr`). The registry calls
  them before it takes its mutex (G2-5); a backend must not assume it is called under the mutex, and the registry
  must keep calling them outside it.
- **No load/unload hooks**: a backend reports identity only. The registry learns of an unload from the
  guard destructors of the unloading image and needs nothing else; do not reintroduce interposers.
- **Allocation and threading:** the interface makes no promise of either; each backend page states its own. Every
  function is `noexcept`.
- **Tests:** everything in `tests/link` and `tests/dlopen` exercises the ELF backend; `link_image_none_backend` and
  `link_image_none_program_hash_equal` exercise the null backend (Debug builds only).
