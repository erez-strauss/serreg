// ce_registry_stub.hpp — a minimal stand-in for libserreg's registry, so the header-only frame layer (serialize,
// deserialize, inspect, dispatch, print) builds as ONE translation unit on Compiler Explorer. Include it once,
// after <es/serreg/...>, in main.cpp. NOT the project's registry (src/registry.cpp): no dedup, no conflicts, no
// published index, no dlopen handling, no dictionary; registrations are kept in a fixed array and looked up by
// linear scan. Enough for: registrar guards (push), find_type / find_call / find_literal (inspect, dispatch),
// null side-dictionary lookups, and the decoders' warning hook. Shared by ce-example3 to ce-example7 (symlinks to ../ce_registry_stub.hpp); ce-example7 (the logger) uses dictionary_policy::full because export_image_scope is not here.
#pragma once
#include <es/serreg/dictionary.hpp>
#include <es/serreg/registry.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace es::serreg
{
struct registry_state
{
    static constinit registry inst;
};
constinit registry registry_state::inst{};

// 2.2.6 (B3): the link-time fuse of registry.hpp needs the one definition src/registry.cpp would provide.
namespace detail
{
template<int V> void validate_read_build() noexcept
{
}
template void validate_read_build<ES_SERREG_VALIDATE_READ>() noexcept;
} // namespace detail

namespace ce_stub
{
// The stub's own list (since 2.1.13 reg_node has no `next`: the real registry keeps heap entries).
inline constexpr std::size_t capacity = 4096;
constinit reg_node* nodes[capacity] = {};
constinit std::size_t count = 0;
inline const reg_node* find(const node_kind nk, const std::uint64_t h) noexcept
{
    for (std::size_t i = 0; i < count; ++i)
        if (nodes[i]->nk == nk && nodes[i]->hash == h)
            return nodes[i];
    return nullptr;
}
} // namespace ce_stub

registry& registry::instance() noexcept
{
    return registry_state::inst;
}
void registry::push(reg_node* const n) noexcept
{
    if (ce_stub::find(n->nk, n->hash))
        return; // already present (alias)
    if (n->nk == node_kind::literal && n->blob.size() >= fmt::literal_text_offset)
    { // the literal text lives inside its ENTRY blob, as in src/registry.cpp
        std::uint32_t len = 0;
        std::memcpy(&len, n->blob.data() + fmt::literal_len_offset, sizeof len);
        n->name = {reinterpret_cast<const char*>(n->blob.data()) + fmt::literal_text_offset, len};
    }
    n->linked = true;
    if (ce_stub::count < ce_stub::capacity)
        ce_stub::nodes[ce_stub::count++] = n;
}
void registry::on_guard_destroyed(reg_node*) noexcept
{
}
const type_desc* registry::find_type(const std::uint64_t h) const noexcept
{
    const reg_node* const p = ce_stub::find(node_kind::type, h);
    return p ? static_cast<const type_desc*>(p->payload) : nullptr;
}
const call_desc* registry::find_call(const std::uint64_t h) const noexcept
{
    const reg_node* const p = ce_stub::find(node_kind::call, h);
    return p ? static_cast<const call_desc*>(p->payload) : nullptr;
}
std::string_view registry::find_literal(const std::uint64_t id) const noexcept
{
    const reg_node* const p = ce_stub::find(node_kind::literal, id);
    return p ? p->name : std::string_view{};
}
// 2.2.5: the two exports capture.hpp needs, so capture_file / write_dictionary_frame / dictionary_tracker work in
// a Compiler Explorer example (ce-example6). Same bytes as src/registry.cpp: an RRI header followed by every
// ENTRY blob in registration order; the stub's version never moves, so `since` > 0 yields an empty delta.
std::uint64_t registry::program_hash() const
{
    std::vector<std::uint64_t> hs;
    for (std::size_t i = 0; i < ce_stub::count; ++i)
        if (ce_stub::nodes[i]->nk == node_kind::type)
            hs.push_back(ce_stub::nodes[i]->hash);
    std::sort(hs.begin(), hs.end());
    return fingerprint64_u64s(hs);
}
std::vector<std::byte> registry::export_image_since(const std::uint64_t since) const
{
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < ce_stub::count; ++i)
        if (ce_stub::nodes[i]->nk != node_kind::scope && since == 0)
            ++n;
    std::vector<std::byte> out(fmt::rri_header_size);
    fmt::writer w{out.data()};
    w.put_bytes(fmt::rri_magic.data(), 4);
    w.put(fmt::format_major);
    w.put(fmt::format_minor);
    w.put(since ? fmt::rri_flag_delta : std::uint32_t{0});
    w.put(fmt::hash_algo_xxh3_64);
    w.put(n);
    w.put(program_hash());
    w.put(std::uint32_t{0});
    w.put(fmt::host_abi_tag);
    for (std::size_t i = 0; i < ce_stub::count && since == 0; ++i)
        if (ce_stub::nodes[i]->nk != node_kind::scope)
            out.insert(out.end(), ce_stub::nodes[i]->blob.begin(), ce_stub::nodes[i]->blob.end());
    return out;
}
// inspect()/dispatch() consult an optional side dictionary (src/dictionary.cpp); these examples never pass one.
const dyn::type* dyn::dictionary::find_type(std::uint64_t) const noexcept
{
    return nullptr;
}
const dyn::call* dyn::dictionary::find_call(std::uint64_t) const noexcept
{
    return nullptr;
}
namespace detail
{
void report_warning(warning, std::uint64_t, std::uint32_t) noexcept
{
}
} // namespace detail
} // namespace es::serreg
