// examples/dictionary_deltas/messages.hpp — the host's own types. The plugins define theirs in their own
// translation units (plugin_tick.cpp, plugin_quote.cpp): the host never sees them, and neither does the
// opening dictionary of the capture it writes (README.md).
#pragma once
#include <es/serreg/serreg.hpp>

#include <cstdint>

namespace feed
{
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Order
{
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
    Side side;
    char sym[8];
};
} // namespace feed

// What every plugin exports: writes `n` frames of its own types into the host's capture file, through the
// one registry and the one libserreg of the process (the plugins link the shared libserreg; the host too).
using plugin_write_fn = bool (*)(es::serreg::capture_file* file, std::uint32_t n);
