// R-9 (schema evolution level 1, flat types): a reader whose type differs from the writer's but matches by name
// (or renamed_from) reads the writer's frames through a projection by member name: widening, defaults, R7.
// The "writer" types live in their own namespace and carry [[=es::serreg::name(...)]] so that both versions of
// each type exist in this process (as an old and a new build of one program would, one of them through the
// dictionary).
#include <es/serreg/serreg.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace evo
{
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Px
{
    std::int32_t mantissa;
    std::int8_t exponent;
};
// The reader's current Order: qty widened (u32 → u64), px renamed from `price`, `flags` and `venue` added (venue
// with a default member initialiser), `legacy` dropped, `lots` grew from 2 to 4 elements, the nested Px kept.
struct Order
{
    std::uint64_t id;
    [[= es::serreg::renamed_from("price")]] Px px;
    std::uint64_t qty;
    Side side;
    std::uint16_t lots[4];
    std::uint8_t flags;
    std::uint32_t venue = 7;
    double weight; // was f32
};
} // namespace evo
namespace evo_old
{
struct[[= es::serreg::name("evo::Order")]] Order
{
    std::uint64_t id;
    evo::Px price;
    std::uint32_t qty;
    evo::Side side;
    std::uint16_t lots[2];
    std::int32_t legacy;
    float weight;
};
// A writer that narrowed nothing the reader can take: `id` is a string of digits now → R7 for that member.
struct[[= es::serreg::name("evo::Order")]] OrderBad
{
    std::int64_t id; // i64 → u64 is not lossless: R7
    std::uint32_t qty;
};
struct[[= es::serreg::name("evo::OldName")]] Renamed
{
    std::uint32_t a;
};
} // namespace evo_old
namespace evo
{
struct[[= es::serreg::renamed_from("evo::OldName")]] NewName
{
    std::uint32_t a;
    std::uint32_t b = 5;
};
} // namespace evo

using namespace es::serreg;

namespace
{
dyn::dictionary process_dictionary()
{
    const auto d = tools::load_dictionary_rri(registry::instance().export_image());
    REQUIRE(d.has_value());
    return *d;
}
template<class... Ts> std::vector<std::byte> frame_of(const serialize_options& o, const Ts&... vs)
{
    std::vector<std::byte> buf;
    vector_sink s{buf};
    REQUIRE(serialize_with(o, s, vs...).has_value());
    return buf;
}
} // namespace

TEST_CASE("R-9 level 1: an old Order projects into the new one by member name (widen, default, "
          "rename, R12)",
          "[evolution]")
{
    const evo_old::Order old{42, {12345, -2}, 1000, evo::Side::sell, {3, 4}, -1, 1.5f};
    detail::touch<evo::Order>();
    const dyn::dictionary d = process_dictionary();
    for (const bool full : {false, true})
    {
        const std::vector<std::byte> buf = frame_of(serialize_options{.full_records = full}, old);
        evo::Order out{};
        CHECK(deserialize(buf, out).error() == error::hash_mismatch); // level 0: not the same type
        tools::projection_report rep;
        const auto r = deserialize(projection_context{&d, &rep}, buf, out);
        REQUIRE(r.has_value());
        CHECK(*r == buf.size());
        CHECK(out.id == 42);
        CHECK((out.px.mantissa == 12345 &&
               out.px.exponent == -2));     // renamed_from("price"), equal layout
        CHECK(out.qty == 1000);             // u32 → u64
        CHECK(out.side == evo::Side::sell); // enum by underlying kind
        CHECK(
            (out.lots[0] == 3 && out.lots[1] == 4 && out.lots[2] == 0 && out.lots[3] == 0)); // R12
        CHECK(out.flags == 0);                                                               // R6
        CHECK(out.venue == 7);     // R6 with the default member initialiser
        CHECK(out.weight == 1.5);  // f32 → f64
        CHECK(rep.copied == 4);    // id, px, side, lots
        CHECK(rep.widened == 2);   // qty, weight
        CHECK(rep.defaulted == 2); // flags, venue
        CHECK(rep.skipped == 1);   // legacy
        CHECK(rep.conflicts == 0);
    }
    // the plan is built once per (writer, T): the notes arrive with the first frame only
    tools::projection_report again;
    evo::Order out{};
    REQUIRE(deserialize(projection_context{&d, &again}, frame_of({}, old), out).has_value());
    CHECK(again.notes.empty());
    CHECK(again.widened == 2);
}

TEST_CASE("R-9 level 1: tools::project<T> and the R7 rule", "[evolution]")
{
    detail::touch<evo::Order>();
    detail::touch<evo_old::OrderBad>(); // the writer's type, as a dictionary would carry it
    const dyn::dictionary d = process_dictionary();
    const dyn::type* const w = d.find_type(schema_hash_v<evo_old::OrderBad>);
    REQUIRE(w != nullptr);
    const evo_old::OrderBad bad{-5, 9};
    const std::byte* const bytes = reinterpret_cast<const std::byte*>(&bad);
    tools::projection_report rep;
    const auto r =
        tools::project<evo::Order>(*w, d, std::span<const std::byte>(bytes, sizeof bad), &rep);
    REQUIRE(r.has_value());
    CHECK(r->id == 0); // R7: i64 → u64 is not projected; the default stays
    CHECK(r->qty == 9);
    CHECK(r->venue == 7);
    CHECK(rep.conflicts == 1);
    REQUIRE_FALSE(rep.notes.empty());
    CHECK(rep.notes[0] == "evo::Order.id: i64 -> u64 not projected (R7)");
    // not the same type by name → hash_mismatch
    const dyn::type* const other = d.find_type(schema_hash_v<evo::Px>);
    REQUIRE(other != nullptr);
    CHECK(tools::project<evo::Order>(*other, d, std::span<const std::byte>(bytes, sizeof bad))
              .error() == error::hash_mismatch);
    // too few bytes
    CHECK(tools::project<evo::Order>(*w, d, std::span<const std::byte>(bytes, 4)).error() ==
          error::truncated);
}

TEST_CASE("R-9 level 1: renamed_from on a class, exact arguments beside a projected one, "
          "call-shape rules",
          "[evolution]")
{
    detail::touch<evo::NewName>();
    const dyn::dictionary d = process_dictionary();
    const evo_old::Renamed old{11};
    using namespace es::serreg::literals;
    const std::vector<std::byte> buf = frame_of({}, "evt"_, old, std::uint32_t{3});
    evo::NewName out{};
    std::uint32_t n = 0;
    const auto r = deserialize(d, buf, "evt"_, out, n);
    REQUIRE(r.has_value());
    CHECK((out.a == 11 && out.b == 5 && n == 3));
    // a different literal is a different call (§2.1 item 5)
    CHECK(deserialize(d, buf, "other"_, out, n).error() == error::hash_mismatch);
    // a different argument count too
    CHECK(deserialize(d, buf, "evt"_, out).error() == error::length_mismatch);
    // the exact path still works through the dictionary overload, and a frame of the same type is read as before
    const std::vector<std::byte> same = frame_of({}, "evt"_, evo::NewName{1, 2}, std::uint32_t{4});
    REQUIRE(deserialize(d, same, "evt"_, out, n).has_value());
    CHECK((out.a == 1 && out.b == 2 && n == 4));
}
