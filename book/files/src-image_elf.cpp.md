# `src/image_elf.cpp`

The default image backend of the registry (`src/image.hpp`; review addendum A4) for ELF systems with glibc:
image identity through `dladdr` and `dl_iterate_phdr`, the `DF_1_NODELETE` probe behind
`registry::unload_guard_active`, and the Q10 marker lookup (spec §6.3b, FR-R12). It is compiled into `libserreg` when
`ES_SERREG_IMAGE_FORMAT=elf` (the default). It has no reflection dependency and no `es/serreg` include.

The file defines no `dlopen()`/`dlclose()` interposers (an earlier design did, to tell a real unload from process
exit and abort on the first, FR-R10): the registry makes every registration a heap entry at load and never aborts
(decision A), so `dlopen` and `dlclose` in a process that links libserreg are libc's.

## Depends on / used by

Includes: `image.hpp`, `<dlfcn.h>` (`dladdr`, `dlopen`, `dlclose`, `dlsym`, `dlinfo`, `RTLD_*`), `<elf.h>`
(`PT_LOAD`, `DT_FLAGS_1`, `DF_1_NODELETE`), `<link.h>` (`dl_iterate_phdr`, `link_map`, `ElfW`), `<atomic>`.

Used by: `src/registry.cpp` through `image::*` (`push`, `report_conflict`, `unload_guard_active`,
`warn_if_second_copy`).

## Structure

- **Anonymous namespace** (`image_elf.cpp:15-32`): the `constinit` atomic `g_main` (`image_elf.cpp:18`) and
  the `dl_iterate_phdr` callback `first_phdr()` (`image_elf.cpp:20-31`).
- **`current()`** (`image_elf.cpp:34-40`): `dladdr(addr).dli_fbase`.
- **`main_image()`** (`image_elf.cpp:42-51`): the main program's mapped base, computed once.
- **`name()`** (`image_elf.cpp:53-59`): `dladdr(base).dli_fname`, else `"<main program>"`.
- **`can_unload()`** (`image_elf.cpp:61-79`): the `DT_FLAGS_1` / `DF_1_NODELETE` probe.
- **`first_registry_marker()`** (`image_elf.cpp:81-92`): `dlsym(RTLD_DEFAULT, "es_serreg_registry_marker_v3")`
  and a call through it; fallback: read the `es_serreg_registry_marker_v2` object of an older copy.

## How it works

### Finding the main program

```cpp
int first_phdr(dl_phdr_info* const info, size_t, void* const out)
{
    // The main program's mapped base (== dladdr's dli_fbase) is the load bias plus the lowest PT_LOAD vaddr.
    ElfW(Addr) lowest = ~ElfW(Addr){0};
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_LOAD && info->dlpi_phdr[i].p_vaddr < lowest)
            lowest = info->dlpi_phdr[i].p_vaddr;
    if (lowest == ~ElfW(Addr){0})
        lowest = 0;
    *static_cast<const void**>(out) = reinterpret_cast<const void*>(info->dlpi_addr + lowest);
    return 1;
}
```

(`image_elf.cpp:20-31`.) `dl_iterate_phdr` visits the main program first; the callback returns 1 to stop after it.
`dlpi_addr` is the load bias — 0 for a `-no-pie` executable — so the base is bias + lowest `PT_LOAD` address, the
same value `dladdr` reports as `dli_fbase` for any address inside the executable (`link_no_pie_main_image`).
`main_image()` (`image_elf.cpp:42-51`) caches it in `g_main` with release/acquire; two threads racing on the first
call store the same value. `dl_iterate_phdr` takes a loader lock, which is why the registry calls `main_image()`
before it takes its mutex (review G2-5).

### The `DF_1_NODELETE` probe

```cpp
bool can_unload(const void* const addr) noexcept
{
    Dl_info di{};
    if (!addr || !dladdr(addr, &di) || !di.dli_fname)
        return false;
    if (di.dli_fbase == main_image() || !*di.dli_fname)
        return false;                                              // main program never unloads
    void* const h = dlopen(di.dli_fname, RTLD_NOLOAD | RTLD_LAZY); // a probe: loads nothing
```

(`image_elf.cpp:61-68`.) With a handle to the already-loaded image it walks the dynamic section through
`dlinfo(RTLD_DI_LINKMAP)` → `l_ld` looking for `DT_FLAGS_1 & DF_1_NODELETE` (set by the link editor for
`-z nodelete`, and by glibc for an image with `STB_GNU_UNIQUE` symbols), then drops the `RTLD_NOLOAD` reference
with `dlclose`. It reads the on-disk flag only: an image opened with `RTLD_NODELETE` still reports `true`, because
glibc keeps that run-time flag in a private `link_map` field (`tests/dlopen/host.cpp` documents the limitation).
`registry::unload_guard_active` returns this value (`dlopen_C_rtld_nodelete`, `dlopen_I_plugin_without_flags_unloads`,
and the checks in `dlopen_A_dlclose_keeps_entries` that the plugin is unloadable and the main program and `nullptr`
are not). It is a diagnostic only: an image that cannot unload simply keeps its references, and the
registry behaves the same either way.

### The second-copy marker

```cpp
using marker_fn = const void* (*)() noexcept;
if (void* const f = dlsym(RTLD_DEFAULT, "es_serreg_registry_marker_v3"))
    return reinterpret_cast<marker_fn>(f)();
const auto* const v2 =
    static_cast<const void* const*>(dlsym(RTLD_DEFAULT, "es_serreg_registry_marker_v2"));
return v2 ? *v2 : nullptr;
```

(`image_elf.cpp:86-91`.) `dlsym(RTLD_DEFAULT, …)` finds the first definition in the global scope, which belongs
to the first copy of libserreg; calling it returns the address of *that* copy's `own_marker` (`src/registry.cpp`).
`warn_if_second_copy()` compares the result with its own `&own_marker`. An earlier marker was an exported data
object whose value was that address; two copies of libserreg in one process then defined the same object, an ODR
violation that AddressSanitizer reports and aborts on (`registry_second_copy_diagnostic` aborted under ASan). An
exported function has no such check. When no `_v3` function exists, the lookup reads the `_v2` object of an older
copy; reading it defines nothing, so an older copy loaded first is still recognised.

## Maintaining it

- **Run the loader tests.** Any change here must pass all of `tests/dlopen` (`dlopen_A`..`dlopen_L`,
  `registry_second_copy_diagnostic`) and `tests/link` (`link_no_pie_main_image`, `link_lto`, `link_gc_sections`),
  plus `premain_registry_count`; the unit binary does not load or unload images.
- **No interposers**. Defining `dlopen`/`dlclose` here would shadow libc's for every image in the process;
  the registry does not need to observe the loader.
- **No registry state here.** This file answers questions; entries, references and the exit hook belong to
  `src/registry.cpp`.
- **The marker name is versioned.** `es_serreg_registry_marker_v3` is a function; do not turn it back into an
  object. Keep the `_v2` fallback as long as a copy with the data marker may share a process with a newer one.
- **Allocation:** this file allocates nothing itself; `dladdr`, `dl_iterate_phdr` and `dlopen(RTLD_NOLOAD)` are
  glibc calls that take the loader lock.
- **Threading:** `g_main` is written lazily with the same value by any racing first caller; every function is
  reentrant.
