// tests/support/test_support.hpp — helpers shared by the test executables (not part of the library).
//
// History: v1.0.1 needed a KNOWN-BUG workaround here (sink_noexcept<S> named claim()/commit() for plain sinks and
// failed to compile). v1.0.2 fixed the library (claim_noexcept partial specialisation); 2.0.1 removed the leftover
// explicit specialisation for fd_sink, so the tests now exercise the library's own derivation for every sink.
#pragma once
#include <es/serreg/deserialize.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/serialize.hpp>
#include <es/serreg/sink.hpp>
#include <es/serreg/tools.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

static_assert(es::serreg::detail::sink_noexcept<es::serreg::fd_sink>,
              "fd_sink::write is noexcept, so serialize() into it must be noexcept");

// v1.0.2: sink_noexcept<S> no longer names claim()/commit() for plain sinks, so plain sinks work unaided;
// the macro is kept as a static check that the library derives the same answer the test expects.
#define ES_SERREG_TEST_PLAIN_SINK(S, N)                                                            \
    static_assert(es::serreg::detail::sink_noexcept<S> == (N), "sink_noexcept<" #S "> mismatch")

namespace es::serreg::test
{

// Materialise the lazy record range of a frame_view (the tests index records by position).
inline std::vector<record_view> all_records(const frame_view& fv)
{
    std::vector<record_view> out;
    out.reserve(fv.size());
    for (const record_view r : fv.records())
        out.push_back(r);
    return out;
}

// 2.1.1 (G2-1): a copy of `bytes` that ends exactly where a PROT_NONE page begins, so reading one byte past the end
// faults — an over-read test that needs no sanitizer runtime. One region per process, reused; `bytes` ≤ 64 KiB.
inline std::span<const std::byte> guarded_copy(const std::span<const std::byte> bytes)
{
    constexpr std::size_t cap = std::size_t{64} << 10;
    static std::byte* const base = [] {
        const auto pg = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        void* const m =
            ::mmap(nullptr, cap + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
            return static_cast<std::byte*>(nullptr);
        ::mprotect(static_cast<std::byte*>(m) + cap, pg, PROT_NONE);
        return static_cast<std::byte*>(m);
    }();
    if (!base || bytes.size() > cap)
        return {};
    std::byte* const at = base + (cap - bytes.size());
    if (!bytes.empty())
        std::memcpy(at, bytes.data(), bytes.size());
    return {at, bytes.size()};
}

// The dictionary embedded in this test binary (section es_serreg_dict, FR-D6). A build with ES_SERREG_EMBED_NONE
// (CMake ES_SERREG_IMAGE_FORMAT=none, preset gcc16-static-none, 2.1.11) has no such section: then the reader must
// report not_found, and the registry's exported image stands in for the section so the rest of the test runs.
inline result<dyn::dictionary> embedded_dictionary_of_self()
{
    auto elf = tools::extract_registry_from_elf("/proc/self/exe");
#if defined(ES_SERREG_EMBED_NONE)
    if (elf || elf.error() != error::not_found)
        return std::unexpected(
            error::unsupported); // a section in a no-embed build: fail the caller's REQUIRE
    return tools::load_dictionary_rri(registry::instance().export_image());
#else
    return elf;
#endif
}

} // namespace es::serreg::test
