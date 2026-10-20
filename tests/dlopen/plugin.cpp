// tests/dlopen/plugin.cpp — a plugin image whose types are unknown to the host (spec §6.3, FR-R6/R7/R12).
// Loading it must add exactly its entities to the process registry; nothing here is referenced by the host.
//   types:    <NS>::Tick, <NS>::Flag (component)   = 2
//   literals: "tick"                               = 1
//   calls:    ("tick", Tick)                       = 1        → 4 new entries at dlopen()
// The namespace is a macro so the same source builds several distinct plugin images (serreg_plugin_b/_c for the
// concurrent-dlclose and churn tests, serreg_plugin_nounique for the attribute-only unload-guard test). Every image
// registers the literal "tick": the second image is an alias (refs++), so it adds 3 entries, not 4.
// 2.1.13: ES_PLUGIN_V2 builds the same plugin with one more Tick member (serreg_plugin_v2): the "same plugin, rebuilt
// with a changed layout" of the reload test. Tick gets a new schema hash, and so does the call; Flag and "tick" stay.
#include <es/serreg/serreg.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef ES_PLUGIN_NS
#define ES_PLUGIN_NS plugin
#endif

namespace ES_PLUGIN_NS
{
enum class Flag : std::uint8_t
{
    none = 0,
    halted = 1
};
struct Tick
{
    std::uint64_t ts;
    double px;
    std::uint32_t size;
    Flag flag;
#ifdef ES_PLUGIN_V2
    std::uint32_t venue;
#endif
};
} // namespace ES_PLUGIN_NS

extern "C" [[gnu::visibility("default")]] int plugin_run(std::vector<std::byte>* const out)
{
    using namespace es::serreg;
    using namespace es::serreg::literals;
    vector_sink s{*out};
    const auto r =
        serialize(s, "tick"_lit,
                  ES_PLUGIN_NS::Tick{1700000000ull, 101.25, 300, ES_PLUGIN_NS::Flag::halted
#ifdef ES_PLUGIN_V2
                                     ,
                                     7
#endif
                  });
    return r ? 0 : 1;
}

extern "C" [[gnu::visibility("default")]] int plugin_expected_entries()
{
    return 4;
}
