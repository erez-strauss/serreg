// tests/unit/test_empty_member.cpp — [[no_unique_address]] empty member (spec §6.1): serialising corpus::WithEmpty
// also registers the component type corpus::Empty (no fields, size 1).
#include "corpus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("6.1 empty [[no_unique_address]] member round-trips and registers its (empty) type",
          "[roundtrip][empty]")
{
    using namespace es::serreg;
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const corpus::WithEmpty in{{}, 77};
    REQUIRE(serialize(out, in));
    CHECK(buf.size() == 16 + 8); // 4-byte payload padded to 8
    corpus::WithEmpty o{{}, 0};
    REQUIRE(deserialize(buf, o));
    CHECK(o == in);
    const type_desc* const e = registry::instance().find_type_by_name("corpus::Empty");
    REQUIRE(e != nullptr);
    CHECK(e->fields.empty());
    CHECK(e->size == 1);
    CHECK(e->padding_gaps.size() == 1); // the whole byte is padding
    const type_desc* const w = registry::instance().find_type_by_name("corpus::WithEmpty");
    REQUIRE(w != nullptr);
    CHECK(w->fields[0].type_hash == e->schema_hash);
    CHECK(w->padding_gaps.empty());
}
