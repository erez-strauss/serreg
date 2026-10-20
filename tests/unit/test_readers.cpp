// tests/unit/test_readers.cpp — the non-typed reader styles (spec §6.4, FR-S4): inspect() (style 2, a lazy
// record range since v1.0.2), dispatch<Ts...>() (style 3) and view<T>() (style 5), including the misaligned and
// unmatched-record paths, the gk 1.1 short-full-record rejection and large frames (gk 1.2 / cc F7: no record cap).
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;
using es::serreg::test::all_records;

namespace
{

template<class... Fs> struct overloaded : Fs...
{
    using Fs::operator()...;
};

template<class... Ts> std::vector<std::byte> frame_of(const Ts&... vs)
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    REQUIRE(serialize(out, vs...));
    return buf;
}

// A copy of `src` starting at an address ≡ 1 (mod 8): every 8-aligned record payload becomes misaligned.
struct misaligned_copy
{
    std::vector<std::byte> store;
    std::span<const std::byte> bytes;
    explicit misaligned_copy(const std::span<const std::byte> src) : store(src.size() + 8)
    {
        std::byte* p = store.data();
        while (reinterpret_cast<std::uintptr_t>(p) % 8 != 1)
            ++p;
        std::memcpy(p, src.data(), src.size());
        bytes = {p, src.size()};
    }
};

} // namespace

TEST_CASE("FR-S4 inspect() reports record count, types and literal flags", "[readers][inspect]")
{
    const auto buf = frame_of("evt"_lit, corpus::sample_order(), corpus::Side::sell, "tail"_lit);
    const auto fv = inspect(buf);
    REQUIRE(fv.has_value());
    CHECK(fv->header.n_records == 4);
    CHECK(fv->header.call_hash ==
          (call_hash_v<decltype("evt"_lit), corpus::Order, corpus::Side, decltype("tail"_lit)>));
    CHECK_FALSE(fv->header.full());
    REQUIRE(fv->size() == 4);
    const auto recs = all_records(*fv);
    REQUIRE(recs.size() == 4);
    CHECK(recs[0].is_literal);
    CHECK(recs[0].hash == decltype("evt"_lit)::id);
    CHECK(recs[0].desc == nullptr);
    CHECK(recs[0].payload.size() == 0); // 1.1 FR-L6: the literal is named by the call signature
    CHECK_FALSE(recs[1].is_literal);
    REQUIRE(recs[1].desc != nullptr);
    CHECK(recs[1].desc->name == "corpus::Order");
    CHECK(recs[1].payload.size() == sizeof(corpus::Order));
    REQUIRE(recs[2].desc != nullptr);
    CHECK(recs[2].desc->k == kind::enum_);
    CHECK(recs[2].payload.size() == 1); // enum payload is its native size; the slot is padded to 8
    CHECK(recs[3].is_literal);
    CHECK(registry::instance().find_literal(recs[3].hash) == "tail");
    // the range is re-iterable and every pass yields the same payload addresses (no copies)
    const auto again = all_records(*fv);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(again[i].payload.data() == recs[i].payload.data());
    CHECK(recs[1].payload.data() == buf.data() + 16 + 0);

    SECTION("full-record frames are inspected from per-record headers")
    {
        std::vector<std::byte> full;
        vector_sink out{full};
        REQUIRE(serialize_with(serialize_options{.full_records = true}, out, "evt"_lit,
                               corpus::sample_cancel()));
        const auto f2 = inspect(full);
        REQUIRE(f2.has_value());
        CHECK(f2->header.full());
        REQUIRE(f2->size() == 2);
        const auto r2 = all_records(*f2);
        CHECK(r2[0].is_literal);
        REQUIRE(r2[1].desc != nullptr);
        CHECK(r2[1].desc->schema_hash == schema_hash_v<corpus::Cancel>);
    }
    SECTION("an unknown call hash is reported, a truncated frame too")
    {
        std::vector<std::byte> bad = buf;
        bad[8] ^= std::byte{0x01}; // flip a bit of call_hash
        const auto r = inspect(bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == error::unknown_call);
        const auto t = inspect(std::span<const std::byte>(buf).first(20));
        REQUIRE_FALSE(t.has_value());
        CHECK(t.error() == error::truncated);
    }
    SECTION("gk 1.17 / cc F7: a foreign compact frame is cut with a side dictionary")
    {
        // Pretend the call is unknown to this process by asking with a dictionary only: build the dictionary from the
        // live registry, then check inspect(bytes, &dict) resolves the same records. (A truly foreign type cannot be
        // produced in-process; the dlopen tests cover that path.)
        dyn::dictionary dict;
        REQUIRE(dict.add_rri(registry::instance().export_image()));
        const auto with_dict = inspect(buf, &dict);
        REQUIRE(with_dict.has_value());
        CHECK(with_dict->size() == 4);
        CHECK(all_records(*with_dict)[1].payload.size() == sizeof(corpus::Order));
    }
}

TEST_CASE("FR-S4 view<T>() gives a zero-copy pointer when aligned and misaligned otherwise",
          "[readers][view]")
{
    const auto buf = frame_of("evt"_lit, corpus::sample_order());
    const auto fv = inspect(buf);
    REQUIRE(fv.has_value());
    REQUIRE(fv->size() == 2);
    const auto recs = all_records(*fv);
    SECTION("aligned payload")
    {
        REQUIRE(reinterpret_cast<std::uintptr_t>(recs[1].payload.data()) % alignof(corpus::Order) ==
                0);
        const auto p = view<corpus::Order>(recs[1]);
        REQUIRE(p.has_value());
        CHECK(**p == corpus::sample_order());
        CHECK(static_cast<const void*>(*p) ==
              static_cast<const void*>(recs[1].payload.data())); // no copy
    }
    SECTION("type mismatch and literal records")
    {
        const auto wrong = view<corpus::Cancel>(recs[1]);
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error() == error::hash_mismatch);
        const auto lit = view<corpus::Order>(recs[0]);
        REQUIRE_FALSE(lit.has_value());
        CHECK(lit.error() == error::hash_mismatch);
    }
    SECTION("deliberately misaligned copy")
    {
        const misaligned_copy mc{buf};
        const auto fv2 = inspect(mc.bytes);
        REQUIRE(fv2.has_value());
        const auto p = view<corpus::Order>(all_records(*fv2)[1]);
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == error::misaligned);
        // ... whereas the copying readers do not care about alignment
        corpus::Order o{};
        REQUIRE(deserialize(mc.bytes, "evt"_lit, o));
        CHECK(o == corpus::sample_order());
        // 1-byte-aligned types can always be viewed
        const auto buf1 = frame_of(corpus::Side::buy);
        const misaligned_copy mc1{buf1};
        const auto fv1 = inspect(mc1.bytes);
        REQUIRE(fv1.has_value());
        const auto side = view<corpus::Side>(all_records(*fv1)[0]);
        REQUIRE(side.has_value());
        CHECK(**side == corpus::Side::buy);
    }
}

TEST_CASE("FR-S4 dispatch<Ts...>() routes each record to the matching overload",
          "[readers][dispatch]")
{
    int orders = 0, cancels = 0, literals = 0, unmatched = 0;
    std::string last_literal;
    std::uint64_t unmatched_hash = 0;
    const auto handler = overloaded{[&](const corpus::Order& o) {
                                        ++orders;
                                        CHECK(o == corpus::sample_order());
                                    },
                                    [&](const corpus::Cancel& c) {
                                        ++cancels;
                                        CHECK(c == corpus::sample_cancel());
                                    },
                                    [&](const literal_view l) {
                                        ++literals;
                                        last_literal = std::string(l.text);
                                    },
                                    [&](const record_view& r) {
                                        ++unmatched;
                                        unmatched_hash = r.hash;
                                    }};

    SECTION("all records matched")
    {
        const auto buf =
            frame_of("evt"_lit, corpus::sample_order(), corpus::sample_cancel(), "done"_lit);
        const auto r = dispatch<corpus::Order, corpus::Cancel>(buf, handler);
        REQUIRE(r.has_value());
        CHECK(*r == buf.size());
        CHECK(orders == 1);
        CHECK(cancels == 1);
        CHECK(literals == 2);
        CHECK(last_literal == "done");
        CHECK(unmatched == 0);
    }
    SECTION("a record matching none of Ts... arrives as record_view")
    {
        const auto buf = frame_of(corpus::sample_order(), corpus::Padded{1, 2, 3});
        REQUIRE(dispatch<corpus::Order, corpus::Cancel>(buf, handler));
        CHECK(orders == 1);
        CHECK(unmatched == 1);
        CHECK(unmatched_hash == schema_hash_v<corpus::Padded>);
    }
    SECTION("handlers that do not accept a category simply skip it")
    {
        const auto buf = frame_of("evt"_lit, corpus::sample_cancel());
        int n = 0;
        const auto r = dispatch<corpus::Cancel>(buf, [&](const corpus::Cancel&) { ++n; });
        REQUIRE(r.has_value());
        CHECK(n == 1);
    }
    SECTION("as_flat types are reconstructed before the call")
    {
        const auto buf = frame_of(corpus::Stamp{std::chrono::nanoseconds{42}});
        std::int64_t seen = 0;
        REQUIRE(dispatch<corpus::Stamp>(buf, [&](const corpus::Stamp& s) { seen = s.t.count(); }));
        CHECK(seen == 42);
    }
    SECTION("the first matching T wins")
    {
        // Stamp and StampFlat share a schema hash; whichever is listed first receives the record.
        const auto buf = frame_of(corpus::StampFlat{7});
        bool flat = false, stamp = false;
        REQUIRE(dispatch<corpus::StampFlat, corpus::Stamp>(
            buf, overloaded{[&](const corpus::StampFlat&) { flat = true; },
                            [&](const corpus::Stamp&) { stamp = true; }}));
        CHECK(flat);
        CHECK_FALSE(stamp);
    }
    SECTION("zero-copy when aligned: the reference points into the frame")
    {
        const auto buf = frame_of(corpus::sample_order());
        const void* seen = nullptr;
        REQUIRE(dispatch<corpus::Order>(buf, [&](const corpus::Order& o) { seen = &o; }));
        CHECK(seen == buf.data() + 16);
        const misaligned_copy mc{buf};
        seen = nullptr;
        bool equal = false;
        REQUIRE(dispatch<corpus::Order>(mc.bytes, [&](const corpus::Order& o) {
            seen = &o;
            equal = o == corpus::sample_order();
        }));
        CHECK(equal);
        CHECK(seen != mc.bytes.data() + 16); // stack copy
    }
    SECTION("errors propagate")
    {
        const auto buf = frame_of(corpus::sample_order());
        const auto r = dispatch<corpus::Order>(std::span<const std::byte>(buf).first(12), handler);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == error::truncated);
    }
}

// ---- gk 1.1: a full-frame record shorter than sizeof(T) must not be delivered -------------------------------
namespace wide
{
// Twelve distinct flat types (one canonical name, hence one schema hash, per N) for the hash-index path.
template<int N> struct tick
{
    std::int32_t v;
};
} // namespace wide

TEST_CASE("2.1.9 dispatch over more than four types uses the compile-time hash index",
          "[readers][dispatch]")
{
    using index = detail::hash_index<
        schema_hash_v<wide::tick<0>>, schema_hash_v<wide::tick<1>>, schema_hash_v<wide::tick<2>>,
        schema_hash_v<wide::tick<3>>, schema_hash_v<wide::tick<4>>, schema_hash_v<wide::tick<5>>,
        schema_hash_v<wide::tick<6>>, schema_hash_v<wide::tick<7>>, schema_hash_v<wide::tick<8>>,
        schema_hash_v<wide::tick<9>>, schema_hash_v<wide::tick<10>>, schema_hash_v<wide::tick<11>>>;
    STATIC_REQUIRE(index::lay.perfect);
    STATIC_REQUIRE(index::find(schema_hash_v<wide::tick<7>>) == 7);
    STATIC_REQUIRE(index::find(schema_hash_v<corpus::Order>) == -1);
    // a key listed twice keeps its first position
    STATIC_REQUIRE(detail::hash_index<11, 22, 11, 33, 44, 55>::find(11) == 0);
    STATIC_REQUIRE(detail::hash_index<11, 22, 11, 33, 44, 55>::find(55) == 5);

    std::vector<int> seen;
    int unmatched = 0;
    const auto on_tick = [&]<int N>(const wide::tick<N>& t) {
        CHECK(t.v == 100 + N);
        seen.push_back(N);
    };
    const auto handler = overloaded{on_tick, [&](const record_view&) { ++unmatched; }};
    const auto buf = frame_of(wide::tick<11>{111}, wide::tick<0>{100}, wide::tick<5>{105},
                              corpus::sample_cancel(), wide::tick<5>{105}, wide::tick<9>{109});
    const auto r =
        dispatch<wide::tick<0>, wide::tick<1>, wide::tick<2>, wide::tick<3>, wide::tick<4>,
                 wide::tick<5>, wide::tick<6>, wide::tick<7>, wide::tick<8>, wide::tick<9>,
                 wide::tick<10>, wide::tick<11>>(buf, handler);
    REQUIRE(r.has_value());
    CHECK(seen == std::vector<int>{11, 0, 5, 5, 9});
    CHECK(unmatched == 1); // corpus::Cancel is not in Ts
}

TEST_CASE("gk 1.1 dispatch rejects short full records", "[readers][dispatch][gk1.1]")
{
    std::vector<std::byte> two;
    vector_sink out{two};
    REQUIRE(serialize_with(serialize_options{.full_records = true}, out, "evt"_lit,
                           corpus::sample_order()));
    // layout: 16 header | lit: 16 rec hdr + 8 | Order: 16 rec hdr + 32   → 88 bytes
    REQUIRE(two.size() == 88);
    const std::size_t order_hdr = 16 + 24;
    std::uint32_t len = 0;
    std::memcpy(&len, two.data() + order_hdr + 8, 4);
    REQUIRE(len == sizeof(corpus::Order));
    const std::uint32_t shorter =
        24; // 8-aligned so the frame stays structurally valid, but < sizeof(Order);
    std::memcpy(two.data() + order_hdr + 8, &shorter,
                4); // frame_len stays 88: the last 8 bytes are unclaimed slack

    const auto fv = inspect(two);
    REQUIRE(fv.has_value());
    REQUIRE(fv->size() == 2);
    CHECK(all_records(*fv)[1].payload.size() == shorter);

    int called = 0, literals = 0;
    const auto r = dispatch<corpus::Order>(two, overloaded{[&](const corpus::Order&) { ++called; },
                                                           [&](literal_view) { ++literals; }});
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::length_mismatch);
    CHECK(called == 0);   // f is never called with an under-sized object
    CHECK(literals == 1); // the walk continues past the bad record
    // view<T>() agrees
    const auto v = view<corpus::Order>(all_records(*fv)[1]);
    REQUIRE_FALSE(v.has_value());
    CHECK(v.error() == error::length_mismatch);
    // ... and so does the typed reader
    corpus::Order o{};
    const auto d = deserialize(two, "evt"_lit, o);
    REQUIRE_FALSE(d.has_value());
    CHECK(d.error() == error::length_mismatch);
}

// ---- gk 1.2 / cc F7 / cc S3: no 32-record cap ----------------------------------------------------------------
namespace
{

template<std::size_t... I> std::vector<std::byte> many_frame(std::index_sequence<I...>)
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    REQUIRE(serialize(
        out, corpus::Point2i{static_cast<std::int32_t>(I), static_cast<std::int32_t>(I * 2)}...));
    return buf;
}
template<std::size_t... I>
void many_roundtrip(const std::span<const std::byte> buf, std::index_sequence<I...>)
{
    std::array<corpus::Point2i, sizeof...(I)> outs{};
    const auto r = deserialize(buf, outs[I]...);
    REQUIRE(r.has_value());
    CHECK(*r == buf.size());
    for (std::size_t i = 0; i < sizeof...(I); ++i)
    {
        CHECK(outs[i].x == static_cast<std::int32_t>(i));
        CHECK(outs[i].y == static_cast<std::int32_t>(i * 2));
    }
}

template<std::size_t N> void check_many()
{
    const auto buf = many_frame(std::make_index_sequence<N>{});
    REQUIRE(buf.size() == 16 + 8 * N);
    const auto fv = inspect(buf);
    REQUIRE(fv.has_value());
    REQUIRE(fv->size() == N);
    std::size_t i = 0;
    for (const record_view r : fv->records())
    {
        REQUIRE(r.desc == &descriptor_v<corpus::Point2i>);
        std::int32_t x = -1;
        std::memcpy(&x, r.payload.data(), 4);
        CHECK(x == static_cast<std::int32_t>(i));
        ++i;
    }
    CHECK(i == N);
    std::size_t seen = 0;
    const auto d = dispatch<corpus::Point2i>(buf, [&](const corpus::Point2i& p) {
        CHECK(p.x == static_cast<std::int32_t>(seen));
        ++seen;
    });
    REQUIRE(d.has_value());
    CHECK(seen == N);
    many_roundtrip(buf, std::make_index_sequence<N>{});
}

} // namespace

TEST_CASE("gk 1.2 41-record frame through serialize → inspect → dispatch → deserialize",
          "[readers][large]")
{
    check_many<41>();
}
TEST_CASE("gk 1.2 300-record frame through serialize → inspect → dispatch → deserialize",
          "[readers][large]")
{
    check_many<300>();
}

// ---- 2.1.10: frame_router and the file-path walks ----------------------------------------------------------------
TEST_CASE("2.1.10 frame_router routes each frame to the handler of its call signature",
          "[readers][router]")
{
    std::vector<std::byte> stream;
    vector_sink out{stream};
    REQUIRE(serialize(out, corpus::sample_order()));              // on(h)
    REQUIRE(serialize(out, "evt"_lit, corpus::sample_order()));   // on(lit, h)
    REQUIRE(serialize(out, corpus::sample_cancel(), "why"_lit));  // on<Sig...>(h), literal last
    REQUIRE(println(out, "px={:.2f} qty={}"_, 2.5, 7u));          // on_println(lit, h)
    REQUIRE(serialize(out, corpus::DynMsg{7, "ab", {1, -2, 3}})); // a dynamic type
    REQUIRE(serialize(out, corpus::Padded{1, 2, 3}));             // no handler → otherwise
    REQUIRE(serialize(out, corpus::DynMsg{8, "cd", {}}));         // the same signature again

    int plain = 0, tagged = 0, cancels = 0, prints = 0, others = 0;
    std::vector<corpus::DynMsg> msgs;
    frame_router router;
    router
        .on([&](const corpus::Order& o) {
            ++plain;
            CHECK(o == corpus::sample_order());
        })
        .on("evt"_lit,
            [&](const corpus::Order& o) {
                ++tagged;
                CHECK(o == corpus::sample_order());
            })
        .on<corpus::Cancel, decltype("why"_lit)>([&](const corpus::Cancel& c) {
            ++cancels;
            CHECK(c == corpus::sample_cancel());
        })
        .on_println("px={:.2f} qty={}"_,
                    [&](const double px, const unsigned qty) {
                        ++prints;
                        CHECK(px == 2.5);
                        CHECK(qty == 7u);
                    })
        .on([&](const corpus::DynMsg& m) { msgs.push_back(m); })
        .otherwise([&](const dyn::dictionary&, const tools::frame_ref& f) {
            ++others;
            CHECK(f.call_hash == call_hash_v<corpus::Padded>);
        });
    CHECK(router.size() == 5);

    const auto walk = tools::for_each_frame(std::span<const std::byte>(stream), router.callback());
    REQUIRE(walk.has_value());
    CHECK(walk->frames == 7);
    CHECK(plain == 1);
    CHECK(tagged == 1);
    CHECK(cancels == 1);
    CHECK(prints == 1);
    CHECK(others == 1);
    REQUIRE(msgs.size() ==
            2); // the decoded values' storage is reused between frames, the handler sees each one
    CHECK(msgs[0] == corpus::DynMsg{7, "ab", {1, -2, 3}});
    CHECK(msgs[1] == corpus::DynMsg{8, "cd", {}});
    CHECK(router.handled() == 6);
    CHECK(router.unhandled() == 1);
    CHECK(router.failed() == 0);

    SECTION("route() on raw frames; a frame that fails to decode is an error, not a handler call")
    {
        const auto first = read_frame_header(stream);
        REQUIRE(first);
        const std::span<const std::byte> order_frame(stream.data(), first->frame_len);
        CHECK(router.route(order_frame).value());
        CHECK(plain == 2);
        std::vector<std::byte> dyn_frame;
        vector_sink d{dyn_frame};
        REQUIRE(serialize(d, corpus::DynMsg{9, "x", {5}}));
        dyn_frame[fmt::frame_header_size] =
            std::byte{0xff}; // the image length no longer matches the record
        const auto r = router.route(dyn_frame);
        REQUIRE_FALSE(r.has_value());
        CHECK(msgs.size() == 2);
        std::vector<std::byte> other;
        vector_sink o{other};
        REQUIRE(serialize(o, corpus::Padded{4, 5, 6}));
        CHECK_FALSE(router.route(other).value()); // no handler: false, no error
    }
    SECTION("on_error sees the failed frame when walking")
    {
        std::vector<std::byte> bad;
        vector_sink b{bad};
        REQUIRE(serialize(b, corpus::DynMsg{9, "x", {5}}));
        bad[fmt::frame_header_size] = std::byte{0xff};
        int errors = 0;
        router.on_error([&](const tools::frame_ref& f, const error e) {
            ++errors;
            CHECK(f.index == 0);
            CHECK(e != error{});
        });
        REQUIRE(tools::for_each_frame(std::span<const std::byte>(bad), router.callback()));
        CHECK(errors == 1);
        CHECK(router.failed() == 1);
    }
}

TEST_CASE("2.1.10 for_each_frame(path) and walk_values(path): mapped and in blocks agree",
          "[readers][file]")
{
    const std::string path = std::string(ES_SERREG_TEST_TMPDIR) + "/router_paths.ser";
    {
        capture_file f(path);
        REQUIRE(f.is_open());
        for (int i = 0; i < 50; ++i)
        {
            REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
            REQUIRE(serialize(f, corpus::DynMsg{static_cast<std::uint32_t>(i), "m", {1, 2}}));
        }
        REQUIRE(f.close());
    }
    std::vector<std::size_t> mapped, blocks;
    const auto collect = [](std::vector<std::size_t>& to) {
        return [&to](const dyn::dictionary&, const tools::frame_ref& f) {
            to.push_back(f.offset);
            return true;
        };
    };
    const auto a = tools::for_each_frame(path, collect(mapped));
    const auto b =
        tools::for_each_frame(path, collect(blocks), {.blocks = true, .block_bytes = 256});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->frames == 100);
    CHECK(mapped == blocks);
    CHECK(
        tools::for_each_frame(std::string(ES_SERREG_TEST_TMPDIR) + "/no-such.ser", collect(mapped))
            .error() == error::not_found);

    struct counter final : tools::decoded_value_visitor
    {
        std::uint64_t n = 0;
        void begin_struct(std::string_view, std::string_view) override
        {
        }
        void end_struct() override
        {
        }
        void begin_array(std::string_view, std::uint32_t) override
        {
        }
        void end_array() override
        {
        }
        void scalar(std::string_view, kind, std::span<const std::byte>) override
        {
            ++n;
        }
        void enumerator(std::string_view, std::string_view, std::string_view, std::int64_t) override
        {
            ++n;
        }
        void literal(std::string_view, std::uint64_t, std::string_view) override
        {
            ++n;
        }
    };
    counter c1, c2, c3;
    REQUIRE(tools::walk_values(path, c1));
    REQUIRE(tools::walk_values(path, c2, {}, {.blocks = true}));
    const auto bytes = tools::read_file(path);
    REQUIRE(bytes.has_value());
    REQUIRE(tools::walk_values(std::span<const std::byte>(*bytes), c3));
    CHECK(c1.n > 0);
    CHECK(c1.n == c2.n);
    CHECK(c1.n == c3.n);
    int typed = 0;
    REQUIRE(tools::walk_values<corpus::DynMsg>(
        path, [&](std::string_view, const corpus::DynMsg&) { ++typed; }, c1));
    CHECK(typed == 50);
}

// ---- cc review 2.1.10, T-10 (2.1.12) --------------------------------------------------------------------------
static_assert(!std::is_copy_constructible_v<frame_router> &&
                  !std::is_move_constructible_v<frame_router>,
              "callback() points at the router, so it stays where it was built");

TEST_CASE("2.1.12 T-10 frame_router::route counts like the walk callback and reports decode errors",
          "[readers][router]")
{
    std::vector<std::byte> order, padded;
    vector_sink o1{order}, o2{padded};
    REQUIRE(serialize(o1, corpus::sample_order()));
    REQUIRE(serialize(o2, corpus::Padded{1, 2, 3}));
    int handled = 0, errors = 0;
    frame_router router;
    router.on([&](const corpus::Order&) { ++handled; })
        .on_error([&](const tools::frame_ref& f, const error) {
            ++errors;
            CHECK(f.call_hash == call_hash_v<corpus::Order>);
        });
    CHECK(router.route(order).value() == true);
    CHECK(router.route(padded).value() == false); // no handler: counted, nothing called
    std::vector<std::byte> bad = order;
    bad.resize(bad.size() - 8); // the record is cut short
    const std::uint32_t len = static_cast<std::uint32_t>(bad.size());
    std::memcpy(bad.data(), &len, 4);
    CHECK_FALSE(router.route(bad));
    CHECK(handled == 1);
    CHECK(errors == 1);
    CHECK(router.handled() == 1);
    CHECK(router.unhandled() == 1);
    CHECK(router.failed() == 1);
}
