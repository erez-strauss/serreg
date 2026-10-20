// tests/dlopen/plugin_scoped.cpp — 2.2.1 (P-16, A-9): a plugin that serialises into a *scoped* sink. Its
// (scope, call) node is a registry entry of a dlopen'd image: the registry must deep-copy it (the scope name must
// outlive the plugin), export_image_scope must list the plugin's call with its types and literal, and a scoped
// delta since the version before the dlopen must carry the host's old types the plugin's call brought into the
// scope (the "entered the scope at" rule of export_image_scope). The scope tag `host::feed_scope` and the member
// type `host::Ping` are the host's: the plugin only adds the call and its literal.
#include <es/serreg/serreg.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace host
{
struct Ping
{ // the host's definition, member for member (the same type: same name, same layout)
    std::uint64_t seq;
};
struct feed_scope
{
};
} // namespace host

namespace plugin_scoped
{
struct Quote
{
    std::uint64_t ts;
    double bid, ask;
    host::Ping origin; // a type the host registered long before this plugin loaded
};
struct feed_sink
{ // a scoped sink: the dictionary of everything serialised into it is host::feed_scope's
    using dictionary_scope = host::feed_scope;
    std::vector<std::byte>& out;
    bool write(const std::byte* const p, const std::size_t n)
    {
        out.insert(out.end(), p, p + n);
        return true;
    }
};
} // namespace plugin_scoped

extern "C" [[gnu::visibility("default")]] int plugin_run(std::vector<std::byte>* const out)
{
    using namespace es::serreg;
    using namespace es::serreg::literals;
    plugin_scoped::feed_sink s{*out};
    const auto r =
        serialize(s, "quote"_lit, plugin_scoped::Quote{1700000000ull, 100.5, 100.75, {7}});
    return r ? 0 : 1;
}

extern "C" [[gnu::visibility("default")]] std::uint64_t plugin_scope_hash()
{
    return es::serreg::detail::scope_hash_v<host::feed_scope>;
}
