// tests/unit/test_registry.cpp — the process registry (spec §6.3, FR-R1..R5, FR-H3, FR-R9): contents, lookups,
// program hash, export/parse round-trip, JSON, version stability and change callbacks. The strict "registered
// before main()" property is checked by premain_check.cpp (a separate executable); here Catch2's own main has
// already run, so the checks are on contents rather than timing.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

#if ES_SERREG_VALIDATE_READ != 0
// B3 (2.2.6): serreg_tests is built with ES_SERREG_VALIDATE_READ=1 on purpose, against the default libserreg
// (tests/unit/CMakeLists.txt), so every TU of it references validate_read_build<1>, which that libserreg does not
// define. This is the one program that knowingly mixes the two values — libserreg's objects instantiate none of
// the templates that branch on the switch (nm finds no copy_flat / bools_canonical there) — and it says so by
// supplying the instantiation itself. A real consumer that does this takes the ODR responsibility over; the
// fuse is checked by ctest validate_read_mismatch_fails_to_link (tests/link).
namespace es::serreg::detail
{
template<int V> void validate_read_build() noexcept
{
}
template void validate_read_build<ES_SERREG_VALIDATE_READ>() noexcept;
} // namespace es::serreg::detail
#endif

namespace
{

// Every entity this executable uses. The calls below were instantiated at compile time and their guards ran at
// load; executing them here changes nothing in the registry (that is what the version test checks).
void touch_all()
{
    std::vector<std::byte> v;
    vector_sink s{v};
    static_cast<void>(serialize(
        s, "registry"_lit, corpus::sample_order(), corpus::sample_cancel(), corpus::Scalars{},
        corpus::Padded{}, corpus::Nested{}, corpus::Derived{}, corpus::MultiDerived{},
        corpus::Venue::nyse, corpus::Arrays{}, corpus::Account{}, corpus::Versioned{},
        corpus::Skipped{}, corpus::Vec3{}, corpus::Vec3i{}, corpus::Stamp{}));
    static_cast<void>(serialize(s, "evt"_lit, corpus::sample_order()));
}

} // namespace

TEST_CASE("FR-R1/FR-R2 the registry holds every entity the binary uses, once", "[registry]")
{
    touch_all();
    const registry& reg = registry::instance();
    // Distinct entities known to be used by this test binary (a lower bound: other TUs add more).
    const char* const names[] = {
        "corpus::Order",      "corpus::Side",
        "corpus::Cancel",     "corpus::Scalars",
        "corpus::Padded",     "corpus::Nested",
        "corpus::Base",       "corpus::Tag2",
        "corpus::Derived",    "corpus::MultiDerived",
        "corpus::Venue",      "corpus::Arrays",
        "corpus::Account",    "corpus::Versioned",
        "corpus::Skipped",    "corpus::Vec<corpus::Side,3>",
        "corpus::Vec<i32,3>", "corpus::StampFlat",
        "corpus::Local",      "corpus::Empty",
        "corpus::WithEmpty"}; // the last three come from other TUs of this executable
    for (const char* const n : names)
    {
        INFO(n);
        CHECK(reg.find_type_by_name(n) != nullptr);
    }
    CHECK(reg.size() >= std::size(names) + 2 /*literals*/ + 2 /*calls*/);
    // no duplicates by (kind, hash); the published index is sorted by (kind, hash) (v1.0.2 lock-free index)
    const registry_index& snap = reg.snapshot();
    const auto& es = snap.entries;
    CHECK(es.size() == reg.size());
    for (std::size_t i = 1; i < es.size(); ++i)
    {
        CHECK((std::pair{es[i - 1].nk, es[i - 1].hash} < std::pair{es[i].nk, es[i].hash}));
        CHECK_FALSE((es[i - 1].nk == es[i].nk && es[i - 1].hash == es[i].hash));
    }
    // one entry per entity; refs counts contributing images (FR-R12) — one here, the main executable
    const auto it = std::find_if(es.begin(), es.end(),
                                 [](const registry_entry& e) { return e.name == "corpus::Order"; });
    REQUIRE(it != es.end());
    CHECK(it->refs == 1);
    CHECK(it->image_base != nullptr);
    CHECK(it->payload == &descriptor_v<corpus::Order>);
    CHECK(snap.find(node_kind::type, schema_hash_v<corpus::Order>) == &*it);
    CHECK(snap.find(node_kind::type, 0xDEADBEEFull) == nullptr);
    CHECK(snap.find(node_kind::literal, schema_hash_v<corpus::Order>) ==
          nullptr); // kind is part of the key
    // 2.2.1 (C-11): the main program's entries are viewed in place — the executable is never unmapped before
    // exit — so the index points into the image (a plugin's entries are copied: tests/dlopen)
    CHECK(it->blob.size() == descriptor_v<corpus::Order>.blob.size());
    CHECK(it->blob.data() == descriptor_v<corpus::Order>.blob.data());
    CHECK(it->name.data() == descriptor_v<corpus::Order>.name.data());
    CHECK(snap.version == reg.version());
    CHECK(snap.conflicts.empty());
    CHECK(reg.conflicts().empty());
    CHECK(reg.images().size() == 1); // everything comes from the main executable
    // the same generation stays valid and identical while nothing changes
    CHECK(&reg.snapshot() == &snap);
}

namespace reg_only
{
struct Inner
{
    std::uint16_t a;
};
struct NeverSerialised
{
    std::uint64_t id;
    Inner in;
};
enum class Flag : std::uint8_t
{
    off,
    on
};
void reader_only_code()
{ // never called: the instantiation registers, as serialize() would (2.1.16, A1)
    es::serreg::register_types<NeverSerialised, Flag, es::serreg::lit<"reader-only">>();
    es::serreg::register_call<es::serreg::lit<"ro">, NeverSerialised, es::serreg::constant<7>>();
}
} // namespace reg_only

TEST_CASE("2.1.16 A1 register_types / register_call register without a serialize call site",
          "[registry][register_types]")
{
    const auto& reg = registry::instance();
    // the types, the component type and the literal are known although nothing serialises them
    REQUIRE(reg.find_type(schema_hash_v<reg_only::NeverSerialised>) != nullptr);
    REQUIRE(reg.find_type(schema_hash_v<reg_only::Inner>) != nullptr);
    REQUIRE(reg.find_type(schema_hash_v<reg_only::Flag>) != nullptr);
    CHECK(reg.find_type_by_name("reg_only::NeverSerialised") != nullptr);
    CHECK(reg.find_literal(lit<"reader-only">::id) == "reader-only");
    // the call signature, with its constant, exactly as deserialize(bytes, "ro"_, out, 7_) would register it
    const call_desc* const c =
        reg.find_call(call_hash_v<lit<"ro">, reg_only::NeverSerialised, constant<7>>);
    REQUIRE(c != nullptr);
    REQUIRE(c->args.size() == 3);
    CHECK(c->args[2].ak == fmt::arg_const);
    CHECK(c->args[2].value == 7);
    // and the exported image carries them, so --elf / an RRI of a reader-only binary lists them
    const auto d = tools::load_dictionary_rri(reg.export_image());
    REQUIRE(d.has_value());
    CHECK(d->find_type(schema_hash_v<reg_only::NeverSerialised>) != nullptr);
    CHECK(d->find_call(call_hash_v<lit<"ro">, reg_only::NeverSerialised, constant<7>>) != nullptr);
}

TEST_CASE("FR-R5 lookups: find_type / find_type_by_name / find_call / find_literal", "[registry]")
{
    touch_all();
    const registry& reg = registry::instance();
    const type_desc* const order = reg.find_type(schema_hash_v<corpus::Order>);
    REQUIRE(order != nullptr);
    CHECK(order == &descriptor_v<corpus::Order>);
    CHECK(order == reg.find_type_by_name("corpus::Order"));
    CHECK(order->fields.size() == 5);
    CHECK(reg.find_type(0xDEADBEEFull) == nullptr);
    CHECK(reg.find_type_by_name("corpus::Nope") == nullptr);

    const call_desc* const cd = reg.find_call(call_hash_v<decltype("evt"_lit), corpus::Order>);
    REQUIRE(cd != nullptr);
    REQUIRE(cd->args.size() == 2);
    CHECK(cd->args[0].is_literal);
    CHECK(cd->args[0].hash == decltype("evt"_lit)::id);
    CHECK_FALSE(cd->args[1].is_literal);
    CHECK(cd->args[1].hash == schema_hash_v<corpus::Order>);
    CHECK(reg.find_call(0x1234ull) == nullptr);

    CHECK(reg.find_literal(decltype("registry"_lit)::id) == "registry");
    CHECK(reg.find_literal(decltype("evt"_lit)::id) == "evt");
    CHECK(reg.find_literal(0x5555ull).empty());
    CHECK(reg.has_conflict(schema_hash_v<corpus::Order>) == false);
}

TEST_CASE("FR-H3 program_hash is the order-independent XXH3-64 of the sorted type hashes",
          "[registry]")
{
    touch_all();
    const registry& reg = registry::instance();
    std::vector<std::uint64_t> hs;
    for (const registry_entry& e : reg.snapshot().entries)
        if (e.nk == node_kind::type)
            hs.push_back(e.hash);
    REQUIRE_FALSE(hs.empty());
    std::sort(hs.begin(), hs.end());
    CHECK(reg.program_hash() == fingerprint64_u64s(hs));
    // literals and calls do not enter the program hash
    std::vector<std::uint64_t> all;
    for (const registry_entry& e : reg.snapshot().entries)
        all.push_back(e.hash);
    std::sort(all.begin(), all.end());
    CHECK(reg.program_hash() != fingerprint64_u64s(all));
    // a reshuffled input gives the same value only after sorting (i.e. the API really sorts)
    std::vector<std::uint64_t> rev(hs.rbegin(), hs.rend());
    CHECK(fingerprint64_u64s(rev) != fingerprint64_u64s(hs));
}

TEST_CASE("FR-R9/FR-D3 export_image() parses back and lists the same entities",
          "[registry][export]")
{
    touch_all();
    const registry& reg = registry::instance();
    const std::vector<std::byte> img = reg.export_image();
    REQUIRE(img.size() >= fmt::rri_header_size);
    CHECK(std::memcmp(img.data(), fmt::rri_magic.data(), 4) == 0);
    dyn::dictionary d;
    REQUIRE(d.add_rri(img));
    CHECK(d.program_hash == reg.program_hash());
    std::size_t types = 0, lits = 0, calls = 0;
    for (const registry_entry& n : reg.snapshot().entries)
    {
        switch (n.nk)
        {
        case node_kind::type: {
            ++types;
            const dyn::type* const t = d.find_type(n.hash);
            REQUIRE(t != nullptr);
            CHECK(t->name == n.name);
            const type_desc* const td = static_cast<const type_desc*>(n.payload);
            CHECK(t->layout_hash == td->layout_hash);
            CHECK(t->size == td->size);
            CHECK(t->fields.size() == td->fields.size());
            CHECK(t->enumerators.size() == td->enumerators.size());
            break;
        }
        case node_kind::literal:
            ++lits;
            REQUIRE(d.find_literal(n.hash) != nullptr);
            CHECK(d.find_literal(n.hash)->text == n.name);
            break;
        case node_kind::call:
            ++calls;
            REQUIRE(d.find_call(n.hash) != nullptr);
            break;
        case node_kind::scope:
            break; // 2.2.1: scope nodes have no ENTRY in the full image
        }
    }
    CHECK(d.types.size() == types);
    CHECK(d.literals.size() == lits);
    CHECK(d.calls.size() == calls);
    // the embedded copy in the ELF image says the same thing (FR-D6)
    const auto elf = es::serreg::test::embedded_dictionary_of_self();
    REQUIRE(elf.has_value());
    for (const auto& [h, t] : d.types)
    {
        INFO(t.name);
        CHECK(elf->find_type(h) != nullptr);
    }
    // dictionary → RRI → dictionary is stable
    dyn::dictionary d2;
    REQUIRE(d2.add_rri(dyn::to_rri(d)));
    CHECK(d2.to_json() == d.to_json());
    // cc F1 (d): to_rri(add_rri(export_image())) is byte-identical to export_image() in its ENTRY stream part
    // (the header may differ in flags/reserved; the entries are re-encoded from parsed data and must match exactly).
    const std::vector<std::byte> back = dyn::to_rri(d);
    REQUIRE(back.size() == img.size());
    const std::span<const std::byte> a(img), b(back);
    CHECK(std::equal(a.begin() + fmt::rri_header_size, a.end(), b.begin() + fmt::rri_header_size));
    // ... and the header fields that carry information agree as well
    CHECK(std::memcmp(a.data(), b.data(), 4) == 0);            // magic
    CHECK(std::memcmp(a.data() + 4, b.data() + 4, 4) == 0);    // major/minor
    CHECK(std::memcmp(a.data() + 16, b.data() + 16, 12) == 0); // n_entries + program_hash
}

TEST_CASE("FR-D2 to_json() names every type and is what the dictionary renders",
          "[registry][export]")
{
    touch_all();
    const registry& reg = registry::instance();
    const std::string js = reg.to_json();
    for (const char* const n :
         {"corpus::Order", "corpus::Side", "corpus::Vec<corpus::Side,3>", "corpus::StampFlat"})
    {
        INFO(n);
        CHECK(js.find(n) != std::string::npos);
    }
    CHECK(js.find("\"registry\"") != std::string::npos); // literal text
    CHECK(js.find("balance_") != std::string::npos);     // private member name
    CHECK(js.find("<base:corpus::Base>") != std::string::npos);
    dyn::dictionary d;
    REQUIRE(d.add_rri(reg.export_image()));
    CHECK(d.to_json() == js);
    CHECK_FALSE(reg.to_toml().empty());
}

TEST_CASE("FR-R3/FR-R11 version is stable at run time and on_change stays silent",
          "[registry][version]")
{
    // Everything this binary uses was registered at load; serializing does not touch the registry (FR-R1, NFR-1).
    registry& reg = registry::instance();
    const std::uint64_t v0 = reg.version();
    const std::size_t n0 = reg.size();
    REQUIRE(v0 >= n0); // one bump per push
    std::atomic<int> fired{0};
    const std::uint64_t cb_id = reg.on_change([&](const registry&) { ++fired; });
    touch_all();
    for (int i = 0; i < 10; ++i)
    {
        std::vector<std::byte> v;
        vector_sink s{v};
        REQUIRE(serialize(s, "late"_lit, corpus::sample_order(), corpus::Padded{}));
        corpus::Order o{};
        corpus::Padded p{};
        REQUIRE(deserialize(v, "late"_lit, o, p));
    }
    CHECK(reg.version() == v0);
    CHECK(reg.size() == n0);
    CHECK(fired.load() == 0);
    CHECK(reg.remove_on_change(
        cb_id)); // `fired` dies with this frame; later pushes (below, test_format17) notify
    // NOTE: a late registration cannot be produced from inside this executable — every serialize<Ts...> in the
    // image was instantiated at compile time and pushed at load. The on_change-fires path is exercised by the
    // dlopen tests (tests/dlopen), where a plugin image really adds entries at run time, and by the hand-built
    // nodes below (what a plugin's guard would do).
}

namespace
{
// Two literals no call site of this binary registers (test_format17.cpp pushes its own the same way): the
// re-entry test below pushes them by hand, as a plugin's guards would.
using reentry_lit = decltype("on_change re-entry (Q10)"_lit);
constinit reg_node reentry_node{nullptr,
                                detail::literal_blob_bytes_v<reentry_lit::str>,
                                reentry_lit::id,
                                {},
                                node_kind::literal,
                                nullptr,
                                nullptr,
                                0,
                                false};
using reentry_inner_lit = decltype("on_change re-entry, pushed by the callback (Q10)"_lit);
constinit reg_node reentry_inner_node{nullptr,
                                      detail::literal_blob_bytes_v<reentry_inner_lit::str>,
                                      reentry_inner_lit::id,
                                      {},
                                      node_kind::literal,
                                      nullptr,
                                      nullptr,
                                      0,
                                      false};
} // namespace

TEST_CASE("FR-R10/FR-R11 Q10 a change callback that registers and publishes is not re-notified",
          "[registry][on_change][reentry]")
{
    registry& reg = registry::instance();
    reg.refresh(); // nothing pending from earlier tests
    const std::uint64_t v0 = reg.version();
    const std::uint64_t g0 = reg.generations();
    std::atomic<int> fired{0};
    std::atomic<int> other_fired{0};
    std::uint64_t version_seen_inside = 0;
    std::uint64_t pushed_from_inside = 0; // the version after the callback's own push
    // The offending callback: it registers a literal (what a dlopen() it triggered would do) and then reads the
    // registry, which publishes that change on this thread — a re-entry into the publisher from a callback.
    const std::uint64_t id = reg.on_change([&](const registry& r) {
        ++fired;
        if (fired.load() > 1)
            return; // would be the double notification; the CHECK below catches it
        registry::instance().push(&reentry_inner_node);
        pushed_from_inside = r.version();
        const registry_index& ix =
            r.snapshot(); // re-enters publish_if_stale(): publishes, no callbacks
        version_seen_inside = ix.version;
        CHECK(ix.find(node_kind::literal, reentry_inner_lit::id) != nullptr); // the read is current
    });
    // A second, well-behaved callback: it must run exactly once too (not again for the inner change).
    const std::uint64_t id2 = reg.on_change([&](const registry&) { ++other_fired; });

    reg.push(&reentry_node); // the outer change: marks stale, bumps the version
    CHECK(reg.version() > v0);
    reg.refresh(); // publishes and runs the callbacks on this thread; must return (no hang, no recursion)
    CHECK(fired.load() == 1);
    CHECK(other_fired.load() == 1);
    CHECK(pushed_from_inside > v0 + 1);               // the inner push moved the version again
    CHECK(version_seen_inside == pushed_from_inside); // and the read inside saw it published
    CHECK(reg.generations() == g0 + 2); // two generations: the outer and the inner change
    // nothing is pending: a later read publishes nothing and notifies nobody
    reg.refresh();
    static_cast<void>(reg.snapshot());
    CHECK(fired.load() == 1);
    CHECK(other_fired.load() == 1);
    CHECK(!reg.find_literal(reentry_lit::id).empty());
    CHECK(!reg.find_literal(reentry_inner_lit::id).empty());
    CHECK(reg.remove_on_change(id));
    CHECK(reg.remove_on_change(id2));
}
