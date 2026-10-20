// FR-D5 (value validation, read side): with the default policy (ES_SERREG_VALIDATE_READ=0) an enum without a
// fixed underlying type is copied as it is — no table is instantiated, no value is rewritten, no error is
// reported. test_limits.cpp (compiled with ES_SERREG_VALIDATE_READ=1) checks the "report" behaviour on the same
// bytes. Separate executable: the policy is per translation unit and serreg_tests has it on.
#include <es/serreg/serreg.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#if ES_SERREG_VALIDATE_READ != 0
#error "this test checks the default policy"
#endif

namespace validate_off
{
enum Plain
{
    plain_a,
    plain_b,
    plain_c
};
struct HasPlain
{
    std::uint32_t id;
    Plain p;
    std::uint32_t tail;
};
} // namespace validate_off

using namespace es::serreg;
using validate_off::HasPlain;
using validate_off::plain_c;

// The bytes of the enum leaf, read without loading a `Plain` (a value outside 0..3 is not a value of the type:
// loading it is the undefined behaviour the policy leaves to the caller; UBSan would report the load).
unsigned leaf_bits(const HasPlain& h)
{
    unsigned u = 0;
    std::memcpy(&u, &h.p, sizeof(h.p));
    return u;
}

TEST_CASE("FR-D5 ES_SERREG_VALIDATE_READ=0: out-of-range enum leaves read through unchanged",
          "[limits][enum][validate]")
{
    STATIC_REQUIRE(!detail::validate_read_enums);
    std::vector<std::byte> buf;
    {
        vector_sink s{buf};
        REQUIRE(serialize(s, HasPlain{1, plain_c, 9}, std::uint32_t{5}).has_value());
    }
    buf[16 + 4] = std::byte{7}; // outside 0..3, the range of the hypothetical 2-bit type
    HasPlain out{};
    std::uint32_t u = 0;
    REQUIRE(deserialize(buf, out, u).has_value());
    CHECK((out.id == 1 && leaf_bits(out) == 7 && out.tail == 9 && u == 5));
    const auto fv = inspect(buf);
    REQUIRE(fv.has_value());
    const record_view rec = *fv->records().begin();
    REQUIRE(view<HasPlain>(rec).has_value());
    CHECK(leaf_bits(**view<HasPlain>(rec)) == 7);
    unsigned delivered = 0;
    REQUIRE(
        dispatch<HasPlain>(buf, [&](const HasPlain& h) { delivered = leaf_bits(h); }).has_value());
    CHECK(delivered == 7);
}
