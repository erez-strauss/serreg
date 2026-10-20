// tests/unit/test_containers.cpp — 2.2.0, wire format 1.6 (docs/plans-for-containers.md): maps and sets as
// members (FR-A6), containers as arguments (the transparent wrapper type), canonical order for unordered
// containers, duplicate keys, the dictionary-driven readers and srdump on both.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <flat_map>
#include <flat_set>
#include <forward_list>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <stack>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

dyn::dictionary process_dictionary()
{
    const auto d = tools::load_dictionary_rri(registry::instance().export_image());
    REQUIRE(d.has_value());
    return *d;
}

} // namespace

// Types of this test; a named namespace, since a type in an unnamed namespace has no unique canonical name.
namespace tc
{
struct Ordered
{
    std::map<std::int32_t, std::int32_t> m;
    bool operator==(const Ordered&) const = default;
};
struct Unordered
{
    std::unordered_map<std::int32_t, std::int32_t> m;
    bool operator==(const Unordered&) const = default;
};
struct Flat
{
    std::flat_map<std::int32_t, std::int32_t> m;
    std::flat_set<std::string> s;
    bool operator==(const Flat&) const = default;
};
struct Sets
{
    std::set<std::string> a;
    std::unordered_set<std::string> b;
    bool operator==(const Sets&) const = default;
};
struct Nested
{
    std::map<std::string, std::vector<std::int32_t>> lists;
    std::vector<std::map<std::uint8_t, std::uint8_t>> maps;
    std::map<std::string, std::map<std::string, std::string>> deep;
    bool operator==(const Nested&) const = default;
};
struct Key
{ // a trivially copyable key without operator<: compared by its bytes when an unordered container is sorted
    std::uint16_t a;
    std::uint8_t b;
    bool operator==(const Key&) const = default;
};
struct KeyHash
{
    std::size_t operator()(const Key& k) const noexcept
    {
        return k.a * 31u + k.b;
    }
};
struct ByBytes
{
    std::unordered_map<Key, std::int32_t, KeyHash> m;
    bool operator==(const ByBytes&) const = default;
};
// component registration through a container: used nowhere but as a map value / set key
struct OnlyMapped
{
    std::uint32_t a;
};
struct OnlyKey
{
    std::uint16_t k;
    bool operator<(const OnlyKey& o) const
    {
        return k < o.k;
    }
};
struct Owner
{
    std::map<std::int32_t, OnlyMapped> m;
    std::set<OnlyKey> s;
};
} // namespace tc

TEST_CASE("FR-A6 maps and sets round-trip as members: ordered, unordered, flat, nested (1.6)",
          "[containers][roundtrip]")
{
    SECTION("the corpus type: string-keyed map, set, unordered map, map of dynamic values")
    {
        const corpus::DynMap in = corpus::sample_dyn_map();
        corpus::DynMap out{};
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
    }
    SECTION("empty containers are the zero slot")
    {
        const corpus::DynMap in{9, {}, {}, {}, {}};
        const auto f = frame_of(in);
        // 16 header + u32 len, u32 0 + the fixed part (4 + 4 slots of 8 = 36, aligned to 40 by the 8-byte round)
        CHECK(f.size() == 16 + 8 + 40);
        corpus::DynMap out = corpus::sample_dyn_map();
        REQUIRE(deserialize(f, out));
        CHECK(out == in);
    }
    SECTION("flat_map / flat_set")
    {
        tc::Flat in;
        in.m = {{3, 30}, {1, 10}, {2, 20}};
        in.s = {"z", "a", "m"};
        tc::Flat out;
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
    }
    SECTION("sets, ordered and unordered")
    {
        const tc::Sets in{{"b", "a", "c"}, {"y", "x", "z"}};
        tc::Sets out;
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
    }
    SECTION("nested: map of vectors, vector of maps, map of maps")
    {
        tc::Nested in;
        in.lists = {{"odd", {1, 3, 5}}, {"even", {}}, {"one", {1}}};
        in.maps = {{{1, 2}}, {}, {{3, 4}, {5, 6}}};
        in.deep = {{"k", {{"a", "1"}, {"b", "2"}}}, {"empty", {}}};
        tc::Nested out;
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
    }
    SECTION("a trivially copyable key without operator< is ordered by its bytes")
    {
        tc::ByBytes in;
        in.m = {{{2, 1}, 21}, {{1, 9}, 19}, {{1, 2}, 12}};
        tc::ByBytes out;
        REQUIRE(deserialize(frame_of(in), out));
        CHECK(out == in);
        // insertion order does not reach the wire
        tc::ByBytes other;
        other.m = {{{1, 2}, 12}, {{2, 1}, 21}, {{1, 9}, 19}};
        CHECK(frame_of(in) == frame_of(other));
    }
}

TEST_CASE(
    "FR-A6 canonical order: identical values give identical bytes whatever the container (1.6)",
    "[containers][determinism]")
{
    // an unordered map filled in two orders (and grown through rehashes) encodes the same bytes
    tc::Unordered a, b;
    for (int i = 0; i < 300; ++i)
        a.m.emplace(i * 7919 % 1000, i);
    for (int i = 299; i >= 0; --i)
        b.m.emplace(i * 7919 % 1000, i);
    REQUIRE(a.m == b.m);
    CHECK(frame_of(a) == frame_of(b));
    // the same entries in a std::map: the same image bytes (the frames differ only in the call hash)
    tc::Ordered o;
    for (const auto& [k, v] : a.m)
        o.m.emplace(k, v);
    const auto fa = frame_of(a), fo = frame_of(o);
    REQUIRE(fa.size() == fo.size());
    CHECK(std::memcmp(fa.data() + 16, fo.data() + 16, fa.size() - 16) == 0);
    // same wire shape ⇒ same layout hash; different C++ container ⇒ different schema hash (the name)
    STATIC_REQUIRE(layout_hash_v<std::map<std::int32_t, std::int32_t>> ==
                   layout_hash_v<std::unordered_map<std::int32_t, std::int32_t>>);
    STATIC_REQUIRE(schema_hash_v<std::map<std::int32_t, std::int32_t>> !=
                   schema_hash_v<std::unordered_map<std::int32_t, std::int32_t>>);
    // an unordered set of strings is written as the sorted set is
    const tc::Sets s{{"b", "a", "c"}, {"b", "a", "c"}};
    const auto fs = frame_of(s);
    const std::span<const std::byte> all = fs;
    // both slots point at identical 8-byte entry blocks of three string slots each; compare the two tails
    const auto slot = [&](const std::size_t at) {
        std::uint32_t off, n;
        std::memcpy(&off, all.data() + 24 + at, 4);
        std::memcpy(&n, all.data() + 24 + at + 4, 4);
        return std::pair{off, n};
    };
    const auto [oa, na] = slot(0);
    const auto [ob, nb] = slot(8);
    REQUIRE((na == 3 && nb == 3));
    // the three strings of each set, in wire order (entry i of a set of strings is one string slot)
    const auto text_of = [&](const std::uint32_t block) {
        std::string t;
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            std::uint32_t so, sn;
            std::memcpy(&so, all.data() + 24 + block + i * 8, 4);
            std::memcpy(&sn, all.data() + 24 + block + i * 8 + 4, 4);
            t.append(reinterpret_cast<const char*>(all.data() + 24 + so), sn);
            t += '|';
        }
        return t;
    };
    CHECK(text_of(oa) == "a|b|c|");
    CHECK(text_of(ob) == "a|b|c|");
}

TEST_CASE("FR-A6 a duplicate key on the wire is invalid_value; the first entry is kept (1.6)",
          "[containers][errors]")
{
    tc::Ordered in;
    in.m = {{1, 10}, {2, 20}};
    auto f = frame_of(in);
    // image at 24: slot {off = 8, n = 2}; entries of 8 bytes (i32 key at 0, i32 value at 4) at image + 8
    std::uint32_t off, n;
    std::memcpy(&off, f.data() + 24, 4);
    std::memcpy(&n, f.data() + 28, 4);
    REQUIRE((off == 8 && n == 2));
    const std::int32_t one = 1;
    std::memcpy(f.data() + 24 + off + 8, &one, 4); // the second key := 1
    tc::Ordered out;
    const auto r = deserialize(f, out);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::invalid_value);
    CHECK(r.error().failed == 1);
    REQUIRE(out.m.size() == 1);
    CHECK(out.m.at(1) == 10);
    // the same image through an unordered map
    tc::Unordered uo;
    std::vector<std::byte> fu = frame_of(tc::Unordered{{{1, 10}, {2, 20}}});
    std::memcpy(fu.data() + 24 + off + 8, &one, 4);
    const auto ru = deserialize(fu, uo);
    REQUIRE_FALSE(ru.has_value());
    CHECK(ru.error() == error::invalid_value);
    CHECK(uo.m.size() == 1);
    // the dictionary-driven verify accepts the bytes (they are well-formed); dump shows both entries
    const dyn::dictionary d = process_dictionary();
    const auto rep = tools::verify(f, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
}

TEST_CASE("FR-A6 a container is an argument of its own: the transparent wrapper type (1.6)",
          "[containers][argument]")
{
    const std::vector<std::int32_t> ids{3, 1, 2};
    // compact: header + record (u32 len, u32 0) + image (slot 8 + 3 × 4 = 20, padded to 24)
    const auto f = frame_of("ids"_, ids);
    CHECK(f.size() == 16 + 8 + 24);
    STATIC_REQUIRE(arg_kind_v<std::vector<std::int32_t>> == fmt::arg_type);
    STATIC_REQUIRE(arg_hash_v<std::vector<std::int32_t>> ==
                   schema_hash_v<std::vector<std::int32_t>>);
    STATIC_REQUIRE(canonical_name_v<std::vector<std::int32_t>> ==
                   std::string_view("std::vector<i32>"));
    STATIC_REQUIRE(dynamic_type<std::vector<std::int32_t>>);
    STATIC_REQUIRE(dynamic_type<std::map<std::string, double>>);
    STATIC_REQUIRE(!dynamic_type<std::string>); // a string argument stays the string category
    STATIC_REQUIRE(detail::is_string_arg_v<std::string>);
    {
        std::vector<std::int32_t> out;
        const auto r = deserialize(f, "ids"_, out);
        REQUIRE(r.has_value());
        CHECK(out == ids);
        // another element type is another type: refused as a frame, nothing written
        std::vector<std::int64_t> wide;
        const auto w = deserialize(f, "ids"_, wide);
        REQUIRE_FALSE(w.has_value());
        CHECK(w.error() == decode_error{error::hash_mismatch, 3, 0, 0});
    }
    { // empty, compact and full records
        const auto e = frame_of("ids"_, std::vector<std::int32_t>{});
        CHECK(e.size() == 16 + 8 + 8);
        std::vector<std::int32_t> out{9};
        REQUIRE(deserialize(e, "ids"_, out));
        CHECK(out.empty());
        std::vector<std::byte> full;
        vector_sink s{full};
        REQUIRE(
            serialize_with(serialize_options{.full_records = true}, s, "ids"_, ids).has_value());
        std::vector<std::int32_t> back;
        REQUIRE(deserialize(full, "ids"_, back));
        CHECK(back == ids);
    }
    { // maps, optionals and variants as arguments; several in one frame
        std::map<std::string, double> px{{"a", 1.5}, {"b", 2.5}};
        std::optional<corpus::Order> maybe = corpus::sample_order();
        std::variant<int, std::string> v = std::string("text");
        const auto g = frame_of(px, maybe, v, std::set<int>{3, 1});
        std::map<std::string, double> px2;
        std::optional<corpus::Order> m2;
        std::variant<int, std::string> v2;
        std::set<int> s2;
        REQUIRE(deserialize(g, px2, m2, v2, s2));
        CHECK((px2 == px && m2 == maybe && v2 == v && s2 == std::set<int>{1, 3}));
    }
    { // the registry has the wrapper entry: one unnamed field at 0, transparent
        const type_desc* const t = registry::instance().find_type_by_name("std::vector<i32>");
        REQUIRE(t != nullptr);
        CHECK(t->transparent);
        CHECK_FALSE(t->is_flat);
        CHECK(t->size == 8);
        REQUIRE(t->fields.size() == 1);
        CHECK((t->fields[0].name.empty() && t->fields[0].offset == 0 &&
               t->fields[0].k == kind::vector_));
        const dyn::dictionary d = process_dictionary();
        const dyn::type* const dt = d.find_type(schema_hash_v<std::vector<std::int32_t>>);
        REQUIRE(dt != nullptr);
        CHECK(dt->transparent);
        CHECK(d.to_json().find("\"transparent\": true") != std::string::npos);
        const auto again = tools::load_dictionary_rri(dyn::to_rri(d));
        REQUIRE(again.has_value());
        CHECK(again->types == d.types);
    }
    { // inspect, dispatch, the router
        const auto fv = inspect(f);
        REQUIRE(fv.has_value());
        std::vector<record_view> recs;
        for (const record_view r : fv->records())
            recs.push_back(r);
        REQUIRE(recs.size() == 2);
        CHECK(recs[1].ak == fmt::arg_type);
        CHECK(recs[1].hash == schema_hash_v<std::vector<std::int32_t>>);
        int seen = 0;
        const auto dr = dispatch<std::vector<std::int32_t>>(f, [&](const auto& x) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>,
                                         std::vector<std::int32_t>>)
                seen += x == ids;
        });
        REQUIRE(dr.has_value());
        CHECK(seen == 1);
        frame_router router;
        int routed = 0;
        router.on("ids"_, [&](const std::vector<std::int32_t>& x) { routed += x == ids; });
        REQUIRE(router.route(f).has_value());
        CHECK(routed == 1);
    }
}

TEST_CASE(
    "FR-A6 srdump shows maps as {key: value}, sets as {…}, a container argument as the value (1.6)",
    "[containers][tools]")
{
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(serialize(s, "map"_, corpus::sample_dyn_map()).has_value());
    const std::size_t f1 = v.size();
    const std::vector<std::int32_t> ids{3, 1, 2};
    REQUIRE(serialize(s, "ids"_, ids).has_value());
    const std::size_t f2 = v.size();
    REQUIRE(println(s, "ids={} map={}"_, ids, tc::Ordered{{{1, 10}}}).has_value());
    const dyn::dictionary d = process_dictionary();
    const std::span<const std::byte> all = v;
    std::string text;
    REQUIRE(tools::dump(all.first(f1), &d, {}, text));
    INFO(text);
    CHECK(text.find("by_name: {\"a\": 1, \"b\": 2, \"c\": 3}") != std::string::npos);
    CHECK(text.find("ids: {10, 20, 30}") != std::string::npos);
    CHECK(text.find("names: {-1: \"minus one\", 4: \"four\", 5: \"five\"}") != std::string::npos);
    std::string json;
    tools::dump_options jo;
    jo.fmt = tools::format::json;
    REQUIRE(tools::dump(all.first(f1), &d, jo, json));
    INFO(json);
    CHECK(json.find("\"by_name\":[{\"key\":\"a\",\"value\":1},{\"key\":\"b\",\"value\":2},{\"key\":"
                    "\"c\",\"value\":3}]") != std::string::npos);
    CHECK(json.find("\"ids\":[10,20,30]") != std::string::npos);
    std::string vec;
    REQUIRE(tools::dump(all.subspan(f1, f2 - f1), &d, {}, vec));
    INFO(vec);
    CHECK(vec.find("[1] [3, 1, 2]") != std::string::npos); // the value, no struct around it
    std::string vjson;
    REQUIRE(tools::dump(all.subspan(f1, f2 - f1), &d, jo, vjson));
    CHECK(vjson.find("\"value\":[3,1,2]") != std::string::npos);
    const auto msg = tools::render_frame(all.subspan(f2), d);
    REQUIRE(msg.has_value());
    CHECK(*msg == "ids=[3, 1, 2] map=tc::Ordered{m: {1: 10}}\n");
    // verify decodes every record through the dictionary walker
    const auto rep = tools::verify(all, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->records == 7);
}

TEST_CASE("FR-A6 a map's key and mapped types and a set's key type register with their owner (1.6)",
          "[containers][registry]")
{
    // tc::OnlyMapped and tc::OnlyKey appear in no other call site of this binary; the owner registers them as
    // vector/optional/variant element types have registered since 2.0, so a dictionary-driven reader of the
    // owner's frames finds every type_hash it meets
    register_types<tc::Owner>();
    const registry& r = registry::instance();
    REQUIRE(r.find_type(schema_hash_v<tc::Owner>) != nullptr);
    CHECK(r.find_type(schema_hash_v<tc::OnlyMapped>) != nullptr);
    CHECK(r.find_type(schema_hash_v<tc::OnlyKey>) != nullptr);
    const dyn::dictionary d = process_dictionary();
    CHECK(d.find_type(schema_hash_v<tc::OnlyMapped>) != nullptr);
    CHECK(d.find_type(schema_hash_v<tc::OnlyKey>) != nullptr);
    // and a frame of the owner verifies with the process dictionary alone
    tc::Owner o;
    o.m = {{1, {10}}, {2, {20}}};
    o.s = {{3}, {1}};
    const auto f = frame_of(o);
    const auto rep = tools::verify(f, &d);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
}

// ---- sequences, tuple-like types, multimaps and std::stack (2.2.3 vocabulary) -------------------------------
namespace tc
{
struct Seqs
{
    std::deque<std::int32_t> d;
    std::list<std::string> l;
    std::forward_list<std::uint8_t> f;
    std::stack<std::int16_t> st;
    bool operator==(const Seqs&) const = default;
};
struct Tuples
{
    std::uint8_t tag;
    std::pair<std::int32_t, double> p;
    std::tuple<std::uint8_t, std::string, std::int64_t> t;
    std::vector<std::pair<std::string, std::int32_t>> vp;
    bool operator==(const Tuples&) const = default;
};
struct Multis
{
    std::multimap<std::int32_t, std::string> mm;
    std::unordered_multimap<std::int32_t, std::int32_t> um;
    bool operator==(const Multis&) const = default;
};
} // namespace tc

TEST_CASE("FR-A6 std::deque, std::list, std::forward_list and std::stack are the vector shape",
          "[containers][sequences]")
{
    tc::Seqs in{{1, 2, 3}, {"a", "", "ccc"}, {7, 8}, {}};
    in.st.push(10);
    in.st.push(20);
    const auto f = frame_of(in);
    tc::Seqs out;
    REQUIRE(deserialize(f, out));
    CHECK(out == in);
    CHECK(out.st.top() == 20); // bottom first on the wire, so the top is the top again
    // a std::deque<i32> member and a std::vector<i32> member write identical slots and blocks
    {
        std::vector<std::int32_t> v{1, 2, 3};
        std::deque<std::int32_t> d{1, 2, 3};
        const auto fv = frame_of("ids"_, v), fd = frame_of("ids"_, d);
        REQUIRE(fv.size() == fd.size());
        CHECK(std::memcmp(fv.data() + 16, fd.data() + 16, fv.size() - 16) == 0);
        // and the deque argument reads back into a vector of the same shape only through its own schema hash
        std::deque<std::int32_t> back;
        REQUIRE(deserialize(fd, "ids"_, back));
        CHECK(back == d);
    }
    // the dictionary reader sees a vector
    const auto d = process_dictionary();
    const dyn::type* t = d.find_type(schema_hash_v<tc::Seqs>);
    REQUIRE(t != nullptr);
    REQUIRE(t->fields.size() == 4);
    for (const dyn::field& fld : t->fields)
        CHECK(fld.k == kind::vector_);
    std::string text;
    REQUIRE(tools::dump(f, &d, {}, text));
    CHECK(text.find("st: [10, 20]") != std::string::npos);
}

TEST_CASE("FR-A6 std::pair and std::tuple are positional parts at canonical offsets, encoded "
          "through std::get",
          "[containers][tuple]")
{
    // the image of std::pair<i32, f64>{7, 2.5}: i32 at 0, 4 bytes of padding, f64 at 8 — whatever the standard
    // library's object layout is
    const std::pair<std::int32_t, double> p{7, 2.5};
    const auto fp = frame_of(p);
    const std::uint8_t image[] = {7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0x40};
    REQUIRE(fp.size() == 16 + 8 + 16); // header, u32 len + u32 0, the 16-byte image
    CHECK(std::memcmp(fp.data() + 24, image, 16) == 0);
    std::pair<std::int32_t, double> pb;
    REQUIRE(deserialize(fp, pb));
    CHECK(pb == p);

    tc::Tuples in{9, {-1, 0.5}, {3, "x", -7}, {{"k", 1}, {"", 2}}};
    const auto f = frame_of(in);
    tc::Tuples out;
    REQUIRE(deserialize(f, out));
    CHECK(out == in);
    // the dictionary names the parts
    const auto d = process_dictionary();
    const dyn::type* tp = d.find_type(schema_hash_v<std::pair<std::int32_t, double>>);
    REQUIRE(tp != nullptr);
    REQUIRE(tp->fields.size() == 2);
    CHECK(tp->fields[0].name == "first");
    CHECK(tp->fields[1].name == "second");
    CHECK(tp->fields[1].offset == 8);
    const dyn::type* tt =
        d.find_type(schema_hash_v<std::tuple<std::uint8_t, std::string, std::int64_t>>);
    REQUIRE(tt != nullptr);
    REQUIRE(tt->fields.size() == 3);
    CHECK(tt->fields[1].name == "_1");
    CHECK(tt->fields[1].k == kind::string_);
    std::string text;
    REQUIRE(tools::dump(f, &d, {}, text));
    CHECK(text.find("first: -1") != std::string::npos);
    CHECK(text.find("_2: -7") != std::string::npos);
}

TEST_CASE("FR-A6 std::multimap and std::unordered_multimap are a vector of pairs sorted by key",
          "[containers][multimap]")
{
    tc::Multis in;
    in.mm.emplace(2, "b1");
    in.mm.emplace(1, "a");
    in.mm.emplace(
        2, "b2"); // insertion order among equal keys is the container's order, and the wire's
    in.um.emplace(5, 50);
    in.um.emplace(3, 30);
    in.um.emplace(5, 51);
    const auto f = frame_of(in);
    tc::Multis out;
    REQUIRE(deserialize(f, out));
    CHECK(out.mm == in.mm);
    CHECK(out.um.size() == 3);
    CHECK(out.um.count(5) == 2);
    // the unordered multimap's entries are written sorted by key: the same keys in another insertion order give
    // the same bytes when the values per key are inserted in the same order
    tc::Multis b;
    b.um.emplace(3, 30);
    b.um.emplace(5, 50);
    b.um.emplace(5, 51);
    b.mm = in.mm;
    CHECK(frame_of(b) == f);
    // in the dictionary: a vector field whose element is the pair type
    const auto d = process_dictionary();
    const dyn::type* t = d.find_type(schema_hash_v<tc::Multis>);
    REQUIRE(t != nullptr);
    CHECK(t->fields[0].k == kind::vector_);
    REQUIRE(t->fields[0].elem.size() == 1);
    CHECK(t->fields[0].elem[0].type_hash == schema_hash_v<std::pair<std::int32_t, std::string>>);
    std::string text;
    REQUIRE(tools::dump(f, &d, {}, text));
    CHECK(text.find("second: \"b1\"") != std::string::npos);
}

// serialize_options::canonical_unordered — A-19: the default sorts an unordered container by key (identical
// values ⇒ identical bytes); false writes its iteration order; readers accept both; an ordered container is
// unaffected.
TEST_CASE(
    "FR-A6 serialize_options::canonical_unordered: sorted by default, iteration order on request, "
    "readers take both",
    "[containers][options]")
{
    tc::Unordered u;
    for (std::int32_t i = 0; i < 200;
         ++i) // above the 64-entry stack table: the sort allocates once
        u.m.emplace(i * 7919 % 1009, i);
    tc::Ordered o;
    for (const auto& [k, v] : u.m)
        o.m.emplace(k, v);
    const auto canon = [&](const auto& x, const bool c) {
        std::vector<std::byte> buf;
        vector_sink s{buf};
        REQUIRE(serialize_with(serialize_options{.canonical_unordered = c}, s, x).has_value());
        return buf;
    };
    const auto bu = canon(u, true), bu_iter = canon(u, false);
    const auto bo = canon(o, true), bo_iter = canon(o, false);
    // the canonical unordered image equals the ordered one after the frame header's call hash (8 bytes)
    REQUIRE(bu.size() == bo.size());
    CHECK(std::equal(bu.begin() + 16, bu.end(), bo.begin() + 16));
    // an ordered container is written the same way whatever the option says
    CHECK(bo == bo_iter);
    // iteration order: same length, same value back, through every reader
    CHECK(bu_iter.size() == bu.size());
    tc::Unordered back{};
    REQUIRE(deserialize(bu_iter, back).has_value());
    CHECK(back == u);
    const auto d = tools::load_dictionary_rri(registry::instance().export_image());
    REQUIRE(d.has_value());
    std::string text;
    REQUIRE(tools::dump(bu_iter, &*d, {}, text).has_value());
    CHECK(text.find("m:") != std::string::npos);
    // the bytes differ only if the container happens to iterate out of key order (implementation-defined);
    // what is guaranteed is that the canonical form never depends on it
    std::unordered_map<std::int32_t, std::int32_t> other(
        1); // another bucket count: another iteration order
    for (auto it = o.m.rbegin(); it != o.m.rend(); ++it)
        other.emplace(*it);
    CHECK(canon(tc::Unordered{other}, true) == bu);
}

// 2.2.6 (review B2): a floating-point key is ordered by std::strong_order, not operator< (which is not a strict
// weak order once a NaN key is in the container). Frozen bytes: -inf, +0, +inf, quiet NaN — IEEE totalOrder.
TEST_CASE("FR-W9 B2 golden: unordered_map<double, i32> keys are written in std::strong_order "
          "(-inf, +0, +inf, NaN)",
          "[containers][golden][B2]")
{
    std::unordered_map<double, std::int32_t> m;
    m.emplace(std::numeric_limits<double>::quiet_NaN(), 4);
    m.emplace(std::numeric_limits<double>::infinity(), 3);
    m.emplace(0.0, 2);
    m.emplace(-std::numeric_limits<double>::infinity(), 1);
    REQUIRE(m.size() == 4); // a NaN key is admitted: the container compares with == only on lookup
    // frozen 2026-10-19 from GCC 16.2 -std=c++26 -freflection, x86-64: header (len 96, flags 0, n 1, call hash),
    // record (u32 72, u32 0), fixed part (slot off 8, n 4), four 16-byte entries {f64 key, i32 value, 4 pad}
    static constexpr unsigned char expected[96] = {
        0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x43, 0x4c, 0xbb, 0x3e, 0x45, 0x9f,
        0x12, 0x9c, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0xff, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xf0, 0x7f, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xf8, 0x7f, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const auto f = frame_of(m);
    REQUIRE(f.size() == sizeof expected);
    const bool same = std::memcmp(f.data(), expected, sizeof expected) == 0;
    CHECK(same);
    // the same multiset of keys in another iteration order gives the same bytes
    std::unordered_map<double, std::int32_t> other(1);
    for (const auto& [k, v] : m)
        other.emplace(k, v);
    CHECK(frame_of(other) == f);
    // the comparator itself: IEEE totalOrder, -0 before +0, NaN after +inf; noexcept for floating point
    STATIC_REQUIRE(noexcept(detail::key_less(0.0, 1.0)));
    CHECK(detail::key_less(-0.0, 0.0));
    CHECK_FALSE(detail::key_less(0.0, -0.0));
    CHECK(detail::key_less(std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(detail::key_less(std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN()));
}

// 2.2.6 (review B7): the writer's size arithmetic is checked — a sequence whose size() × stride overflows size_t
// (or passes the u32 image limit) makes serialize() return error::invalid_argument instead of wrapping into a
// small image. The lying sequence is a std::stack's underlying container: the one place the vocabulary takes a
// user-defined sequence (the adapter's protected member is walked through sequence_of).
namespace tc
{
struct Wide
{
    std::uint64_t a;
    std::uint64_t b;
};
struct LyingSeq
{ // size() claims SIZE_MAX / 4 + 1 elements of 16 bytes: the product wraps to 0 in size_t
    LyingSeq() = default;
    ~LyingSeq()
    {
    } // not trivially copyable, so the stack is a dynamic type and not flat bytes
    using value_type = Wide;
    using reference = Wide&;
    using const_reference = const Wide&;
    using size_type = std::size_t;
    std::size_t size() const noexcept
    {
        return SIZE_MAX / 4 + 1;
    }
    bool empty() const noexcept
    {
        return false;
    }
    const Wide* begin() const noexcept
    {
        return nullptr;
    }
    const Wide* end() const noexcept
    {
        return nullptr;
    }
    void push_back(const Wide&)
    {
    }
    void pop_back()
    {
    }
    Wide& back()
    {
        return scratch;
    }
    const Wide& back() const
    {
        return scratch;
    }
    Wide scratch{};
};
} // namespace tc

TEST_CASE("B7 a sequence whose size() x element stride overflows size_t is refused with "
          "invalid_argument, not wrapped into a small image",
          "[containers][overflow][B7]")
{
    // the checked block arithmetic itself
    std::size_t tail = 10;
    detail::add_block(tail, 8, 3, 16);
    CHECK(tail == 16 + 48);
    detail::add_block(tail, 8, SIZE_MAX / 4 + 1, 16); // the product overflows
    CHECK(tail == detail::measure_limit);
    detail::add_block(tail, 1, 1, 1); // and stays saturated
    CHECK(tail == detail::measure_limit);
    tail = 0;
    detail::add_block(tail, 1, std::size_t{1} << 31,
                      2); // exactly the limit: refused too (> u32 max)
    CHECK(tail == detail::measure_limit);
    tail = 0;
    detail::add_block(tail, 1, 1, 0xFFFFFFFFu); // the largest image that still fits a u32 length
    CHECK(tail == 0xFFFFFFFFu);
    // through serialize(): a lying sequence behind std::stack
    using lying_stack = std::stack<tc::Wide, tc::LyingSeq>;
    STATIC_REQUIRE(dynamic_type<lying_stack>);
    const lying_stack s;
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const auto r = serialize(out, s);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::invalid_argument);
    CHECK(buf.empty());
    alignas(16) std::byte stack_buf[256];
    span_sink sp{stack_buf};
    const auto r2 = serialize(sp, s);
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error() == error::invalid_argument);
    CHECK(sp.used == 0);
}
