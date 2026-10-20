// tests/probe/depth4.cpp — compile-time probe (cc F9 / FR-H4 memoisation): a depth-4 nesting with 4 members per
// level (4 + 16 + 64 + 256 = 340 leaf scalars, 85 distinct struct types along the way is NOT the point — every
// level reuses the same 4 types, so the memoised hashes are hit 4× per level). The ctest compiles this TU with a
// 60 s wall-clock ceiling; before the memoisation the nested canonical strings were recomputed at every use and
// depth 5 hit the constexpr-operation limit.
#include <es/serreg/serreg.hpp>

#include <vector>

namespace probe
{
struct L0a
{
    std::int32_t a;
    std::int32_t b;
    std::int64_t c;
    std::uint8_t d;
};
struct L0b
{
    double x;
    float y;
    std::uint16_t z;
    bool w;
};
struct L0c
{
    std::uint64_t id;
    char sym[8];
    std::int32_t n;
    std::uint32_t m;
};
struct L0d
{
    std::int8_t s[4];
    std::uint32_t u;
    std::int16_t v;
    std::int64_t k;
};
template<class A, class B, class C, class D> struct Level
{
    A a;
    B b;
    C c;
    D d;
};
using L1a = Level<L0a, L0b, L0c, L0d>;
using L1b = Level<L0b, L0c, L0d, L0a>;
using L1c = Level<L0c, L0d, L0a, L0b>;
using L1d = Level<L0d, L0a, L0b, L0c>;
using L2a = Level<L1a, L1b, L1c, L1d>;
using L2b = Level<L1b, L1c, L1d, L1a>;
using L2c = Level<L1c, L1d, L1a, L1b>;
using L2d = Level<L1d, L1a, L1b, L1c>;
using L3a = Level<L2a, L2b, L2c, L2d>;
using L3b = Level<L2b, L2c, L2d, L2a>;
using L3c = Level<L2c, L2d, L2a, L2b>;
using L3d = Level<L2d, L2a, L2b, L2c>;
using L4 = Level<L3a, L3b, L3c, L3d>; // depth 4
} // namespace probe

static_assert(es::serreg::descriptor_v<probe::L4>.fields.size() == 4);
static_assert(es::serreg::descriptor_v<probe::L4>.size == sizeof(probe::L4));
static_assert(es::serreg::schema_hash_v<probe::L4> != es::serreg::layout_hash_v<probe::L4>);

namespace probe
{
// Round-trips one default-constructed L4 through a vector_sink.
class app
{
public:
    int run() const
    {
        std::vector<std::byte> v;
        es::serreg::vector_sink s{v};
        const L4 l4{};
        if (!es::serreg::serialize(s, l4))
            return 1;
        L4 back{};
        return es::serreg::deserialize(v, back) ? 0 : 1;
    }
};
} // namespace probe

int main()
{
    return probe::app{}.run();
}
