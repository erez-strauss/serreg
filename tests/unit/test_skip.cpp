// tests/unit/test_skip.cpp — cc F4 / cc S7 (FR-S10): a [[=skip]] member is never instantiated by the reflection
// layer, so (a) a skipped `const char*` compiles (pointers are otherwise rejected — see tests/negative), (b) a
// skipped nested struct's type never enters the registry nor the ELF dictionary section (the ctest
// srdump_elf_no_skipped_type checks `srdump --elf serreg_tests` for the same), and (c) the skipped bytes are
// scrubbed like padding.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace es::serreg;

namespace skipns
{
struct Hidden
{
    std::int32_t a;
    std::int32_t b;
}; // must never be registered
struct WithPtr
{
    std::uint64_t key;
    [[= es::serreg::skip]] const char* name; // pointer: allowed only because it is skipped
    std::uint32_t n;
};
struct SkipsNested
{
    std::int32_t x;
    [[= es::serreg::skip]] Hidden h; // supported type, but skipped: absent everywhere
    std::int32_t y;
};
struct SkipsArrayOfHidden
{
    [[= es::serreg::skip]] Hidden hs[2];
    std::uint8_t z;
};
} // namespace skipns

TEST_CASE(
    "cc F4 [[=skip]] const char* member compiles; the pointee type is absent from the descriptor",
    "[skip][F4]")
{
    STATIC_REQUIRE(descriptor_v<skipns::WithPtr>.fields.size() == 2);
    STATIC_REQUIRE(descriptor_v<skipns::WithPtr>.fields[0].name == std::string_view("key"));
    STATIC_REQUIRE(descriptor_v<skipns::WithPtr>.fields[1].name == std::string_view("n"));
    STATIC_REQUIRE(descriptor_v<skipns::WithPtr>.size == sizeof(skipns::WithPtr));
    STATIC_REQUIRE(descriptor_v<skipns::WithPtr>.padding_gaps.size() ==
                   2); // the pointer slot + tail padding
    STATIC_REQUIRE(descriptor_v<skipns::WithPtr>.padding_gaps[0].offset == 8 &&
                   descriptor_v<skipns::WithPtr>.padding_gaps[0].len == 8);
    STATIC_REQUIRE(fingerprint64("struct<skipns::WithPtr>[24,8]{key:u64@0[8,8];n:u32@16[4,4];}") ==
                   schema_hash_v<skipns::WithPtr>);
    skipns::WithPtr in{0x11, "not on the wire", 3};
    std::vector<std::byte> buf;
    vector_sink out{buf};
    REQUIRE(serialize(out, in));
    std::uintptr_t on_wire = 1;
    std::memcpy(&on_wire, buf.data() + 16 + 8, sizeof on_wire);
    CHECK(on_wire == 0);
    skipns::WithPtr o{};
    o.name = "stale";
    REQUIRE(deserialize(buf, o));
    CHECK(o.key == 0x11);
    CHECK(o.n == 3);
    CHECK(o.name == nullptr); // the native image arrives zeroed in the skipped slot
}

TEST_CASE("cc F4 a skipped nested struct type is absent from the registry and the descriptor",
          "[skip][F4]")
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    REQUIRE(serialize(out, skipns::SkipsNested{1, {2, 3}, 4},
                      skipns::SkipsArrayOfHidden{{{5, 6}, {7, 8}}, 9}));
    const registry& reg = registry::instance();
    CHECK(reg.find_type_by_name("skipns::SkipsNested") != nullptr);
    CHECK(reg.find_type_by_name("skipns::SkipsArrayOfHidden") != nullptr);
    CHECK(reg.find_type_by_name("skipns::Hidden") == nullptr);
    CHECK(reg.find_type(schema_hash_v<skipns::Hidden>) == nullptr);
    STATIC_REQUIRE(descriptor_v<skipns::SkipsNested>.fields.size() == 2);
    STATIC_REQUIRE(descriptor_v<skipns::SkipsNested>.fields[1].name == std::string_view("y"));
    STATIC_REQUIRE(descriptor_v<skipns::SkipsNested>.padding_gaps.size() == 1);
    STATIC_REQUIRE(descriptor_v<skipns::SkipsNested>.padding_gaps[0].offset == 4 &&
                   descriptor_v<skipns::SkipsNested>.padding_gaps[0].len == 8);
    STATIC_REQUIRE(descriptor_v<skipns::SkipsArrayOfHidden>.fields.size() == 1);
    // the ELF section of this binary does not carry the skipped type either (FR-D6)
    const auto elf = es::serreg::test::embedded_dictionary_of_self();
    REQUIRE(elf.has_value());
    CHECK(elf->find_type_by_name("skipns::SkipsNested") != nullptr);
    CHECK(elf->find_type_by_name("skipns::Hidden") == nullptr);
    CHECK(elf->find_type(schema_hash_v<skipns::Hidden>) == nullptr);
    // the exported RRI and the JSON rendering do not mention it
    CHECK(reg.to_json().find("skipns::Hidden") == std::string::npos);
    // the skipped bytes are zero on the wire and the value round-trips
    for (std::size_t i = 16 + 4; i < 16 + 12; ++i)
        CHECK(buf[i] == std::byte{0});
    skipns::SkipsNested a{};
    skipns::SkipsArrayOfHidden b{};
    REQUIRE(deserialize(buf, a, b));
    CHECK((a.x == 1 && a.y == 4 && a.h.a == 0 && a.h.b == 0));
    CHECK((b.z == 9 && b.hs[1].b == 0));
}
