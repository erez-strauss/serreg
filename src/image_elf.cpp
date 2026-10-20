// src/image_elf.cpp — the ELF/glibc image backend (src/image.hpp; review addendum A4; 2.1.8): image identity
// through dladdr / dl_iterate_phdr and the DF_1_NODELETE probe (FR-R12). Until 2.1.12 it also interposed dlopen()
// and dlclose() to tell an unload from process exit (the FR-R10 abort mode); 2.1.13 copies every registration to
// the heap at load and never aborts, so libserreg no longer defines either symbol.
#include "image.hpp"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <atomic>

namespace es::serreg::image
{
namespace
{
// Computed on first use, outside the registry mutex; racing first users store the same value.
constinit std::atomic<const void*> g_main{nullptr};

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
} // namespace

const void* current(const void* const addr) noexcept
{
    Dl_info di{};
    if (addr && dladdr(addr, &di))
        return di.dli_fbase;
    return nullptr;
}

const void* main_image() noexcept
{
    const void* m = g_main.load(std::memory_order_acquire);
    if (!m)
    {
        dl_iterate_phdr(&first_phdr, &m);
        g_main.store(m, std::memory_order_release);
    }
    return m;
}

const char* name(const void* const base) noexcept
{
    Dl_info di{};
    if (base && dladdr(base, &di) && di.dli_fname && *di.dli_fname)
        return di.dli_fname;
    return "<main program>";
}

bool can_unload(const void* const addr) noexcept
{
    Dl_info di{};
    if (!addr || !dladdr(addr, &di) || !di.dli_fname)
        return false;
    if (di.dli_fbase == main_image() || !*di.dli_fname)
        return false;                                              // main program never unloads
    void* const h = dlopen(di.dli_fname, RTLD_NOLOAD | RTLD_LAZY); // a probe: loads nothing
    if (!h)
        return false;
    bool nodelete = false;
    link_map* lm = nullptr;
    if (dlinfo(h, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_ld)
        for (const ElfW(Dyn)* d = lm->l_ld; d->d_tag != DT_NULL; ++d)
            if (d->d_tag == DT_FLAGS_1 && (d->d_un.d_val & DF_1_NODELETE))
                nodelete = true;
    dlclose(h); // undo the NOLOAD reference only
    return !nodelete;
}

const void* first_registry_marker() noexcept
{
    // 2.1.9: the marker is a function (no exported object: two copies of an object are an ODR violation under
    // ASan). A copy of 2.1.5–2.1.8 exports the data symbol _v2 instead; reading it defines nothing, so an older
    // copy is still recognised.
    using marker_fn = const void* (*)() noexcept;
    if (void* const f = dlsym(RTLD_DEFAULT, "es_serreg_registry_marker_v3"))
        return reinterpret_cast<marker_fn>(f)();
    const auto* const v2 =
        static_cast<const void* const*>(dlsym(RTLD_DEFAULT, "es_serreg_registry_marker_v2"));
    return v2 ? *v2 : nullptr;
}

} // namespace es::serreg::image
