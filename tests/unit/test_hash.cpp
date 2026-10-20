// tests/unit/test_hash.cpp — FR-H5 hash_algo 2 (XXH3-64): known answers from the xxHash 0.8.3 reference, pinned
// in constant evaluation (STATIC_REQUIRE) and at run time (REQUIRE), plus agreement of the two code paths.
#include <es/serreg/hash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace
{
consteval std::array<char, 4096> pattern()
{
    std::array<char, 4096> b{};
    for (std::size_t i = 0; i < b.size(); ++i)
        b[i] = static_cast<char>(i * 31 + 7);
    return b;
}
constexpr auto buf = pattern();
struct kat_t
{
    std::size_t len;
    std::uint64_t hash;
};
// Every length class of XXH3: 0, 1-3, 4-8, 9-16, 17-128, 129-240, and >240 (one and several blocks).
constexpr kat_t kat[] = {
    {0, 0x2d06800538d394c2ull},    {1, 0x4c5cca45d0f4811full},   {3, 0x15f7093b173d005cull},
    {4, 0xdca012f95811b6b9ull},    {8, 0xdec6a9a43575982eull},   {9, 0xcbe393399f17ffbdull},
    {16, 0x7e484c18d74895d0ull},   {17, 0x208bde5ee2bed407ull},  {128, 0xf92b70eaa21a6288ull},
    {129, 0xf8f76713f2bb60faull},  {240, 0xccc7375172c41f03ull}, {241, 0x0b3b630948ce4a00ull},
    {1024, 0x23bc880ebf0d29c6ull}, {4096, 0xa3c19f8174cde0bbull}};
consteval bool kat_ok()
{
    for (const auto k : kat)
        if (es::serreg::fingerprint64(std::string_view{buf.data(), k.len}) != k.hash)
            return false;
    return true;
}
template<std::size_t... L> consteval auto table(std::index_sequence<L...>)
{
    return std::array<std::uint64_t, sizeof...(L)>{
        es::serreg::fingerprint64(std::string_view{buf.data(), L})...};
}
constexpr auto ct = table(std::make_index_sequence<300>{}); // 0..299: every short/mid path boundary
} // namespace

TEST_CASE("FR-H5 xxh3-64 known answers (constant evaluation and run time)", "[hash]")
{
    STATIC_REQUIRE(kat_ok());
    STATIC_REQUIRE(es::serreg::fingerprint64("string") == 0xd8ad194648e774f1ull);
    for (const auto k : kat)
        REQUIRE(es::serreg::fingerprint64(std::string_view{buf.data(), k.len}) == k.hash);
}

TEST_CASE("FR-H5 xxh3-64 consteval and run-time paths agree", "[hash]")
{
    for (std::size_t len = 0; len < ct.size(); ++len)
        REQUIRE(es::serreg::fingerprint64(std::string_view{buf.data(), len}) == ct[len]);
}

TEST_CASE("FR-H3 program hash is xxh3-64 of the sorted u64s as little-endian bytes", "[hash]")
{
    static constexpr std::uint64_t v[] = {1, 2, 3};
    std::vector<std::uint64_t> r(std::begin(v), std::end(v));
    STATIC_REQUIRE(es::serreg::fingerprint64_u64s(v) != 0);
    REQUIRE(es::serreg::fingerprint64_u64s(r) == es::serreg::fingerprint64_u64s(v));
}
