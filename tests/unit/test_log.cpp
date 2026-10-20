// tests/unit/test_log.cpp — 2.2.1 (format 1.7, DECISIONS A-* logging): the logger over a scoped capture file,
// the level gate (no argument evaluation below the level), the compile-time floor, the envelope by policy
// (thread id, sequence, clock), the scoped dictionary (only the scope's calls, their literals and types),
// srdump's --level / --since / --until rendering, frame_router::on_log, and a logger over a plain sink with the
// full dictionary.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace logtest
{
struct app_scope
{
};
struct other_scope
{
};
struct Pos
{
    std::int32_t id;
    double qty;
};
} // namespace logtest

namespace
{
std::uint64_t ticks = 1'000;
std::uint64_t test_clock() noexcept
{
    return ticks += 10; // strictly increasing, deterministic
}
std::string tmp_path(const std::string& name)
{
    return std::string(ES_SERREG_TEST_TMPDIR) + "/" + name;
}
using file_t = scoped_capture_file<logtest::app_scope>;
constexpr log_options plain{.clock = clock_policy::user};
constexpr log_options traced{.thread = thread_policy::per_message,
                             .sequence = sequence_policy::per_sink,
                             .clock = clock_policy::user};
constexpr log_options floored{.floor = level::warn, .clock = clock_policy::user};
} // namespace

TEST_CASE("A log: the gate evaluates nothing below the level; the floor compiles the call out",
          "[log][gate]")
{
    const std::string path = tmp_path("log_gate.ser");
    capture_options co;
    co.info_host = false;
    co.clock = &test_clock;
    file_t f(path, co);
    REQUIRE(f.is_open());
    logger<plain, file_t> lg{f, level::info};
    lg.set_clock(&test_clock);
    int evaluated = 0;
    const auto cost = [&] {
        ++evaluated;
        return 7;
    };
    CHECK_FALSE(ES_SERREG_LOG(lg, debug, "debug {}"_, cost()));
    CHECK(evaluated == 0);
    CHECK(ES_SERREG_LOG(lg, info, "info {}"_, cost()));
    CHECK(evaluated == 1);
    CHECK_FALSE(lg.enabled(level::trace));
    lg.set_level(level::trace);
    CHECK(lg.enabled(level::trace));
    CHECK(lg.trace("now {}"_, 1));
    CHECK(lg.failed() == 0);

    logger<floored, file_t> fl{f};
    STATIC_REQUIRE(decltype(fl)::floor == level::warn);
    CHECK_FALSE(fl.enabled(level::info));
    CHECK(fl.info("never registered {}"_,
                  cost())); // compiled out: true — but a member call evaluates its
    CHECK(evaluated == 2);  // arguments; only the macro skips them
    CHECK_FALSE(ES_SERREG_LOG(fl, info, "never {}"_, cost()));
    CHECK(evaluated == 2);
    CHECK(fl.warn("floored {}"_, 2));
    REQUIRE(f.close());

    const auto rep = tools::verify(path);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 3); // info, trace, warn
    ::unlink(path.c_str());
}

TEST_CASE("A log: the envelope by policy — level, file:line, time, thread id, sequence — renders "
          "in srdump",
          "[log][render]")
{
    const std::string path = tmp_path("log_render.ser");
    capture_options co;
    co.info_host = false;
    co.clock = &test_clock;
    co.log_clock = "user";
    co.info_thread = true;
    file_t f(path, co);
    REQUIRE(f.is_open());
    logger<plain, file_t> a{f};
    logger<traced, file_t> b{f};
    a.set_clock(&test_clock);
    b.set_clock(&test_clock);
    const std::uint64_t t0 = ticks;
    const int line_a = __LINE__ + 1;
    REQUIRE(ES_SERREG_LOG(a, info, "started with {} workers"_, 4));
    REQUIRE(a.warn("book {} depth {}"_, "AAPL", 7));
    REQUIRE(ES_SERREG_LOG(b, error, "px={:.2f} order={}"_, 1.25, 42));
    REQUIRE(ES_SERREG_LOG(b, error, "second {}"_, 2));
    REQUIRE(f.close());

    // the dictionary: log calls, with the envelope constants
    const auto ci = tools::info(path);
    REQUIRE(ci.has_value());
    CHECK(ci->dict.calls.size() >=
          4); // every log call of this binary under the scope (test cases share it)
    CHECK(ci->writer_item(fmt::info_dictionary) == "scoped:logtest::app_scope");
    CHECK(ci->writer_item(fmt::info_clock) == "user");
    CHECK(!ci->writer_item(fmt::info_thread).empty());
    for (const auto& [h, k] : ci->dict.calls)
    {
        CHECK(k.is_log());
        CHECK(k.is_print());
        CHECK(k.newline());
        REQUIRE(k.args.size() >= 6);
        CHECK(k.args[log_layout::level_arg].ak == fmt::arg_const);
        CHECK(k.args[log_layout::file_arg].is_literal);
        CHECK(k.args[log_layout::line_arg].ak == fmt::arg_const);
        CHECK(k.args[log_layout::mask_arg].ak == fmt::arg_const);
        const std::uint8_t mask = static_cast<std::uint8_t>(k.args[log_layout::mask_arg].value);
        CHECK(log_layout::clock_of(mask) == clock_policy::user);
        CHECK(k.args[log_layout::format_arg(mask)].is_literal);
    }

    // --level: one line per log frame, the envelope before the message
    tools::dump_options o;
    o.logs_only = true;
    std::string text;
    REQUIRE(tools::dump(path, nullptr, o, text));
    const std::string file = __FILE__;
    const std::string base = file.substr(file.find_last_of('/') + 1);
    CHECK(text.find("INFO  " + base + ":" + std::to_string(line_a) + " started with 4 workers\n") !=
          std::string::npos);
    CHECK(text.find("WARN  book AAPL depth 7\n") !=
          std::string::npos); // no call site: file "-" is not shown
    CHECK(text.find("ERROR " + base + ":") != std::string::npos);
    CHECK(text.find("] #1 px=1.25 order=42\n") != std::string::npos); // [tid] #seq
    CHECK(text.find("] #2 second 2\n") != std::string::npos);
    // the user clock renders as the raw tick count
    CHECK(text.find(std::to_string(t0 + 10) + " INFO") != std::string::npos);
    // --level warn hides the info line
    o.min_level = 3;
    text.clear();
    REQUIRE(tools::dump(path, nullptr, o, text));
    CHECK(text.find("started with") == std::string::npos);
    CHECK(text.find("book AAPL") != std::string::npos);
    CHECK(text.find("px=1.25") != std::string::npos);
    // --since/--until on the writer's clock
    o.min_level = 0;
    o.since = t0 + 20;
    o.until = t0 + 30;
    text.clear();
    REQUIRE(tools::dump(path, nullptr, o, text));
    CHECK(text.find("started with") == std::string::npos);
    CHECK(text.find("book AAPL") != std::string::npos);
    CHECK(text.find("px=1.25") != std::string::npos);
    CHECK(text.find("second 2") == std::string::npos);
    // JSON objects
    o.fmt = tools::format::json;
    o.since = 0;
    o.until = UINT64_MAX;
    text.clear();
    REQUIRE(tools::dump(path, nullptr, o, text));
    CHECK(text.find("\"log\":{") != std::string::npos);
    CHECK(text.find("\"level\":\"error\"") != std::string::npos);
    CHECK(text.find("\"sequence\":2") != std::string::npos);
    CHECK(text.find("\"message\":\"px=1.25 order=42\"") != std::string::npos);

    // frame_router::on_log: unhandled log frames arrive rendered
    frame_router r;
    std::vector<std::pair<std::uint8_t, std::string>> got;
    r.on_log(
        [&](const std::uint8_t lvl, const std::string_view line) { got.emplace_back(lvl, line); });
    const auto w = tools::for_each_frame(path, r.callback());
    REQUIRE(w.has_value());
    REQUIRE(got.size() == 4);
    CHECK(got[0].first == 2);
    CHECK(got[0].second.find("started with 4 workers") != std::string::npos);
    CHECK(got[1].first == 3);
    CHECK(got[2].first == 4);
    CHECK(r.handled() == 4);

    // frame_router::on_log(level, h): split by level into structured records; the highest floor at or below the
    // frame's level wins; frames below every floor are dropped without rendering; the line handler keeps the rest
    frame_router r2;
    std::vector<std::string> warns, errors;
    bool envelope_ok = true;
    r2.on_log(level::warn, [&](const tools::log_record& v) {
          warns.push_back(v.message);
      }).on_log(level::error, [&](const tools::log_record& v) {
        envelope_ok =
            envelope_ok && v.has_thread && v.has_sequence && v.line > 0 && !v.file.empty();
        errors.push_back(v.format + " | " + v.message);
    });
    REQUIRE(tools::for_each_frame(path, r2.callback()).has_value());
    CHECK(
        envelope_ok); // the traced policy: thread id and sequence present, file:line from the macro
    REQUIRE(warns.size() == 1);
    CHECK(warns[0] == "book AAPL depth 7");
    REQUIRE(errors.size() == 2);
    CHECK(errors[0] == "px={:.2f} order={} | px=1.25 order=42");
    CHECK(errors[1] == "second {} | second 2");
    CHECK(r2.handled() == 4); // the info line was dropped (below the lowest floor) and counted
    CHECK(r2.unhandled() == 0);
    // with a line handler beside the floors: info goes to the line handler, warn/error to the floors
    frame_router r3;
    std::vector<std::uint8_t> lines;
    int structured = 0;
    r3.on_log([&](const std::uint8_t lvl, std::string_view) {
          lines.push_back(lvl);
      }).on_log(level::warn, [&](const tools::log_record&) { ++structured; });
    REQUIRE(tools::for_each_frame(path, r3.callback()).has_value());
    CHECK((lines.size() == 1 && lines[0] == 2));
    CHECK(structured == 3);
    // tools::render_log on one frame
    const auto bytes = tools::read_file(path);
    REQUIRE(bytes.has_value());
    int seen = 0;
    std::optional<tools::log_record> second;
    const auto walk =
        tools::for_each_frame(*bytes, [&](const dyn::dictionary& d, const tools::frame_ref& fr) {
            if (fr.call && fr.call->is_log() && seen++ == 1)
                if (const auto rec = tools::render_log(fr.bytes, d))
                    second = *rec;
            return true;
        });
    REQUIRE(walk.has_value());
    REQUIRE(second.has_value());
    CHECK(second->level == 3);
    CHECK(second->message == "book AAPL depth 7");
    CHECK(second->format == "book {} depth {}");
    ::unlink(path.c_str());
}

TEST_CASE(
    "A log: the scoped dictionary holds the scope's calls, their literals and types — nothing else",
    "[log][scope]")
{
    const std::string path = tmp_path("log_scope.ser");
    capture_options co;
    co.info_host = false;
    co.clock = &test_clock;
    {
        file_t f(path, co);
        REQUIRE(f.is_open());
        logger<plain, file_t> lg{f};
        lg.set_clock(&test_clock);
        REQUIRE(lg.info("pos {}"_, logtest::Pos{1, 2.5}));
        // an archive message of the same process goes elsewhere: not in this scope
        std::vector<std::byte> buf;
        vector_sink vs{buf};
        REQUIRE(serialize(vs, "elsewhere"_lit, corpus::sample_order()));
        // and a message under another scope
        scoped_capture_file<logtest::other_scope> g(tmp_path("log_other.ser"), co);
        REQUIRE(g.is_open());
        REQUIRE(serialize(g, "other"_lit, corpus::sample_cancel()));
        REQUIRE(g.close());
        REQUIRE(f.close());
    }
    const registry& reg = registry::instance();
    CHECK(reg.scope_calls(detail::scope_hash_v<logtest::app_scope>) >= 1);
    CHECK(reg.scope_calls(detail::scope_hash_v<logtest::other_scope>) == 1);
    const auto ci = tools::info(path);
    REQUIRE(ci.has_value());
    CHECK(ci->dict.find_type(schema_hash_v<logtest::Pos>) != nullptr);
    CHECK(ci->dict.find_type(schema_hash_v<corpus::Order>) == nullptr);
    CHECK(ci->dict.find_type(schema_hash_v<corpus::Cancel>) == nullptr);
    CHECK(ci->dict.find_literal(decltype("elsewhere"_lit)::id) == nullptr);
    CHECK(ci->dict.find_literal(decltype("other"_lit)::id) == nullptr);
    CHECK(ci->dict.find_literal(decltype("pos {}"_)::id) != nullptr);
    // the full image has all of them
    dyn::dictionary all;
    REQUIRE(all.add_rri(reg.export_image()));
    CHECK(all.find_type(schema_hash_v<logtest::Pos>) != nullptr);
    CHECK(all.find_type(schema_hash_v<corpus::Order>) != nullptr);
    CHECK(all.calls.size() > ci->dict.calls.size());
    // the scope's image, stand-alone, parses and verifies the file
    dyn::dictionary scoped;
    REQUIRE(scoped.add_rri(reg.export_image_scope(detail::scope_hash_v<logtest::app_scope>)));
    CHECK(scoped.calls.size() == ci->dict.calls.size());
    const auto rep = tools::verify(path);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    const auto other = tools::info(tmp_path("log_other.ser"));
    REQUIRE(other.has_value());
    CHECK(other->dict.calls.size() == 1);
    CHECK(other->dict.find_type(schema_hash_v<corpus::Cancel>) != nullptr);
    CHECK(other->dict.find_type(schema_hash_v<logtest::Pos>) == nullptr);
    ::unlink(path.c_str());
    ::unlink(tmp_path("log_other.ser").c_str());
}

TEST_CASE("A log: a logger over a plain sink needs dictionary_policy::full; the frames decode with "
          "the registry",
          "[log][full]")
{
    std::vector<std::byte> buf;
    vector_sink vs{buf};
    constexpr log_options full{.clock = clock_policy::user, .dictionary = dictionary_policy::full};
    STATIC_REQUIRE(!scoped_sink<vector_sink>);
    logger<full, vector_sink> lg{vs};
    lg.set_clock(&test_clock);
    REQUIRE(lg.debug("plain {} {}"_, 1, "two"));
    REQUIRE(ES_SERREG_LOG(lg, fatal, "last {}"_, 3.5));
    dyn::dictionary d;
    REQUIRE(d.add_rri(registry::instance().export_image()));
    std::size_t n = 0;
    const auto w = tools::for_each_frame(
        std::span<const std::byte>(buf),
        [&](const dyn::dictionary&, const tools::frame_ref& fr) {
            const auto m = tools::render_frame(fr.bytes, d);
            REQUIRE(m.has_value());
            if (n++ == 0)
                CHECK(m->find("DEBUG plain 1 two") != std::string::npos);
            else
                CHECK(m->find("FATAL ") != std::string::npos);
            return true;
        },
        &d);
    REQUIRE(w.has_value());
    CHECK(n == 2);
}
