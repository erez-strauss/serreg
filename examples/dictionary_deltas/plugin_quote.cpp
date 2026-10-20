// examples/dictionary_deltas/plugin_quote.cpp — the second plugin: another type (plugin_quote::Quote) and call
// signature ("quote"_lit, Quote), loaded after the first plugin's frames, so the capture gets a second delta
// frame — linked from the first one, not from the opening block (README.md).
#include "messages.hpp"

#include <es/serreg/serreg.hpp>

#include <cstdint>

namespace plugin_quote
{
struct Quote
{
    std::uint32_t id;
    std::int32_t bid;
    std::int32_t ask;
};
} // namespace plugin_quote

extern "C" [[gnu::visibility("default")]] bool plugin_write(es::serreg::capture_file* const file,
                                                            const std::uint32_t n)
{
    using namespace es::serreg::literals;
    for (std::uint32_t i = 0; i < n; ++i)
        if (!es::serreg::serialize(*file, "quote"_lit,
                                   plugin_quote::Quote{i, 1000 + static_cast<std::int32_t>(i),
                                                       1001 + static_cast<std::int32_t>(i)}))
            return false;
    return true;
}
