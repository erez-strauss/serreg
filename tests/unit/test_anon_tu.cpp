// tests/unit/test_anon_tu.cpp — corpus::Local, the anonymous-namespace corpus type, named explicitly since 2.1.14
// (spec §6.2b, FR-R4), serialised
// in the same TU (and the same frame) as ordinary namespace-scope types and a literal.
#include "corpus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE(
    "6.2b anonymous-namespace type round-trips and registers under its explicit name corpus::Local",
    "[roundtrip][anon]")
{
    using namespace es::serreg;
    using namespace es::serreg::literals;
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const Local in{0x1234, 0x5678};
    REQUIRE(
        serialize(out, "anon"_lit, in,
                  corpus::sample_cancel())); // internal-linkage + external-linkage blobs, one TU
    Local o{};
    corpus::Cancel c{};
    REQUIRE(deserialize(buf, "anon"_lit, o, c));
    CHECK(o.a == in.a);
    CHECK(o.b == in.b);
    CHECK(c == corpus::sample_cancel());
    std::vector<std::byte> alone;
    vector_sink out2{alone};
    REQUIRE(serialize(out2, in));
    const type_desc* const d = registry::instance().find_type_by_name("corpus::Local");
    REQUIRE(d != nullptr);
    CHECK(d->schema_hash == schema_hash_v<Local>);
    CHECK(d->fields.size() == 2);
    CHECK(registry::instance().find_call(call_hash_v<Local>) != nullptr);
}
