// tests/unit/test_print.cpp — v1.1.0: the argument model (FR-A1..A4: top-level scalars and runtime strings),
// compact literal elision (FR-L6), print / println (FR-P1..P6), tools::render_frame,
// deserialize_print, dispatch over string/scalar records, and the 1.0 compatibility corpus.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <string_view>
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

template<class... Fs> struct overloaded : Fs...
{
    using Fs::operator()...;
};

struct plain_sink
{
    std::vector<std::byte>& v;
    bool write(const std::byte* const p, const std::size_t n)
    {
        v.insert(v.end(), p, p + n);
        return true;
    }
};

// A dictionary of this process (what a reader on another box would load from the capture / ELF).
dyn::dictionary process_dictionary()
{
    const auto d = tools::load_dictionary_rri(registry::instance().export_image());
    REQUIRE(d.has_value());
    return *d;
}

} // namespace

// ---- FR-A1/A2: scalars and strings as arguments -------------------------------------------------------------
TEST_CASE("FR-A1 every scalar kind round-trips as a top-level argument", "[print][args]")
{
    const auto buf =
        frame_of(std::int8_t{-1}, std::uint8_t{200}, std::int16_t{-300}, std::uint16_t{60000},
                 std::int32_t{-70000}, std::uint32_t{4000000000u}, std::int64_t{-5},
                 std::uint64_t{1} << 63, 1.5f, -2.25, true, 'x', std::byte{0xAB});
    CHECK(buf.size() == 16 + 13 * 8); // each scalar padded to 8 (FR-A1)
    std::int8_t a;
    std::uint8_t b;
    std::int16_t c;
    std::uint16_t d;
    std::int32_t e;
    std::uint32_t f;
    std::int64_t g;
    std::uint64_t h;
    float i;
    double j;
    bool k;
    char l;
    std::byte m;
    REQUIRE(deserialize(buf, a, b, c, d, e, f, g, h, i, j, k, l, m).has_value());
    CHECK((a == -1 && b == 200 && c == -300 && d == 60000 && e == -70000 && f == 4000000000u &&
           g == -5 && h == (std::uint64_t{1} << 63) && i == 1.5f && j == -2.25 && k && l == 'x' &&
           m == std::byte{0xAB}));
    // padding bytes after a scalar are zero (deterministic frames, FR-S6)
    CHECK(buf[16 + 1] == std::byte{0});
    CHECK(buf[16 + 7] == std::byte{0});
}

TEST_CASE("FR-A2 runtime strings: every spelling in, std::string / string_view out",
          "[print][args]")
{
    const std::string s = "std::string";
    const std::string_view sv = "view";
    const char* p = "pointer";
    char arr[16] = "array\0garbage";
    const auto buf = frame_of(s, sv, p, arr, "literal-text", std::string{});
    // u32 len + bytes, padded to 8, per record: 16+16, 8, 16, 16, 16, 8
    CHECK(buf.size() == 16 + 16 + 8 + 16 + 16 + 16 + 8);
    std::string o1, o2, o3, o4, o5, o6;
    REQUIRE(deserialize(buf, o1, o2, o3, o4, o5, o6).has_value());
    CHECK(o1 == "std::string");
    CHECK(o2 == "view");
    CHECK(o3 == "pointer");
    CHECK(o4 == "array"); // NUL-terminated text, not the whole buffer
    CHECK(o5 == "literal-text");
    CHECK(o6.empty());
    // string_view outputs view the frame
    std::string_view v1, v2, v3, v4, v5, v6;
    REQUIRE(deserialize(buf, v1, v2, v3, v4, v5, v6).has_value());
    CHECK(v1 == "std::string");
    CHECK((v3.data() >= reinterpret_cast<const char*>(buf.data()) &&
           v3.data() < reinterpret_cast<const char*>(buf.data() + buf.size())));
    // the call is the same whatever the spelling: names are categories (plan §1)
    CHECK(read_frame_header(buf)->call_hash ==
          (call_hash_v<std::string, std::string_view, const char*, char[16], const char*,
                       std::string>));
    CHECK(read_frame_header(buf)->call_hash ==
          (call_hash_v<std::string, std::string, std::string, std::string, std::string,
                       std::string>));
    // a nullptr const char* is an empty string
    const char* null = nullptr;
    std::string on;
    REQUIRE(deserialize(frame_of(null), on).has_value());
    CHECK(on.empty());
}

TEST_CASE("FR-A3 frame_size: constexpr without strings, exact with the runtime overload",
          "[print][args]")
{
    STATIC_REQUIRE(frame_size<int, double>() == 16 + 8 + 8);
    STATIC_REQUIRE(has_string_arg_v<int, const char*>);
    STATIC_REQUIRE(!has_string_arg_v<int, corpus::Order>);
    const std::string s(13, 'x');
    CHECK(frame_size(serialize_options{}, 7, s) == 16 + 8 + fmt::align8(4 + 13));
    CHECK(frame_size(serialize_options{.full_records = true}, 7, s) ==
          16 + (16 + 8) + (16 + fmt::align8(4 + 13)));
    const auto buf = frame_of(7, s);
    CHECK(buf.size() == frame_size(serialize_options{}, 7, s));
}

TEST_CASE("FR-A2 strings on plain sinks: small, and past the 4 KiB stack buffer (streamed)",
          "[print][args]")
{
    for (const std::size_t n :
         {std::size_t{0}, std::size_t{5}, std::size_t{4000}, std::size_t{70'000}})
    {
        const std::string big(n, 'q');
        std::vector<std::byte> v;
        plain_sink ps{v};
        const auto r = serialize(ps, "pre"_, big, 42, corpus::sample_order());
        REQUIRE(r.has_value());
        CHECK(*r == v.size());
        std::string out;
        int i{};
        corpus::Order o{};
        REQUIRE(deserialize(v, "pre"_, out, i, o).has_value());
        CHECK(out.size() == n);
        CHECK(i == 42);
        CHECK(o == corpus::sample_order());
        // and in full-records mode
        std::vector<std::byte> vf;
        plain_sink pf{vf};
        REQUIRE(serialize_with(serialize_options{.full_records = true}, pf, "pre"_, big, 42)
                    .has_value());
        std::string outf;
        int j{};
        REQUIRE(deserialize(vf, "pre"_, outf, j).has_value());
        CHECK(outf == big);
        CHECK(j == 42);
        CHECK(tools::verify(vf).has_value()); // structure parses (the call is in the registry)
    }
}

TEST_CASE("FR-A4 inspect/dispatch see string and scalar records", "[print][args]")
{
    const auto buf = frame_of("abc", 42, 1.5, "evt"_, corpus::Side::sell);
    const auto fv = inspect(buf);
    REQUIRE(fv.has_value());
    REQUIRE(fv->size() == 5);
    std::vector<record_view> recs(fv->begin(), fv->end());
    CHECK(recs[0].ak == fmt::arg_string);
    CHECK(recs[0].text() == "abc");
    CHECK(recs[1].ak == fmt::arg_scalar);
    CHECK(recs[1].k == kind::i32);
    CHECK(recs[1].payload.size() == 4);
    CHECK(recs[2].k == kind::f64);
    CHECK(recs[3].is_literal);
    CHECK(recs[3].payload.empty()); // FR-L6
    CHECK(recs[4].ak == fmt::arg_type);
    std::string seen;
    const auto r = dispatch<int, double, corpus::Side>(
        buf,
        overloaded{[&](const std::string_view s) { seen += "s:" + std::string(s) + ";"; },
                   [&](const int i) { seen += "i:" + std::to_string(i) + ";"; },
                   [&](const double d) { seen += "d:" + std::to_string(d).substr(0, 3) + ";"; },
                   [&](const literal_view l) { seen += "l:" + std::string(l.text) + ";"; },
                   [&](const corpus::Side s) {
                       seen += s == corpus::Side::sell ? "side:sell;" : "side:?;";
                   }});
    REQUIRE(r.has_value());
    CHECK(seen == "s:abc;i:42;d:1.5;l:evt;side:sell;");
}

// ---- FR-L6: compact literal records have no bytes ------------------------------------------------------------
TEST_CASE("FR-L6 a compact literal occupies no bytes; full records keep the id", "[print][literal]")
{
    CHECK(frame_of("heartbeat"_).size() == 16);
    CHECK(frame_of("evt"_, corpus::sample_order()).size() == 16 + sizeof(corpus::Order));
    std::vector<std::byte> full;
    vector_sink out{full};
    REQUIRE(serialize_with(serialize_options{.full_records = true}, out, "evt"_, 1).has_value());
    CHECK(full.size() == 16 + (16 + 8) + (16 + 8));
    std::uint64_t id;
    std::memcpy(&id, full.data() + 16 + 16, 8);
    CHECK(id == decltype("evt"_)::id);
    // typed reader on both
    int i{};
    REQUIRE(deserialize(full, "evt"_, i).has_value());
    REQUIRE(deserialize(frame_of("heartbeat"_), "heartbeat"_).has_value());
    CHECK(deserialize(frame_of("heartbeat"_), "other"_).error() == error::hash_mismatch);
}

// ---- FR-P1..P6: print / println -----------------------------------------------------------------------------
TEST_CASE("FR-P1 print frames are plain frames with a print call hash", "[print]")
{
    std::vector<std::byte> a, b, c;
    vector_sink sa{a}, sb{b}, sc{c};
    REQUIRE(print(sa, "hello {} {} {}"_, "new", 100, "worlds"_).has_value());
    REQUIRE(serialize(sb, "hello {} {} {}"_, "new", 100, "worlds"_).has_value());
    REQUIRE(println(sc, "hello {} {} {}"_, "new", 100, "worlds"_).has_value());
    CHECK(a.size() ==
          32); // plan §3: 16 header + string(4+3→8) + i32(8); the two literals cost nothing
    CHECK(a.size() == b.size());
    // same records, different call hashes (call(4) / print(4) / println(4))
    CHECK(std::equal(a.begin() + 16, a.end(), b.begin() + 16));
    CHECK(read_frame_header(a)->call_hash != read_frame_header(b)->call_hash);
    CHECK(read_frame_header(a)->call_hash != read_frame_header(c)->call_hash);
    CHECK(read_frame_header(a)->call_hash ==
          (print_call_hash_v<"hello {} {} {}", const char*, int, decltype("worlds"_)>));
    // template-argument spelling
    std::vector<std::byte> d;
    vector_sink sd{d};
    REQUIRE(print<"hello {} {} {}">(sd, "new", 100, "worlds"_).has_value());
    CHECK(d == a);
    // the registry saw the print calls with their flags
    const call_desc* const cd = registry::instance().find_call(read_frame_header(a)->call_hash);
    REQUIRE(cd != nullptr);
    CHECK(cd->flags == fmt::call_flag_print);
    CHECK(cd->args.size() == 4);
    CHECK(cd->args[1].ak == fmt::arg_string);
    CHECK(cd->args[2].ak == fmt::arg_scalar);
    CHECK(cd->args[2].k == kind::i32);
    const call_desc* const cn = registry::instance().find_call(read_frame_header(c)->call_hash);
    REQUIRE(cn != nullptr);
    CHECK(cn->flags == (fmt::call_flag_print | fmt::call_flag_newline));
}

TEST_CASE("FR-P4 tools::render_frame formats every argument category", "[print][render]")
{
    const dyn::dictionary d0 = process_dictionary(); // may predate this test's registrations
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(print(s, "hello {} {} {}"_, "new", 100, "worlds"_).has_value());
    const std::size_t f1 = v.size();
    REQUIRE(println(s, "px={:.2f} qty={:>4} side={} order={} ok={} c={}"_, 2.5, 7u,
                    corpus::Side::sell, corpus::sample_order(), true, 'z')
                .has_value());
    const std::size_t f2 = v.size();
    REQUIRE(print(s, "{{literal braces}} {1} then {0}"_, "first", "second"_).has_value());
    const std::size_t f3 = v.size();
    REQUIRE(print(s, "hex {:#x} neg {} f32 {:.1f} i8 {}"_, 255u, -3ll, 1.25f, std::int8_t{-7})
                .has_value());
    const std::size_t f4 = v.size();
    REQUIRE(serialize(s, "not a print call", 1).has_value());
    const dyn::dictionary d = process_dictionary(); // now holds every call above
    const std::span<const std::byte> all = v;
    const auto r1 = tools::render_frame(all.first(f1), d);
    REQUIRE(r1.has_value());
    CHECK(*r1 == "hello new 100 worlds");
    const auto r2 = tools::render_frame(all.subspan(f1, f2 - f1), d);
    REQUIRE(r2.has_value());
    CHECK(*r2 ==
          "px=2.50 qty=   7 side=sell order=corpus::Order{id: 42, px: 1050000, qty: 100, side: "
          "corpus::Side::buy (1), sym: \"AAPL\\0\\0\\0\\0\"} ok=true c=z\n");
    const auto r3 = tools::render_frame(all.subspan(f2, f3 - f2), d);
    REQUIRE(r3.has_value());
    CHECK(*r3 == "{literal braces} second then first");
    const auto r4 = tools::render_frame(all.subspan(f3, f4 - f3), d);
    REQUIRE(r4.has_value());
    CHECK(*r4 == "hex 0xff neg -3 f32 1.2 i8 -7");
    const auto r5 = tools::render_frame(all.subspan(f4), d);
    REQUIRE_FALSE(r5.has_value());
    CHECK(r5.error() == error::invalid_argument); // not a print call
    // a dictionary without the format literal cannot render
    dyn::dictionary no_lit = d;
    no_lit.literals.clear();
    CHECK(tools::render_frame(all.first(f1), no_lit).error() == error::unknown_literal);
    static_cast<void>(d0);
}

TEST_CASE("FR-P5 srdump text renders print frames, --raw shows records, JSON adds message",
          "[print][tools]")
{
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(write_dictionary_frame(s).has_value()); // dictionary first (a stream)
    REQUIRE(println(s, "qty={} px={:.1f}"_, 3, 9.75).has_value());
    REQUIRE(serialize(s, "plain", 1).has_value());
    std::string text;
    REQUIRE(tools::dump(v, nullptr, {}, text).has_value());
    CHECK(text.find("println 0x") != std::string::npos);
    CHECK(text.find("  print: qty=3 px=9.8\n") != std::string::npos);
    CHECK(text.find("[0] \"qty={} px={:.1f}\"") == std::string::npos); // records hidden
    CHECK(text.find("  [0] string = \"plain\"\n  [1] i32 = 1\n") != std::string::npos);
    tools::dump_options raw;
    raw.raw = true;
    std::string rtext;
    REQUIRE(tools::dump(v, nullptr, raw, rtext).has_value());
    CHECK(rtext.find("  print: qty=3 px=9.8\n") != std::string::npos); // message stays
    CHECK(rtext.find("[1] i32 = 3") != std::string::npos);             // and the records show
    tools::dump_options jo;
    jo.fmt = tools::format::json;
    std::string js;
    REQUIRE(tools::dump(v, nullptr, jo, js).has_value());
    CHECK(js.find("\"message\":\"qty=3 px=9.8\\n\"") != std::string::npos);
    CHECK(js.find("\"type\":\"i32\",\"value\":3") != std::string::npos);
    CHECK(js.find("\"type\":\"string\",\"value\":\"plain\"") != std::string::npos);
    // the dictionary text names the forms and the categories
    CHECK(text.find("println 0x") != std::string::npos);
    CHECK(text.find("(lit, i32, f64)") != std::string::npos);
    CHECK(text.find("(string, i32)") != std::string::npos);
    // verify decodes print frames like any other
    const auto rep = tools::verify(v);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->records == 5);
}

TEST_CASE("FR-P1 deserialize_print / deserialize_println are the typed readers", "[print]")
{
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(println(s, "{} {}"_, "text", 5).has_value());
    std::string t;
    int i{};
    REQUIRE(deserialize_println(v, "{} {}"_, t, i).has_value());
    CHECK(t == "text");
    CHECK(i == 5);
    CHECK(deserialize_print(v, "{} {}"_, t, i).error() == error::hash_mismatch);    // wrong form
    CHECK(deserialize(v, "{} {}"_, t, i).error() == error::hash_mismatch);          // plain call
    CHECK(deserialize_println(v, "{}  {}"_, t, i).error() == error::hash_mismatch); // other text
}

// ---- format 1.6 only (2.2.0; 1.5 in 2.1.15, 1.4 in 2.1.14, 1.3 since 2.1.0) ----------------------------------------------------------------
TEST_CASE("FR-H5 captures of an older minor or hash algorithm are refused, not guessed at",
          "[print][compat]")
{
    // Formats 1.0-1.2 used FNV-1a-64 ids (hash_algo 1); 2.1.0 computes XXH3-64 (hash_algo 2) only, so a file of
    // an earlier minor is bad_version and a current header naming another algorithm is unsupported. 2.1.14 (1.4)
    // changed canonical names, so a 1.3 file is refused too; 2.1.15 (1.5) added constant arguments; 2.2.0 (1.6)
    // added the map/set shapes and the transparent flag. 2.2.1 (1.7) readers accept 1.6 and 1.7 (the 1.6 fixture
    // is read in test_format17.cpp); a 1.7 file relabelled 1.6 is not tried here — its CHAIN would be read as a
    // frame.
    const std::string dir = std::string(ES_SERREG_TEST_SOURCE_DIR) + "/../fixtures/golden/";
    const auto cap = tools::read_file(dir + "orders.ser");
    REQUIRE(cap.has_value());
    REQUIRE(static_cast<unsigned>((*cap)[6]) == fmt::format_minor);
    REQUIRE(tools::verify(*cap).has_value());
    for (const unsigned minor : {0u, 1u, 2u, 3u, 4u, 5u, 8u, 9u})
    {
        std::vector<std::byte> other(*cap);
        other[6] = static_cast<std::byte>(minor);
        CHECK(tools::verify(other).error() == error::bad_version);
    }
    std::vector<std::byte> fnv(*cap);
    fnv[12] = std::byte{1}; // hash_algo 1 (FNV-1a-64)
    CHECK(tools::verify(fnv).error() == error::unsupported);
    const auto rri = tools::read_file(dir + "orders.rri");
    REQUIRE(rri.has_value());
    std::vector<std::byte> rri12(*rri);
    rri12[6] = std::byte{2};
    CHECK(tools::load_dictionary_rri(rri12).error() == error::bad_version);
    std::uint64_t doff, dlen;
    std::memcpy(&doff, cap->data() + 24, 8);
    std::memcpy(&dlen, cap->data() + 32, 8);
    std::vector<std::byte> flags(*cap);
    const std::size_t first =
        fmt::align8(doff + dlen) + fmt::chain_size; // 1.7: the CHAIN precedes the frames
    flags[first + 4] = std::byte{0x80};             // unknown frame flag
    const auto rep2 = tools::verify(flags);
    REQUIRE(rep2.has_value());
    CHECK_FALSE(rep2->ok());
    CHECK(read_frame_header(std::span<const std::byte>(flags).subspan(first)).error() ==
          error::bad_version);
}

// print / println: the qualified, the template-argument and the ADL spellings write the same frame.
TEST_CASE("FR-P1 print/println: sr::println, println<\"…\"> and ADL println write the same frame",
          "[print][names]")
{
    namespace sr = es::serreg;
    std::vector<std::byte> a, b, c;
    {
        vector_sink sa{a}, sb{b}, sc{c};
        REQUIRE(sr::println(sa, "px={:.2f} qty={}"_, 2.5, 7).has_value());
        REQUIRE(sr::println<"px={:.2f} qty={}">(sb, 2.5, 7).has_value());
        REQUIRE(
            println(sc, "px={:.2f} qty={}"_, 2.5, 7).has_value()); // ADL through the sink argument
    }
    CHECK(a == b);
    CHECK(a == c);
    double px = 0;
    int qty = 0;
    REQUIRE(deserialize_println(a, "px={:.2f} qty={}"_, px, qty).has_value());
    CHECK((px == 2.5 && qty == 7));
}
