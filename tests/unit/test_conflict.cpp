// tests/unit/test_conflict.cpp — FR-R4: two definitions with the same schema hash but different layout hashes are a
// conflict, not an alias. Reflection-generated types cannot produce this (the schema string is a superset of the
// layout string, so equal schema hashes imply equal layout hashes short of a 64-bit fingerprint collision); the registry's
// push() logic is exercised with a hand-built node instead. Kept in its own executable: the injected conflict stays
// in the process registry for its lifetime. The two-image variant (real plugins, stderr warning, srdump exit 1) is
// tests/dlopen (dlopen_H_conflicting_plugins).
#include "corpus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace es::serreg;

namespace
{
// A second "definition" of corpus::Cancel: same descriptor, different layout hash, distinct storage.
constexpr type_desc make_impostor()
{
    type_desc d = descriptor_v<corpus::Cancel>;
    d.layout_hash ^= 0x1ull;
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

std::uint32_t refs_of(const registry& reg, const std::uint64_t schema_hash)
{
    const registry_entry* const e = reg.snapshot().find(node_kind::type, schema_hash);
    return e ? e->refs : 0;
}
} // namespace

TEST_CASE("FR-R4 same schema hash + different layout hash is recorded as a conflict",
          "[registry][conflict]")
{
    registry& reg = registry::instance();
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(serialize(s, corpus::sample_cancel())); // the genuine Cancel is registered (at load)
    REQUIRE(reg.conflicts().empty());
    REQUIRE_FALSE(reg.has_conflict(schema_hash_v<corpus::Cancel>));
    const std::size_t n0 = reg.size();
    const std::uint64_t v0 = reg.version();
    const type_desc* const genuine = reg.find_type(schema_hash_v<corpus::Cancel>);
    REQUIRE(genuine == &descriptor_v<corpus::Cancel>);
    REQUIRE(refs_of(reg, schema_hash_v<corpus::Cancel>) == 1);

    reg.push(&impostor_node); // what a second image's guard would do

    const std::vector<conflict> conflicts = reg.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].schema_hash == schema_hash_v<corpus::Cancel>);
    CHECK(conflicts[0].name == "corpus::Cancel");
    CHECK(conflicts[0].image_a == reg.images().front()); // both "images" are this executable here
    CHECK(conflicts[0].image_b == conflicts[0].image_a);
    CHECK(reg.has_conflict(schema_hash_v<corpus::Cancel>));
    CHECK_FALSE(reg.has_conflict(schema_hash_v<corpus::Order>));
    CHECK_FALSE(impostor_node.linked); // conflicts are never linked ...
    CHECK(reg.size() == n0);           // ... nor counted ...
    CHECK(reg.find_type(schema_hash_v<corpus::Cancel>) ==
          genuine); // the first definition stays authoritative
    CHECK(refs_of(reg, schema_hash_v<corpus::Cancel>) ==
          1); // ... nor do they count as a referencing image
    // v1.0.2: every mutation (including a rejected conflict) publishes a new index generation, so the version
    // moves; the conflict is visible in that generation's conflict list.
    CHECK(reg.version() > v0);
    CHECK(reg.snapshot().conflicts.size() == 1);
    CHECK(reg.snapshot().version == reg.version());
    // an identical-layout second definition is an alias, not a conflict (refs++)
    constexpr static type_desc twin = descriptor_v<corpus::Cancel>;
    static constinit reg_node twin_node{
        &twin, twin.blob, twin.schema_hash, twin.name, node_kind::type, nullptr, nullptr, 0, false};
    reg.push(&twin_node);
    CHECK(reg.conflicts().size() == 1);
    CHECK(reg.size() == n0);
    CHECK(refs_of(reg, schema_hash_v<corpus::Cancel>) == 2);
    CHECK(reg.find_type(schema_hash_v<corpus::Cancel>) == genuine);
}
