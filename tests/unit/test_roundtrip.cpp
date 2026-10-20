// tests/unit/test_roundtrip.cpp — serialize → deserialize (spec §10.2 #4): FR-S1 typed round-trip, FR-S3 frame
// size, FR-S5 raw payloads, FR-S6 padding scrub determinism, FR-S8 as_flat, FR-S10 skip, FR-W2 full records,
// and the error paths sink_full / hash_mismatch / truncated.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace
{

template<class... Ts> std::vector<std::byte> frame_of(const Ts&... vs)
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const auto r = serialize(out, vs...);
    REQUIRE(r.has_value());
    REQUIRE(*r == buf.size());
    return buf;
}

// A write-only (non-claiming) sink: exercises the stack-buffer path of serialize()/serialize_raw().
struct plain_sink
{
    std::vector<std::byte>& v;
    bool write(const std::byte* const p, const std::size_t n)
    {
        v.insert(v.end(), p, p + n);
        return true;
    }
};

template<class... Fs> struct overloaded : Fs...
{
    using Fs::operator()...;
};

// Builds a T whose padding bytes hold `fill` before the fields are assigned.
template<class T, class Init> T with_padding(const int fill, Init&& init)
{
    T v;
    std::memset(&v, fill, sizeof v);
    init(v);
    return v;
}

} // namespace
ES_SERREG_TEST_PLAIN_SINK(plain_sink, false); // KNOWN-BUG workaround, see test_support.hpp

TEST_CASE("FR-S1 typed round-trip of every flat shape in the corpus", "[roundtrip]")
{
    SECTION("scalars")
    {
        const corpus::Scalars in{-5, 2.5, 7u, -9};
        corpus::Scalars out{};
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
    }
    SECTION("padded")
    {
        const corpus::Padded in{0xAB, 0x1122334455667788ull, 0xCDEF};
        corpus::Padded out{};
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
    }
    SECTION("nested")
    {
        const corpus::Nested in{{1, 2.0, 3u, 4}, {5, 6, 7}, 8};
        corpus::Nested out{};
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
    }
    SECTION("bases")
    {
        corpus::Derived d{};
        d.id = 11;
        d.x = -12;
        corpus::MultiDerived m{};
        m.id = 21;
        m.t = 22;
        m.k = 23;
        corpus::Derived d2{};
        corpus::MultiDerived m2{};
        REQUIRE(deserialize(frame_of(d, m), d2, m2));
        CHECK(d2 == d);
        CHECK(m2 == m);
    }
    SECTION("enums and arrays")
    {
        corpus::Arrays a{{1, 2, 3, 4}, {0.5, 1.5}, "SYMBOL", {{1, 2, 3}, {4, 5, 6}}};
        corpus::Arrays a2{};
        corpus::Side s = corpus::Side::sell, s2{};
        corpus::Venue v = corpus::arca, v2{};
        REQUIRE(deserialize(frame_of(a, s, v), a2, s2, v2));
        CHECK(a2 == a);
        CHECK(s2 == s);
        CHECK(v2 == v);
    }
    SECTION("private members (FR-S9), template specialization, annotated type")
    {
        const corpus::Account acct{77, -1000};
        const corpus::Vec3 vec{{corpus::Side::buy, corpus::Side::sell, corpus::Side::buy}};
        const corpus::Versioned ver{1, 2, 3};
        corpus::Account acct2;
        corpus::Vec3 vec2{};
        corpus::Versioned ver2{};
        REQUIRE(deserialize(frame_of(acct, vec, ver), acct2, vec2, ver2));
        CHECK(acct2 == acct);
        CHECK(vec2 == vec);
        CHECK((ver2.id == 1 && ver2.venue == 2 && ver2.legacy == 3));
    }
}

TEST_CASE("FR-L1 literals mixed with objects round-trip and are id-checked", "[roundtrip][literal]")
{
    const corpus::Order in = corpus::sample_order();
    const auto buf = frame_of("evt"_lit, in, "x"_lit);
    CHECK(buf.size() == frame_size<decltype("evt"_lit), corpus::Order, decltype("x"_lit)>());
    corpus::Order out{};
    const auto r = deserialize(buf, "evt"_lit, out, "x"_lit);
    REQUIRE(r.has_value());
    CHECK(*r == buf.size());
    CHECK(out == in);
    // Same arity and shape but a different literal → distinct call signature → hash_mismatch (FR-L4).
    corpus::Order out2{};
    const auto wrong = deserialize(buf, "evt"_lit, out2, "y"_lit);
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error() == error::hash_mismatch);
}

TEST_CASE("FR-L1 the four literal spellings are one literal (v1.0.3)", "[roundtrip][literal]")
{
    constexpr lit<"order rejected"> named; // no UDL, no macro
    const corpus::Order in = corpus::sample_order();
    const auto a = frame_of("order rejected"_lit, in);
    const auto b = frame_of("order rejected"_, in); // the short UDL
    const auto c = frame_of(named, in);
    const auto d = frame_of(ES_SERREG_LIT("order rejected"), in);
    std::vector<std::byte> e;
    vector_sink es{e};
    REQUIRE(serialize<"order rejected">(es, in).has_value()); // literal as template argument
    CHECK(a == b);
    CHECK(a == c);
    CHECK(a == d);
    CHECK(a == e);
    STATIC_REQUIRE(std::is_same_v<decltype("order rejected"_lit), decltype("order rejected"_)>);
    STATIC_REQUIRE(call_hash_v<decltype(named), corpus::Order> ==
                   call_hash_v<decltype("order rejected"_lit), corpus::Order>);
    corpus::Order out{};
    REQUIRE(deserialize(e, named, out).has_value());
    CHECK(out == in);
}

TEST_CASE("FR-L2 literal text lives inside the ENTRY blob, once (v1.0.3 footprint)", "[literal]")
{
    using L = lit<"footprint probe">;
    static_cast<void>(frame_of(L{}, corpus::sample_order()));
    // lit<S>::text() views the COMDAT blob at the documented offset — no separate static copy of the text.
    const auto blob = literal_blob_v<L::str>;
    CHECK(L::text() == "footprint probe");
    CHECK(static_cast<const void*>(L::text().data()) ==
          static_cast<const void*>(blob.data() + fmt::literal_text_offset));
    std::uint32_t len = 0;
    std::memcpy(&len, blob.data() + fmt::literal_len_offset, sizeof len);
    CHECK(len == L::size);
    // The registry's name for the literal is the text, taken from the blob at push() time.
    const auto& reg = registry::instance();
    CHECK(reg.find_literal(L::id) == "footprint probe");
    const auto* e = reg.snapshot().find(node_kind::literal, L::id);
    REQUIRE(e != nullptr);
    CHECK(e->name == "footprint probe");
    CHECK(static_cast<const void*>(e->name.data()) ==
          static_cast<const void*>(e->blob.data() +
                                   fmt::literal_text_offset)); // views its owned copy
}

TEST_CASE("FR-S3 frame_size<>() equals the bytes written", "[roundtrip]")
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const auto r1 = serialize(out, "evt"_lit, corpus::sample_order());
    REQUIRE(r1.has_value());
    CHECK(*r1 == frame_size<decltype("evt"_lit), corpus::Order>());
    const auto r2 = serialize(out, corpus::sample_cancel(), corpus::Side::buy, corpus::Padded{});
    REQUIRE(r2.has_value());
    CHECK(*r2 == frame_size<corpus::Cancel, corpus::Side, corpus::Padded>());
    CHECK(buf.size() == *r1 + *r2);
    const auto r3 = serialize_with(serialize_options{.full_records = true}, out, "evt"_lit,
                                   corpus::sample_order());
    REQUIRE(r3.has_value());
    CHECK(*r3 ==
          frame_size<decltype("evt"_lit), corpus::Order>(serialize_options{.full_records = true}));
}

TEST_CASE("FR-S6 padding scrub is deterministic", "[roundtrip][determinism]")
{
    const auto init = [](corpus::Padded& p) {
        p.tag = 1;
        p.value = 2;
        p.code = 3;
    };
    const corpus::Padded a = with_padding<corpus::Padded>(0xFF, init);
    const corpus::Padded b = with_padding<corpus::Padded>(0x00, init);
    REQUIRE(a == b); // logically equal ...
    SECTION("default options: identical bytes whatever the padding held")
    {
        const auto fa = frame_of(a), fb = frame_of(b);
        REQUIRE(fa.size() == fb.size());
        CHECK(std::memcmp(fa.data(), fb.data(), fa.size()) == 0);
        // and the gap bytes are zero in the payload (frame header is 16 bytes; gaps at 1+7 and 18+6)
        for (const gap g : descriptor_v<corpus::Padded>.padding_gaps)
            for (std::uint32_t i = 0; i < g.len; ++i)
                CHECK(fa[16 + g.offset + i] == std::byte{0});
    }
    SECTION("scrub_padding = false: the garbage leaks through")
    {
        std::vector<std::byte> va, vb;
        vector_sink sa{va}, sb{vb};
        REQUIRE(serialize_with(serialize_options{.scrub_padding = false}, sa, a));
        REQUIRE(serialize_with(serialize_options{.scrub_padding = false}, sb, b));
        REQUIRE(va.size() == vb.size());
        CHECK(std::memcmp(va.data(), vb.data(), va.size()) != 0);
    }
    SECTION("nested gaps (flattened table) are scrubbed too")
    {
        const auto n1 = with_padding<corpus::Nested>(0xAA, [](corpus::Nested& n) {
            n.head = {1, 2, 3, 4};
            n.body = {5, 6, 7};
            n.n = 8;
        });
        const auto n2 = with_padding<corpus::Nested>(0x55, [](corpus::Nested& n) {
            n.head = {1, 2, 3, 4};
            n.body = {5, 6, 7};
            n.n = 8;
        });
        const auto f1 = frame_of(n1), f2 = frame_of(n2);
        CHECK(std::memcmp(f1.data(), f2.data(), f1.size()) == 0);
    }
}

TEST_CASE("FR-W2 full-records mode round-trips and is validated per record", "[roundtrip][full]")
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const corpus::Order in = corpus::sample_order();
    const corpus::Cancel c = corpus::sample_cancel();
    const auto w = serialize_with(serialize_options{.full_records = true}, out, "evt"_lit, in, c);
    REQUIRE(w.has_value());
    const auto h = read_frame_header(buf);
    REQUIRE(h.has_value());
    CHECK(h->full());
    CHECK(h->n_records == 3);
    corpus::Order o2{};
    corpus::Cancel c2{};
    REQUIRE(deserialize(buf, "evt"_lit, o2, c2));
    CHECK(o2 == in);
    CHECK(c2 == c);
    // Full records carry their own schema hash: asking for a different (same-size) type at a record → hash_mismatch.
    static_assert(sizeof(corpus::Account) == sizeof(corpus::Cancel));
    corpus::Account p{};
    const auto bad = deserialize(buf, "evt"_lit, o2, p);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == error::hash_mismatch);
}

TEST_CASE("FR-S2 span_sink exhaustion reports sink_full and writes nothing", "[roundtrip][sink]")
{
    std::byte small[24]{};
    span_sink s{small};
    const auto r = serialize(s, corpus::sample_order()); // needs 48 bytes
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::sink_full);
    CHECK(s.used == 0);
    // exact fit succeeds
    std::byte exact[frame_size<corpus::Order>()];
    span_sink e{exact};
    REQUIRE(serialize(e, corpus::sample_order()));
    CHECK(e.used == sizeof exact);
    corpus::Order back{};
    REQUIRE(deserialize(e.bytes(), back));
    CHECK(back == corpus::sample_order());
}

TEST_CASE("FR-S4 deserialize with the wrong signature fails before touching outputs",
          "[roundtrip][errors]")
{
    const auto buf = frame_of(corpus::sample_order());
    corpus::Cancel c{99, 99};
    const auto r = deserialize(buf, c);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::hash_mismatch);
    CHECK((c.id == 99 && c.reason == 99));
    // 2.1.15 (decision C): expected = the frame + one argument; the frame itself was refused
    CHECK(r.error().expected == 2);
    CHECK(r.error().actual == 0);
    CHECK(r.error().failed == 0);
    // arity mismatch on a compact frame
    corpus::Order o{};
    corpus::Order o2{};
    const auto r2 = deserialize(buf, o, o2);
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error() == error::length_mismatch);
    CHECK(r2.error() == decode_error{error::length_mismatch, 3, 0, 0});
}

TEST_CASE(
    "FR-S4 a failure reports expected items, decoded items and the first that failed (2.1.15)",
    "[roundtrip][errors][decode_error]")
{
    // decode_error is an error code with three counters: it converts to and compares with `error`
    static_assert(std::is_same_v<decltype(deserialize(std::span<const std::byte>{},
                                                      std::declval<corpus::Order&>())),
                                 std::expected<std::size_t, decode_error>>);
    static_assert(std::is_convertible_v<decode_error, error>);
    constexpr decode_error de{error::truncated, 3, 1, 2};
    static_assert(de == error::truncated && to_string(de) == "truncated");
    const auto forward = [](const std::span<const std::byte> b) -> result<std::size_t> {
        corpus::Order o{};
        const auto r = deserialize(b, o);
        if (!r)
            return std::unexpected(r.error()); // a result<T> takes the code
        return *r;
    };
    CHECK(forward({}).error() == error::truncated);
    {
        corpus::Order o{};
        const auto r = deserialize(std::span<const std::byte>{}, "evt"_, o);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == decode_error{error::truncated, 3, 0, 0}); // no frame header at all
    }
    // success: the bytes consumed, as before
    const corpus::Order in = corpus::sample_order();
    const auto buf = frame_of("evt"_, in, std::string_view{"tail text"}, 7);
    {
        corpus::Order o{};
        std::string s;
        int n = 0;
        const auto r = deserialize(buf, "evt"_, o, s, n);
        REQUIRE(r.has_value());
        CHECK(*r == buf.size());
    }
    // compact: the frame is accepted, the string record (argument 3) runs past frame_len → nothing is written
    {
        auto cut = buf;
        const std::size_t str_at =
            16 + sizeof(corpus::Order); // literal: 0 bytes; Order; then u32 len
        REQUIRE(str_at % 8 == 0);
        const std::uint32_t huge = 4000;
        std::memcpy(cut.data() + str_at, &huge, 4);
        corpus::Order o{};
        std::string s = "old";
        int n = -1;
        const auto r = deserialize(cut, "evt"_, o, s, n);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == decode_error{error::truncated, 5, 1, 3});
        CHECK((o == corpus::Order{} && s == "old" && n == -1));
    }
    // full records: the hash of record 2 is not the one asked for → the frame is accepted, argument 2 is named
    {
        std::vector<std::byte> full;
        vector_sink out{full};
        REQUIRE(serialize_with(serialize_options{.full_records = true}, out, "evt"_, in,
                               corpus::sample_cancel())
                    .has_value());
        corpus::Order o{};
        corpus::Account acct{};
        const auto r = deserialize(full, "evt"_, o, acct);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == decode_error{error::hash_mismatch, 4, 1, 3});
        CHECK(o == corpus::Order{}); // validated before anything is written
        // a different literal in a full frame is the first argument failing
        const auto r2 = deserialize(full, "other"_, o, acct);
        REQUIRE_FALSE(r2.has_value());
        CHECK(r2.error() == decode_error{error::hash_mismatch, 4, 1, 1});
    }
    // the print readers report the same way (the format literal is argument 1)
    {
        std::vector<std::byte> p;
        vector_sink out{p};
        REQUIRE(print(out, "n={} s={}"_, 5, "abc").has_value());
        int n = 0;
        std::string s;
        REQUIRE(deserialize_print(p, "n={} s={}"_, n, s).has_value());
        const auto r = deserialize_println(p, "n={} s={}"_, n, s); // println != print
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == decode_error{error::hash_mismatch, 4, 0, 0});
    }
}

// ---- 2.1.15, format 1.5: compile-time constants as arguments ---------------------------------------------
TEST_CASE("FR-A5 constants: no bytes in a compact frame, the value's record in a full one (1.5)",
          "[roundtrip][const]")
{
    const corpus::Order in = corpus::sample_order();
    // compact: the literal and the four constants occupy nothing — the frame is header + Order
    const auto buf = frame_of("fill"_, 42_, in, -7_, 1.5_, k<corpus::Side::sell>);
    CHECK(buf.size() == 16 + fmt::align8(sizeof(corpus::Order)));
    static_assert(frame_size<decltype("fill"_), decltype(42_), corpus::Order>() ==
                  frame_size<corpus::Order>());
    const auto h = read_frame_header(buf);
    REQUIRE(h.has_value());
    CHECK(h->n_records == 6);
    CHECK(h->call_hash == call_hash_v<decltype("fill"_), constant<42>, corpus::Order, constant<-7>,
                                      constant<1.5>, constant<corpus::Side::sell>>);
    {
        corpus::Order o{};
        const auto r = deserialize(buf, "fill"_, 42_, o, -7_, 1.5_, k<corpus::Side::sell>);
        REQUIRE(r.has_value());
        CHECK(*r == buf.size());
        CHECK(o == in);
    }
    { // another value is another signature: refused as a frame, nothing written
        corpus::Order o{};
        const auto r = deserialize(buf, "fill"_, 43_, o, -7_, 1.5_, k<corpus::Side::sell>);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == decode_error{error::hash_mismatch, 7, 0, 0});
        CHECK(o == corpus::Order{});
        // and a run-time int in its place is yet another one
        int n = 0;
        CHECK(deserialize(buf, "fill"_, n, o, -7_, 1.5_, k<corpus::Side::sell>).error() ==
              error::hash_mismatch);
    }
    // inspect: a constant is presented as the record it stands for, flagged is_const, value from the call entry
    {
        const auto fv = inspect(buf);
        REQUIRE(fv.has_value());
        std::vector<record_view> recs;
        for (const record_view r : fv->records())
            recs.push_back(r);
        REQUIRE(recs.size() == 6);
        CHECK(recs[0].is_literal);
        CHECK((recs[1].is_const && recs[1].ak == fmt::arg_scalar && recs[1].k == kind::i32));
        REQUIRE(recs[1].payload.size() == 4);
        std::int32_t i = 0;
        std::memcpy(&i, recs[1].payload.data(), 4);
        CHECK(i == 42);
        CHECK((!recs[2].is_const && recs[2].ak == fmt::arg_type));
        std::memcpy(&i, recs[3].payload.data(), 4);
        CHECK(i == -7);
        REQUIRE((recs[4].is_const && recs[4].k == kind::f64 && recs[4].payload.size() == 8));
        double d = 0;
        std::memcpy(&d, recs[4].payload.data(), 8);
        CHECK(d == 1.5);
        REQUIRE((recs[5].is_const && recs[5].ak == fmt::arg_type && recs[5].payload.size() == 1));
        CHECK(recs[5].hash == schema_hash_v<corpus::Side>);
        CHECK(recs[5].payload[0] == std::byte{2});
    }
    // dispatch delivers constants like the values they stand for
    {
        int ints = 0, sum = 0, orders = 0, sides = 0;
        double dsum = 0;
        const auto r = dispatch<int, double, corpus::Order, corpus::Side>(
            buf,
            overloaded{[&](const int& x) { ++ints, sum += x; }, [&](const double& x) { dsum += x; },
                       [&](const corpus::Order& o) { orders += o == in; },
                       [&](const corpus::Side& s) { sides += s == corpus::Side::sell; },
                       [](const literal_view&) {}, [](const record_view&) {}});
        REQUIRE(r.has_value());
        CHECK((ints == 2 && sum == 35 && dsum == 1.5 && orders == 1 && sides == 1));
    }
    // full records: self-describing — each constant is the scalar / enum record a run-time value would be
    std::vector<std::byte> full;
    {
        vector_sink out{full};
        const serialize_options o{.full_records = true};
        REQUIRE(serialize_with(o, out, "fill"_, 42_, in, k<corpus::Side::sell>).has_value());
        std::vector<std::byte> runtime;
        vector_sink out2{runtime};
        REQUIRE(serialize_with(o, out2, "fill"_, 42, in, corpus::Side::sell).has_value());
        REQUIRE(full.size() == runtime.size());
        // identical but for the call hash in the frame header
        CHECK(std::memcmp(full.data(), runtime.data(), 8) == 0);
        CHECK(std::memcmp(full.data() + 16, runtime.data() + 16, full.size() - 16) == 0);
        corpus::Order got{};
        REQUIRE(deserialize(full, "fill"_, 42_, got, k<corpus::Side::sell>).has_value());
        CHECK(got == in);
        // a full frame is validated per record: the value on the wire must be the one the reader names
        const auto r = deserialize(full, "fill"_, 41_, got, k<corpus::Side::sell>);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == decode_error{error::hash_mismatch, 5, 1, 2});
        // and the same frame reads as run-time values
        int n = 0;
        corpus::Side s{};
        REQUIRE(deserialize(full, "fill"_, n, got, s).has_value());
        CHECK((n == 42 && s == corpus::Side::sell));
    }
    // plain (non-claiming) sinks stream the same bytes
    {
        std::vector<std::byte> a, b;
        plain_sink pa{a}, pb{b};
        REQUIRE(serialize(pa, "fill"_, 42_, in, -7_, 1.5_, k<corpus::Side::sell>).has_value());
        CHECK(a == buf);
        REQUIRE(serialize_with(serialize_options{.full_records = true}, pb, "fill"_, 42_, in,
                               k<corpus::Side::sell>)
                    .has_value());
        CHECK(b == full);
    }
}

TEST_CASE("FR-A5 constants through the dictionary: RRI round trip, verify, dump, JSON, print (1.5)",
          "[roundtrip][const][tools]")
{
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(serialize(s, "limit"_, 5'000'000'000_, k<true>, k<'x'>, k<42u>, 0x10_).has_value());
    const std::size_t f1 = v.size();
    REQUIRE(print(s, "n={} side={} px={:.2f} ok={}"_, 42_, k<corpus::Side::buy>, 2.5_, k<false>)
                .has_value());
    const std::size_t f2 = v.size();
    REQUIRE(serialize_with(serialize_options{.full_records = true}, s, "limit"_, 5'000'000'000_,
                           k<true>, k<'x'>, k<42u>, 0x10_)
                .has_value());
    CHECK(f1 == 16); // a frame of a literal and constants only is its header
    CHECK(f2 - f1 == 16);
    const auto rri = registry::instance().export_image();
    const auto d = tools::load_dictionary_rri(rri);
    REQUIRE(d.has_value());
    const dyn::call* const c =
        d->find_call(call_hash_v<decltype("limit"_), constant<5000000000ll>, constant<true>,
                                 constant<'x'>, constant<42u>, constant<16>>);
    REQUIRE(c != nullptr);
    REQUIRE(c->args.size() == 6);
    CHECK((c->args[1].ak == fmt::arg_const && c->args[1].k == kind::i64 &&
           c->args[1].value == 5000000000ull));
    CHECK((c->args[2].k == kind::boolean && c->args[2].value == 1));
    CHECK((c->args[3].k == kind::char_ && c->args[3].value == 'x'));
    CHECK((c->args[4].k == kind::u32 && c->args[4].value == 42));
    CHECK((c->args[5].k == kind::i32 && c->args[5].value == 16));
    // to_rri(load(rri)) keeps every call entry byte for byte, and JSON/TOML round-trip the value
    const auto again = tools::load_dictionary_rri(dyn::to_rri(*d));
    REQUIRE(again.has_value());
    CHECK(again->calls == d->calls);
    // 2.1.16 (B7): the value is typed, the exact bits stay next to it
    CHECK(d->to_json().find("\"kind\": \"const\", \"scalar\": \"i64\", \"value\": 5000000000, "
                            "\"bits\": \"0x000000012a05f200\"") != std::string::npos);
    CHECK(d->to_json().find("\"scalar\": \"bool\", \"value\": true, \"bits\"") !=
          std::string::npos);
    CHECK(d->to_json().find("\"scalar\": \"char\", \"value\": \"x\", \"bits\"") !=
          std::string::npos);
    CHECK(
        d->to_toml().find("scalar = \"i64\"\nvalue = 5000000000\nbits = \"0x000000012a05f200\"") !=
        std::string::npos);
    // the registry's own view of the call
    const call_desc* const cd = registry::instance().find_call(c->hash);
    REQUIRE(cd != nullptr);
    CHECK((cd->args[1].ak == fmt::arg_const && cd->args[1].value == 5000000000ull));
    const std::span<const std::byte> all = v;
    // verify: the compact and the full frame agree with the dictionary
    for (const auto frame : {all.first(f1), all.subspan(f1, f2 - f1), all.subspan(f2)})
    {
        const auto rep = tools::verify(frame, &*d);
        REQUIRE(rep.has_value());
        CHECK(rep->ok());
    }
    std::string compact_text, full_text;
    REQUIRE(tools::dump(all.first(f1), &*d, {}, compact_text));
    REQUIRE(tools::dump(all.subspan(f2), &*d, {}, full_text));
    INFO(compact_text);
    CHECK(compact_text.find("5000000000") != std::string::npos);
    CHECK(compact_text.find("true") != std::string::npos);
    CHECK(compact_text.find("42") != std::string::npos);
    CHECK(compact_text.find("16") != std::string::npos);
    CHECK(full_text.find("5000000000") != std::string::npos);
    // print rendering takes the constants from the dictionary
    const auto text = tools::render_frame(all.subspan(f1, f2 - f1), *d);
    REQUIRE(text.has_value());
    CHECK(*text == "n=42 side=buy px=2.50 ok=false");
    // a full frame whose constant differs from the call's value is reported by verify
    std::vector<std::byte> bad(all.subspan(f2).begin(), all.subspan(f2).end());
    // records: literal (16 + 8), then the i64 constant: header 16, payload 8
    bad[16 + 24 + 16] ^= std::byte{1};
    const auto rep = tools::verify(bad, &*d);
    REQUIRE(rep.has_value());
    CHECK_FALSE(rep->ok());
    // a dictionary entry whose constant claims bytes above its scalar's size is refused (corrupt_dictionary)
    {
        dyn::dictionary broken = *d;
        broken.calls.at(c->hash).args[4].value = 0x1'0000'002aull; // a u32 with bit 32 set
        CHECK_FALSE(tools::load_dictionary_rri(dyn::to_rri(broken)).has_value());
    }
}

TEST_CASE("FR-D5 truncated buffers are rejected", "[roundtrip][errors]")
{
    const auto buf = frame_of("evt"_lit, corpus::sample_order());
    corpus::Order o{};
    for (const std::size_t cut : {std::size_t{0}, std::size_t{8}, std::size_t{15}, std::size_t{16},
                                  buf.size() - 8, buf.size() - 1})
    {
        const auto r = deserialize(std::span<const std::byte>(buf).first(cut), "evt"_lit, o);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == error::truncated);
    }
}

TEST_CASE("FR-S5 serialize_raw / deserialize_raw carry the payload only", "[roundtrip][raw]")
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const corpus::Order in = corpus::sample_order();
    const auto w = serialize_raw(out, in);
    REQUIRE(w.has_value());
    CHECK(*w == sizeof(corpus::Order));
    CHECK(buf.size() == sizeof(corpus::Order));
    corpus::Order o{};
    const auto r = deserialize_raw(buf, o);
    REQUIRE(r.has_value());
    CHECK(*r == sizeof(corpus::Order));
    CHECK(o == in);
    const auto t = deserialize_raw(std::span<const std::byte>(buf).first(10), o);
    REQUIRE_FALSE(t.has_value());
    CHECK(t.error() == error::truncated);
    // raw with a plain (non-claiming) sink and scrub on: padding zeroed
    std::vector<std::byte> v2;
    plain_sink ps{v2};
    const auto pad = with_padding<corpus::Padded>(0xFF, [](corpus::Padded& p) {
        p.tag = 1;
        p.value = 2;
        p.code = 3;
    });
    REQUIRE(serialize_raw(ps, pad));
    CHECK(v2[1] == std::byte{0});
    CHECK(v2[23] == std::byte{0});
}

TEST_CASE("FR-S8 as_flat maps a type onto its flat image and back", "[roundtrip][as_flat]")
{
    const corpus::Stamp in{std::chrono::nanoseconds{1'234'567'890'123ll}};
    const auto buf = frame_of("stamp"_lit, in);
    CHECK(buf.size() == 16 + 0 + sizeof(corpus::StampFlat)); // 1.1: literal = 0 bytes (FR-L6)
    corpus::Stamp out{};
    REQUIRE(deserialize(buf, "stamp"_lit, out));
    CHECK(out == in);
    // On the wire it *is* a StampFlat: the frame is readable as one, and the registry knows only the flat name.
    corpus::StampFlat flat{};
    REQUIRE(deserialize(buf, "stamp"_lit, flat));
    CHECK(flat.ns == in.t.count());
    CHECK(registry::instance().find_type_by_name("corpus::StampFlat") != nullptr);
    CHECK(registry::instance().find_type_by_name("corpus::Stamp") == nullptr);
}

TEST_CASE("FR-S10 skipped members are zero on the wire and absent from the descriptor",
          "[roundtrip][skip]")
{
    corpus::Skipped in{};
    in.key = 0x0102030405060708ull;
    in.cache = 0xDEADBEEFCAFEBABEull;
    in.n = 7;
    const auto buf = frame_of(in);
    const std::byte* const payload = buf.data() + 16;
    std::uint64_t cache_on_wire = 1;
    std::memcpy(&cache_on_wire, payload + offsetof(corpus::Skipped, cache), 8);
    CHECK(cache_on_wire == 0);
    corpus::Skipped out{};
    out.cache = 0x1111;
    REQUIRE(deserialize(buf, out));
    CHECK(out.key == in.key);
    CHECK(out.n == in.n);
    CHECK(out.cache == 0); // the reader takes the native image; the skipped slot arrives zeroed
    for (const field_desc& f : descriptor_v<corpus::Skipped>.fields)
        CHECK(f.name != "cache");
}

// ---- 2.2.6 (review B4): const and volatile members are rejected (tests/negative); mutable and skipped const pass --
namespace cvns
{
struct WithMutable
{
    std::uint64_t key;
    mutable std::uint32_t hits; // writable through a const object: an ordinary member on the wire
    bool operator==(const WithMutable&) const = default;
};
struct DynWithMutable
{ // a dynamic class: members are written one by one, so a skipped const member is never touched by a reader
    std::string name;
    mutable std::uint32_t hits;
    [[= es::serreg::skip]] const std::uint32_t tag;
};
} // namespace cvns

TEST_CASE(
    "B4 a mutable member serialises like any other; a [[=skip]] const member of a dynamic class "
    "is accepted and left alone",
    "[roundtrip][cv][B4]")
{
    STATIC_REQUIRE(flat_type<cvns::WithMutable>);
    STATIC_REQUIRE(dynamic_type<cvns::DynWithMutable>);
    const cvns::WithMutable in{0x1122334455667788ull, 42};
    const auto buf = frame_of(in);
    cvns::WithMutable out{0, 0};
    REQUIRE(deserialize(buf, out));
    CHECK(out == in);
    bool hits_named = false;
    for (const field_desc& f : descriptor_v<cvns::WithMutable>.fields)
        hits_named = hits_named || f.name == "hits";
    CHECK(hits_named);
    const cvns::DynWithMutable din{"m", 3, 7};
    cvns::DynWithMutable dout{"", 0, 9};
    REQUIRE(deserialize(frame_of(din), dout));
    CHECK((dout.name == din.name && dout.hits == din.hits));
    CHECK(dout.tag == 9);
    for (const field_desc& f : descriptor_v<cvns::DynWithMutable>.fields)
        CHECK(f.name != "tag");
}

// ---- 2.1.1 (T0): G2-1 — typed deserialize is bounded by frame_len, record by record ---------------------------
namespace
{
void set_frame_len(std::vector<std::byte>& f, const std::uint32_t len)
{
    std::memcpy(f.data(), &len, 4);
}
} // namespace

TEST_CASE("G2-1 compact (string, u64) whose frame_len is the minimum: the string may not eat the "
          "next record",
          "[roundtrip][hostile]")
{
    auto f = frame_of(std::string("hello"), std::uint64_t{1});
    const std::uint32_t min = static_cast<std::uint32_t>(frame_size<std::string, std::uint64_t>());
    REQUIRE(min == 32);
    set_frame_len(f, min);
    f.resize(min); // the span is exactly the (lying) frame: a read past it faults (guard page)
    const auto g = test::guarded_copy(f);
    REQUIRE(g.size() == min);
    CHECK(inspect(g).error() == error::truncated);
    std::string s = "untouched";
    std::uint64_t u = 99;
    CHECK(deserialize(g, s, u).error() == error::truncated);
    CHECK(s ==
          "untouched"); // validate-before-write: no output is written for a frame that does not fit
    CHECK(u == 99);
    std::string_view sv;
    CHECK(deserialize(g, sv, u).error() == error::truncated);
}

TEST_CASE("G2-1 a lying frame_len inside a larger capture is refused, not read from the next frame",
          "[roundtrip][hostile]")
{
    auto f = frame_of(std::string("hello"), std::uint64_t{1});
    const auto next = frame_of(std::string("x"), std::uint64_t{0x4141414141414141});
    set_frame_len(f, 32);
    f.resize(32);
    f.insert(f.end(), next.begin(), next.end());
    std::string s;
    std::uint64_t u = 0;
    CHECK(deserialize(f, s, u).error() == error::truncated);
    CHECK(u == 0);
}

TEST_CASE(
    "G2-1 compact (DynMsg, Order): an image length past frame_len is refused before any output",
    "[roundtrip][hostile][dynamic]")
{
    auto f = frame_of(corpus::sample_dyn_msg(), corpus::sample_order());
    const std::uint32_t min =
        static_cast<std::uint32_t>(frame_size<corpus::DynMsg, corpus::Order>());
    REQUIRE(f.size() > min);
    set_frame_len(f, min);
    f.resize(min);
    const auto g = test::guarded_copy(f);
    corpus::DynMsg d{};
    corpus::Order o{};
    CHECK(deserialize(g, d, o).error() == error::truncated);
    CHECK(o.id == 0);
}

TEST_CASE("G2-1 full records: a truncated tail and inconsistent string lengths are refused",
          "[roundtrip][hostile]")
{
    const serialize_options full{.full_records = true};
    std::vector<std::byte> v;
    vector_sink out{v};
    REQUIRE(serialize_with(full, out, std::string("hello"), 7));
    // (a) frame_len cut after the first record: the second record header is not in the frame
    {
        auto f = v;
        const std::uint32_t cut =
            16 + 16 + 16; // frame header + string record (16 header + align8(4+5))
        set_frame_len(f, cut);
        f.resize(cut);
        const auto g = test::guarded_copy(f);
        std::string s;
        int i = 0;
        CHECK(deserialize(g, s, i).error() == error::truncated);
    }
    // (b) the string's own u32 len disagrees with the record's payload_len
    {
        auto f = v;
        const std::uint32_t bad = 200;
        std::memcpy(f.data() + 16 + 16, &bad, 4);
        std::string s;
        int i = 0;
        CHECK(deserialize(f, s, i).error() == error::length_mismatch);
    }
    // the untouched frame still round-trips
    std::string s;
    int i = 0;
    REQUIRE(deserialize(v, s, i));
    CHECK(s == "hello");
    CHECK(i == 7);
}
