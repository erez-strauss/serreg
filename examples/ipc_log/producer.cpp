// examples/ipc_log/producer.cpp — the writing half of the ipc_log example: binary structured logging through a
// named es::ipc SPSC queue to a consumer that knows only one of the producer's types (messages.hpp).
//
//   ipc_log_producer --name serreg_ipc_log --expect expected.txt [--rounds 3] [--linger-ms 20000]
//
// What goes into the queue, in order (record types: messages.hpp):
//   1. record_dictionary: the producer's registry dictionary (registry::export_image(), an RRI) — every type, call
//      signature and format literal this program can send, so the consumer can render what it was not compiled with;
//   2. record_frame: es::serreg frames, serialized in place into the ring record (es::ipc::sink): ~100
//      distinct println formats (scalars, strings, literals, enums, flat structs, structs with std::string
//      and std::vector members), Fill frames (the one type the consumer knows), one LARGE batch per round (a struct
//      with a std::vector of tens of thousands of samples, > 100 KB) between small ones, and one deliberately
//      foreign frame (a call hash nobody registered) that the consumer must report and survive;
//   3. record_end: the number of frames sent.
// For every frame it also writes, with std::format, the text the consumer must print (--expect): the ctest compares
// the two byte for byte.
//
// Sizing (README): the ring holds at least twice the dictionary and twice the largest frame, rounded up by
// es::ipc::ring::for_capacity; both sizes are known only at run time.
//
// The region outlives this process until the consumer has attached: the consumer unlinks the name once it is in,
// and this process waits for that (or --linger-ms) before it exits, so a consumer started late still finds the
// queue. The queue keeps every record until the consumer reads it (SPSC), so the order of start does not matter.
#include "messages.hpp"

#include <es/ipc/ipc.hpp>
#include <es/ipc/region.hpp>
#include <es/serreg/serreg.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// ---- the producer's own types: the consumer is NOT compiled with these ----------------------------------------
namespace app
{
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
enum class Level : std::uint8_t
{
    debug,
    info,
    warn,
    error
};
struct Quote // flat
{
    std::uint64_t id;
    std::int64_t bid;
    std::int64_t ask;
    std::uint32_t size;
    Side side;
};
struct Point
{
    std::int32_t x;
    std::int32_t y;
};
struct Session // dynamic: a string and a vector of strings
{
    std::uint32_t id;
    std::string user;
    std::vector<std::string> roles;
};
struct Path // dynamic: a vector of structs
{
    std::string name;
    std::vector<Point> points;
};
struct Batch // the large one: tens of thousands of samples
{
    std::uint32_t id;
    std::string source;
    std::vector<std::int32_t> samples;
};
} // namespace app

namespace ipc_log
{
using namespace es::serreg::literals;

// ---- the reference text: what the consumer's renderer prints for each argument category ------------------------
// (README "rendering"): std::format of the format string with scalars as themselves, strings and literals as their
// text, enums as the enumerator name; a struct argument as its one-line text form Type{field: value, ...}, where a
// string is quoted, an enum member is Type::name (value) and a vector is [a, b, ...].
std::string quoted(const std::string_view s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + '"';
}
std::string_view name_of(const app::Side s)
{
    return s == app::Side::buy ? "buy" : "sell";
}
std::string_view name_of(const app::Level l)
{
    constexpr std::string_view names[] = {"debug", "info", "warn", "error"};
    return names[static_cast<std::size_t>(l)];
}
std::string text_of(const app::Quote& q)
{
    return std::format("app::Quote{{id: {}, bid: {}, ask: {}, size: {}, side: app::Side::{} ({})}}",
                       q.id, q.bid, q.ask, q.size, name_of(q.side), static_cast<int>(q.side));
}
std::string text_of(const app::Point& p)
{
    return std::format("app::Point{{x: {}, y: {}}}", p.x, p.y);
}
std::string text_of(const app::Session& s)
{
    std::string roles;
    for (const auto& r : s.roles)
        roles += (roles.empty() ? "" : ", ") + quoted(r);
    return std::format("app::Session{{id: {}, user: {}, roles: [{}]}}", s.id, quoted(s.user),
                       roles);
}
std::string text_of(const app::Path& p)
{
    std::string pts;
    for (const auto& q : p.points)
        pts += (pts.empty() ? "" : ", ") + text_of(q);
    return std::format("app::Path{{name: {}, points: [{}]}}", quoted(p.name), pts);
}
std::string text_of(const app::Batch& b)
{
    std::string xs;
    xs.reserve(b.samples.size() * 8);
    for (const auto x : b.samples)
    {
        if (!xs.empty())
            xs += ", ";
        xs += std::to_string(x);
    }
    return std::format("app::Batch{{id: {}, source: {}, samples: [{}]}}", b.id, quoted(b.source),
                       xs);
}

// ---- generated formats: one distinct format literal per index I, four argument shapes ---------------------------
// "gen NN/shape S: ..." with the index written into the literal at compile time, so each I is a different call
// signature in the dictionary (a different literal id and call hash).
template<std::size_t N> struct text_buffer
{
    char s[N];
};
template<std::size_t I, std::size_t N> consteval text_buffer<N> numbered(const char (&pattern)[N])
{
    static_assert(I < 100);
    text_buffer<N> b{};
    for (std::size_t i = 0; i < N; ++i)
        b.s[i] = pattern[i];
    for (std::size_t i = 0; i + 1 < N; ++i)
        if (b.s[i] == 'N' && b.s[i + 1] == 'N')
        {
            b.s[i] = static_cast<char>('0' + I / 10);
            b.s[i + 1] = static_cast<char>('0' + I % 10);
            break;
        }
    return b;
}
template<std::size_t I> struct generated
{
    static constexpr auto a = numbered<I>("gen NN/a: sensor {} reads {:.3f} V");
    static constexpr auto b = numbered<I>("gen NN/b: user {} level {} ok={}");
    static constexpr auto c = numbered<I>("gen NN/c: {:>8} | {:<6} | {:#x}");
    static constexpr auto d = numbered<I>("gen NN/d: quote {}");
};

// ---- the application ---------------------------------------------------------------------------------------------
struct options
{
    std::string name = "serreg_ipc_log";
    std::string expect; // reference text file (required)
    std::uint32_t rounds = 3;
    std::uint32_t linger_ms = 20000;
};

class producer_app
{
public:
    explicit producer_app(options o) : o_(std::move(o))
    {
    }

    int run()
    {
        ref_ = std::fopen(o_.expect.c_str(), "w");
        if (!ref_)
        {
            std::println(stderr, "ipc_log_producer: cannot write {}", o_.expect);
            return 1;
        }
        const int rc = produce();
        if (std::fclose(ref_) != 0)
            return 1;
        return rc;
    }

private:
    using sink_t = es::ipc::sink;

    // generated println formats; with the hand-written ones in round(): ~100 distinct formats
    static constexpr std::uint32_t generated_formats = 80;

    int produce()
    {
        // 1. Measure: the dictionary and the largest frame decide the ring size.
        const std::vector<std::byte> dict = es::serreg::registry::instance().export_image();
        std::size_t largest = 0;
        for (std::uint32_t r = 0; r < o_.rounds; ++r)
            largest = std::max(largest,
                               es::serreg::frame_size(es::serreg::serialize_options{}, batch(r)));
        const std::size_t need = std::max(2 * (dict.size() + 64), 2 * (largest + 64));
        const auto g = es::ipc::ring::for_capacity(need, 64, 1);
        // 2. The named region and the queue.
        auto mem = es::ipc::region::create(o_.name, es::ipc::ring::bytes_for(g));
        if (!mem)
        {
            std::println(stderr, "ipc_log_producer: region {}: {}", o_.name,
                         std::make_error_code(mem.error()).message());
            return 1;
        }
        auto ring = es::ipc::ring::create(*mem, g, es::ipc::ring_kind::spsc);
        auto prod = ring ? es::ipc::producer::attach(*ring) : std::unexpected(ring.error());
        if (!prod)
        {
            std::println(stderr, "ipc_log_producer: ring: {}",
                         std::make_error_code(prod.error()).message());
            return 1;
        }
        std::println(stderr,
                     "ipc_log_producer: dictionary {} bytes, largest frame {} bytes, ring {} bytes "
                     "({} units)",
                     dict.size(), largest, ring->capacity_bytes(), g.units);
        // 3. The dictionary, in band, before any frame.
        if (!prod->write(dict.data(), dict.size(), record_dictionary))
            return fail("dictionary record");
        sink_t out{*prod, record_frame};
        // 4. The frames.
        for (std::uint32_t r = 0; r < o_.rounds && ok_; ++r)
            round(out, r);
        if (ok_)
            foreign(*prod);
        if (!ok_)
            return 1;
        // 5. End of stream: the frame count.
        const std::uint64_t n = frames_;
        if (!prod->write(reinterpret_cast<const std::byte*>(&n), sizeof n, record_end))
            return fail("end record");
        std::println(stderr, "ipc_log_producer: sent {} frames", frames_);
        linger(*mem);
        return 0;
    }

    // Keeps the region (and its name) until the consumer unlinked the name — it does so once attached.
    void linger(es::ipc::region& mem) const
    {
        struct stat st = {};
        const std::string path(mem.path());
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(o_.linger_ms);
        while (::stat(path.c_str(), &st) == 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (::stat(path.c_str(), &st) != 0)
            mem.keep(); // the consumer removed the name: never unlink a later run's region
        else
            std::println(stderr, "ipc_log_producer: no consumer attached within {} ms",
                         o_.linger_ms);
    }

    int fail(const std::string_view what)
    {
        std::println(stderr, "ipc_log_producer: cannot write the {}", what);
        ok_ = false;
        return 1;
    }

    // One println frame, and its reference line.
    template<es::serreg::fixed_string F, class... A>
    void line(sink_t& out, const std::string& expected, const A&... a)
    {
        if (!ok_)
            return;
        if (const auto r = es::serreg::println(out, es::serreg::lit<F>{}, a...); !r)
        {
            std::println(stderr, "ipc_log_producer: serialize: {}",
                         es::serreg::to_string(r.error()));
            ok_ = false;
            return;
        }
        ++frames_;
        std::print(ref_, "{}\n", expected);
    }

    void fill(sink_t& out, const Fill& f)
    {
        if (!ok_)
            return;
        if (!es::serreg::serialize(out, "fill"_, f))
        {
            ok_ = false;
            return;
        }
        ++frames_;
        std::print(ref_, "{}", fill_text(f));
    }

    static app::Batch batch(const std::uint32_t r)
    {
        app::Batch b{r, std::format("adc-{}", r), {}};
        const std::size_t n = r == 1 ? 60000 : 25000 + 5000 * std::size_t{r};
        b.samples.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            b.samples.push_back(static_cast<std::int32_t>((i * 7919 + r * 104729) % 200001) -
                                100000);
        return b;
    }

    // The reference text of a generated line: the runtime format string (the literal's text) and the values.
    template<class... A> static std::string reference(const std::string_view f, const A&... a)
    {
        return std::vformat(f, std::make_format_args(a...));
    }
    template<std::size_t I> void generated_line(sink_t& out, const std::uint32_t r)
    {
        using G = generated<I>;
        const auto k = static_cast<std::int32_t>(I + 100 * r);
        if constexpr (I % 4 == 0)
        {
            const double v = 0.125 * k;
            line<es::serreg::fixed_string{G::a.s}>(out, reference(G::a.s, k, v), k, v);
        }
        else if constexpr (I % 4 == 1)
        {
            const std::string user = std::format("u{}", k);
            const auto lv = static_cast<app::Level>(k % 4);
            const bool ok = k % 3 != 0;
            line<es::serreg::fixed_string{G::b.s}>(out, reference(G::b.s, user, name_of(lv), ok),
                                                   user, lv, ok);
        }
        else if constexpr (I % 4 == 2)
        {
            const auto u = static_cast<std::uint32_t>(k * 977);
            line<es::serreg::fixed_string{G::c.s}>(
                out, reference(G::c.s, k, std::string_view{"lit"}, u), k, "lit"_, u);
        }
        else
        {
            const app::Quote q{static_cast<std::uint64_t>(k), 1000 + k, 1002 + k,
                               static_cast<std::uint32_t>(k % 50),
                               k % 2 ? app::Side::sell : app::Side::buy};
            line<es::serreg::fixed_string{G::d.s}>(out, reference(G::d.s, text_of(q)), q);
        }
    }
    template<std::size_t... I>
    void generated_lines(sink_t& out, const std::uint32_t r, std::index_sequence<I...>)
    {
        (generated_line<I>(out, r), ...);
    }

    // One round: hand-written formats, generated formats, Fills, and the large batch in the middle.
    void round(sink_t& out, const std::uint32_t r)
    {
        const auto ri = static_cast<std::int32_t>(r);
        line<"round {} begins">(out, std::format("round {} begins", r), r);
        line<"hello {} {} {}">(out, "hello new 100 worlds", "new", 100, "worlds"_);
        line<"px={:.2f} qty={} side={}">(
            out, std::format("px={:.2f} qty={} side=sell", 101.25 + r, 7 + ri), 101.25 + r, 7 + ri,
            app::Side::sell);
        line<"bool {} char {} byte-sized {}">(
            out, std::format("bool true char x byte-sized {}", 200 + r), true, 'x',
            static_cast<std::uint8_t>(200 + r));
        line<"i64 {} u64 {} i16 {} f32 {}">(
            out,
            std::format("i64 {} u64 {} i16 {} f32 {}", -9000000000LL - r,
                        18000000000000000000ULL + r, static_cast<std::int16_t>(-300 - ri), 0.1f),
            -9000000000LL - r, 18000000000000000000ULL + r, static_cast<std::int16_t>(-300 - ri),
            0.1f);
        line<"hex {:#010x} oct {:o} bin {:b}">(
            out, std::format("hex {:#010x} oct {:o} bin {:b}", 48879u + r, 8u, 5u), 48879u + r, 8u,
            5u);
        line<"[{:^12}] [{:*<7}] [{:>5}]">(
            out, std::format("[{:^12}] [{:*<7}] [{:>5}]", "centered", "left", 42), "centered",
            "left"_, 42);
        line<"level {} message {}">(out, "level warn message disk at 91%", app::Level::warn,
                                    "disk at 91%");
        line<"escaped {{braces}} and {} value">(
            out, std::format("escaped {{braces}} and {} value", r), r);
        line<"positional {1} before {0}">(out, "positional second before first", "first", "second");
        const app::Quote q{9000 + r, 1999, 2001, 300, app::Side::buy};
        line<"quote {}">(out, std::format("quote {}", text_of(q)), q);
        line<"two quotes {} / {}">(out, std::format("two quotes {} / {}", text_of(q), text_of(q)),
                                   q, q);
        const app::Session s{7 + r, "alice", {"admin", "ops", std::format("r{}", r)}};
        line<"session {}">(out, std::format("session {}", text_of(s)), s);
        const app::Session empty{0, "", {}};
        line<"empty session {}">(out, std::format("empty session {}", text_of(empty)), empty);
        const app::Path p{std::format("route-{}", r), {{1, 2}, {3, 4}, {-5, 6 + ri}}};
        line<"path {} has {} points">(out, std::format("path {} has 3 points", text_of(p)), p, 3);
        line<"padded {:>10}|{:<10}|">(
            out, std::format("padded {:>10}|{:<10}|", text_of(app::Point{1, 2}), "x"),
            app::Point{1, 2}, "x");
        line<"user {} logged in from {}:{}">(out,
                                             std::format("user bob logged in from 10.0.0.{}:22", r),
                                             "bob", std::format("10.0.0.{}", r), 22);
        line<"temperature {:+.1f} C">(out, std::format("temperature {:+.1f} C", -3.25 - r),
                                      -3.25 - r);
        line<"quoted \"{}\" and backslash \\ {}">(
            out, std::format("quoted \"{}\" and backslash \\ {}", "q", r), "q", r);
        fill(out, Fill{1000 + r, 1, 100, 101.5});

        // generated formats, first half; the large batch; small ones and a Fill; second half
        generated_lines(out, r, std::make_index_sequence<generated_formats / 2>{});
        const app::Batch b = batch(r);
        line<"batch {}">(out, std::format("batch {}", text_of(b)), b);
        fill(out, Fill{2000 + r, 2, -50, 99.875});
        line<"after batch {} ({} samples)">(
            out, std::format("after batch {} ({} samples)", r, b.samples.size()), r,
            static_cast<std::uint64_t>(b.samples.size()));
        generated_second_half(out, r);
        fill(out, Fill{3000 + r, 3, 7, 0.0001});
        line<"round {} ends">(out, std::format("round {} ends", r), r);
    }
    void generated_second_half(sink_t& out, const std::uint32_t r)
    {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (generated_line<generated_formats / 2 + I>(out, r), ...);
        }(std::make_index_sequence<generated_formats / 2>{});
    }

    // A frame whose call hash is in no dictionary (a "newer producer" message): the consumer must print
    // "unknown call 0x..." and go on. Built with a vector_sink, the hash flipped, then copied into the ring.
    void foreign(es::ipc::producer& prod)
    {
        std::vector<std::byte> f;
        es::serreg::vector_sink vs{f};
        if (!es::serreg::println(vs, "this text never reaches the consumer {}"_, 1))
        {
            ok_ = false;
            return;
        }
        std::uint64_t h = 0;
        // frame header: u32 len, u16 flags, u16 n, u64 call hash
        std::memcpy(&h, f.data() + 8, sizeof h);
        h ^= 0x5eed'0000'0000'0001ULL;
        std::memcpy(f.data() + 8, &h, sizeof h);
        if (!prod.write(f.data(), f.size(), record_frame))
        {
            ok_ = false;
            return;
        }
        ++frames_;
        std::print(ref_, "unknown call 0x{:016x}\n", h);
    }

    options o_;
    std::FILE* ref_ = nullptr;
    std::uint64_t frames_ = 0;
    bool ok_ = true;
};

// --name N, --expect FILE, --rounds N, --linger-ms N
bool parse(const std::span<char* const> args, options& o)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view a = args[i];
        if (i + 1 >= args.size())
            return false;
        const std::string_view v = args[++i];
        const auto number = [&](std::uint32_t& out) {
            return std::from_chars(v.data(), v.data() + v.size(), out).ec == std::errc{};
        };
        if (a == "--name")
            o.name = v;
        else if (a == "--expect")
            o.expect = v;
        else if (a == "--rounds"      ? !number(o.rounds)
                 : a == "--linger-ms" ? !number(o.linger_ms)
                                      : true)
            return false;
    }
    return !o.expect.empty() && o.rounds >= 1;
}
} // namespace ipc_log

int main(const int argc, char** const argv)
{
    ipc_log::options o;
    if (!ipc_log::parse(std::span<char* const>(argv + 1, static_cast<std::size_t>(argc - 1)), o))
    {
        std::println(
            stderr,
            "usage: ipc_log_producer --expect FILE [--name REGION] [--rounds N] [--linger-ms MS]");
        return 2;
    }
    return ipc_log::producer_app{std::move(o)}.run();
}
