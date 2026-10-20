// tests/dlopen/plugin_alias.cpp — review G2-10 (2.1.5): a plugin whose every registry entry already exists in the
// host (the host serializes host::Ping with the same signature before loading it). Loading it adds no entry, only
// aliases; the published version still moves, so on_change observers must be told (before 2.1.5 they were not).
#include <es/serreg/serreg.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace host
{
struct Ping
{
    std::uint64_t seq;
};
} // namespace host

extern "C" [[gnu::visibility("default")]] int plugin_run(std::vector<std::byte>* const out)
{
    es::serreg::vector_sink s{*out};
    return es::serreg::serialize(s, host::Ping{2}) ? 0 : 1;
}
extern "C" [[gnu::visibility("default")]] int plugin_expected_entries()
{
    return 0;
}
