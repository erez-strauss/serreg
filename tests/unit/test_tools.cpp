// tests/unit/test_tools.cpp — v1.0.2 tools-layer items (plan §1.1, §1.5): verify runs the decoder and checks
// literal ids (gk 1.4), dump's streaming overload (cc F19), JSON 64-bit rendering (gk 1.12), diff verdicts for
// enumerator value changes / duplicate names / calls / literals (gk 1.11), dyn::dictionary conflicts (cc F16),
// srdump --version / --program-hash (cc F24); 2.1.16 (plan A5): tools::cxx_header and srdump --cxx.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace wide
{
struct Wide
{
    std::uint64_t big;
    std::int64_t neg;
    std::uint32_t small;
};
} // namespace wide

namespace
{

// A stream with a leading dictionary frame so it can be verified/dumped stand-alone.
std::vector<std::byte> stream_with_dict()
{
    std::vector<std::byte> s;
    vector_sink out{s};
    REQUIRE(write_dictionary_frame(out));
    return s;
}

dyn::type make_enum(const char* const name, const std::uint64_t schema,
                    std::initializer_list<std::pair<const char*, std::int64_t>> es)
{
    dyn::type t;
    t.name = name;
    t.schema_hash = schema;
    t.layout_hash = 0xfdccdf0b5609277bull;
    t.size = 1;
    t.align = 1;
    t.is_enum = true;
    t.is_flat = true;
    t.underlying = kind::u8;
    for (const auto& [n, v] : es)
        t.enumerators.emplace_back(n, v);
    return t;
}
dyn::type make_struct(const char* const name, const std::uint64_t schema,
                      const std::uint64_t layout)
{
    dyn::type t;
    t.name = name;
    t.schema_hash = schema;
    t.layout_hash = layout;
    t.size = 8;
    t.align = 4;
    t.is_flat = true;
    t.fields.push_back(dyn::field{"x", kind::i32, 0, 4, 4, 0, 0, 0, 0, {}});
    t.fields.push_back(dyn::field{"y", kind::i32, 4, 4, 4, 0, 0, 0, 0, {}});
    return t;
}
const tools::diff_entry* find_entry(const tools::diff_report& r, const std::string_view name)
{
    for (const auto& e : r.entries)
        if (e.name == name)
            return &e;
    return nullptr;
}
// "0x" + 16 lower-case hex digits, as every generator renders a 64-bit id (2.1.16: the cxx_header asserts)
std::string internal_hex(const std::uint64_t v)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s("0x0000000000000000");
    for (int i = 0; i < 16; ++i)
        s[static_cast<std::size_t>(17 - i)] = digits[(v >> (4 * i)) & 0xf];
    return s;
}

} // namespace

// ---- gk 1.4: verify compares the on-wire literal id with the call signature ---------------------------------
TEST_CASE("gk 1.4 a flipped literal id makes tools::verify report a problem",
          "[tools][verify][gk1.4]")
{
    SECTION("compact frame (1.1: the literal is named by the call hash, no bytes to flip — FR-L6)")
    {
        std::vector<std::byte> s = stream_with_dict();
        vector_sink out{s};
        const std::size_t at = s.size();
        REQUIRE(serialize(out, "evt"_lit, corpus::sample_order()));
        REQUIRE(tools::verify(s)->ok());
        CHECK(s.size() - at == 16 + sizeof(corpus::Order)); // the literal occupies no bytes
        s[at + 8] ^= std::byte{0x01}; // the call hash is the only place the literal exists
        const auto rep = tools::verify(s);
        REQUIRE(rep.has_value());
        CHECK_FALSE(rep->ok());
        REQUIRE(rep->problems.size() == 1);
        CHECK(rep->problems[0].find("unknown call") != std::string::npos);
        // ... and the typed reader rejects it as well
        corpus::Order o{};
        const auto d = deserialize(std::span<const std::byte>(s).subspan(at), "evt"_lit, o);
        REQUIRE_FALSE(d.has_value());
        CHECK(d.error() == error::hash_mismatch);
    }
    SECTION("full-record frame: header slot and payload are both checked")
    {
        std::vector<std::byte> s = stream_with_dict();
        vector_sink out{s};
        const std::size_t at = s.size();
        REQUIRE(serialize_with(serialize_options{.full_records = true}, out, "evt"_lit,
                               corpus::sample_order()));
        REQUIRE(tools::verify(s)->ok());
        std::vector<std::byte> a = s, b = s;
        a[at + 16] ^= std::byte{0x01};      // record header schema/id slot
        b[at + 16 + 16] ^= std::byte{0x01}; // 8-byte payload id
        for (const auto* v : {&a, &b})
        {
            const auto rep = tools::verify(*v);
            REQUIRE(rep.has_value());
            CHECK_FALSE(rep->ok());
            REQUIRE_FALSE(rep->problems.empty());
            CHECK(rep->problems[0].find("literal id mismatch") != std::string::npos);
        }
    }
    SECTION("a decode error inside a record body is a verify problem (verify runs the decoder)")
    {
        // Corrupt the enum payload of a Side record: the decoder still walks it (unknown enumerator values are data),
        // so instead shorten the frame's declared record count to create a structural mismatch.
        std::vector<std::byte> s = stream_with_dict();
        vector_sink out{s};
        const std::size_t at = s.size();
        REQUIRE(serialize(out, corpus::sample_cancel(), corpus::Side::sell));
        const std::uint16_t n = 1;
        std::memcpy(s.data() + at + 6, &n, 2);
        const auto rep = tools::verify(s);
        REQUIRE(rep.has_value());
        CHECK_FALSE(rep->ok());
    }
}

// ---- cc F19: streaming dump --------------------------------------------------------------------------------
TEST_CASE("cc F19 tools::dump streaming overload delivers per-frame chunks and stops when asked",
          "[tools][dump]")
{
    std::vector<std::byte> s = stream_with_dict();
    vector_sink out{s};
    for (int i = 0; i < 3; ++i)
        REQUIRE(serialize(out, "evt"_lit, corpus::sample_order()));
    struct ctx
    {
        std::vector<std::string> chunks;
        std::size_t stop_after = SIZE_MAX;
    };
    const auto sink = [](const std::string_view chunk, void* const p) {
        auto& c = *static_cast<ctx*>(p);
        c.chunks.emplace_back(chunk);
        return c.chunks.size() < c.stop_after;
    };
    ctx all;
    REQUIRE(tools::dump(s, nullptr, tools::dump_options{}, +sink, &all));
    std::string joined;
    for (const auto& c : all.chunks)
        joined += c;
    std::string whole;
    REQUIRE(tools::dump(s, nullptr, tools::dump_options{}, whole));
    CHECK(joined == whole);        // identical rendering
    CHECK(all.chunks.size() >= 4); // dictionary + 3 frames at least
    ctx stop;
    stop.stop_after = 2;
    const auto r = tools::dump(s, nullptr, tools::dump_options{}, +sink, &stop);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::io_error);
    CHECK(stop.chunks.size() == 2);
    // --frames A..B stops early (cc F18): only frame 1 is rendered
    tools::dump_options one;
    one.first_frame = 1;
    one.last_frame = 1;
    std::string t;
    REQUIRE(tools::dump(s, nullptr, one, t));
    CHECK(t.find("frame 1 ") != std::string::npos);
    CHECK(t.find("frame 2 ") == std::string::npos);
    CHECK(t.find("frame 0  call") == std::string::npos);
}

// ---- gk 1.12: 64-bit integers in JSON -----------------------------------------------------------------------
TEST_CASE("gk 1.12 JSON renders i64/u64 as quoted decimal strings; text mode is unchanged",
          "[tools][json][gk1.12]")
{
    std::vector<std::byte> s = stream_with_dict();
    vector_sink out{s};
    REQUIRE(serialize(out, wide::Wide{1ull << 60, -(1ll << 60), 7}));
    std::string js;
    tools::dump_options o;
    o.fmt = tools::format::json;
    REQUIRE(tools::dump(s, nullptr, o, js));
    CHECK(js.find("\"big\":\"1152921504606846976\"") != std::string::npos);
    CHECK(js.find("\"neg\":\"-1152921504606846976\"") != std::string::npos);
    CHECK(js.find("\"small\":7") != std::string::npos); // 32-bit values stay numbers
    std::string text;
    REQUIRE(tools::dump(s, nullptr, tools::dump_options{}, text));
    CHECK(text.find("big: 1152921504606846976") != std::string::npos);
    CHECK(text.find("\"1152921504606846976\"") == std::string::npos);
}

// ---- gk 1.11: diff verdicts ----------------------------------------------------------------------------------
TEST_CASE("gk 1.11 diff: enumerator value change is breaking, additions/renames are not, calls and "
          "literals are diffed",
          "[tools][diff][gk1.11]")
{
    dyn::dictionary a, b;
    a.types[0x10] = make_enum("d::Side", 0x10, {{"buy", 1}, {"sell", 2}});
    b.types[0x11] = make_enum("d::Side", 0x11, {{"buy", 1}, {"sell", 3}}); // value changed
    a.types[0x20] = make_enum("d::Venue", 0x20, {{"nyse", 0}, {"nasdaq", 5}});
    b.types[0x21] = make_enum("d::Venue", 0x21,
                              {{"nyse", 0}, {"nasdaq", 5}, {"arca", 6}}); // compatible addition
    a.types[0x30] = make_enum("d::Flag", 0x30, {{"on", 1}});
    b.types[0x31] = make_enum("d::Flag", 0x31, {{"enabled", 1}}); // renamed only
    a.types[0x40] = make_enum("d::Gone", 0x40, {{"x", 1}, {"y", 2}});
    b.types[0x41] = make_enum("d::Gone", 0x41, {{"x", 1}}); // value removed
    a.types[0x50] = make_struct("d::P", 0x50, 0xAA);
    b.types[0x51] = make_struct("d::P", 0x51, 0xAA); // same layout, other names/hash
    b.types[0x52] = make_struct("d::P", 0x52, 0xBB); // duplicate canonical name in b
    a.calls[0x100] = dyn::call{0x100, 0, {{true, 0x1, fmt::arg_literal}, {false, 0x50}}};
    b.calls[0x100] = dyn::call{0x100, 0, {{true, 0x1, fmt::arg_literal}, {false, 0x51}}}; // changed
    b.calls[0x200] = dyn::call{0x200, 0, {{false, 0x51}}};                                // added
    a.calls[0x300] = dyn::call{0x300, 0, {}};                                             // removed
    a.literals[0x1] = dyn::literal{0x1, "evt"};
    b.literals[0x1] = dyn::literal{0x1, "event"}; // changed text
    b.literals[0x2] = dyn::literal{0x2, "new"};

    const tools::diff_report r = tools::diff(a, b);
    CHECK(r.breaking);
    REQUIRE(find_entry(r, "d::Side"));
    CHECK(find_entry(r, "d::Side")->verdict == "breaking");
    REQUIRE(find_entry(r, "d::Venue"));
    CHECK(find_entry(r, "d::Venue")->verdict == "metadata-only");
    REQUIRE(find_entry(r, "d::Flag"));
    CHECK(find_entry(r, "d::Flag")->verdict == "names-only");
    REQUIRE(find_entry(r, "d::Gone"));
    CHECK(find_entry(r, "d::Gone")->verdict == "breaking");
    REQUIRE(find_entry(r, "d::P"));
    CHECK(find_entry(r, "d::P")->verdict == "names-only");
    std::size_t dup = 0;
    for (const auto& e : r.entries)
        if (e.name == "d::P" && e.verdict == "duplicate-name")
        {
            ++dup;
            CHECK(e.schema_b == 0x52);
        }
    CHECK(dup == 1);
    std::size_t call_added = 0, call_removed = 0, call_changed = 0, lit_added = 0, lit_changed = 0;
    for (const auto& e : r.entries)
    {
        if (e.verdict == "call added")
        {
            ++call_added;
            CHECK(e.schema_b == 0x200);
        }
        if (e.verdict == "call removed")
        {
            ++call_removed;
            CHECK(e.schema_a == 0x300);
        }
        if (e.verdict == "call changed")
        {
            ++call_changed;
            CHECK(e.schema_a == 0x100);
        }
        if (e.verdict == "literal added")
            ++lit_added;
        if (e.verdict == "literal changed")
            ++lit_changed;
    }
    CHECK(call_added == 1);
    CHECK(call_removed == 1);
    CHECK(call_changed == 1);
    CHECK(lit_added == 1);
    CHECK(lit_changed == 1);
    const std::string text = r.to_text();
    CHECK(text.find("BREAKING") != std::string::npos);
    CHECK(text.find("duplicate-name") != std::string::npos);
    CHECK(text.find("1 added, 1 removed, 1 changed") != std::string::npos);
    // a dictionary diffed with itself is entirely identical and not breaking
    const tools::diff_report same = tools::diff(a, a);
    CHECK_FALSE(same.breaking);
    for (const auto& e : same.entries)
        CHECK(e.verdict == "identical");
    // an enum value change alone (no other differences) is breaking
    dyn::dictionary c, d;
    c.types[0x10] = make_enum("e::S", 0x10, {{"a", 1}});
    d.types[0x11] = make_enum("e::S", 0x11, {{"a", 2}});
    CHECK(tools::diff(c, d).breaking);
    // a call added alone is not breaking in the default direction (old-data: a new reader reads old data)
    dyn::dictionary e, f;
    f.calls[0x9] = dyn::call{0x9, 0, {}};
    const tools::diff_report only_call = tools::diff(e, f);
    CHECK_FALSE(only_call.breaking);
    REQUIRE(only_call.entries.size() == 1);
    CHECK(only_call.entries[0].verdict == "call added");
}

TEST_CASE(
    "2.1.16 decision E: --diff --direction decides whether removals or additions are breaking",
    "[tools][diff][direction]")
{
    // old: Order, Cancel, ("new order", Order), (Cancel, "why"), "new order", "why"
    // new: Order unchanged, Cancel and its call and literal gone, ("fill", Order) and "fill" added
    dyn::dictionary a, b;
    a.types[0x50] = make_struct("d::Order", 0x50, 0xAA);
    b.types[0x50] = make_struct("d::Order", 0x50, 0xAA);
    a.types[0x60] = make_struct("d::Cancel", 0x60, 0xCC);
    a.calls[0x100] = dyn::call{0x100, 0, {{true, 0x1, fmt::arg_literal}, {false, 0x50}}};
    b.calls[0x100] = dyn::call{0x100, 0, {{true, 0x1, fmt::arg_literal}, {false, 0x50}}};
    a.calls[0x200] = dyn::call{0x200, 0, {{false, 0x60}, {true, 0x2, fmt::arg_literal}}};
    b.calls[0x300] = dyn::call{0x300, 0, {{true, 0x3, fmt::arg_literal}, {false, 0x50}}};
    a.literals[0x1] = dyn::literal{0x1, "new order"};
    b.literals[0x1] = dyn::literal{0x1, "new order"};
    a.literals[0x2] = dyn::literal{0x2, "why"};
    b.literals[0x3] = dyn::literal{0x3, "fill"};
    const auto breaking_of = [](const tools::diff_report& r, const std::string_view verdict) {
        std::size_t n = 0;
        for (const auto& e : r.entries)
            if (e.verdict == verdict && e.breaking)
                ++n;
        return n;
    };
    // default: old-data — the three removals break, the two additions do not
    const auto old_data = tools::diff(a, b);
    CHECK(old_data.direction == tools::diff_direction::old_data);
    CHECK(old_data.breaking);
    CHECK(old_data.breaking_removals == 3);
    CHECK(old_data.breaking_additions == 0);
    CHECK(old_data.breaking_changes == 0);
    CHECK(breaking_of(old_data, "removed") == 1);
    CHECK(breaking_of(old_data, "call removed") == 1);
    CHECK(breaking_of(old_data, "literal removed") == 1);
    CHECK(breaking_of(old_data, "call added") == 0);
    CHECK(old_data.to_text().find("BREAKING (direction old-data: 0 changed, 3 removed, 0 added)") !=
          std::string::npos);
    CHECK(old_data.to_text().find("removed         d::Cancel") != std::string::npos);
    CHECK(old_data.to_text().find(
              "breaking: removed: a reader built from the new side cannot read old "
              "frames of this type typed") != std::string::npos);
    // new-data — the two additions break, the removals do not
    const auto new_data = tools::diff(a, b, tools::diff_direction::new_data);
    CHECK(new_data.breaking);
    CHECK(new_data.breaking_removals == 0);
    CHECK(new_data.breaking_additions == 2);
    CHECK(breaking_of(new_data, "call added") == 1);
    CHECK(breaking_of(new_data, "literal added") == 1);
    CHECK(breaking_of(new_data, "removed") == 0);
    CHECK(new_data.to_text().find("BREAKING (direction new-data: 0 changed, 0 removed, 2 added)") !=
          std::string::npos);
    // both — all five; none — today's behaviour: the same seven lines, nothing breaking, exit 0
    CHECK(tools::diff(a, b, tools::diff_direction::both).breaking_removals == 3);
    CHECK(tools::diff(a, b, tools::diff_direction::both).breaking_additions == 2);
    const auto none = tools::diff(a, b, tools::diff_direction::none);
    CHECK_FALSE(none.breaking);
    CHECK(none.entries.size() == old_data.entries.size());
    CHECK(none.to_text().find("(direction none)") != std::string::npos);
    // a layout change breaks in every direction
    dyn::dictionary c, d;
    c.types[0x70] = make_struct("d::P", 0x70, 0xAA);
    d.types[0x71] = make_struct("d::P", 0x71, 0xBB);
    for (const auto dir : {tools::diff_direction::none, tools::diff_direction::old_data,
                           tools::diff_direction::new_data, tools::diff_direction::both})
    {
        const auto r = tools::diff(c, d, dir);
        CHECK(r.breaking);
        CHECK(r.breaking_changes == 1);
    }
    // the CLI: the flag and the config key set it; a bad value is a usage error
    cli::options o;
    CHECK(o.direction == tools::diff_direction::old_data);
    {
        std::string arg0 = "srdump", f1 = "--direction", f2 = "new-data", f3 = "--diff", f4 = "a",
                    f5 = "b";
        char* argv[] = {arg0.data(), f1.data(), f2.data(), f3.data(), f4.data(), f5.data()};
        CHECK(cli::parse(6, argv, o) == 0);
        CHECK(o.direction == tools::diff_direction::new_data);
        std::string bad = "sideways";
        char* argv2[] = {arg0.data(), f1.data(), bad.data()};
        CHECK(cli::parse(3, argv2, o) != 0);
    }
    {
        std::string err;
        CHECK(cli::parse_config("direction = \"both\"\n", err));
        CHECK_FALSE(cli::parse_config("direction = \"sideways\"\n", err));
        CHECK(err.find("old-data, new-data, both or none") != std::string::npos);
    }
}

// ---- cc F16: dictionary conflicts ---------------------------------------------------------------------------
TEST_CASE("cc F16 dyn::dictionary records same-hash different-body definitions as conflicts; "
          "verify reports them",
          "[tools][conflict]")
{
    dyn::dictionary a;
    REQUIRE(a.add_rri(registry::instance().export_image()));
    REQUIRE(a.conflicts.empty());
    dyn::dictionary impostor;
    dyn::type t = *a.find_type(schema_hash_v<corpus::Cancel>);
    t.layout_hash ^= 1;
    impostor.types[t.schema_hash] = t;
    dyn::dictionary merged = a;
    merged.merge(impostor);
    REQUIRE(merged.conflicts.size() == 1);
    CHECK(merged.conflicts[0].kept.layout_hash == layout_hash_v<corpus::Cancel>);
    CHECK(merged.conflicts[0].dropped.layout_hash == (layout_hash_v<corpus::Cancel> ^ 1));
    CHECK(merged.find_type(schema_hash_v<corpus::Cancel>)->layout_hash ==
          layout_hash_v<corpus::Cancel>); // first wins
    // identical duplicates are not conflicts
    dyn::dictionary twin = a;
    twin.merge(a);
    CHECK(twin.conflicts.empty());
    // the same through the RRI path: two ENTRYs for one hash in one stream
    const std::vector<std::byte> rri_a = dyn::to_rri(a), rri_i = dyn::to_rri(impostor);
    std::vector<std::byte> both(rri_a);
    both.insert(both.end(), rri_i.begin() + static_cast<std::ptrdiff_t>(fmt::rri_header_size),
                rri_i.end());
    const std::uint32_t n =
        static_cast<std::uint32_t>(a.types.size() + a.literals.size() + a.calls.size() + 1);
    std::memcpy(both.data() + 16, &n, 4);
    dyn::dictionary parsed;
    REQUIRE(parsed.add_rri(both));
    CHECK(parsed.conflicts.size() == 1);
    // verify() of a good capture with a conflicting side dictionary reports the conflict as a problem
    std::vector<std::byte> s = stream_with_dict();
    vector_sink out{s};
    REQUIRE(serialize(out, corpus::sample_cancel()));
    REQUIRE(tools::verify(s)->ok());
    const auto rep = tools::verify(s, &merged);
    REQUIRE(rep.has_value());
    CHECK_FALSE(rep->ok());
    bool mentions = false;
    for (const auto& p : rep->problems)
        if (p.find("conflict") != std::string::npos)
            mentions = true;
    CHECK(mentions);
}

// ---- cc F24: version / program hash --------------------------------------------------------------------------
TEST_CASE("cc F24 tools::library_version matches the project and cli::run knows --version",
          "[tools][cli]")
{
    CHECK(tools::library_version == "2.2.6");
    // cli::run writes to stdout; here only the exit code is observable without redirecting, so exercise parse paths.
    const char* argv_version[] = {"srdump", "--version"};
    CHECK(cli::run(2, const_cast<char**>(argv_version)) == 0);
    const char* argv_bad[] = {"srdump", "--frames"};
    CHECK(cli::run(2, const_cast<char**>(argv_bad)) == 2);
    const char* argv_none[] = {"srdump"};
    CHECK(cli::run(1, const_cast<char**>(argv_none)) == 2);
}

// ---- v1.0.3: the CLI as a library (cli::app, parse/execute) --------------------------------------------------
TEST_CASE("cli::parse hands unknown flags and extra operands to the application", "[tools][cli]")
{
    int verbose = 0;
    std::string level, second_operand;
    const cli::app my{.name = "myapp",
                      .version = "9.9",
                      .extra_usage = "  --trace              more output\n",
                      .on_extra = [&](const std::string_view a, std::span<char* const>& rest) {
                          if (a == "--trace") // (--verbose is srdump's own since 2.2.6)
                              return ++verbose, true;
                          if (a == "--depth" &&
                              !rest.empty()) // (--level is srdump's own since 2.2.1)
                          {
                              level = rest.front();
                              rest = rest.subspan(1);
                              return true;
                          }
                          if (!a.starts_with("-"))
                              return second_operand = a, true;
                          return false;
                      }};
    const char* argv1[] = {"myapp",   "--trace", "--json",      "--depth", "3",
                           "cap.ser", "more",    "--dict-only", "--trace"};
    cli::options o;
    CHECK(cli::parse(static_cast<int>(std::size(argv1)), const_cast<char**>(argv1), o, my) == 0);
    CHECK(verbose == 2);
    CHECK(level == "3");
    CHECK(o.fmt == tools::format::json);
    CHECK(o.dict_only);
    CHECK(o.capture == "cap.ser");
    CHECK(second_operand == "more");

    // a flag the application rejects is still a usage error (2), named after the application
    const char* argv2[] = {"myapp", "--bogus"};
    cli::options o2;
    CHECK(cli::parse(2, const_cast<char**>(argv2), o2, my) == 2);
    CHECK(cli::parse(2, const_cast<char**>(argv2), o2) == 2); // and without any on_extra

    // usage text carries the application's name and its own flags
    const std::string u = cli::usage(my);
    CHECK(u.starts_with("usage: myapp [options]"));
    CHECK(u.find("--trace              more output") != std::string::npos);
    CHECK(u.find("--verify") != std::string::npos);
    CHECK(cli::usage().starts_with("usage: srdump"));

    // execute() of the parsed --help/--version is exit 0; a missing capture is 2
    cli::options h;
    h.help = true;
    CHECK(cli::execute(h, my) == 0);
    cli::options v;
    v.version = true;
    CHECK(cli::execute(v, my) == 0);
    CHECK(cli::execute(cli::options{}, my) == 2);
    const char* argv3[] = {"myapp", "--version"};
    CHECK(cli::run(2, const_cast<char**>(argv3), my) == 0);
}

TEST_CASE("2.0.6 tools::for_each_frame walks data frames with the dictionary as of each frame",
          "[tools]")
{
    // A bare stream: dictionary frame, a flat frame, a println frame (both signatures already used in this binary).
    auto s = stream_with_dict();
    vector_sink out{s};
    REQUIRE(serialize(out, wide::Wide{1, -2, 3}));
    REQUIRE(println(out, "{} {}"_, "text", 5));

    std::vector<tools::frame_ref> seen;
    const auto w = tools::for_each_frame(s, [&](const dyn::dictionary& d,
                                                const tools::frame_ref& f) {
        CHECK(d.find_call(f.call_hash) == f.call); // the dictionary knows every call: it came first
        seen.push_back(f);
        return true;
    });
    REQUIRE(w);
    CHECK(w->problems.empty());
    CHECK(w->frames == 2); // the dictionary frame is consumed, not delivered
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].index == 1);
    CHECK(seen[0].offset < seen[1].offset);
    CHECK_FALSE(seen[0].is_print());
    CHECK(seen[1].is_print());
    wide::Wide back{};
    REQUIRE(deserialize(seen[0].bytes, back)); // the frame bytes feed the typed readers directly
    CHECK(back.neg == -2);
    std::string t;
    int i = 0;
    REQUIRE(deserialize_println(seen[1].bytes, "{} {}"_, t, i));
    CHECK((t == "text" && i == 5));
    const auto text = tools::render_frame(seen[1].bytes, w->dict);
    REQUIRE(text);
    CHECK(*text == "text 5\n");

    int calls = 0; // returning false stops the walk
    const auto first =
        tools::for_each_frame(s, [&](const dyn::dictionary&, const tools::frame_ref&) {
            ++calls;
            return false;
        });
    REQUIRE(first);
    CHECK((calls == 1 && first->frames == 1));

    std::vector<std::byte> junk(s.begin(),
                                s.end()); // a frame length that runs past the end: a listed problem
    junk.resize(junk.size() - 8);
    const auto cut = tools::for_each_frame(
        junk, [](const dyn::dictionary&, const tools::frame_ref&) { return true; });
    REQUIRE(cut);
    CHECK_FALSE(cut->problems.empty());
    std::vector<std::byte> bad{std::byte{'E'}, std::byte{'S'}, std::byte{'S'},
                               std::byte{'R'}}; // truncated FILE
    CHECK_FALSE(tools::for_each_frame(
        bad, [](const dyn::dictionary&, const tools::frame_ref&) { return true; }));
}

// ---- 2.1.0: format 1.3 ABI tag (FR-W4) ------------------------------------------------------------------
namespace
{
std::vector<std::byte> wchar_stream(const std::uint8_t wchar_size_in_tag)
{
    std::vector<std::byte> v;
    vector_sink s{v};
    REQUIRE(write_dictionary_frame(s).has_value());
    REQUIRE(serialize(s, wchar_t{L'A'}).has_value());
    // dictionary frame = 16-byte frame header + RRI; the ABI tag is the u64 at RRI +32, wchar size its byte 4
    v[fmt::frame_header_size + 32 + 4] = static_cast<std::byte>(wchar_size_in_tag);
    return v;
}
} // namespace

TEST_CASE("FR-W4 1.3 headers carry the writer's ABI tag", "[tools][abi]")
{
    const auto rri = registry::instance().export_image();
    std::uint64_t tag{};
    std::memcpy(&tag, rri.data() + 32, 8);
    CHECK(tag == fmt::host_abi_tag);
    CHECK(fmt::unpack_abi(tag).wchar_size == sizeof(wchar_t));
    CHECK(fmt::unpack_abi(tag).long_size == sizeof(long));
    const auto d = tools::load_dictionary_rri(rri);
    REQUIRE(d.has_value());
    CHECK(d->same_abi());
    const std::string dir = std::string(ES_SERREG_TEST_SOURCE_DIR) + "/../fixtures/golden/";
    const auto cap = tools::read_file(dir + "orders.ser");
    REQUIRE(cap.has_value());
    std::memcpy(&tag, cap->data() + 40, 8);
    CHECK(fmt::unpack_abi(tag).version == 1);
    CHECK(fmt::unpack_abi(tag).ptr_size == 8);
}

TEST_CASE(
    "FR-W4 a foreign ABI tag sizes a top-level wchar_t by the writer and is shown by the dump",
    "[tools][abi]")
{
    const auto native = wchar_stream(static_cast<std::uint8_t>(sizeof(wchar_t)));
    const auto rep_native = tools::verify(native);
    REQUIRE(rep_native.has_value());
    CHECK(rep_native->ok());
    // a Windows writer: wchar is 2 bytes; the record is still one 8-byte slot, its first 2 bytes are the value
    const auto win = wchar_stream(2);
    const auto rep = tools::verify(win);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    std::string text;
    REQUIRE(tools::dump(win, nullptr, tools::dump_options{}, text).has_value());
    CHECK(text.find("differs from this host") != std::string::npos);
    CHECK(text.find("wchar2") != std::string::npos);
    CHECK(text.find("65") != std::string::npos); // L'A' read as 2 bytes
}

TEST_CASE("FR-W4 dictionaries of two writer ABIs in one capture are an abi_mismatch problem",
          "[tools][abi]")
{
    auto a = wchar_stream(static_cast<std::uint8_t>(sizeof(wchar_t)));
    const auto b = wchar_stream(2);
    a.insert(a.end(), b.begin(), b.end());
    const auto rep = tools::verify(a);
    REQUIRE(rep.has_value());
    CHECK_FALSE(rep->ok());
    bool found = false;
    for (const auto& p : rep->problems)
        found = found || p.find("abi:") != std::string::npos;
    CHECK(found);
}

// ---- 2.1.16 (plan A5): C++ definitions generated from a dictionary (srdump --cxx) ------------------------------
TEST_CASE(
    "2.1.16 A5 tools::cxx_header regenerates the process dictionary's types with their hashes",
    "[tools][cxx]")
{
    const auto d = tools::load_dictionary_rri(registry::instance().export_image());
    REQUIRE(d.has_value());
    const std::string h = tools::cxx_header(*d);
    const auto has = [&](const std::string_view s) { return h.find(s) != std::string::npos; };
    CHECK(h.starts_with("// generated by srdump --cxx: "));
    CHECK(has("#pragma once\n#include <es/serreg/serreg.hpp>\n"));
    CHECK(has("\nnamespace corpus\n{\n"));
    CHECK(has("} // namespace corpus\n"));
    CHECK(has("enum class Side : std::uint8_t\n{\n    buy = 1,\n    sell = 2,\n};"));
    CHECK(
        has("struct Order\n{\n    std::uint64_t id;\n    std::int64_t px;\n    std::uint32_t qty;\n"
            "    corpus::Side side;\n    std::array<char, 8> sym;\n};"));
    CHECK(has("static_assert(es::serreg::schema_hash_v<corpus::Order> == " +
              internal_hex(schema_hash_v<corpus::Order>) + "ull);"));
    CHECK(has("static_assert(es::serreg::layout_hash_v<corpus::Order> == " +
              internal_hex(layout_hash_v<corpus::Order>) + "ull);"));
    CHECK(has("static_assert(sizeof(corpus::Order) == 32 && alignof(corpus::Order) == 8);"));
    CHECK(has("static_assert(offsetof(corpus::Order, sym) == 21);"));
    CHECK(has("static_assert(es::serreg::canonical_name_v<corpus::Order> == \"corpus::Order\");"));
    // natural padding is not spelled out; a gap the natural layout would not leave (a [[=skip]]ped member of the
    // original) is a skipped padding member; a flat base is a base class; annotations carry
    CHECK(has("struct Padded\n{\n    std::uint8_t tag;\n    std::uint64_t value;\n    "
              "std::uint16_t code;\n};"));
    CHECK(has("struct Skipped\n{\n    std::uint64_t key;\n    [[=es::serreg::skip]] std::byte "
              "_skip0[8];\n"
              "    std::uint32_t n;\n};"));
    CHECK(has("struct WithEmpty\n{\n    [[no_unique_address]] corpus::Empty e;\n    std::int32_t "
              "v;\n};"));
    CHECK(has("struct Derived : corpus::Base\n{"));
    CHECK(has("struct MultiDerived : corpus::Base, corpus::Tag2\n{"));
    CHECK(has("struct [[=es::serreg::since(1)]] Versioned\n{\n    std::uint32_t id;\n"
              "    [[=es::serreg::since(3)]] std::uint32_t venue;\n"
              "    [[=es::serreg::deprecated(5)]] std::uint32_t legacy;\n};"));
    // a template specialisation gets a sanitized identifier and the name annotation
    CHECK(has("struct [[=es::serreg::name(\"corpus::Vec<i32,3>\")]] Vec_i32_3\n{\n"
              "    std::array<std::int32_t, 3> v;\n};"));
    // dynamic members, the std vocabulary spelled back, a dependency (Side) declared before its user
    CHECK(has("std::variant<std::monostate, corpus::DynMsg> maybe_msg;"));
    CHECK(has("static_assert(es::serreg::schema_hash_v<std::monostate> == "));
    CHECK(h.find("enum class Side") < h.find("corpus::Side side;"));
    CHECK_FALSE(has("struct monostate"));
    CHECK_FALSE(has("install_handlers"));
    // deterministic
    CHECK(tools::cxx_header(*d) == h);
    // the router skeleton: one handler per call signature, in the forms router.hpp accepts
    tools::cxx_options ro;
    ro.router = true;
    const std::string r = tools::cxx_header(*d, ro);
    CHECK(r.find("inline void install_handlers(es::serreg::frame_router& r)\n{\n"
                 "    using namespace es::serreg::literals;\n") != std::string::npos);
    CHECK(r.find("    r.on(\"evt\"_, []([[maybe_unused]] const corpus::Order& a1) { /* TODO */ }); "
                 "// call " +
                 internal_hex(call_hash_v<lit<"evt">, corpus::Order>) + "\n") != std::string::npos);
    CHECK(r.find("    r.on([]([[maybe_unused]] corpus::Side a0) { /* TODO */ }); // call " +
                 internal_hex(call_hash_v<corpus::Side>) + "\n") != std::string::npos);
    CHECK(r.find("    r.on_println(\"px={:.2f} qty={}\"_,\n        []([[maybe_unused]] double a1, "
                 "[[maybe_unused]] std::uint32_t a2) { /* TODO */ }); // call " +
                 internal_hex(println_call_hash_v<"px={:.2f} qty={}", double, std::uint32_t>) +
                 "\n") != std::string::npos);
    CHECK(r.find("es::serreg::constant<corpus::Side::sell>") != std::string::npos);
    CHECK(r.find("r.on<std::string_view, std::int32_t, double, es::serreg::lit<\"evt\">, "
                 "corpus::Side>(") != std::string::npos);
    // --namespace: declared elsewhere, named as before
    tools::cxx_options no;
    no.namespaces.emplace_back("corpus", "regen::v2");
    const std::string n = tools::cxx_header(*d, no);
    CHECK(n.find("\nnamespace regen::v2\n{\n") != std::string::npos);
    CHECK(n.find("struct [[=es::serreg::name(\"corpus::Order\")]] Order\n{") != std::string::npos);
    CHECK(n.find("regen::v2::Side side;") != std::string::npos);
    CHECK(n.find("static_assert(es::serreg::schema_hash_v<regen::v2::Order> == " +
                 internal_hex(schema_hash_v<corpus::Order>) + "ull);") != std::string::npos);
}

TEST_CASE(
    "2.1.16 A5 cxx_header spells names a declaration cannot, and srdump --cxx refuses two types "
    "of one name",
    "[tools][cxx][cli]")
{
    dyn::dictionary d;
    d.types[0x1111] = make_struct("ns::Box<i32>", 0x1111, 0xaaaa);
    d.types[0x2222] = make_struct("ns::Outer<ns::Box<i32>>::Inner", 0x2222, 0xbbbb);
    d.types[0x3333] = make_enum("ns::Box<i32>::Kind", 0x3333, {{"a", -1}, {"b", 2}});
    d.types[0x4444] =
        make_struct("std::pair<i32,ns::Box<i32>>", 0x4444, 0xcccc); // spelled, never defined
    const std::string h = tools::cxx_header(d);
    const auto has = [&](const std::string_view s) { return h.find(s) != std::string::npos; };
    CHECK(has("struct [[=es::serreg::name(\"ns::Box<i32>\")]] Box_i32\n{"));
    CHECK(has("struct [[=es::serreg::name(\"ns::Outer<ns::Box<i32>>::Inner\")]] "
              "Outer_ns_Box_i32_Inner\n{"));
    // a class nested in a dictionary type is declared beside it, under its full canonical name
    CHECK(has(
        "enum class [[=es::serreg::name(\"ns::Box<i32>::Kind\")]] Box_i32_Kind : std::uint8_t\n{\n"
        "    a = -1,\n    b = 2,\n};"));
    CHECK_FALSE(has("struct pair"));
    CHECK(has("static_assert(es::serreg::schema_hash_v<std::pair<std::int32_t, ns::Box_i32>> == "
              "0x0000000000004444ull);"));
    CHECK(has("#include <utility>\n"));

    // the CLI: two sources whose types share a name cannot both be declared
    dyn::dictionary a, b;
    a.types[0x1111] = make_struct("dup::T", 0x1111, 0xaaaa);
    b.types[0x9999] = make_struct("dup::T", 0x9999, 0xbbbb);
    const std::string dir = ES_SERREG_TEST_TMPDIR;
    const std::string pa = dir + "/cxx_a.rri", pb = dir + "/cxx_b.rri", out = dir + "/cxx_out.hpp";
    REQUIRE(tools::write_file(pa, dyn::to_rri(a)).has_value());
    REQUIRE(tools::write_file(pb, dyn::to_rri(b)).has_value());
    const char* argv_dup[] = {"srdump",   "--dict", pa.c_str(), "--dict",
                              pb.c_str(), "--cxx",  out.c_str()};
    CHECK(cli::run(static_cast<int>(std::size(argv_dup)), const_cast<char**>(argv_dup)) == 1);
    const char* argv_one[] = {"srdump",    "--dict",       pa.c_str(),    "--cxx",
                              out.c_str(), "--cxx-router", "--namespace", "dup=one"};
    CHECK(cli::run(static_cast<int>(std::size(argv_one)), const_cast<char**>(argv_one)) == 0);
    const auto written = tools::read_file(out);
    REQUIRE(written.has_value());
    const std::string_view text(reinterpret_cast<const char*>(written->data()), written->size());
    CHECK(text.find("namespace one\n{\n\nstruct [[=es::serreg::name(\"dup::T\")]] T\n{") !=
          std::string_view::npos);
    CHECK(text.find("install_handlers") != std::string_view::npos);
    const char* argv_bad[] = {"srdump",    "--dict",      pa.c_str(), "--cxx",
                              out.c_str(), "--namespace", "dup"};
    CHECK(cli::run(static_cast<int>(std::size(argv_bad)), const_cast<char**>(argv_bad)) == 2);
    // --config keys
    std::string err;
    CHECK(cli::parse_config("cxx = \"t.hpp\"\ncxx_router = true\nnamespace = [\"a=b\", \"c=d\"]\n",
                            err));
    CHECK_FALSE(cli::parse_config("cxx_router = \"yes\"\n", err));
}

namespace holey
{
struct Holey // pahole: a 3-byte hole after `a`, 2 bytes of tail padding; size 12, payload 7
{
    std::uint8_t a;
    std::uint32_t b;
    std::uint16_t c;
};
struct Outer // nested padding: Holey's 5 bytes count as padding of Outer too
{
    Holey h;
    std::uint32_t n;
};
} // namespace holey

TEST_CASE("T-4 dictionary text: pahole-style holes, tail padding and payload/padding percentages "
          "for flat "
          "types",
          "[tools][dump][layout]")
{
    std::vector<std::byte> s = stream_with_dict();
    vector_sink out{s};
    REQUIRE(serialize(out, "evt"_lit, corpus::sample_order()));
    REQUIRE(serialize(out, holey::Outer{{1, 2, 3}, 4}));
    std::string text;
    tools::dump_options o;
    o.dict_only = true;
    REQUIRE(tools::dump(s, nullptr, o, text));
    // corpus::Order: 29 payload bytes in 32, tail padding after sym[8] at 21
    CHECK(text.contains(
        "  sym: array<char,8> @21 [8,1]\n  /* tail: 3 bytes padding */\n"
        "  /* size 32: payload 29 (90.6%), padding 3 (9.4%) = holes 0/0 B, tail 3 B */\n"));
    // holey::Holey: one hole, tail padding
    CHECK(text.contains("  a: u8 @0 [1,1]\n  /* 3 bytes hole */\n  b: u32 @4 [4,4]\n"));
    CHECK(text.contains(
        "  /* size 12: payload 7 (58.3%), padding 5 (41.7%) = holes 1/3 B, tail 2 B */\n"));
    // holey::Outer: no holes of its own, the nested member's 5 bytes are reported separately
    CHECK(text.contains(
        "  /* size 16: payload 11 (68.8%), padding 5 (31.3%) = holes 0/0 B, tail 0 B, "
        "inside nested members 5 B */\n"));
    // the totals line counts flat, non-transparent types only
    CHECK(text.contains("layout      "));
    CHECK(text.contains(" flat types, "));
}
