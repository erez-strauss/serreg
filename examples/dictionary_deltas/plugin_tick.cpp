// examples/dictionary_deltas/plugin_tick.cpp — the first plugin: one type (plugin_tick::Tick) and one call
// signature ("tick"_lit, Tick) that the host does not know. Instantiating serialize() here registers both in the
// process registry when the host dlopen()s this library, which is after the capture's opening dictionary was
// written — so the next frame is preceded by a dictionary delta frame (README.md).
#include "messages.hpp"

#include <es/serreg/serreg.hpp>

#include <cstdint>

namespace plugin_tick
{
struct Tick
{
    std::uint32_t id;
    double px;
};
} // namespace plugin_tick

extern "C" [[gnu::visibility("default")]] bool plugin_write(es::serreg::capture_file* const file,
                                                            const std::uint32_t n)
{
    using namespace es::serreg::literals;
    for (std::uint32_t i = 0; i < n; ++i)
        if (!es::serreg::serialize(*file, "tick"_lit, plugin_tick::Tick{i, 100.25 + i}))
            return false;
    return true;
}
