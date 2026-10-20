// src/image.hpp — the executable-format backend of the registry (review addendum A4, study §4 item 7; 2.1.8).
//
// The registry (registry.cpp) is portable C++: the node list, the published index, conflicts, export. What it
// needs to know about the program image — which loaded image an address belongs to, which one is the main
// program, whether an image can be unloaded, and when an unload happens — comes from exactly one backend,
// chosen when libserreg is built (CMake ES_SERREG_IMAGE_FORMAT):
//
//   image_elf.cpp   (default) dladdr, dl_iterate_phdr, DT_FLAGS_1 (no interposer since 2.1.13)
//   image_none.cpp  one image, nothing unloads; only with the static library (CMake refuses `none` for
//                   libserreg.so: a shared library serves images that can be unloaded)
//
// Free functions, no vtable: registration is load-time, not the frame hot path, but it should not virtual-call.
// Nothing here is used by serialize()/deserialize().
#pragma once

namespace es::serreg::image
{

// Base address of the loaded image that contains `addr`; nullptr when unknown (registration continues).
const void* current(const void* addr) noexcept;
// Base of the main program; computed once, never under the registry mutex (it may take a loader lock — G2-5).
const void* main_image() noexcept;
// Path or name of the image at `base`, for diagnostics; "<main program>" when unknown.
const char* name(const void* base) noexcept;
// registry::unload_guard_active: can the image containing `addr` really be unloaded (not the main program, not
// DF_1_NODELETE)? Always false for the null backend. Diagnostic only: the registry does not depend on it (2.1.13).
bool can_unload(const void* addr) noexcept;
// Q10 (2.1.5; a function since 2.1.9): what the first `es_serreg_registry_marker_v3()` visible in the process's
// global scope returns — the address of the private marker of the libserreg copy that defined it — (or the value of
// an older copy's `es_serreg_registry_marker_v2` object), or nullptr when the backend cannot look.
const void* first_registry_marker() noexcept;

} // namespace es::serreg::image
