// tests/bench/hrtt_common.hpp — the half-round-trip-time (half-RTT) harness shared by serreg_hrtt_raw and
// serreg_hrtt_serreg (v2.0.4; es::ipc rings with run-time geometry and huge pages since 2.0.5; the variable-size
// es::ipc rings since 2.1.13a). Two parties, two SPMC broadcast rings (es::ipc::ring of ring_kind::spmc, one
// consumer each; `--ring spsc` uses SPSC queues instead):
//
//     ping ──[ ring "<name>-a2b" ]──▶ pong          ping writes a message stamped t0
//     ping ◀──[ ring "<name>-b2a" ]── pong          pong stamps t1 (seen) and t2 (reply) and echoes it
//                                                   ping stamps t3 when the reply is decoded
//
// half RTT = (t3 - t0) / 2 — one message each way, including every encode and decode on both sides. Because
// CLOCK_MONOTONIC is one clock for the whole machine, the two one-way latencies (t1 - t0, t3 - t2) and pong's
// turnaround (t2 - t1) are reported as well. Exactly one message is in flight: ping sends the next one only
// after the reply arrived, so each sample is a pure latency, not a throughput artefact.
//
// Roles: `--role both` (default) runs pong as a forked process (`--mode process`, default) or as a thread
// (`--mode thread`); `--role ping` / `--role pong` run one side each, for two separately launched processes
// (what scripts/bench-hrtt-*.sh do): ping creates both shared-memory rings, pong attaches to them by name and
// learns their geometry and kind from the ring headers. A record is a 16-byte header {size, seq, type, check} and
// the payload, rounded up to 64-byte units; `--slots N --slot-bytes B` (kept from the slot rings) ask for room for N
// messages of up to B bytes: a ring of at least N × align_up(16 + B, 64) bytes (ring::for_capacity rounds the unit
// count up to a power of two). `--huge 2m|1g` puts the rings on explicit huge pages (hugetlbfs for processes,
// MAP_HUGETLB for threads); `--lock` mlock()s them.
//
// Writers claim a record of exactly the message size and commit it (raw: the packet built in place; serreg:
// es::ipc::sink, so serialize() builds the frame in the record). Readers peek() the record in place and release()
// it after the reply was built (pong) or the reply decoded (ping).
//
// A codec supplies: `name`, `description`, `wire_bytes()`, `send_ping(prod, seq, t0)`,
// `read_pong(frame, sample&) -> bool`, `echo(frame, prod, t1) -> std::optional<std::uint64_t>` (the sequence
// echoed; nullopt on a decode failure) and `send_stop(prod)`.
#pragma once

#include <es/ipc/ipc.hpp>
#include <es/serreg/tools.hpp>

#include <sched.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace hrtt
{
namespace ipc = es::ipc;

inline constexpr std::uint64_t stop_seq = ~std::uint64_t{0};

// Bytes a message of up to `payload` bytes occupies in a ring: the 16-byte record header and the payload, rounded
// up to the 64-byte unit (es::ipc::ring's defaults: 64-byte units and alignment, payload at record + 16).
inline constexpr std::size_t record_header_bytes = 16;
inline constexpr std::size_t ring_unit_bytes = 64;
constexpr std::size_t record_bytes(const std::size_t payload) noexcept
{
    return (record_header_bytes + payload + ring_unit_bytes - 1) / ring_unit_bytes *
           ring_unit_bytes;
}

constexpr std::string_view to_string(const ipc::ring_kind k) noexcept
{
    return k == ipc::ring_kind::spsc ? "SPSC" : "SPMC";
}

inline std::int64_t now_ns() noexcept
{
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
}

struct sample
{
    std::uint64_t seq = 0;
    std::int64_t t0 = 0, t1 = 0, t2 = 0;
};

struct options
{
    std::string role = "both";    // both | ping | pong
    std::string mode = "process"; // process | thread   (role both only)
    std::string name;             // shared-memory base name
    std::uint64_t iters = 1'000'000;
    std::uint64_t warmup = 100'000;
    int cpu_ping = -1, cpu_pong = -1;
    double timeout_s = 30;   // pong waits this long for ping's rings; ping for pong to join
    std::string samples_out; // optional: one half-RTT per line (ns)
    bool quiet = false;
    bool pause =
        false; // spin with a pause hint (kinder to a hyper-thread sibling, adds wake-up latency)
    // ring size: room for `slots` messages of up to `slot_bytes` bytes (one message in flight needs one record)
    std::uint32_t slots = 1024;
    std::uint32_t slot_bytes = 256;
    ipc::ring_kind kind = ipc::ring_kind::spmc;
    ipc::pages pages = ipc::pages::normal;
    bool lock = false;
    std::size_t capacity() const noexcept
    {
        return std::size_t{slots} * record_bytes(slot_bytes);
    }
    // 64-byte units, two gates (as the slot rings had); an SPSC ring uses one of them
    ipc::geometry shape() const noexcept
    {
        return ipc::ring::for_capacity(capacity(), ring_unit_bytes, 2);
    }
    // for_capacity() stops at the largest ring (2^31 units): a request beyond it is refused, not shrunk
    bool shape_ok() const noexcept
    {
        const ipc::geometry g = shape();
        return slots != 0 && slot_bytes != 0 && ipc::ring::valid(g) &&
               std::size_t{g.units} * g.unit_bytes >= capacity();
    }
    ipc::region_options region() const noexcept
    {
        return {.pages = pages, .lock = lock};
    }
};

inline void usage(const char* const argv0, const std::string_view what)
{
    std::println(
        "usage: {} [--role both|ping|pong] [--mode process|thread] [--name NAME] [--iters N]\n"
        "          [--warmup N] [--cpus PING,PONG] [--cpu N] [--timeout SEC] [--samples FILE] "
        "[--pause] [--quiet]\n"
        "          [--slots N] [--slot-bytes N] [--ring spmc|spsc] [--huge 2m|1g] [--lock]\n"
        "half-RTT benchmark over two es::ipc variable-size rings — {}\n"
        "  --slots N --slot-bytes B  ring room for N messages of up to B bytes: at least\n"
        "                            N x align_up(16 + B, 64) bytes (default 1024 x 256)\n"
        "  --ring spmc|spsc          SPMC broadcast rings with one reader (default) or SPSC "
        "queues;\n"
        "                            --role pong learns it from the ring header",
        argv0, what);
}

inline bool parse_u64(const char* const s, std::uint64_t& v)
{
    const std::string_view t{s};
    return std::from_chars(t.data(), t.data() + t.size(), v).ec == std::errc{};
}

inline std::optional<options> parse(const int argc, char** const argv, const std::string_view what)
{
    options o;
    o.name = "serreg-hrtt-" + std::to_string(::getpid());
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a{argv[i]};
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        std::uint64_t n = 0;
        if (a == "--help" || a == "-h")
        {
            usage(argv[0], what);
            std::exit(0);
        }
        else if (a == "--quiet")
            o.quiet = true;
        else if (a == "--pause")
            o.pause = true;
        else if (a == "--lock")
            o.lock = true;
        else if (a == "--huge" && (v = next()) &&
                 (std::string_view(v) == "2m" || std::string_view(v) == "1g"))
            o.pages = std::string_view(v) == "2m" ? ipc::pages::huge_2m : ipc::pages::huge_1g;
        else if (a == "--ring" && (v = next()) &&
                 (std::string_view(v) == "spmc" || std::string_view(v) == "spsc"))
            o.kind = std::string_view(v) == "spsc" ? ipc::ring_kind::spsc : ipc::ring_kind::spmc;
        else if ((a == "--slots" || a == "--slot-bytes") && (v = next()) && parse_u64(v, n) &&
                 n <= (1u << 30))
            (a == "--slots" ? o.slots : o.slot_bytes) = static_cast<std::uint32_t>(n);
        else if ((a == "--role" || a == "--mode" || a == "--name" || a == "--samples") &&
                 (v = next()))
            (a == "--role"   ? o.role
             : a == "--mode" ? o.mode
             : a == "--name" ? o.name
                             : o.samples_out) = v;
        else if ((a == "--iters" || a == "--warmup") && (v = next()) && parse_u64(v, n))
            (a == "--iters" ? o.iters : o.warmup) = n;
        else if (a == "--timeout" && (v = next()))
            o.timeout_s = std::strtod(v, nullptr);
        else if (a == "--cpu" && (v = next()))
            o.cpu_ping = o.cpu_pong = std::atoi(v); // with --role ping|pong: this process's CPU
        else if (a == "--cpus" && (v = next()))
        {
            const char* const comma = std::strchr(v, ',');
            o.cpu_ping = std::atoi(v);
            o.cpu_pong = comma ? std::atoi(comma + 1) : -1;
        }
        else
        {
            std::println(stderr, "bad argument: {}", argv[i]);
            usage(argv[0], what);
            return std::nullopt;
        }
    }
    if ((o.role != "both" && o.role != "ping" && o.role != "pong") ||
        (o.mode != "process" && o.mode != "thread") || o.iters == 0 || !o.shape_ok())
    {
        usage(argv[0], what);
        return std::nullopt;
    }
    return o;
}

inline bool pin(const int cpu) noexcept
{
    if (cpu < 0)
        return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    return ::sched_setaffinity(0, sizeof set, &set) == 0;
}

// Busy-waits for the next message and returns it in place (valid until release()); nullopt when the consumer was
// lapped (evicted or reaped: it never sees another message). An empty peek() is one cursor load. The ring's
// consumer::wait() always pauses; here the pause is optional, and every 65536 empty polls the thread yields so an
// oversubscribed machine (ctest -j) still makes progress.
inline std::optional<ipc::message> wait_msg(ipc::consumer& c, const bool pause) noexcept
{
    for (std::uint32_t spins = 1;; ++spins)
    {
        if (auto m = c.peek())
            return m;
        if (c.lapped()) [[unlikely]]
            return std::nullopt;
        if (pause)
            ipc::cpu_relax();
        if ((spins & 0xFFFF) == 0)
            ::sched_yield();
    }
}

// ---- the two sides -------------------------------------------------------------------------------------------

template<class Codec>
int run_pong(const ipc::ring& a2b, const ipc::ring& b2a, Codec& codec, const options& o)
{
    auto in = ipc::consumer::join(a2b);
    auto out = ipc::producer::attach(b2a);
    if (!in || !out)
    {
        std::println(stderr, "pong: cannot join a2b ({:d}) / attach b2a ({:d})", !!in, !!out);
        return 3;
    }
    for (std::uint64_t n = 0;; ++n)
    {
        const auto m = wait_msg(*in, o.pause);
        if (!m)
            return 5; // lapped
        const std::int64_t t1 = now_ns();
        const auto seq = codec.echo(m->data, *out, t1); // reads the a2b record in place
        in->release();
        if (!seq)
        {
            std::println(stderr, "pong: decode failed at message {}", n);
            return 4;
        }
        if (*seq == stop_seq)
            return in->lapped() ? 5 : 0;
    }
}

struct results
{
    std::vector<std::int64_t> half, a2b, b2a, turn;
    std::int64_t wall_ns = 0;
};

template<class Codec>
int run_ping(const ipc::ring& a2b, const ipc::ring& b2a, Codec& codec, const options& o,
             results& res)
{
    auto out = ipc::producer::attach(a2b);
    auto in = ipc::consumer::join(b2a); // before the first ping, so no reply can be missed
    if (!in || !out)
    {
        std::println(stderr, "ping: cannot attach a2b / join b2a");
        return 3;
    }
    const std::int64_t deadline = now_ns() + static_cast<std::int64_t>(o.timeout_s * 1e9);
    while (a2b.active_consumers() == 0) // pong has not joined yet
    {
        if (now_ns() > deadline)
        {
            std::println(stderr, "ping: no pong joined within {:.0f} s", o.timeout_s);
            return 6;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    res.half.resize(o.iters);
    res.a2b.resize(o.iters);
    res.b2a.resize(o.iters);
    res.turn.resize(o.iters);
    const std::uint64_t total = o.warmup + o.iters;
    std::int64_t wall0 = 0;
    for (std::uint64_t i = 0; i < total; ++i)
    {
        if (i == o.warmup)
            wall0 = now_ns();
        const std::int64_t t0 = now_ns();
        codec.send_ping(*out, i, t0);
        sample s;
        const auto m = wait_msg(*in, o.pause);
        if (!m)
            return 5; // lapped
        const bool ok = codec.read_pong(m->data, s);
        const std::int64_t t3 = now_ns();
        in->release();
        if (!ok || s.seq != i || s.t0 != t0)
        {
            std::println(stderr, "ping: bad reply at {} (ok={:d} seq={})", i, ok, s.seq);
            return 4;
        }
        if (i >= o.warmup)
        {
            const std::uint64_t k = i - o.warmup;
            res.half[k] = (t3 - t0) / 2;
            res.a2b[k] = s.t1 - t0;
            res.b2a[k] = t3 - s.t2;
            res.turn[k] = s.t2 - s.t1;
        }
    }
    res.wall_ns = now_ns() - wall0;
    codec.send_stop(*out);
    return in->lapped() ? 5 : 0;
}

// ---- reporting -------------------------------------------------------------------------------------------------

inline std::string read_first_line(const char* const path, const std::string_view key = {})
{
    std::FILE* const f = std::fopen(path, "r");
    if (!f)
        return "?";
    char line[512];
    std::string r = "?";
    while (std::fgets(line, sizeof line, f))
    {
        std::string_view l{line};
        if (!key.empty() && !l.starts_with(key))
            continue;
        if (const auto c = l.find(':'); !key.empty() && c != std::string_view::npos)
            l.remove_prefix(c + 1);
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t'))
            l.remove_prefix(1);
        while (!l.empty() && (l.back() == '\n' || l.back() == ' '))
            l.remove_suffix(1);
        r = l;
        break;
    }
    std::fclose(f);
    return r;
}

inline void print_row(const char* const label, std::vector<std::int64_t>& v)
{
    std::sort(v.begin(), v.end());
    const auto q = [&](const double p) {
        const auto k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
        return v[std::min(k, v.size() - 1)];
    };
    long double sum = 0;
    for (const auto x : v)
        sum += static_cast<long double>(x);
    std::println("{:<22} {:7} {:7} {:7} {:7} {:7} {:8} {:9} {:9.1f}", label, v.front(), q(0.50),
                 q(0.90), q(0.99), q(0.999), q(0.9999), v.back(),
                 static_cast<double>(sum / static_cast<long double>(v.size())));
}

inline std::int64_t clock_overhead_ns()
{
    std::int64_t best = 1'000'000;
    for (int r = 0; r < 64; ++r)
    {
        const std::int64_t a = now_ns();
        std::int64_t b = a;
        for (int i = 0; i < 100; ++i)
            b = now_ns();
        best = std::min(best, (b - a) / 100);
    }
    return best;
}

template<class Codec>
void report(const Codec& codec, const options& o, results& r, const std::string_view transport,
            const ipc::ring& ring, const std::string_view memory)
{
    utsname u{};
    ::uname(&u);
    char host[256] = {};
    ::gethostname(host, sizeof host - 1);
    std::println("es::serreg half-RTT benchmark: {}", codec.name);
    std::println("  message:     {}", codec.description);
    const auto g = ring.shape();
    std::println(
        "  wire bytes:  {} per message, a {}-byte record (16-byte record header + payload, "
        "{}-byte units)",
        codec.wire_bytes(), ring.record_units(codec.wire_bytes()) * g.unit_bytes, g.unit_bytes);
    std::println("  ring:        {} variable-size, {} units = {} B of records, {} bytes per ring "
                 "(room for {} x {} B messages asked)",
                 to_string(ring.kind()), g.units, ring.capacity_bytes(), ring.bytes(), o.slots,
                 o.slot_bytes);
    std::println("  transport:   {}", transport);
    std::println("  memory:      {}", memory);
    std::println("  library:     es::serreg {}   host {}   kernel {} {}",
                 es::serreg::tools::library_version, static_cast<const char*>(host),
                 static_cast<const char*>(u.release), static_cast<const char*>(u.machine));
    std::println("  cpu:         {}   ({} online)", read_first_line("/proc/cpuinfo", "model name"),
                 ::sysconf(_SC_NPROCESSORS_ONLN));
    std::println("  pinning:     ping cpu {}, pong cpu {}{}", o.cpu_ping, o.cpu_pong,
                 o.cpu_ping < 0 || o.cpu_pong < 0 ? "  (unpinned sides are scheduled freely)" : "");
    std::println("  waiting:     busy spin{} on peek() (yield every 65536 empty polls)",
                 o.pause ? " with a pause hint" : "");
    std::println("  clock:       CLOCK_MONOTONIC, {} ns per read (best of 64×100)",
                 clock_overhead_ns());
    std::println("  samples:     {} round trips after {} warm-up, one message in flight", o.iters,
                 o.warmup);
    std::println(
        "  wall:        {:.3f} s, {:.0f} round trips/s\n", static_cast<double>(r.wall_ns) / 1e9,
        static_cast<double>(o.iters) * 1e9 / static_cast<double>(r.wall_ns ? r.wall_ns : 1));
    std::println("{:<22} {:>7} {:>7} {:>7} {:>7} {:>7} {:>8} {:>9} {:>9}", "nanoseconds", "min",
                 "p50", "p90", "p99", "p99.9", "p99.99", "max", "mean");
    if (!o.samples_out.empty())
    {
        if (std::FILE* const f = std::fopen(o.samples_out.c_str(), "w"))
        {
            for (const auto x : r.half)
                std::println(f, "{}", x);
            std::fclose(f);
        }
    }
    print_row("half RTT (t3-t0)/2", r.half);
    print_row("one-way ping->pong", r.a2b);
    print_row("one-way pong->ping", r.b2a);
    print_row("pong turnaround", r.turn);
    std::fflush(stdout);
}

// ---- process / thread orchestration ----------------------------------------------------------------------------

struct ring_pair
{
    ipc::region a2b_mem, b2a_mem;
    ipc::ring a2b, b2a;
};

inline std::string describe(const ipc::region& m, const options& o)
{
    return std::format("{}, {}-byte pages{}{}",
                       m.path().empty() ? std::string_view("anonymous shared mapping") : m.path(),
                       m.page_size(), m.huge() ? " (explicit huge pages)" : "",
                       o.lock ? ", mlock()ed" : "");
}

// ping side: create both regions and rings. Named (for a separate pong process) or anonymous (threads).
inline std::optional<ring_pair> create_rings(const options& o, const bool named)
{
    const std::size_t bytes = ipc::ring::bytes_for(o.shape());
    auto make = [&](const char* const dir) {
        return named ? ipc::region::create(o.name + dir, bytes, o.region())
                     : ipc::region::anonymous(bytes, o.region());
    };
    auto a = make("-a2b");
    auto b = a ? make("-b2a") : std::unexpected(a.error());
    if (!a || !b)
    {
        std::println(stderr, "ping: region for {}-* ({} pages{}): {}", o.name,
                     ipc::to_string(o.pages), o.lock ? ", locked" : "",
                     std::make_error_code(!a ? a.error() : b.error()).message());
        return std::nullopt;
    }
    const auto ra = ipc::ring::create(*a, o.shape(), o.kind);
    const auto rb = ipc::ring::create(*b, o.shape(), o.kind);
    if (!ra || !rb)
    {
        std::println(stderr, "ping: ring::create ({}): {}", to_string(o.kind),
                     std::make_error_code(!ra ? ra.error() : rb.error()).message());
        return std::nullopt;
    }
    return ring_pair{std::move(*a), std::move(*b), *ra, *rb};
}

// attach() takes the expected kind and refuses another one with errc::invalid_argument; pong does not know which
// kind ping chose, so it tries SPMC, then SPSC (errc::resource_unavailable_try_again: not constructed yet).
inline std::expected<ipc::ring, std::errc> attach_any(const ipc::region& m) noexcept
{
    auto r = ipc::ring::attach(m, ipc::ring_kind::spmc);
    if (r || r.error() != std::errc::invalid_argument)
        return r;
    return ipc::ring::attach(m, ipc::ring_kind::spsc);
}

// pong side of two processes: wait for ping's regions to exist and its rings to be constructed, then attach.
// region::open() finds them on /dev/shm or any hugetlbfs mount; the geometry and kind come from the ring header.
inline std::optional<ring_pair> open_rings(const options& o)
{
    const std::int64_t deadline = now_ns() + static_cast<std::int64_t>(o.timeout_s * 1e9);
    for (;;)
    {
        auto a = ipc::region::open(o.name + "-a2b", {.lock = o.lock});
        auto b = ipc::region::open(o.name + "-b2a", {.lock = o.lock});
        if (a && b)
        {
            const auto ra = attach_any(*a);
            const auto rb = attach_any(*b);
            if (ra && rb)
                return ring_pair{std::move(*a), std::move(*b), *ra, *rb};
        }
        if (now_ns() > deadline)
        {
            std::println(stderr, "pong: rings {}-a2b/-b2a not available within {:.0f} s", o.name,
                         o.timeout_s);
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// One benchmark run: `Codec` encodes and decodes the messages, `options` (from parse()) says which side(s) run
// here and how. run() returns the process exit code.
template<class Codec> class app
{
public:
    explicit app(const options& o) : o_{o}
    {
    }

    int run()
    {
        if (codec_.wire_bytes() > o_.slot_bytes)
        {
            std::println(stderr,
                         "a {}-byte message is larger than --slot-bytes {} (the largest message "
                         "the ring is sized for)",
                         codec_.wire_bytes(), o_.slot_bytes);
            return 2;
        }
        if (o_.role == "pong")
            return run_pong_side();

        const bool threads = o_.role == "both" && o_.mode == "thread";
        const auto rings = create_rings(o_, !threads);
        if (!rings)
            return 6;
        results res;
        std::string transport;
        const int rc =
            threads ? run_threads(*rings, res, transport) : run_processes(*rings, res, transport);
        if (rc != 0)
        {
            std::println(stderr, "benchmark failed (exit {})", rc);
            return rc;
        }
        if (!o_.quiet)
            report(codec_, o_, res, transport, rings->a2b, describe(rings->a2b_mem, o_));
        else
            std::println("ok: {} round trips", o_.iters);
        return 0;
    }

private:
    const options o_;
    Codec codec_;

    // --role pong: attach to the rings a separately launched ping created
    int run_pong_side()
    {
        if (!pin(o_.cpu_pong))
            std::println(stderr, "pong: cannot pin to cpu {}", o_.cpu_pong);
        const auto rings = open_rings(o_);
        return rings ? run_pong(rings->a2b, rings->b2a, codec_, o_) : 6;
    }

    // --mode thread: pong on a second thread, both on anonymous rings
    int run_threads(const ring_pair& rings, results& res, std::string& transport)
    {
        transport = std::format("2 threads in one process, 2 {} rings (ping->pong, pong->ping)",
                                to_string(o_.kind));
        int pong_rc = 0;
        std::thread pong([&] {
            pin(o_.cpu_pong);
            pong_rc = run_pong(rings.a2b, rings.b2a, codec_, o_);
        });
        pin(o_.cpu_ping);
        const int rc = run_ping(rings.a2b, rings.b2a, codec_, o_, res);
        pong.join();
        return rc ? rc : pong_rc;
    }

    // --mode process: pong forked here (--role both) or launched separately (--role ping)
    int run_processes(const ring_pair& rings, results& res, std::string& transport)
    {
        pid_t child = -1;
        if (o_.role == "both")
        {
            child = ::fork();
            if (child == 0)
            { // the pong process opens the rings by name, exactly like a separately launched one
                // 2.1.12 (review I-17): if ping dies after the rendezvous, pong must not spin forever
                ::prctl(PR_SET_PDEATHSIG, SIGTERM);
                if (::getppid() == 1)
                    std::_Exit(6);
                std::fflush(stdout);
                options po = o_;
                po.role = "pong";
                pin(o_.cpu_pong);
                const auto own = open_rings(po);
                std::_Exit(own ? run_pong(own->a2b, own->b2a, codec_, po) : 6);
            }
            if (child < 0)
            {
                std::println(stderr, "fork: {}",
                             std::strerror(errno)); // no perror (2.1.9 rule, review I-17)
                return 6;
            }
        }
        transport = "2 processes (" +
                    std::string(o_.role == "both" ? "forked" : "launched separately") +
                    "), 2 shared-memory " + std::string(to_string(o_.kind)) + " rings " + o_.name +
                    "-a2b (ping->pong) and " + o_.name + "-b2a (pong->ping)";
        if (!pin(o_.cpu_ping))
            std::println(stderr, "ping: cannot pin to cpu {}", o_.cpu_ping);
        int rc = run_ping(rings.a2b, rings.b2a, codec_, o_, res);
        if (child > 0)
        {
            if (rc != 0) // ping gave up before sending the stop message: pong would wait forever
                ::kill(child, SIGTERM);
            int st = 0;
            ::waitpid(child, &st, 0);
            const int pong_rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            rc = rc ? rc : pong_rc;
        }
        return rc;
    }
};

} // namespace hrtt
