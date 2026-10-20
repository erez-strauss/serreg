// tests/dlopen/plugin_conf_b.cpp — a SECOND image carrying conf::Msg with the same schema hash but a different
// layout hash (FR-R4 conflict; cc F15/F16). Reflection cannot produce this from source (the schema string is a
// superset of the layout string), so this image hand-builds the node the way tests/unit/test_conflict.cpp does —
// but from a separate DSO, with its own ENTRY blob in its own `es_serreg_dict` section, so that
//   * dlopen(a) then dlopen(b) → one stderr "schema conflict" line naming both images, conflicts().size() == 1;
//   * `srdump --elf a.so --elf b.so` sees two bodies for one hash and exits 1.
// Nothing here instantiates descriptor_v<conf::Msg>, so the genuine blob is NOT in this image's section.
#include "conf_msg.hpp"

#include <es/serreg/serreg.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
using namespace es::serreg;

consteval std::vector<std::byte> impostor_bytes()
{
    std::vector<std::byte> v = detail::type_blob_bytes<conf::Msg>(); // genuine ENTRY bytes ...
    const auto lh = std::bit_cast<std::array<std::byte, 8>>(layout_hash_v<conf::Msg> ^ 0x1ull);
    for (std::size_t i = 0; i < 8; ++i)
        v[8 + 8 + i] = lh[i]; // ... with the layout_hash column patched
    return v;
}
struct impostor_p
{
    static consteval std::vector<std::byte> bytes()
    {
        return impostor_bytes();
    }
};
[[gnu::section("es_serreg_dict"), gnu::used,
  gnu::retain]] alignas(8) constexpr auto impostor_blob = detail::to_blob_array<impostor_p>();

consteval type_desc make_impostor()
{
    type_desc d = detail::make_type_desc_base<conf::Msg>();
    d.layout_hash ^= 0x1ull;
    d.blob = impostor_blob;
    return d;
}
constexpr type_desc impostor_desc = make_impostor();
constinit reg_node impostor_node{&impostor_desc,
                                 impostor_desc.blob,
                                 impostor_desc.schema_hash,
                                 impostor_desc.name,
                                 node_kind::type,
                                 nullptr,
                                 nullptr,
                                 0,
                                 false};
const detail::guard impostor_guard{
    &impostor_node}; // pushed at dlopen(): what a real second definition's guard does
} // namespace

extern "C" [[gnu::visibility("default")]] std::uint64_t conf_b_schema_hash()
{
    return impostor_desc.schema_hash;
}
