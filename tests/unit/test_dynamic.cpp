// tests/unit/test_dynamic.cpp — v2.0.0 dynamic types (docs/plans-for-v2.0.md): std::string / vector / optional /
// variant members as canonical images — byte-exact layout (§2), typed round trips of every shape, empty and
// engaged forms, large vectors through the streaming path, dispatch/inspect, the dyn decoder's bounds checks,
// print with dynamic arguments, and the 1.1 compatibility corpus.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <variant>
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
struct plain_sink
{
    std::vector<std::byte>& v;
    bool write(const std::byte* const p, const std::size_t n)
    {
        v.insert(v.end(), p, p + n);
        return true;
    }
};
dyn::dictionary process_dictionary()
{
    const auto d = tools::load_dictionary_rri(registry::instance().export_image());
    REQUIRE(d.has_value());
    return *d;
}
std::uint32_t u32_at(const std::span<const std::byte> b, const std::size_t at)
{
    std::uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
} // namespace

TEST_CASE("2.0 §2 byte-exact canonical image of the plan example", "[dynamic][golden]")
{
    const auto buf = frame_of(corpus::sample_dyn_msg()); // {7, "ab", {1,-2,3}}
    REQUIRE(buf.size() == 16 + 8 + 32);                  // header, prefix, image 28 padded to 32
    const std::span<const std::byte> rec(buf.data() + 16, buf.size() - 16);
    CHECK(u32_at(rec, 0) == 28); // len
    CHECK(u32_at(rec, 4) == 0);  // reserved
    const std::span<const std::byte> img = rec.subspan(8, 28);
    CHECK(u32_at(img, 0) == 7);   // id
    CHECK(u32_at(img, 4) == 20);  // name.off
    CHECK(u32_at(img, 8) == 2);   // name.n
    CHECK(u32_at(img, 12) == 22); // xs.off (aligned to i16)
    CHECK(u32_at(img, 16) == 3);  // xs.n
    CHECK((img[20] == std::byte{'a'} && img[21] == std::byte{'b'}));
    std::int16_t xs[3];
    std::memcpy(xs, img.data() + 22, 6);
    CHECK((xs[0] == 1 && xs[1] == -2 && xs[2] == 3));
    CHECK(buf[16 + 8 + 28] == std::byte{0}); // pad
    // identical values ⇒ identical bytes
    CHECK(frame_of(corpus::DynMsg{7, "ab", {1, -2, 3}}) == buf);
}

TEST_CASE("2.0 typed round trip of every dynamic shape", "[dynamic][roundtrip]")
{
    const corpus::DynMsg m = corpus::sample_dyn_msg();
    const corpus::DynNested n = corpus::sample_dyn_nested();
    const corpus::DynArr a = corpus::sample_dyn_arr();
    const corpus::DynBase b = corpus::sample_dyn_base();
    const auto buf = frame_of("dyn"_, m, n, a, b, 5);
    corpus::DynMsg m2;
    corpus::DynNested n2;
    corpus::DynArr a2;
    corpus::DynBase b2;
    int i = 0;
    REQUIRE(deserialize(buf, "dyn"_, m2, n2, a2, b2, i).has_value());
    CHECK(m2 == m);
    CHECK(n2 == n);
    CHECK(a2 == a);
    CHECK(b2 == b);
    CHECK(i == 5);
    // outputs that held other values are fully replaced (vectors shrink, optionals reset)
    corpus::DynNested dirty;
    dirty.msgs.resize(9);
    dirty.maybe = corpus::sample_order();
    dirty.v = 77;
    REQUIRE(deserialize(frame_of(corpus::DynNested{}), dirty).has_value());
    CHECK(dirty == corpus::DynNested{});
}

TEST_CASE("2.0 empty and engaged forms: empty string/vector, nullopt, every alternative, monostate",
          "[dynamic][roundtrip]")
{
    corpus::DynNested n{};
    for (int alt = 0; alt < 3; ++alt)
    {
        if (alt == 0)
            n.v = std::int32_t{-9};
        else if (alt == 1)
            n.v = corpus::sample_order();
        else
            n.v = std::string(300, 'z');
        corpus::DynNested back;
        REQUIRE(deserialize(frame_of(n), back).has_value());
        CHECK(back == n);
        CHECK(back.v.index() == static_cast<std::size_t>(alt));
    }
    corpus::DynArr a{};
    CHECK(a.maybe_msg.index() == 0); // monostate
    corpus::DynArr a2 = corpus::sample_dyn_arr();
    REQUIRE(deserialize(frame_of(a), a2).has_value());
    CHECK(a2 == a);
    CHECK(a2.maybe_msg.index() == 0);
    // the image of an all-empty value is just the fixed part
    const auto e = frame_of(corpus::DynMsg{});
    CHECK(u32_at(std::span<const std::byte>(e).subspan(16), 0) == 20);
}

TEST_CASE("2.0 large vectors: plain sink beyond the chunk (heap path) and claiming sink (no heap)",
          "[dynamic][large]")
{
    corpus::DynMsg big{1, std::string(70'000, 's'), std::vector<std::int16_t>(100'000, 3)};
    std::vector<std::byte> v;
    plain_sink ps{v};
    REQUIRE(serialize(ps, big, "tail"_).has_value());
    corpus::DynMsg back;
    REQUIRE(deserialize(v, back, "tail"_).has_value());
    CHECK(back == big);
    std::vector<std::byte> c;
    vector_sink cs{c};
    REQUIRE(serialize_with(serialize_options{.full_records = true}, cs, big).has_value());
    std::vector<std::byte> t;
    plain_sink pt{t};
    REQUIRE(serialize_with(serialize_options{.full_records = true}, pt, big).has_value());
    CHECK(c == t); // claiming and plain paths produce identical bytes
    corpus::DynMsg back2;
    REQUIRE(deserialize(c, back2).has_value());
    CHECK(back2 == big);
    CHECK(tools::verify(c).has_value());
}

TEST_CASE("2.0 inspect / dispatch / view over dynamic records", "[dynamic][readers]")
{
    const corpus::DynMsg m = corpus::sample_dyn_msg();
    const auto buf = frame_of(m, corpus::sample_order(), "x"_);
    const auto fv = inspect(buf);
    REQUIRE(fv.has_value());
    REQUIRE(fv->size() == 3);
    std::vector<record_view> recs(fv->begin(), fv->end());
    CHECK(recs[0].ak == fmt::arg_type);
    REQUIRE(recs[0].desc != nullptr);
    CHECK_FALSE(recs[0].desc->is_flat);
    CHECK(recs[0].payload.size() == 28); // the image, prefix stripped
    CHECK(recs[1].payload.size() == sizeof(corpus::Order));
    CHECK(recs[2].is_literal);
    int seen = 0;
    const auto r = dispatch<corpus::DynMsg, corpus::Order>(buf, [&](const auto& x) {
        using X = std::remove_cvref_t<decltype(x)>;
        if constexpr (std::is_same_v<X, corpus::DynMsg>)
            seen += x == m;
        else if constexpr (std::is_same_v<X, corpus::Order>)
            seen += 10 * (x == corpus::sample_order());
        else if constexpr (std::is_same_v<X, literal_view>)
            seen += 100 * (x.text == "x");
    });
    REQUIRE(r.has_value());
    CHECK(seen == 111);
    STATIC_REQUIRE(!flat_type<corpus::DynMsg>); // view<T>() is constrained to flat types
}

TEST_CASE("2.0 corrupt images are rejected, never over-read", "[dynamic][negative]")
{
    const auto good = frame_of(corpus::sample_dyn_nested());
    const std::size_t img = 16 + 8; // image start
    const dyn::dictionary d = process_dictionary();
    struct edit
    {
        std::size_t at;
        std::uint32_t value;
        const char* what;
    };
    for (const edit e :
         {edit{img + 0, 0xffffff00u, "msgs.off out of range"},
          edit{img + 4, 0x10000000u, "msgs.n overflow"}, edit{img + 12, 7u, "optional n not 0/1"},
          edit{img + 20, 9u, "variant index beyond alternatives"},
          edit{img + 16, 0xfffffff0u, "variant off out of range"}})
    {
        std::vector<std::byte> bad = good;
        std::memcpy(bad.data() + e.at, &e.value, 4);
        corpus::DynNested out;
        CHECK_FALSE(deserialize(bad, out).has_value());
        const auto rep = tools::verify(bad, &d);
        REQUIRE(rep.has_value());
        CHECK_FALSE(rep->ok());
        std::string text;
        static_cast<void>(tools::dump(bad, &d, {}, text)); // must not crash; problems are reported
    }
    // a length prefix larger than the record
    std::vector<std::byte> bad = good;
    const std::uint32_t huge = 0x7fffffff;
    std::memcpy(bad.data() + 16, &huge, 4);
    corpus::DynNested out;
    CHECK(deserialize(bad, out).error() == error::truncated);
    CHECK_FALSE(inspect(bad).has_value());
}

TEST_CASE("2.0 tools: dump text/JSON of dynamic values and print with a dynamic argument",
          "[dynamic][tools]")
{
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(write_dictionary_frame(s).has_value());
    REQUIRE(serialize(s, corpus::sample_dyn_arr()).has_value());
    REQUIRE(println(s, "msg={}"_, corpus::sample_dyn_msg()).has_value());
    std::string text;
    REQUIRE(tools::dump(v, nullptr, {}, text).has_value());
    CHECK(text.find("names: [\"n0\", \"n1\"]") != std::string::npos);
    CHECK(text.find("side: null") != std::string::npos);
    CHECK(text.find("maybe_msg: variant<1:corpus::DynMsg> {") != std::string::npos);
    CHECK(text.find("  print: msg=corpus::DynMsg{id: 7, name: \"ab\", xs: [1, -2, 3]}\n") !=
          std::string::npos);
    CHECK(text.find("struct corpus::DynArr") != std::string::npos);
    CHECK(text.find("  dynamic\n") != std::string::npos); // the dictionary marks non-flat types
    tools::dump_options jo;
    jo.fmt = tools::format::json;
    std::string js;
    REQUIRE(tools::dump(v, nullptr, jo, js).has_value());
    CHECK(js.find("\"side\":null") != std::string::npos);
    CHECK(js.find("\"maybe_msg\":{\"alt\":1,\"type\":\"corpus::DynMsg\",\"value\":{\"id\":7") !=
          std::string::npos);
    CHECK(js.find("\"tags\":[\"x\",\"yy\"]") != std::string::npos);
    const auto rep = tools::verify(v);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
}

TEST_CASE("2.0 registry: dynamic types and their element types are registered before main()",
          "[dynamic][registry]")
{
    static_cast<void>(frame_of(corpus::sample_dyn_nested()));
    const auto& reg = registry::instance();
    CHECK(reg.find_type(schema_hash_v<corpus::DynNested>) != nullptr);
    CHECK(reg.find_type(schema_hash_v<corpus::DynMsg>) != nullptr); // vector element
    CHECK(reg.find_type(schema_hash_v<corpus::Order>) != nullptr); // optional / variant alternative
    static_cast<void>(frame_of(corpus::sample_dyn_arr()));
    CHECK(reg.find_type(schema_hash_v<std::monostate>) != nullptr);
    CHECK(reg.find_type(schema_hash_v<corpus::Side>) != nullptr);
}

TEST_CASE("2.0.3 hostile dictionary cannot wrap the decoder element stride (fuzz regression)",
          "[dynamic][negative][fuzz]")
{
    // Found by serreg_fuzz capture (seed 639290) under -D_GLIBCXX_ASSERTIONS: a vector element of size
    // 0xFFFFFFFF with align 2 had its stride rounded up in 32 bits → 0, so the block check `n × stride ≤ image`
    // passed and the element subspan ran off the image.
    const auto frame = frame_of(corpus::sample_dyn_msg());
    dyn::dictionary d = process_dictionary();
    dyn::type* const t = [&]() -> dyn::type* {
        for (auto& [h, ty] : d.types)
            if (ty.name == "corpus::DynMsg")
                return &ty;
        return nullptr;
    }();
    REQUIRE(t != nullptr);
    REQUIRE(t->fields.size() == 3);
    REQUIRE(t->fields[2].k == kind::vector_);
    for (const std::uint16_t align : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{8}})
    {
        dyn::field& e = t->fields[2].elem[0];
        e.size = 0xFFFFFFFFu;
        e.align = align;
        const auto rep = tools::verify(frame, &d);
        REQUIRE(rep.has_value());
        CHECK_FALSE(rep->ok()); // a decode problem, not a crash or an over-read
        std::string text;
        static_cast<void>(tools::dump(frame, &d, {}, text));
    }
}

// ---- 2.0.6: tools::walk_values, for_each_frame_in_file, mapped_file ----------------------------------------
namespace
{
// Counts the generic events; the typed ones are counted by the lambdas.
struct count_visitor final : tools::decoded_value_visitor
{
    int structs = 0, leaves = 0, vectors = 0, variants = 0;
    void begin_struct(std::string_view, std::string_view) override
    {
        ++structs;
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
        ++leaves;
    }
    void enumerator(std::string_view, std::string_view, std::string_view, std::int64_t) override
    {
        ++leaves;
    }
    void literal(std::string_view, std::uint64_t, std::string_view) override
    {
        ++leaves;
    }
    void string(std::string_view, std::string_view) override
    {
        ++leaves;
    }
    void begin_vector(std::string_view, std::uint32_t) override
    {
        ++vectors;
    }
    void end_vector() override
    {
    }
    void null_value(std::string_view) override
    {
        ++leaves;
    }
    void begin_variant(std::string_view, std::uint32_t, std::string_view) override
    {
        ++variants;
    }
    void end_variant() override
    {
    }
};
template<class... Fs> struct overload : Fs...
{
    using Fs::operator()...;
};
// A bare stream: a dictionary frame, then the given frames.
std::vector<std::byte> stream_of(const std::vector<std::vector<std::byte>>& frames)
{
    std::vector<std::byte> s;
    vector_sink out{s};
    REQUIRE(write_dictionary_frame(out));
    for (const auto& f : frames)
        s.insert(s.end(), f.begin(), f.end());
    return s;
}
} // namespace

TEST_CASE("2.0.6 walk_values delivers the outermost compiled type and walks the rest",
          "[dynamic][walk]")
{
    const auto nested = corpus::sample_dyn_nested(); // msgs: 2 DynMsg; maybe: Order; v: std::string
    const auto arr = corpus::sample_dyn_arr();       // variant<monostate, DynMsg> holding a DynMsg
    const auto base = corpus::sample_dyn_base();     // a flat Order base inside a dynamic class
    const auto s = stream_of(
        {frame_of(nested), frame_of(arr), frame_of(base), frame_of(corpus::sample_order())});

    SECTION("the whole record when its type is compiled")
    {
        count_visitor rest;
        int n = 0;
        const auto w = tools::walk_values<corpus::DynNested>(
            s, [&](std::string_view, const corpus::DynNested& v) { n += v == nested; }, rest);
        REQUIRE(w);
        CHECK(w->problems.empty());
        CHECK(n == 1);
    }
    SECTION(
        "inner values — vector elements, an optional, a variant alternative, a base, a flat record")
    {
        count_visitor rest;
        std::vector<corpus::DynMsg> msgs;
        std::vector<corpus::Order> orders;
        const auto w = tools::walk_values<corpus::DynMsg, corpus::Order>(
            s,
            overload{[&](std::string_view, const corpus::DynMsg& m) { msgs.push_back(m); },
                     [&](std::string_view, const corpus::Order& o) { orders.push_back(o); }},
            rest);
        REQUIRE(w);
        CHECK(w->problems.empty());
        REQUIRE(msgs.size() == 3); // two in DynNested::msgs, one as DynArr's variant alternative
        CHECK(msgs[0] == nested.msgs[0]);
        CHECK(msgs[1] == nested.msgs[1]);
        CHECK(msgs[2] == corpus::sample_dyn_msg());
        REQUIRE(orders.size() ==
                3); // DynNested::maybe, DynBase's base, the flat Order record (native bytes)
        for (const auto& o : orders)
            CHECK(o == corpus::sample_order());
        CHECK(rest.structs ==
              3); // DynNested, DynArr, DynBase were walked (their inner types were delivered)
    }
    SECTION("no compiled types: everything is walked")
    {
        count_visitor rest;
        const auto w = tools::walk_values<>(s, [](std::string_view, const auto&) {}, rest);
        REQUIRE(w);
        CHECK(w->problems.empty());
        CHECK(rest.structs >= 8);
    }
}

TEST_CASE("2.0.6 for_each_frame_in_file (blocks) and mapped_file see the same frames",
          "[dynamic][walk]")
{
    std::vector<std::vector<std::byte>> frames;
    for (int i = 0; i < 400; ++i) // ~60 KB: many 4 KiB blocks, frames straddling block boundaries
        frames.push_back(i % 2 ? frame_of(corpus::sample_dyn_nested())
                               : frame_of(corpus::sample_order()));
    const auto s = stream_of(frames);
    const std::string path = std::string(ES_SERREG_TEST_TMPDIR) + "/walk_blocks.ser";
    REQUIRE(tools::write_file(path, s));

    std::vector<std::pair<std::size_t, std::vector<std::byte>>> via_map, via_blocks;
    const auto map = tools::mapped_file::open(path);
    REQUIRE(map);
    CHECK(map->size() == s.size());
    const auto a =
        tools::for_each_frame(map->bytes(), [&](const dyn::dictionary&, const tools::frame_ref& f) {
            via_map.emplace_back(f.offset, std::vector<std::byte>(f.bytes.begin(), f.bytes.end()));
            return true;
        });
    const auto b = tools::for_each_frame_in_file(
        path,
        [&](const dyn::dictionary& d, const tools::frame_ref& f) {
            CHECK(f.call == d.find_call(f.call_hash));
            via_blocks.emplace_back(f.offset,
                                    std::vector<std::byte>(f.bytes.begin(), f.bytes.end()));
            return true;
        },
        {.block_bytes = 4096});
    REQUIRE((a && b));
    CHECK(a->problems.empty());
    CHECK(b->problems.empty());
    CHECK(a->frames == 400);
    CHECK(b->frames == 400);
    CHECK(via_map == via_blocks);

    count_visitor rest;
    int typed = 0;
    const auto w = tools::walk_values_in_file<corpus::DynMsg>(
        path, [&](std::string_view, const corpus::DynMsg&) { ++typed; }, rest, {},
        {.block_bytes = 4096});
    REQUIRE(w);
    CHECK(typed == 400); // 200 DynNested frames × 2 msgs

    std::vector<std::byte> cut(s.begin(),
                               s.end() - 8); // truncated last frame: a problem, not a crash
    REQUIRE(tools::write_file(path, cut));
    const auto t = tools::for_each_frame_in_file(
        path, [](const dyn::dictionary&, const tools::frame_ref&) { return true; });
    REQUIRE(t);
    CHECK(t->frames == 399);
    CHECK_FALSE(t->problems.empty());
    CHECK(tools::mapped_file::open(path + ".missing").error() == error::not_found);
    CHECK(tools::for_each_frame_in_file(path + ".missing", [](const dyn::dictionary&,
                                                              const tools::frame_ref&) {
              return true;
          }).error() == error::not_found);
}

TEST_CASE("2.0.6 for_each_frame_in_file agrees with for_each_frame on every capture fixture, incl. "
          "back-patched",
          "[dynamic][walk]")
{
    const std::string dir = std::string(ES_SERREG_TEST_SOURCE_DIR) + "/../fixtures/corpus/capture/";
    std::vector<std::string> files = {"bare_frames.bin", "big_bare.bin", "dict_frame_stream.bin",
                                      "dynamic-1.6.ser", "orders-1.6.ser"};
    // A back-patched FILE made from orders-1.6.ser: the complete dictionary moves to the end, the header points
    // there and the original dictionary area stays in place (spec §11), frames end where the new one starts.
    {
        auto f = tools::read_file(dir + "orders-1.6.ser");
        REQUIRE(f);
        auto v = *f;
        std::uint64_t dict_offset, dict_len;
        std::memcpy(&dict_offset, v.data() + 24, 8);
        std::memcpy(&dict_len, v.data() + 32, 8);
        REQUIRE(dict_offset == fmt::file_header_size);
        const std::vector<std::byte> rri(v.begin() + 48,
                                         v.begin() + 48 + static_cast<std::ptrdiff_t>(dict_len));
        v.resize(fmt::align8(v.size()));
        const std::uint64_t new_off = v.size();
        v.insert(v.end(), rri.begin(), rri.end());
        std::uint32_t flags;
        std::memcpy(&flags, v.data() + 8, 4);
        flags |= fmt::file_flag_backpatched;
        std::memcpy(v.data() + 8, &flags, 4);
        std::memcpy(v.data() + 24, &new_off, 8);
        REQUIRE(tools::write_file(std::string(ES_SERREG_TEST_TMPDIR) + "/backpatched.ser", v));
        files.push_back(std::string(ES_SERREG_TEST_TMPDIR) + "/backpatched.ser");
    }
    for (const auto& name : files)
    {
        const std::string path = name.starts_with('/') ? name : dir + name;
        INFO(path);
        const auto bytes = tools::read_file(path);
        REQUIRE(bytes);
        std::vector<std::pair<std::size_t, std::size_t>> mem,
            blk; // (offset, length) per data frame
        const auto a =
            tools::for_each_frame(*bytes, [&](const dyn::dictionary&, const tools::frame_ref& f) {
                mem.emplace_back(f.offset, f.bytes.size());
                return true;
            });
        const auto b =
            tools::for_each_frame_in_file(path,
                                          [&](const dyn::dictionary&, const tools::frame_ref& f) {
                                              blk.emplace_back(f.offset, f.bytes.size());
                                              return true;
                                          },
                                          {.block_bytes = 4096});
        REQUIRE(a);
        REQUIRE(b);
        CHECK(mem == blk);
        CHECK(a->frames == b->frames);
        CHECK(a->dict.types.size() == b->dict.types.size());
        CHECK(a->dict.calls.size() == b->dict.calls.size());
    }
}

// ---- 2.1.1 (T0): G2-2 — identical values give identical bytes on every sink ----------------------------------
namespace g22
{
struct M
{
    std::string s;
    std::vector<std::int16_t> xs;
    bool operator==(const M&) const = default;
};
struct N
{ // an odd-length string before an aligned vector: the alignment gap sits between two tail blocks
    std::string s;
    std::vector<std::int64_t> xs;
    std::optional<std::string> o;
};
} // namespace g22

TEST_CASE("G2-2 an empty vector is measured as it is encoded (no tail, slot {0,0})",
          "[dynamic][canonical]")
{
    const g22::M m{"a", {}};
    CHECK(detail::measure_image(m) == detail::image_size<g22::M>() + 1); // "a" only; nothing for xs
    const auto f = frame_of(m);
    g22::M out{"x", {9}};
    REQUIRE(deserialize(f, out));
    CHECK(out == m);
}

TEST_CASE("G2-2 a span_sink over non-zero memory writes the same bytes as vector_sink",
          "[dynamic][canonical]")
{
    const g22::N n{"abc", {1, -2}, std::string("zz")};
    const g22::M m{"a", {}};
    for (const bool use_n : {false, true})
    {
        const auto ref = use_n ? frame_of(n) : frame_of(m);
        std::vector<std::byte> buf(512, std::byte{0xA5});
        span_sink ss{buf};
        const auto w = use_n ? serialize(ss, n) : serialize(ss, m);
        REQUIRE(w);
        REQUIRE(*w == ref.size());
        CHECK(std::memcmp(buf.data(), ref.data(), ref.size()) == 0);
    }
}

TEST_CASE(
    "G2-2 capture_file: the second dynamic frame is byte-identical to the first (reused buffer)",
    "[dynamic][canonical]")
{
    const std::string path = std::string(ES_SERREG_TEST_TMPDIR) + "/g22_capture.ser";
    const g22::N big{
        "a much longer string that dirties the buffer", {1, 2, 3, 4, 5}, std::string("xyz")};
    const g22::N small{"abc", {1, -2}, std::string("zz")};
    {
        capture_file f;
        REQUIRE(f.open(path));
        REQUIRE(serialize(f, big)); // leaves non-zero bytes in the reused claim buffer
        REQUIRE(serialize(f, small));
        REQUIRE(f.close());
    }
    const auto cap = tools::read_file(path);
    REQUIRE(cap.has_value());
    const auto ref = frame_of(small);
    std::vector<std::vector<std::byte>> frames;
    REQUIRE(tools::for_each_frame(*cap, [&](const dyn::dictionary&, const tools::frame_ref& fr) {
        frames.emplace_back(fr.bytes.begin(), fr.bytes.end());
        return true;
    }));
    REQUIRE(frames.size() == 2);
    CHECK(frames[1] == ref);
    std::remove(path.c_str());
}

TEST_CASE("G2-2 a small dynamic frame through a plain sink (stack buffer) has zero gaps",
          "[dynamic][canonical]")
{
    const g22::N n{"abc", {1, -2}, std::string("zz")};
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    {
        fd_sink out{fds[1]};
        REQUIRE(serialize(out, g22::N{"a much longer string first", {7, 7, 7}, std::nullopt}));
        REQUIRE(serialize(out, n));
    }
    ::close(fds[1]);
    std::vector<std::byte> got(4096);
    std::size_t have = 0;
    for (ssize_t r; (r = ::read(fds[0], got.data() + have, got.size() - have)) > 0;)
        have += static_cast<std::size_t>(r);
    ::close(fds[0]);
    const auto ref = frame_of(n);
    std::uint32_t first_len;
    std::memcpy(&first_len, got.data(), 4);
    REQUIRE(have == first_len + ref.size());
    CHECK(std::memcmp(got.data() + first_len, ref.data(), ref.size()) == 0);
}

// ---- 2.1.3 (T2): G2-6 one set of slot rules, G2-9 walkers agree, G2-10 reader holes, G2-12 bools ----------------
namespace t2
{
struct V
{
    std::vector<std::string> v;
};
struct W
{
    std::uint32_t id;
    std::variant<std::monostate, std::string> a;
    std::variant<std::int32_t, std::string> b;
};
struct B
{
    std::uint32_t id;
    bool on;
    bool arr[2];
};
struct D
{
    bool f;
    std::string s;
};
constexpr std::size_t img =
    16 + 8; // a one-record compact frame: frame header, u32 len, u32 0, image
std::uint64_t warn_hash = 0;
std::uint32_t warn_count = 0, warn_calls = 0;
void on_warning(const warning w, const std::uint64_t h, const std::uint32_t n) noexcept
{
    if (w == warning::noncanonical_bool)
    {
        warn_hash = h;
        warn_count += n;
        ++warn_calls;
    }
}
struct handler_scope
{
    warning_handler prev = set_warning_handler(&on_warning);
    handler_scope()
    {
        warn_hash = 0;
        warn_count = warn_calls = 0;
    }
    ~handler_scope()
    {
        set_warning_handler(prev);
    }
};
void put_u32(std::vector<std::byte>& b, const std::size_t at, const std::uint32_t v)
{
    std::memcpy(b.data() + at, &v, 4);
}
// deserialize and tools::verify must agree on a corrupt image: both reject it
template<class T> void both_reject(const std::vector<std::byte>& bad, const char* what)
{
    INFO(what);
    const dyn::dictionary d = process_dictionary();
    T out{};
    const auto r = deserialize(bad, out);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::length_mismatch);
    const auto rep = tools::verify(bad, &d);
    REQUIRE(rep.has_value());
    CHECK_FALSE(rep->ok());
}
} // namespace t2

TEST_CASE("G2-6 an empty vector slot {0xFFFFFFFF, 0}: deserialize and verify both reject it",
          "[dynamic][negative][g2-6]")
{
    auto f = frame_of(g22::M{"a", {}});
    REQUIRE(u32_at(f, t2::img + 8) == 0); // xs = {0, 0}
    t2::put_u32(f, t2::img + 8, 0xFFFFFFFFu);
    t2::both_reject<g22::M>(f, "empty vector, off past the image"); // 2.1.2: verify said ok
}

TEST_CASE("G2-6 n == 0 needs off == 0, and a non-empty block never starts inside the fixed part",
          "[dynamic][negative][g2-6]")
{
    const auto good = frame_of(g22::M{"ab", {1}});
    auto zero_n = good; // string slot {off, 0} with off inside the image: no writer produces it
    t2::put_u32(zero_n, t2::img + 4, 0);
    t2::both_reject<g22::M>(zero_n, "string n == 0, off != 0");
    auto into_fixed = good; // string slot {4, 2}: the tail would overlap the slots themselves
    t2::put_u32(into_fixed, t2::img + 0, 4);
    t2::both_reject<g22::M>(into_fixed, "block inside the fixed part");
    g22::M out;
    REQUIRE(deserialize(good, out)); // the honest frame still decodes
    CHECK(out == g22::M{"ab", {1}});
}

TEST_CASE("G2-6 slots that share one tail cannot expand a small image (byte budget)",
          "[dynamic][negative][g2-6]")
{
    t2::V v;
    for (int i = 0; i < 64; ++i)
        v.v.push_back(std::string(64, static_cast<char>('a' + i % 26)));
    auto f = frame_of(v);
    t2::V ok;
    REQUIRE(deserialize(f, ok)); // honest: 64 disjoint tails, within the budget
    CHECK(ok.v == v.v);
    // every string slot now claims the same 3000 bytes: 64 × 3000 = 192 000 bytes from a 4 616-byte image
    const std::uint32_t block =
        u32_at(f, t2::img + 0); // the vector's element block (64 slots of 8 bytes)
    const std::uint32_t first_tail = u32_at(f, t2::img + block);
    const std::uint32_t image_len = u32_at(f, 16);
    REQUIRE(first_tail + 3000 <= image_len);
    for (std::uint32_t i = 0; i < 64; ++i)
    {
        t2::put_u32(f, t2::img + block + 8 * i, first_tail);
        t2::put_u32(f, t2::img + block + 8 * i + 4, 3000);
    }
    t2::both_reject<t2::V>(f, "64 slots over one tail");
    // one pair of overlapping in-range slots is already over the budget when their sum exceeds the image
    auto g = frame_of(v);
    t2::put_u32(g, t2::img + block + 4,
                image_len - first_tail); // slot 0 runs to the end of the image
    t2::both_reject<t2::V>(g, "one slot covering all later tails");
}

TEST_CASE("G2-6 a valueless variant: std::monostate when the variant has one, else invalid_value",
          "[dynamic][variant][g2-6]")
{
    auto f = frame_of(t2::W{7, std::string("hi"), std::string("yo")}, std::uint32_t{99});
    constexpr std::size_t a_at = t2::img + detail::member_image_offset<t2::W, 1>();
    constexpr std::size_t b_at = t2::img + detail::member_image_offset<t2::W, 2>();
    REQUIRE(u32_at(f, a_at + 4) == 2); // alternative 1 (std::string) is n = 2
    t2::put_u32(f, a_at, 0);           // a := valueless on the wire
    t2::put_u32(f, a_at + 4, 0);
    {
        t2::W out{0, std::string("old"), 5};
        std::uint32_t u = 0;
        REQUIRE(deserialize(f, out, u));
        CHECK(out.a.index() == 0); // std::monostate
        CHECK(std::get<std::string>(out.b) == "yo");
        CHECK(u == 99);
    }
    t2::put_u32(f, b_at,
                0); // b := valueless as well: variant<int32_t, string> cannot hold "no value"
    t2::put_u32(f, b_at + 4, 0);
    t2::W out{0, std::string("old"), std::string("keep")};
    std::uint32_t u = 0;
    const auto r = deserialize(f, out, u);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::invalid_value);
    // 2.1.15 (decision C): 3 items expected (the frame, W, u32); W failed, the frame and the u32 were decoded
    CHECK(r.error().expected == 3);
    CHECK(r.error().actual == 2);
    CHECK(r.error().failed == 1);
    CHECK(std::get<std::string>(out.b) == "keep"); // left as it was, not alternative 0
    CHECK(out.id == 7);                            // the rest of the value ...
    CHECK(u == 99);                                // ... and the next record were still read
    // dispatch reports it and goes on with the next record
    int ws = 0, us = 0;
    const auto dr = dispatch<t2::W, std::uint32_t>(f, [&](const auto& x) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>, t2::W>)
            ++ws;
        else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>, std::uint32_t>)
            us += x == 99;
    });
    CHECK(dr.error() == error::invalid_value);
    CHECK(ws == 0);
    CHECK(us == 1);
    // the dictionary-driven tools show it as null; the image itself is well-formed
    const dyn::dictionary d = process_dictionary();
    const auto rep = tools::verify(f, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    std::string text;
    REQUIRE(tools::dump(f, &d, {}, text));
    CHECK(text.find("null") != std::string::npos);
    // walk_values: the typed decode fails, the value is walked generically and the fallback is reported (G2-9)
    const auto s = stream_of({f});
    count_visitor rest;
    int typed = 0;
    const auto w =
        tools::walk_values<t2::W>(s, [&](std::string_view, const t2::W&) { ++typed; }, rest);
    REQUIRE(w.has_value());
    CHECK(typed == 0);
    CHECK(w->typed == 0);
    CHECK(w->fallbacks == 1);
    REQUIRE(w->problems.size() == 1);
    CHECK(w->problems[0].find("invalid_value") != std::string::npos);
    CHECK(rest.leaves >= 2); // id, a = null, b = null, and the u32 record
}

TEST_CASE(
    "FR-S4 a record that fails while it is decoded: expected, actual and the failed item (2.1.15)",
    "[dynamic][errors][decode_error]")
{
    t2::V v;
    for (int i = 0; i < 4; ++i)
        v.v.push_back(std::string(40, static_cast<char>('a' + i)));
    // (u32, V, u32): the image of V is well-formed on the outside (its length fields agree with the frame), so
    // the frame is accepted; one string slot is then made to cover the later tails, which the image decoder
    // refuses while it decodes argument 2
    auto f = frame_of(std::uint32_t{5}, v, std::uint32_t{9});
    constexpr std::size_t img = 16 + 8 + 8; // frame header, the u32 record, u32 len + u32 0
    const std::uint32_t image_len = u32_at(f, 16 + 8);
    const std::uint32_t block = u32_at(f, img + 0);
    const std::uint32_t first_tail = u32_at(f, img + block);
    {
        std::uint32_t a = 0, b = 0;
        t2::V out;
        const auto ok = deserialize(f, a, out, b);
        REQUIRE(ok.has_value());
        CHECK((a == 5 && b == 9 && out.v == v.v));
    }
    t2::put_u32(f, img + block + 4, image_len - first_tail);
    std::uint32_t a = 0, b = 77;
    t2::V out;
    const auto r = deserialize(f, a, out, b);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() != error::invalid_value); // a hard failure: decoding stopped there
    CHECK(r.error().expected == 4);           // the frame + three arguments
    CHECK(r.error().actual == 2);             // the frame and the first u32
    CHECK(r.error().failed == 2);             // argument 2, the image
    CHECK(a == 5);                            // before the failure: complete
    CHECK(b == 77);                           // after it: untouched
}

TEST_CASE("G2-12 a bool byte other than 0/1 reads as true, with an optional warning",
          "[roundtrip][bool][g2-12]")
{
    const t2::handler_scope hs;
    auto f = frame_of(t2::B{1, true, {false, true}});
    constexpr std::size_t payload = 16;
    f[payload + offsetof(t2::B, on)] = std::byte{0x02};
    f[payload + offsetof(t2::B, arr)] = std::byte{0x07};
    t2::B out{};
    REQUIRE(deserialize(f, out));
    CHECK(std::bit_cast<std::uint8_t>(out.on) == 1); // a valid bool, read as true
    CHECK(std::bit_cast<std::uint8_t>(out.arr[0]) == 1);
    CHECK(std::bit_cast<std::uint8_t>(out.arr[1]) == 1);
    CHECK(t2::warn_calls == 1);
    CHECK(t2::warn_count == 2);
    CHECK(t2::warn_hash == schema_hash_v<t2::B>);
    // a zero-copy view cannot repair the bytes in place
    const auto fv = inspect(f);
    REQUIRE(fv.has_value());
    for (const record_view& r : fv->records())
        CHECK(view<t2::B>(r).error() == error::invalid_value);
    // dispatch falls back to a repaired copy
    bool seen = false;
    REQUIRE(dispatch<t2::B>(f, [&](const t2::B& b) {
        seen = std::bit_cast<std::uint8_t>(b.on) == 1 && std::bit_cast<std::uint8_t>(b.arr[0]) == 1;
    }));
    CHECK(seen);
    CHECK(t2::warn_calls == 2); // the copy dispatch made was repaired and reported too
    // verify: a warning, not a problem
    const dyn::dictionary d = process_dictionary();
    const auto rep = tools::verify(f, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    REQUIRE(rep->warnings.size() == 2);
    CHECK(rep->warnings[0].find("on: bool byte 0x02 read as true") != std::string::npos);
    // without a handler nothing is reported, the value is still repaired
    set_warning_handler(nullptr);
    t2::B again{};
    REQUIRE(deserialize(f, again));
    CHECK(std::bit_cast<std::uint8_t>(again.on) == 1);
    CHECK(t2::warn_calls == 2);
}

TEST_CASE("G2-12 a top-level bool argument and a bool inside a dynamic image are repaired too",
          "[roundtrip][bool][g2-12]")
{
    const t2::handler_scope hs;
    auto f = frame_of(true);
    f[16] = std::byte{0x05};
    bool b = false;
    REQUIRE(deserialize(f, b));
    CHECK(std::bit_cast<std::uint8_t>(b) == 1);
    CHECK(t2::warn_calls == 1);
    CHECK(t2::warn_hash == 0);
    const dyn::dictionary d = process_dictionary();
    auto rep = tools::verify(f, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->warnings.size() == 1);

    auto g = frame_of(t2::D{true, "x"});
    g[t2::img + detail::member_image_offset<t2::D, 0>()] = std::byte{0xFF};
    t2::D out{};
    REQUIRE(deserialize(g, out));
    CHECK(std::bit_cast<std::uint8_t>(out.f) == 1);
    CHECK(out.s == "x");
    CHECK(t2::warn_calls == 2);
    CHECK(t2::warn_hash == schema_hash_v<t2::D>);
    rep = tools::verify(g, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    REQUIRE(rep->warnings.size() == 1);
    CHECK(rep->warnings[0].find("f: bool byte 0xff read as true") != std::string::npos);
}

namespace scrub213
{
struct P
{
    std::uint8_t a;
    std::uint32_t b; // gap [1, 4)
};
struct D
{
    std::string s;
    P ps[2];       // a C array of padded structs inside a dynamic type
    bool flags[2]; // a C array of bools inside a dynamic type
    bool lone;     // a bool member of a dynamic type (the byte is normalised, not the value: -O2)
};
} // namespace scrub213

TEST_CASE("2.1.13 a C array of padded structs or bools inside a dynamic type is scrubbed like a "
          "top-level record",
          "[dynamic][scrub]")
{
    alignas(scrub213::D) std::byte raw[sizeof(scrub213::D)];
    std::memset(raw, 0xAB, sizeof raw); // what uninitialised padding may hold
    auto* const d = ::new (static_cast<void*>(raw)) scrub213::D;
    d->s = "x";
    d->ps[0].a = 1;
    d->ps[0].b = 2;
    d->ps[1].a = 3;
    d->ps[1].b = 4;
    std::memset(static_cast<void*>(&d->flags[0]), 0x05, 1); // a non-canonical bool byte
    d->flags[1] = false;
    std::memset(static_cast<void*>(&d->lone), 0x07, 1);
    const auto f = frame_of(*d);
    d->~D();
    const std::size_t ps = t2::img + detail::member_image_offset<scrub213::D, 1>();
    const std::size_t fl = t2::img + detail::member_image_offset<scrub213::D, 2>();
    for (std::size_t e = 0; e < 2; ++e)
        for (std::size_t i = 1; i < 4; ++i)
        {
            INFO("element " << e << " padding byte " << i);
            CHECK(f[ps + e * sizeof(scrub213::P) + i] == std::byte{0});
        }
    CHECK(f[ps] == std::byte{1});
    CHECK(f[ps + sizeof(scrub213::P)] == std::byte{3});
    CHECK(f[fl] == std::byte{1});
    CHECK(f[fl + 1] == std::byte{0});
    CHECK(f[t2::img + detail::member_image_offset<scrub213::D, 3>()] == std::byte{1});
    scrub213::D back{};
    REQUIRE(deserialize(f, back));
    CHECK(back.s == "x");
    CHECK(back.ps[1].b == 4);
    CHECK(back.flags[0]);
    CHECK_FALSE(back.flags[1]);
}

TEST_CASE("G2-9 an unknown frame flag: both walkers skip the frame and report it",
          "[tools][walkers][g2-9]")
{
    auto odd = frame_of(g22::M{"a", {1}});
    odd[4] =
        std::byte{0x80}; // flags bit 7: not defined (bits 2-4 are 1.7's CRC, checkpoint and info)
    const auto s = stream_of({frame_of(g22::M{"b", {}}), odd, frame_of(g22::M{"c", {2}})});
    const std::string path = std::string(ES_SERREG_TEST_TMPDIR) + "/g29_flags.ser";
    {
        std::FILE* const fp = std::fopen(path.c_str(), "wb");
        REQUIRE(fp);
        REQUIRE(std::fwrite(s.data(), 1, s.size(), fp) == s.size());
        std::fclose(fp);
    }
    int mem = 0, blk = 0;
    const auto a = tools::for_each_frame(s, [&](const dyn::dictionary&, const tools::frame_ref&) {
        ++mem;
        return true;
    });
    const auto b =
        tools::for_each_frame_in_file(path, [&](const dyn::dictionary&, const tools::frame_ref&) {
            ++blk;
            return true;
        });
    REQUIRE((a && b));
    CHECK(mem == 2);
    CHECK(blk == 2);
    CHECK(a->frames == b->frames);
    REQUIRE(a->problems.size() == 1);
    REQUIRE(b->problems.size() == 1);
    CHECK(a->problems[0].find("unknown frame flags 128 (frame skipped)") != std::string::npos);
    CHECK(b->problems[0].find("unknown frame flags 128 (frame skipped)") != std::string::npos);
    std::remove(path.c_str());
}

TEST_CASE("G2-9/G2-10 a literal id with two bodies is a conflict in both walkers and in verify",
          "[tools][walkers][g2-9][g2-10]")
{
    const dyn::dictionary proc = process_dictionary();
    REQUIRE_FALSE(proc.literals.empty());
    dyn::dictionary other;
    const dyn::literal& l = proc.literals.begin()->second;
    other.literals.emplace(l.id, dyn::literal{l.id, l.text + " (another body)"});
    const std::vector<std::byte> rri = dyn::to_rri(other);
    std::vector<std::byte> dict_frame(fmt::frame_header_size + fmt::align8(rri.size()));
    const auto len = static_cast<std::uint32_t>(dict_frame.size());
    const std::uint16_t flags = fmt::frame_flag_dict, n = 0;
    std::memcpy(dict_frame.data(), &len, 4);
    std::memcpy(dict_frame.data() + 4, &flags, 2);
    std::memcpy(dict_frame.data() + 6, &n, 2);
    std::memcpy(dict_frame.data() + 8, &fmt::dict_frame_sentinel, 8);
    std::memcpy(dict_frame.data() + 16, rri.data(), rri.size());
    const auto s = stream_of({dict_frame, frame_of(g22::M{"a", {}})});
    const std::string path = std::string(ES_SERREG_TEST_TMPDIR) + "/g29_conflict.ser";
    {
        std::FILE* const fp = std::fopen(path.c_str(), "wb");
        REQUIRE(fp);
        REQUIRE(std::fwrite(s.data(), 1, s.size(), fp) == s.size());
        std::fclose(fp);
    }
    const auto ok = [](const tools::frame_walk& w) {
        return std::any_of(w.problems.begin(), w.problems.end(), [](const std::string& p) {
            return p.starts_with("literal conflict: ") &&
                   p.find("another body") != std::string::npos;
        });
    };
    const auto cb = [](const dyn::dictionary&, const tools::frame_ref&) { return true; };
    const auto a = tools::for_each_frame(s, cb);
    const auto b = tools::for_each_frame_in_file(path, cb);
    REQUIRE((a && b));
    CHECK(ok(*a));
    CHECK(ok(*b)); // 2.1.2: the block reader did not report dictionary conflicts at all
    CHECK(a->dict.id_conflicts.size() == 1);
    const auto rep = tools::verify(s);
    REQUIRE(rep.has_value());
    CHECK_FALSE(rep->ok());
    std::remove(path.c_str());
}

TEST_CASE("G2-10 dump returns the walker's error code, not always length_mismatch",
          "[tools][g2-10]")
{
    dyn::dictionary d = process_dictionary();
    REQUIRE(d.types.erase(schema_hash_v<corpus::DynMsg>) ==
            1); // DynNested's element type is unknown now
    const auto f = frame_of(corpus::sample_dyn_nested());
    std::string text;
    const auto r = tools::dump(f, &d, {}, text);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::unknown_type);
}

TEST_CASE("G2-10 the original dictionary area of a back-patched FILE must have 8-byte entries",
          "[tools][walkers][g2-10]")
{
    const std::string dir = std::string(ES_SERREG_TEST_SOURCE_DIR) + "/../fixtures/corpus/capture/";
    auto f = tools::read_file(dir + "orders-1.6.ser");
    REQUIRE(f);
    auto v = *f;
    std::uint64_t dict_len;
    std::memcpy(&dict_len, v.data() + 32, 8);
    const std::vector<std::byte> rri(v.begin() + 48,
                                     v.begin() + 48 + static_cast<std::ptrdiff_t>(dict_len));
    v.resize(fmt::align8(v.size()));
    const std::uint64_t new_off = v.size();
    v.insert(v.end(), rri.begin(), rri.end());
    std::uint32_t flags;
    std::memcpy(&flags, v.data() + 8, 4);
    flags |= fmt::file_flag_backpatched;
    std::memcpy(v.data() + 8, &flags, 4);
    std::memcpy(v.data() + 24, &new_off, 8);
    // the first ENTRY of the *original* area claims 4 more bytes: skipping it would start the frames unaligned
    const std::size_t e0 = fmt::file_header_size + fmt::rri_header_size;
    std::uint32_t total;
    std::memcpy(&total, v.data() + e0, 4);
    total += 4;
    std::memcpy(v.data() + e0, &total, 4);
    const std::string path = std::string(ES_SERREG_TEST_TMPDIR) + "/g210_backpatched.ser";
    REQUIRE(tools::write_file(path, v));
    const auto cb = [](const dyn::dictionary&, const tools::frame_ref&) { return true; };
    const auto a = tools::for_each_frame(v, cb);
    const auto b = tools::for_each_frame_in_file(path, cb);
    REQUIRE_FALSE(a.has_value());
    REQUIRE_FALSE(b.has_value());
    CHECK(a.error() == error::corrupt_dictionary);
    CHECK(b.error() == error::corrupt_dictionary);
    std::remove(path.c_str());
}

// ---- 2.1.5: std::basic_string<char, traits, Alloc> (review R§9.3) ------------------------------------------
namespace t5
{
struct P
{
    std::uint32_t id;
    std::pmr::string name;
    std::vector<std::pmr::string> tags;
    std::pmr::vector<std::int32_t> xs;
};
struct S
{
    std::uint32_t id;
    std::string name;
    std::vector<std::string> tags;
    std::vector<std::int32_t> xs;
};
} // namespace t5

TEST_CASE("R§9.3 std::pmr::string members: the std::string wire form, the reader's allocator kept",
          "[dynamic][pmr]")
{
    // same kind and schema text as std::string: the allocator is not part of the data
    STATIC_REQUIRE(detail::is_dyn_member_v<std::pmr::string>);
    STATIC_REQUIRE(descriptor_v<t5::P>.fields[1].k == kind::string_);
    STATIC_REQUIRE(descriptor_v<t5::P>.fields[2].elem->k == kind::string_);
    STATIC_REQUIRE(descriptor_v<t5::P>.fields[3].k == kind::vector_);
    std::pmr::monotonic_buffer_resource arena;
    const t5::P p{7,
                  std::pmr::string("a name longer than the small-string buffer", &arena),
                  {std::pmr::string("x", &arena), std::pmr::string("yy", &arena)},
                  std::pmr::vector<std::int32_t>({1, -2, 3}, &arena)};
    const t5::S s{7, "a name longer than the small-string buffer", {"x", "yy"}, {1, -2, 3}};
    const auto fp = frame_of(p);
    const auto fs = frame_of(s);
    REQUIRE(fp.size() == fs.size());
    CHECK(std::equal(fp.begin() + 16, fp.end(),
                     fs.begin() + 16)); // identical image; only the call hash differs
    // decode into objects that use an arena: assign() keeps the output's allocator
    std::pmr::monotonic_buffer_resource out_arena;
    t5::P out{0, std::pmr::string(&out_arena), {}, std::pmr::vector<std::int32_t>(&out_arena)};
    REQUIRE(deserialize(fp, out));
    CHECK(out.id == 7);
    CHECK(out.name == p.name);
    CHECK(out.name.get_allocator().resource() == &out_arena);
    CHECK(out.xs.get_allocator().resource() == &out_arena);
    REQUIRE(out.tags.size() == 2);
    CHECK(out.tags[1] == "yy");
    CHECK(std::vector<std::int32_t>(out.xs.begin(), out.xs.end()) == s.xs);
    const dyn::dictionary d = process_dictionary();
    const auto rep = tools::verify(fp, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
}

TEST_CASE("R§9.3 a std::pmr::string argument is a string record, read back into any string type",
          "[dynamic][pmr]")
{
    std::pmr::monotonic_buffer_resource arena;
    const std::pmr::string text("hello from an arena string, long enough to allocate", &arena);
    const auto f = frame_of(text);
    CHECK(f ==
          frame_of(std::string(text.begin(), text.end()))); // the same frame, call hash included
    std::pmr::monotonic_buffer_resource out_arena;
    std::pmr::string out(&out_arena);
    REQUIRE(deserialize(f, out));
    CHECK(out == text);
    CHECK(out.get_allocator().resource() == &out_arena);
    std::string plain;
    REQUIRE(deserialize(f, plain));
    CHECK(std::string_view(plain) == std::string_view(text));
    STATIC_REQUIRE_FALSE(detail::decode_noexcept<std::pmr::string&>); // it allocates
}
