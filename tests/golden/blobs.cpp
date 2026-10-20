// tests/golden/blobs.cpp — format lock of the compile-time ENTRY blobs (cc F1 / plan §1.6 (a)): the XXH3-64 of
// the exact bytes of detail::type_blob_bytes_v<T> and its size are frozen for every corpus type. A blob is the
// unit that goes into the ELF section, the RRI and every capture dictionary, so any change to the ENTRY/FIELD
// encoding (format.hpp) — reordered columns, a widened field, a different padding rule — fails here at compile
// time. Refreezing is a wire-format change: format/major bump + CHANGELOG note. [golden-print] prints the values.
#include "corpus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <print>
#include <span>
#include <vector>

using namespace es::serreg;

namespace blobs16
{
enum Plain
{
    one,
    two
};
} // namespace blobs16

namespace
{
template<class T> constexpr std::span<const std::byte> blob_of()
{
    return std::span<const std::byte>(detail::type_blob_bytes_v<T>);
}
template<class T> constexpr std::uint64_t blob_hash()
{
    return fingerprint64(blob_of<T>());
}
template<class T> constexpr std::size_t blob_size()
{
    return blob_of<T>().size();
}
} // namespace

#define ES_BLOB(T, hash, size)                                                                     \
    STATIC_REQUIRE(blob_hash<T>() == hash##ull);                                                   \
    STATIC_REQUIRE(blob_size<T>() == size)

TEST_CASE("cc F1 golden ENTRY blob hashes and sizes", "[golden][blob]")
{
    // refrozen for format 1.3 / XXH3-64 (2.1.0) from GCC 16.2 -std=c++26 -freflection, x86-64
    ES_BLOB(corpus::Scalars, 0x5b676741452a6b58, 192);
    ES_BLOB(corpus::Padded, 0x9c624beb5c8b5b77, 168);
    ES_BLOB(corpus::Nested, 0x985a016d2d832d77, 168);
    ES_BLOB(corpus::Base, 0x5bcc049be962ad3b, 88);
    ES_BLOB(corpus::Tag2, 0x5902239f89a2fcab, 88);
    ES_BLOB(corpus::Derived, 0x09a1b21fbca7201a, 144);
    ES_BLOB(corpus::MultiDerived, 0x3be7a1faa058abf1, 200);
    // enum blobs refrozen for format 1.6 (2.2.0): the reserved byte after the underlying kind is now the flags
    // byte (enum_flag_fixed); sizes unchanged. Struct and literal blobs did not move.
    ES_BLOB(corpus::Side, 0x48f8e7e66398d256, 88);
    ES_BLOB(corpus::Venue, 0x32332c620c674179, 104);
    ES_BLOB(corpus::Arrays, 0x0f1c70994e73fec3, 360);
    ES_BLOB(corpus::Empty, 0x259ee063f75f1c87, 56);
    ES_BLOB(corpus::WithEmpty, 0xaf733e0ec3c4adeb, 128);
    ES_BLOB(corpus::Account, 0xeba4c8cf16deeded, 136);
    ES_BLOB(corpus::Versioned, 0x4465c1a3300fe9db, 168);
    ES_BLOB(corpus::Unversioned, 0x69654e8ecbd3754f, 176);
    ES_BLOB(corpus::Skipped, 0xef6d85f79f571f2b, 128);
    ES_BLOB(corpus::Vec3, 0x16f5c0f06dc1311d, 136);
    ES_BLOB(corpus::Vec3i, 0xcbabeeeeb41e6ee4, 128);
    ES_BLOB(corpus::Stamp, 0x1b2e8756e2dcc4be, 96);
    ES_BLOB(corpus::StampFlat, 0x1b2e8756e2dcc4be, 96);
    ES_BLOB(corpus::Order, 0x8bb3bcf5a04fdc0c, 264);
    ES_BLOB(corpus::Cancel, 0x160d8de017e45e0f, 128);
    ES_BLOB(corpus::Point2i, 0xc389055b4a970534, 128);
    ES_BLOB(corpus::Pair2i, 0x65ceb4da864f1002, 128);
    ES_BLOB(Local, 0x6c69ffbf01e9159d, 128); // 2.1.14: named "corpus::Local"
    ES_BLOB(t::WBool, 0x1ec28def688265c9, 96);
    ES_BLOB(t::WChar, 0xf98414d124de6e5b, 88);
    ES_BLOB(t::WEnum, 0xcf5bf442e357e181, 96);
    ES_BLOB(t::WNeg, 0x836652e4c440290e, 88);
    ES_BLOB(t::Mode, 0xaf46dfef4612d7da, 64); // 1.6 refreeze (flags byte)
    // 2.0 dynamic types: FIELD entries with kinds string_/vector_/optional_/variant_ and nested element FIELDs
    ES_BLOB(corpus::DynMsg, 0x49d43c2ec5419e62, 192);
    ES_BLOB(corpus::DynNested, 0x584f85e6e0c71876, 328);
    ES_BLOB(corpus::DynArr, 0xc3060b35d803ae72, 368);
    ES_BLOB(corpus::DynBase, 0x26ba4a1abc9df924, 152);
    STATIC_REQUIRE((blob_of<corpus::DynMsg>()[32] & std::byte{fmt::struct_flag_flat}) ==
                   std::byte{0}); // flags byte (after hashes, size, align): not flat
    STATIC_REQUIRE((blob_of<corpus::Order>()[32] & std::byte{fmt::struct_flag_flat}) !=
                   std::byte{0});
    // structural invariants of every blob (format.hpp ENTRY header): total_len multiple of 8, tag, version 1
    STATIC_REQUIRE(blob_size<corpus::Order>() % 8 == 0);
    STATIC_REQUIRE(blob_of<corpus::Order>()[4] == std::byte{fmt::tag_struct} &&
                   blob_of<corpus::Order>()[5] == std::byte{1});
    STATIC_REQUIRE(blob_of<corpus::Side>()[4] == std::byte{fmt::tag_enum} &&
                   blob_of<corpus::Side>()[5] == std::byte{1});
    // 1.6: enum ENTRY byte 32 is the underlying kind, byte 33 the flags (fixed underlying type or not)
    STATIC_REQUIRE(blob_of<corpus::Side>()[32] == std::byte{static_cast<std::uint8_t>(kind::u8)} &&
                   blob_of<corpus::Side>()[33] == std::byte{fmt::enum_flag_fixed});
    STATIC_REQUIRE(blob_of<corpus::Venue>()[33] ==
                   std::byte{fmt::enum_flag_fixed}); // `enum Venue : int`
    STATIC_REQUIRE(
        blob_of<blobs16::Plain>()[32] == std::byte{static_cast<std::uint8_t>(kind::u32)} &&
        blob_of<blobs16::Plain>()[33] == std::byte{0}); // no fixed type: decision D kind, no flag
    // 1.6: a struct with a map and a set member — FIELD kinds map_/set_ with 2 and 1 element descriptors
    STATIC_REQUIRE(blob_of<corpus::DynMap>()[4] == std::byte{fmt::tag_struct} &&
                   (blob_of<corpus::DynMap>()[32] & std::byte{fmt::struct_flag_transparent}) ==
                       std::byte{0});
    STATIC_REQUIRE(
        (blob_of<std::vector<std::int32_t>>()[32] & std::byte{fmt::struct_flag_transparent}) !=
        std::byte{0}); // a container passed as an argument: the transparent wrapper type
    // the descriptor's blob span is exactly this array
    STATIC_REQUIRE(descriptor_v<corpus::Order>.blob.size() == blob_size<corpus::Order>());
    STATIC_REQUIRE(descriptor_v<corpus::Order>.blob.data() == blob_of<corpus::Order>().data());
    // literal and call blobs are frozen too (§6.6, FR-L3; call ENTRY tag 4)
    using namespace es::serreg::literals;
    STATIC_REQUIRE(fingerprint64(std::span<const std::byte>(
                       detail::literal_blob_bytes_v<decltype("evt"_lit)::str>)) ==
                   0x52155f6f3763912full);
    STATIC_REQUIRE(
        std::span<const std::byte>(detail::literal_blob_bytes_v<decltype("evt"_lit)::str>).size() ==
        32);
    STATIC_REQUIRE(fingerprint64(std::span<const std::byte>(
                       detail::call_blob_bytes_v<decltype("evt"_lit), corpus::Order>)) ==
                   0xf2a2b77e38e0193eull);
    STATIC_REQUIRE(
        std::span<const std::byte>(detail::call_blob_bytes_v<decltype("evt"_lit), corpus::Order>)
            .size() == 56);
    // format 1.1: a print call ENTRY — flags = print, ARG kinds literal/string/scalar with the scalar kind byte
    //   48 00 00 00 | 04 01 00 00 | <call hash> | 03 00 | 01 00 | 00 00 00 00 |
    //   01 00 ×6 <lit id> | 03 00 ×6 <fingerprint64("string")> | 02 03(i32) ×6 <fingerprint64("i32")>
    constexpr auto pb = std::span<const std::byte>(
        detail::call_blob_bytes_v<print_tag, lit<"x {}">, const char*, int>);
    STATIC_REQUIRE(fingerprint64(pb) == 0x39c7bd272ba4951bull);
    STATIC_REQUIRE(pb.size() == 72);
    STATIC_REQUIRE(pb[16] == std::byte{3} && pb[18] == std::byte{fmt::call_flag_print});
    STATIC_REQUIRE(pb[24] == std::byte{fmt::arg_literal} && pb[40] == std::byte{fmt::arg_string} &&
                   pb[56] == std::byte{fmt::arg_scalar} &&
                   pb[57] == static_cast<std::byte>(kind::i32));
    // format 1.5: a constant's ARG is followed by its value — ("evt", 42_, Order, k<Side::sell>):
    //   60 00 00 00 | 04 01 00 00 | <call hash> | 04 00 | 00 00 | 00 00 00 00 |
    //   01 00 ×6 <lit id> | 04 03(i32) ×6 <fingerprint64("i32")> 2a 00 00 00 00 00 00 00 |
    //   00 00 ×6 <Order hash> | 04 00 ×6 <Side hash> 02 00 00 00 00 00 00 00
    constexpr auto cb = std::span<const std::byte>(
        detail::call_blob_bytes_v<decltype("evt"_lit), constant<42>, corpus::Order,
                                  constant<corpus::Side::sell>>);
    STATIC_REQUIRE(cb.size() == 8 + 16 + 16 + 24 + 16 + 24);
    STATIC_REQUIRE(cb[0] == std::byte{0x68} && cb[4] == std::byte{fmt::tag_call});
    STATIC_REQUIRE(cb[16] == std::byte{4});
    STATIC_REQUIRE(cb[24] == std::byte{fmt::arg_literal});
    STATIC_REQUIRE(cb[40] == std::byte{fmt::arg_const} &&
                   cb[41] == static_cast<std::byte>(kind::i32));
    // fingerprint64("i32") = 0x5c23eefcac97a25f, little-endian
    STATIC_REQUIRE(cb[48] == std::byte{0x5f} && cb[49] == std::byte{0xa2} &&
                   cb[54] == std::byte{0x23} && cb[55] == std::byte{0x5c});
    STATIC_REQUIRE(cb[56] == std::byte{42} && cb[57] == std::byte{0} && cb[63] == std::byte{0});
    STATIC_REQUIRE(cb[64] == std::byte{fmt::arg_type});
    STATIC_REQUIRE(cb[80] == std::byte{fmt::arg_const} && cb[81] == std::byte{0});
    STATIC_REQUIRE(cb[96] == std::byte{2} && cb[97] == std::byte{0} && cb[103] == std::byte{0});
}

namespace
{
template<class T> void print_blob(const char* const label)
{
    const auto b = blob_of<T>();
    std::println("  ES_BLOB({:<22}, 0x{:016x}, {});", label, blob_hash<T>(), b.size());
}
} // namespace

TEST_CASE("golden blob table", "[golden-print]")
{
    print_blob<corpus::Scalars>("corpus::Scalars");
    print_blob<corpus::Padded>("corpus::Padded");
    print_blob<corpus::Nested>("corpus::Nested");
    print_blob<corpus::Base>("corpus::Base");
    print_blob<corpus::Tag2>("corpus::Tag2");
    print_blob<corpus::Derived>("corpus::Derived");
    print_blob<corpus::MultiDerived>("corpus::MultiDerived");
    print_blob<corpus::Side>("corpus::Side");
    print_blob<corpus::Venue>("corpus::Venue");
    print_blob<corpus::Arrays>("corpus::Arrays");
    print_blob<corpus::Empty>("corpus::Empty");
    print_blob<corpus::WithEmpty>("corpus::WithEmpty");
    print_blob<corpus::Account>("corpus::Account");
    print_blob<corpus::Versioned>("corpus::Versioned");
    print_blob<corpus::Unversioned>("corpus::Unversioned");
    print_blob<corpus::Skipped>("corpus::Skipped");
    print_blob<corpus::Vec3>("corpus::Vec3");
    print_blob<corpus::Vec3i>("corpus::Vec3i");
    print_blob<corpus::Stamp>("corpus::Stamp");
    print_blob<corpus::StampFlat>("corpus::StampFlat");
    print_blob<corpus::Order>("corpus::Order");
    print_blob<corpus::Cancel>("corpus::Cancel");
    print_blob<corpus::Point2i>("corpus::Point2i");
    print_blob<corpus::Pair2i>("corpus::Pair2i");
    print_blob<Local>("Local");
    print_blob<t::WBool>("t::WBool");
    print_blob<t::WChar>("t::WChar");
    print_blob<t::WEnum>("t::WEnum");
    print_blob<t::WNeg>("t::WNeg");
    print_blob<t::Mode>("t::Mode");
    print_blob<corpus::DynMsg>("corpus::DynMsg");
    print_blob<corpus::DynNested>("corpus::DynNested");
    print_blob<corpus::DynArr>("corpus::DynArr");
    print_blob<corpus::DynBase>("corpus::DynBase");
    using namespace es::serreg::literals;
    const auto lb =
        std::span<const std::byte>(detail::literal_blob_bytes_v<decltype("evt"_lit)::str>);
    const auto cb =
        std::span<const std::byte>(detail::call_blob_bytes_v<decltype("evt"_lit), corpus::Order>);
    std::println("  literal evt: 0x{:016x}, {}\n  call(evt,Order): 0x{:016x}, {}",
                 fingerprint64(lb), lb.size(), fingerprint64(cb), cb.size());
    SUCCEED();
}
