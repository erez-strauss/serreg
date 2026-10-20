// tests/unit/test_ipc.cpp — es::ipc, the variable-size rings (docs/ipc/ipc-design.md §8; es::ipc_dynamic until the 2.1.13a switch): variable-size SPSC
// and SPMC rings. Geometry and attach validation; every boundary of the skip logic, records of max_message(),
// wrap-arounds (also across 2^32); types, full policies, deferred commits; SPSC persistence and hand-over; SPMC
// broadcast, join while running, evict, busy; corrupt headers; threads and processes (fork) with every byte verified
// for units 16/32/64; reaping, producer take-over; sink + es::serreg::serialize. Its own executable
// (serreg_ipc_tests). Four cases were written as failing tests for defects found while writing the suite (B1
// SPSC geometry, B2 skip release in peek(), B3 sizes above 4 GiB, B4 stale pid in reaping) and pass since the fixes.
#include <es/ipc/ipc.hpp>
#include <es/ipc/region.hpp>
#include <es/serreg/serreg.hpp>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace dyn = es::ipc;
namespace ipc = es::ipc;
using es::ipc::full_policy;

namespace dtest
{
struct Tick
{
    std::uint64_t seq;
    std::int64_t px;
    std::uint32_t qty;
    char sym[4];
};
struct Book
{
    std::string venue;
    std::vector<std::int64_t> levels;
};
} // namespace dtest

namespace
{
using steady = std::chrono::steady_clock;
constexpr auto time_limit = std::chrono::seconds(60); // a hang becomes a failure, not a stuck ctest

// ---- rings in anonymous shared memory (threads, fork children) ---------------------------------------------------
template<class Ring = dyn::ring> struct anon_ring
{
    ipc::region mem;
    Ring ring;
    dyn::geometry g;

    std::byte* base() const
    {
        return static_cast<std::byte*>(mem.data());
    }
};
template<class Ring = dyn::ring>
anon_ring<Ring> make_ring(const dyn::geometry g, const dyn::ring_kind k = dyn::ring_kind::spmc)
{
    const std::size_t bytes = Ring::bytes_for(g);
    REQUIRE(bytes > 0);
    auto m = ipc::region::anonymous(bytes);
    REQUIRE(m);
    auto r = Ring::create(*m, g, k);
    REQUIRE(r);
    return {std::move(*m), *r, g};
}

// ---- raw views of the shared layout (to simulate a buggy peer and to observe gates) ------------------------------
std::uint64_t gate_word(const std::byte* const base, const std::uint32_t id)
{
    auto* const w =
        reinterpret_cast<std::uint64_t*>(const_cast<std::byte*>(base) + dyn::detail::gates_offset +
                                         std::size_t{id} * dyn::detail::line);
    return std::atomic_ref<std::uint64_t>(*w).load();
}
void set_gate_word(std::byte* const base, const std::uint32_t id, const std::uint64_t v)
{
    auto* const w = reinterpret_cast<std::uint64_t*>(base + dyn::detail::gates_offset +
                                                     std::size_t{id} * dyn::detail::line);
    std::atomic_ref<std::uint64_t>(*w).store(v);
}
// The gate's position word (layout 2: own at +0, pos at +8).
std::uint32_t gate_pos(const std::byte* const base, const std::uint32_t id)
{
    auto* const w = reinterpret_cast<std::uint32_t*>(
        const_cast<std::byte*>(base) + dyn::detail::gates_offset +
        std::size_t{id} * dyn::detail::line + offsetof(dyn::detail::gate, pos));
    return std::atomic_ref<std::uint32_t>(*w).load();
}
void set_gate_pos(std::byte* const base, const std::uint32_t id, const std::uint32_t v)
{
    auto* const w = reinterpret_cast<std::uint32_t*>(base + dyn::detail::gates_offset +
                                                     std::size_t{id} * dyn::detail::line +
                                                     offsetof(dyn::detail::gate, pos));
    std::atomic_ref<std::uint32_t>(*w).store(v);
}
void set_cursor(std::byte* const base, const std::uint32_t v)
{
    std::atomic_ref<std::uint32_t>(
        *reinterpret_cast<std::uint32_t*>(base + dyn::detail::cursor_offset))
        .store(v);
}
dyn::detail::ring_header& ring_header(std::byte* const base)
{
    return *reinterpret_cast<dyn::detail::ring_header*>(base);
}
// The header of the record at `pos` (the creator's geometry: for SPMC rings data_offset follows it directly).
template<class Ring>
dyn::detail::record_header& header_at(const anon_ring<Ring>& t, const std::uint32_t pos)
{
    const std::size_t off = dyn::detail::data_offset(t.g, Ring::align) +
                            std::size_t{pos & (t.g.units - 1)} * t.g.unit_bytes;
    return *reinterpret_cast<dyn::detail::record_header*>(t.base() + off);
}
void forge_header(dyn::detail::record_header& h, const std::uint32_t size, const std::uint32_t seq,
                  const std::uint32_t type)
{
    h = {size, seq, type, dyn::detail::header_check(size, seq, type)};
}

// ---- deterministic payloads: every byte of every message is checked ----------------------------------------------
constexpr std::size_t pattern_bytes = (std::size_t{1} << 21) + 1024;
const std::byte* pattern()
{
    static const std::vector<std::byte> p = [] {
        std::vector<std::byte> v(pattern_bytes);
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = static_cast<std::byte>((i * 2654435761u) >> 13);
        return v;
    }();
    return p.data();
}
std::uint64_t mix(std::uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
struct shape
{
    std::size_t size;
    std::uint32_t type;
};
// Mostly small messages, some medium ones and, every few hundred, one of ~80 % of the ring.
shape message_shape(const std::uint64_t seq, const std::size_t max_message)
{
    const std::uint64_t h = mix(seq);
    std::size_t size = 0;
    if (h % 331 == 0)
        size = max_message * 4 / 5 - static_cast<std::size_t>((h >> 20) % 64);
    else if (h % 7 == 0)
        size = static_cast<std::size_t>((h >> 12) % (max_message / 8 + 1));
    else
        size = static_cast<std::size_t>((h >> 12) % 161);
    return {size, static_cast<std::uint32_t>((h >> 40) % dyn::type_reserved_first)};
}
void fill(const std::span<std::byte> d, const std::uint64_t seq)
{
    const std::size_t off = seq % 509;
    if (!d.empty())
        std::memcpy(d.data(), pattern() + off, d.size());
    if (d.size() >= sizeof seq)
        std::memcpy(d.data(), &seq, sizeof seq);
}
bool verify(const dyn::message& m, const std::uint64_t seq, const std::size_t max_message)
{
    const shape s = message_shape(seq, max_message);
    if (m.data.size() != s.size || m.type != s.type)
        return false;
    const std::size_t off = seq % 509;
    if (s.size < sizeof seq)
        return s.size == 0 || std::memcmp(m.data.data(), pattern() + off, s.size) == 0;
    return std::memcmp(m.data.data(), &seq, sizeof seq) == 0 &&
           std::memcmp(m.data.data() + 8, pattern() + off + 8, s.size - 8) == 0;
}

// Writes messages [first, first + n) with the deterministic shapes; full_policy::fail with a deadline.
template<class P> bool produce(P& p, const std::uint64_t first, const std::uint64_t n)
{
    const auto deadline = steady::now() + time_limit;
    const std::size_t maxm = p.max_message();
    for (std::uint64_t seq = first; seq < first + n; ++seq)
    {
        const shape s = message_shape(seq, maxm);
        for (;;)
        {
            auto r = p.try_claim(s.size, s.type);
            if (r)
            {
                fill(*r, seq);
                if (!p.commit(s.size))
                    return false;
                break;
            }
            if (r.error() != std::errc::resource_unavailable_try_again || steady::now() > deadline)
                return false;
            std::this_thread::yield();
        }
    }
    return true;
}
struct consumed
{
    std::uint64_t received = 0, bad = 0;
    bool lapped = false, timed_out = false;
    std::uint64_t corrupt = 0;
};
// Reads n messages starting at sequence `first` and verifies every byte.
template<class C>
consumed consume(C& c, const std::uint64_t first, const std::uint64_t n, const std::size_t maxm)
{
    consumed r;
    const auto deadline = steady::now() + time_limit;
    while (r.received < n)
    {
        const std::size_t got = c.poll([&](const dyn::message& m) {
            r.bad += !verify(m, first + r.received, maxm);
            ++r.received;
        });
        if (got == 0)
        {
            if (c.lapped())
            {
                r.lapped = true;
                break;
            }
            if (steady::now() > deadline)
            {
                r.timed_out = true;
                break;
            }
            std::this_thread::yield();
        }
    }
    r.corrupt = c.corrupt();
    return r;
}
// Single-thread pump: writes message `seq`, draining the consumers whenever the ring is full.
template<class P, class... C> void pump(P& p, const std::uint64_t seq, C&&... cs)
{
    const shape s = message_shape(seq, p.max_message());
    for (int attempt = 0;; ++attempt)
    {
        auto r = p.try_claim(s.size, s.type);
        if (r)
        {
            fill(*r, seq);
            REQUIRE(p.commit(s.size));
            return;
        }
        REQUIRE(r.error() == std::errc::resource_unavailable_try_again);
        REQUIRE(attempt < 4);
        (cs(), ...);
    }
}

// ---- fork helpers (as tests/unit/test_ipc.cpp) -------------------------------------------------------------------
template<class F> int in_child(F&& f)
{
    const pid_t pid = ::fork();
    if (pid == 0)
        std::_Exit(f());
    int st = 0;
    ::waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
int wait_child(const pid_t pid)
{
    int st = 0;
    ::waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
// Runs f(ready) in a child that is SIGKILLed once it calls ready(): no destructor, no leave()/detach().
template<class F> void killed_child(F&& f)
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        ::close(fds[0]);
        const int w = fds[1];
        f([w] {
            (void)::write(w, "x", 1);
            for (;;)
                ::pause();
        });
        std::_Exit(1);
    }
    ::close(fds[1]);
    char c = 0;
    REQUIRE(::read(fds[0], &c, 1) == 1);
    ::close(fds[0]);
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st));
}
std::string unique_name(const char* const tag)
{
    return std::string("serreg-dyn-") + tag + "-" + std::to_string(::getpid());
}
// The first hugetlbfs mount with free 2 MiB pages, preferring one this user can write to; empty if none.
std::string huge_2m_mount()
{
    std::string found;
    ipc::region::for_each_hugetlbfs([&](const char* const mnt, const std::size_t pg) {
        if (pg != ipc::page_bytes(ipc::pages::huge_2m))
            return true;
        struct statfs s = {};
        if (::statfs(mnt, &s) == 0 && (s.f_bfree > 0 || s.f_blocks == 0) &&
            (found.empty() || ::access(mnt, W_OK) == 0))
            found = mnt;
        return ::access(found.c_str(), W_OK) != 0;
    });
    return found;
}
std::vector<std::byte> bytes_of(const std::string_view s)
{
    std::vector<std::byte> v(s.size());
    if (!s.empty())
        std::memcpy(v.data(), s.data(), s.size());
    return v;
}
std::string string_of(const dyn::message& m)
{
    return std::string(reinterpret_cast<const char*>(m.data.data()), m.data.size());
}
template<class P>
void put(P& p, const std::string_view s, const std::uint32_t type = dyn::type_data)
{
    const auto b = bytes_of(s);
    REQUIRE(p.write(b.data(), b.size(), type));
}
template<class C> std::vector<std::string> drain(C& c)
{
    std::vector<std::string> out;
    c.poll([&](const dyn::message& m) { out.push_back(string_of(m)); });
    return out;
}
} // namespace

// =================================================================================================================
// geometry, create, attach
// =================================================================================================================
TEST_CASE("ipc: geometry validation and bytes_for", "[ipc]")
{
    using dyn::geometry;
    CHECK(dyn::ring::valid({64, 1024, 4}));
    CHECK(dyn::ring::valid({16, 8, 1}));       // 8 x 16 = 2 granules of 64
    CHECK_FALSE(dyn::ring::valid({16, 4, 1})); // 64 bytes: one granule only
    CHECK(dyn::ring::valid({64, 2, 1}));
    CHECK_FALSE(dyn::ring::valid({64, 1, 1}));
    CHECK_FALSE(dyn::ring::valid({64, 1000, 1})); // not a power of two
    CHECK_FALSE(dyn::ring::valid({8, 1024, 1}));
    CHECK_FALSE(dyn::ring::valid({48, 1024, 1}));
    CHECK_FALSE(dyn::ring::valid({128, 1024, 1}));
    CHECK_FALSE(dyn::ring::valid({64, 1024, 0}));
    // at most 256 readers (detail::max_consumers_limit)
    STATIC_REQUIRE(dyn::detail::max_consumers_limit == 256);
    CHECK(dyn::ring::valid({64, 1024, 256}));
    CHECK_FALSE(dyn::ring::valid({64, 1024, 257}));
    CHECK_FALSE(dyn::ring::valid({64, 1024, 4096}));
    CHECK(dyn::ring::bytes_for({64, 1024, 256}) == 256 + 256 * 64 + 1024 * 64);
    CHECK(dyn::ring::bytes_for({64, 1024, 257}) == 0);
    CHECK(dyn::ring::bytes_for({64, 1024, 4096}) == 0);
    CHECK(dyn::ring::valid({64, 1u << 31, 1}));
    CHECK_FALSE(dyn::ring::valid({64, 0, 1}));
    // a compile-time unit accepts only its own unit size
    CHECK(dyn::basic_ring<32>::valid({32, 64, 1}));
    CHECK_FALSE(dyn::basic_ring<32>::valid({64, 64, 1}));
    CHECK(dyn::basic_ring<32>::bytes_for({64, 64, 1}) == 0);
    // a large Align raises the granule: 64-byte units need at least 2 x 4096 bytes
    CHECK_FALSE(dyn::basic_ring<dyn::dynamic, 4096>::valid({64, 64, 1}));
    CHECK(dyn::basic_ring<dyn::dynamic, 4096>::valid({64, 128, 1}));

    CHECK(dyn::ring::bytes_for({64, 0, 1}) == 0);
    // header 256 + one gate line per consumer, rounded up to 64, then the records
    CHECK(dyn::ring::bytes_for({64, 1024, 4}) == 256 + 4 * 64 + 1024 * 64);
    CHECK(dyn::ring::bytes_for({16, 1024, 1}) == 320 + 1024 * 16);
    CHECK(dyn::ring::bytes_for({32, 1024, 3}) == 448 + 1024 * 32);
    // Align 4096: the record area starts on a 4096 boundary
    CHECK(dyn::basic_ring<dyn::dynamic, 4096>::bytes_for({64, 128, 2}) == 4096 + 128 * 64);
    STATIC_REQUIRE(dyn::ring::bytes_for({64, 16, 1}) == 320 + 1024);
}

TEST_CASE("ipc: for_capacity rounds up to a valid power-of-two geometry", "[ipc]")
{
    const auto a = dyn::ring::for_capacity(1000);
    CHECK(a.unit_bytes == 64);
    CHECK(a.units == 16);
    CHECK(a.max_consumers == 8);
    CHECK(dyn::ring::for_capacity(1024).units == 16);
    CHECK(dyn::ring::for_capacity(1025).units == 32);
    CHECK(dyn::ring::for_capacity(0).units == 2);
    const auto b = dyn::ring::for_capacity(1, 16, 3); // at least 2 granules of 64 bytes
    CHECK(b.unit_bytes == 16);
    CHECK(b.units == 8);
    CHECK(b.max_consumers == 3);
    CHECK(dyn::basic_ring<32>::for_capacity(100).unit_bytes == 32);
    CHECK(dyn::basic_ring<32>::for_capacity(100).units == 4);
    CHECK(dyn::basic_ring<16, 16>::for_capacity(1).units == 2);
    CHECK(dyn::basic_ring<dyn::dynamic, 4096>::for_capacity(1).units == 128);
    for (const std::size_t cap :
         {std::size_t{1}, std::size_t{4097}, std::size_t{1} << 20, std::size_t{3} << 20})
        for (const std::uint32_t u : {16u, 32u, 64u})
        {
            const auto g = dyn::ring::for_capacity(cap, u);
            CHECK(dyn::ring::valid(g));
            CHECK(std::size_t{g.units} * g.unit_bytes >= cap);
            CHECK((g.units == 2 || std::size_t{g.units / 2} * g.unit_bytes < cap ||
                   std::size_t{g.units / 2} * g.unit_bytes < 128));
        }
}

TEST_CASE("ipc: create refuses bad arguments", "[ipc]")
{
    const dyn::geometry g{64, 64, 2};
    auto m = ipc::region::anonymous(dyn::ring::bytes_for(g) + 4096);
    REQUIRE(m);
    auto* const base = static_cast<std::byte*>(m->data());
    const std::size_t need = dyn::ring::bytes_for(g);
    CHECK(dyn::ring::create(nullptr, need, g).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::create(base + 8, need, g).error() ==
          std::errc::invalid_argument); // not 64-aligned
    CHECK(dyn::ring::create(base + 32, need, g).error() ==
          std::errc::invalid_argument); // not 64-aligned
    CHECK(dyn::ring::create(base, need, {64, 63, 2}).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::create(base, need, {24, 64, 2}).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::create(base, need, {64, 64, 0}).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::create(base, need, {64, 64, 257}).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::create(base, need, {64, 64, 257}, dyn::ring_kind::spsc).error() ==
          std::errc::invalid_argument);
    CHECK(dyn::ring::create(base, need, g, static_cast<dyn::ring_kind>(3)).error() ==
          std::errc::invalid_argument);
    CHECK(dyn::ring::create(base, need - 1, g).error() == std::errc::no_buffer_space);
    CHECK(dyn::ring::create(base, 64, g).error() == std::errc::no_buffer_space);
    CHECK(dyn::basic_ring<32>::create(base, need, g).error() == std::errc::invalid_argument);
    CHECK(dyn::basic_ring<dyn::dynamic, 4096>::create(base, need, g).error() ==
          std::errc::invalid_argument);
    CHECK(dyn::ring::create(base + 64, need, g)); // 64-aligned is enough
    auto r = dyn::ring::create(base, need, g);
    REQUIRE(r);
    CHECK(r->bytes() == need);
    CHECK(r->shape().unit_bytes == 64);
    CHECK(r->shape().units == 64);
    CHECK(r->shape().max_consumers == 2);
    CHECK(r->kind() == dyn::ring_kind::spmc);
    CHECK_FALSE(r->is_spsc());
    CHECK(r->capacity_bytes() == 4096);
    CHECK(r->max_message() == 4096 - 16);
    CHECK(r->cursor() == 0);
    CHECK(r->active_consumers() == 0);
    CHECK_FALSE(r->has_producer());
    CHECK(r->record_units(0) == 1);
    CHECK(r->record_units(48) == 1);
    CHECK(r->record_units(49) == 2);
}

TEST_CASE("ipc: at most 256 consumers — create, bytes_for, attach and join at the boundary",
          "[ipc]")
{
    const dyn::geometry ok{64, 64, 256};
    const dyn::geometry over{64, 64, 257};
    CHECK(dyn::ring::bytes_for(over) == 0);
    const std::size_t big = dyn::ring::bytes_for(ok) + 4096; // also room for 257 gates
    auto m = ipc::region::anonymous(big);
    REQUIRE(m);
    auto* const base = static_cast<std::byte*>(m->data());
    CHECK(dyn::ring::create(base, big, over).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::attach(base, big).error() ==
          std::errc::resource_unavailable_try_again); // nothing written
    auto r = dyn::ring::create(base, big, ok);
    REQUIRE(r);
    CHECK(r->shape().max_consumers == 256);
    // 256 consumers can join; the 257th is busy
    std::vector<dyn::consumer> cs;
    for (int i = 0; i < 256; ++i)
    {
        auto c = dyn::consumer::join(*r);
        REQUIRE(c);
        cs.push_back(std::move(*c));
    }
    CHECK(r->active_consumers() == 256);
    CHECK(dyn::consumer::join(*r).error() == std::errc::device_or_resource_busy);
    CHECK(cs.back().id() == 255);
    cs.clear();
    CHECK(r->active_consumers() == 0);
    CHECK(dyn::ring::attach(base, big));
    // a header that claims 257 consumers (data_offset and total_bytes consistent with 257) is refused
    auto& h = ring_header(base);
    const dyn::detail::ring_header saved = h;
    h.max_consumers = 257;
    h.data_offset = dyn::detail::data_offset(over, 64);
    h.total_bytes = h.data_offset + 64 * 64;
    REQUIRE(h.total_bytes <= big);
    CHECK(dyn::ring::attach(base, big).error() == std::errc::invalid_argument);
    h = saved;
    CHECK(dyn::ring::attach(base, big));
}

TEST_CASE("ipc: attach validates the shared header", "[ipc]")
{
    const dyn::geometry g{32, 256, 3};
    const std::size_t need = dyn::ring::bytes_for(g);
    auto m = ipc::region::anonymous(need + 4096);
    REQUIRE(m);
    auto* const base = static_cast<std::byte*>(m->data());

    // nothing constructed yet: try again
    CHECK(dyn::ring::attach(base, need).error() == std::errc::resource_unavailable_try_again);
    CHECK(dyn::ring::attach(nullptr, need).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::attach(base, 63).error() == std::errc::invalid_argument);

    auto created = dyn::ring::create(base, need, g, dyn::ring_kind::spmc);
    REQUIRE(created);
    auto a = dyn::ring::attach(base, need);
    REQUIRE(a);
    CHECK(a->shape().unit_bytes == 32);
    CHECK(a->shape().units == 256);
    CHECK(a->shape().max_consumers == 3);
    CHECK(a->bytes() == need);
    CHECK(dyn::ring::attach(*m)); // region overload
    CHECK(dyn::ring::attach(base + 64, need).error() == std::errc::resource_unavailable_try_again);
    CHECK(dyn::ring::attach(base + 8, need).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::attach(base, need - 1).error() == std::errc::no_buffer_space);
    CHECK(dyn::ring::attach(base, need, dyn::ring_kind::spsc).error() ==
          std::errc::invalid_argument); // wrong kind
    // compile-time unit / alignment mismatches
    CHECK(dyn::basic_ring<32>::attach(base, need));
    CHECK(dyn::basic_ring<64>::attach(base, need).error() == std::errc::invalid_argument);
    CHECK(dyn::basic_ring<16>::attach(base, need).error() == std::errc::invalid_argument);
    CHECK(dyn::basic_ring<dyn::dynamic, 128>::attach(base, need).error() ==
          std::errc::invalid_argument);
    CHECK(dyn::basic_ring<32, 32>::attach(base, need).error() == std::errc::invalid_argument);

    auto& h = ring_header(base);
    const dyn::detail::ring_header saved = h;
    const auto corrupted = [&](auto&& change) {
        h = saved;
        change(h);
        const auto r = dyn::ring::attach(base, need);
        h = saved;
        return r ? std::errc{} : r.error();
    };
    CHECK(corrupted([](auto& x) { x.magic = 0x1234; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.magic = 0; }) == std::errc::resource_unavailable_try_again);
    CHECK(corrupted([](auto& x) { x.version = 1; }) == std::errc::not_supported); // layout 1
    CHECK(corrupted([](auto& x) { x.version = 3; }) == std::errc::not_supported);
    CHECK(corrupted([](auto& x) { x.version = 0; }) == std::errc::not_supported);
    CHECK(corrupted([](auto& x) { x.kind = 7; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.unit = 48; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.unit = 64; }) ==
          std::errc::invalid_argument); // total_bytes inconsistent
    CHECK(corrupted([](auto& x) { x.align = 128; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.units = 255; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.units = 512; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.units = 0; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.max_consumers = 0; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.max_consumers = 4; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.max_consumers = 0xFFFFFFFF; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.data_offset += 64; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.data_offset = 0; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.total_bytes += 1; }) == std::errc::invalid_argument);
    CHECK(corrupted([](auto& x) { x.total_bytes = 1ull << 60; }) == std::errc::invalid_argument);
    CHECK(dyn::ring::attach(base, need)); // restored
    CHECK(corrupted([](auto& x) { x.max_consumers = 256; }) ==
          std::errc::invalid_argument); // offsets for 3

    // a private copy of the header: changing the shared header later does not move an attached handle
    auto p = dyn::producer::attach(*a);
    REQUIRE(p);
    h.units = 1u << 30;
    put(*p, "still in range");
    CHECK(a->shape().units == 256);
    h = saved;
}

TEST_CASE("ipc: attach with a compile-time unit refuses a ring of another unit", "[ipc]")
{
    for (const std::uint32_t u : {16u, 32u, 64u})
    {
        auto t = make_ring({u, 64, 1});
        CHECK(static_cast<bool>(dyn::basic_ring<16>::attach(t.mem)) == (u == 16));
        CHECK(static_cast<bool>(dyn::basic_ring<32>::attach(t.mem)) == (u == 32));
        CHECK(static_cast<bool>(dyn::basic_ring<64>::attach(t.mem)) == (u == 64));
        CHECK(dyn::ring::attach(t.mem));
        CHECK(dyn::basic_ring<dyn::dynamic, 16>::attach(t.mem).error() ==
              std::errc::invalid_argument);
    }
    auto t16 = make_ring<dyn::basic_ring<16, 16>>({16, 64, 1});
    CHECK(dyn::basic_ring<16, 16>::attach(t16.mem));
    CHECK(dyn::basic_ring<16>::attach(t16.mem).error() == std::errc::invalid_argument);
    CHECK(dyn::ring::attach(t16.mem).error() == std::errc::invalid_argument);
}

TEST_CASE("ipc: an SPSC ring created with max_consumers 1 attaches", "[ipc][spsc]")
{
    auto t = make_ring({64, 64, 1}, dyn::ring_kind::spsc);
    auto a = dyn::ring::attach(t.mem, dyn::ring_kind::spsc);
    REQUIRE(a);
    CHECK(a->is_spsc());
    CHECK(a->shape().max_consumers == 1);
    CHECK(dyn::ring::attach(t.mem, dyn::ring_kind::spmc).error() == std::errc::invalid_argument);
}

// regression (fixed in 2.1.x; ring.hpp create): for SPSC the header stored max_consumers = 1 but
// data_offset/total_bytes were computed from the caller's geometry (max_consumers = 8 here), so attach()
// recomputed them for one consumer and refused the ring.
TEST_CASE("ipc: an SPSC ring created with the default max_consumers attaches", "[ipc][spsc]")
{
    auto t = make_ring({64, 64, 8}, dyn::ring_kind::spsc);
    CHECK(t.ring.shape().max_consumers == 1);
    auto a = dyn::ring::attach(t.mem, dyn::ring_kind::spsc);
    REQUIRE(a);
    // and the attached handle sees the creator's records at the same place
    auto p = dyn::producer::attach(t.ring);
    REQUIRE(p);
    put(*p, "hello");
    auto c = dyn::consumer::join(*a);
    REQUIRE(c);
    CHECK(drain(*c) == std::vector<std::string>{"hello"});
}

TEST_CASE("ipc: runtime units 16, 32 and 64, and Align 16 / 4096", "[ipc]")
{
    for (const std::uint32_t u : {16u, 32u, 64u})
    {
        auto t = make_ring({u, 1024 / u * 4, 2}); // 4 KiB
        const std::size_t maxm = t.ring.max_message();
        std::uint64_t got = 0, bad = 0;
        CHECK(t.ring.unit() == u);
        CHECK(t.ring.capacity_bytes() == 4096);
        CHECK(t.ring.record_units(0) == 64 / u); // granule 64 (Align)
        auto p = dyn::producer::attach(t.ring);
        auto c = dyn::consumer::join(t.ring);
        REQUIRE((p && c));
        for (std::uint64_t seq = 0; seq < 500; ++seq)
            pump(*p, seq,
                 [&] { c->poll([&](const dyn::message& m) { bad += !verify(m, got++, maxm); }); });
        c->poll([&](const dyn::message& m) { bad += !verify(m, got++, maxm); });
        CHECK(got == 500);
        CHECK(bad == 0);
        CHECK(c->corrupt() == 0);
    }
    { // Align 16 with 16-byte units: an empty record is one unit
        auto t = make_ring<dyn::basic_ring<16, 16>>({16, 16, 1});
        CHECK(t.ring.record_units(0) == 1);
        CHECK(t.ring.record_units(1) == 2);
        auto p = dyn::basic_producer<16, 16>::attach(t.ring);
        auto c = dyn::basic_consumer<16, 16>::join(t.ring);
        REQUIRE((p && c));
        std::vector<std::uint32_t> pos;
        for (int i = 0; i < 20; ++i)
        {
            REQUIRE(p->claim(0));
            REQUIRE(p->commit(0));
            c->poll([&](const dyn::message& m) { pos.push_back(m.position); });
        }
        REQUIRE(pos.size() == 20);
        for (std::uint32_t i = 0; i < 20; ++i)
            CHECK(pos[i] == i);
    }
    { // Align 4096: records start on 4096-byte boundaries
        auto t = make_ring<dyn::basic_ring<64, 4096>>({64, 256, 1});
        CHECK(reinterpret_cast<std::uintptr_t>(&header_at(t, 0)) % 4096 == 0);
        auto p = dyn::basic_producer<64, 4096>::attach(t.ring);
        auto c = dyn::basic_consumer<64, 4096>::join(t.ring);
        REQUIRE((p && c));
        std::vector<std::uint32_t> pos;
        for (int i = 0; i < 9; ++i)
        {
            put(*p, "x");
            c->poll([&](const dyn::message& m) {
                pos.push_back(m.position);
                CHECK(reinterpret_cast<std::uintptr_t>(m.data.data() - 16) % 4096 == 0);
            });
        }
        REQUIRE(pos.size() == 9);
        CHECK(pos[1] == 64);
        CHECK(pos[4] == 0 + 256); // wrapped once: 4 records of 64 units fill the ring
    }
}

// =================================================================================================================
// single thread: the producer API, records and the skip logic
// =================================================================================================================
TEST_CASE("ipc: round trip of payload sizes 0..600 with positions and types", "[ipc]")
{
    for (const std::uint32_t u : {16u, 32u, 64u})
    {
        auto t = make_ring({u, 4096 / u, 1});
        auto p = dyn::producer::attach(t.ring);
        auto c = dyn::consumer::join(t.ring);
        REQUIRE((p && c));
        std::uint32_t expect_pos = 0;
        std::size_t bad = 0;
        for (std::uint32_t n = 0; n <= 600; ++n)
        {
            std::vector<std::byte> b(n);
            for (std::uint32_t i = 0; i < n; ++i)
                b[i] = static_cast<std::byte>(n + i);
            REQUIRE(p->write(b.data(), n, n * 7));
            const std::size_t got = c->poll([&](const dyn::message& m) {
                const std::uint32_t to_end = 4096 / u - (expect_pos & (4096 / u - 1));
                if (t.ring.record_units(n) > to_end)
                    expect_pos += to_end; // a skip record padded the end of the buffer
                bad += m.position != expect_pos || m.type != n * 7 || m.data.size() != n ||
                       (n && std::memcmp(m.data.data(), b.data(), n) != 0);
                expect_pos += static_cast<std::uint32_t>(t.ring.record_units(n));
            });
            CHECK(got == 1);
        }
        CHECK(bad == 0);
        CHECK(t.ring.cursor() == expect_pos);
        CHECK(c->corrupt() == 0);
    }
}

TEST_CASE("ipc: a record that exactly fills the end of the buffer needs no skip", "[ipc]")
{
    auto t = make_ring({64, 16, 1}); // 16 units of 64 bytes
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    for (int i = 0; i < 4; ++i)
        put(*p, "abc"); // positions 0..3
    CHECK(drain(*c).size() == 4);
    REQUIRE(p->position() == 4);
    // 12 units to the end: 12 * 64 - 16 bytes fit exactly
    auto s = p->claim(12 * 64 - 16, 5);
    REQUIRE(s);
    CHECK(s->size() == 12 * 64 - 16);
    std::memset(s->data(), 0x5A, s->size());
    REQUIRE(p->commit(s->size()));
    CHECK(t.ring.cursor() == 16);
    std::optional<dyn::message> m = c->peek();
    REQUIRE(m);
    CHECK(m->position == 4);
    CHECK(m->type == 5);
    CHECK(m->data.size() == 12 * 64 - 16);
    CHECK(m->data.back() == std::byte{0x5A});
    CHECK(c->release());
    put(*p, "next"); // starts at position 16 = index 0
    m = c->peek();
    REQUIRE(m);
    CHECK(m->position == 16);
    CHECK(string_of(*m) == "next");
    CHECK(c->release());
}

TEST_CASE("ipc: a record one unit too big for the end is preceded by a skip record", "[ipc]")
{
    auto t = make_ring({64, 16, 1});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    for (int i = 0; i < 4; ++i)
        put(*p, "abc");
    CHECK(drain(*c).size() == 4);
    auto s = p->claim(12 * 64 - 16 + 1, 6); // 13 units, 12 to the end
    // the skip is published at once; the record at position 16 needs 13 units, but the consumer still holds the
    // 12 of the skip (full_policy::fail: try again)
    REQUIRE_FALSE(s);
    CHECK(s.error() == std::errc::resource_unavailable_try_again);
    CHECK(t.ring.cursor() == 16);
    CHECK(p->position() == 16);
    CHECK(c->pending_units() == 12);
    CHECK(c->poll([](const dyn::message&) {}) == 0); // passes over the skip, delivers nothing
    CHECK(c->position() == 16);
    s = p->claim(12 * 64 - 16 + 1, 6);
    REQUIRE(s);
    CHECK(t.ring.cursor() == 16);
    std::memset(s->data(), 0x6B, s->size());
    REQUIRE(p->commit(s->size()));
    CHECK(t.ring.cursor() == 29);
    std::vector<dyn::message> seen;
    const auto nested_775 = c->poll([&](const dyn::message& m) {
        seen.push_back(m);
        CHECK(m.data.size() == 12 * 64 - 15);
        CHECK(m.data.front() == std::byte{0x6B});
        CHECK(m.data.back() == std::byte{0x6B});
    }) == 1; // Catch2 assertions inside the lambda: not nested (-Wshadow)
    CHECK(nested_775);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].position == 16); // the skip record is never delivered
    CHECK(seen[0].type == 6);
    CHECK(c->corrupt() == 0);

    // to_end == 1: a one-unit skip (payload 48 bytes) before a 2-unit record
    while (p->position() % 16 != 15)
        put(*p, "f");
    drain(*c);
    put(*p, std::string(60, 'z')); // 2 units
    const auto v = drain(*c);
    REQUIRE(v.size() == 1);
    CHECK(v[0] == std::string(60, 'z'));
    CHECK(c->position() == 32 + 2);
    CHECK(c->corrupt() == 0);
}

TEST_CASE("ipc: records of max_message() (the whole ring), from a full and an empty start", "[ipc]")
{
    for (const std::uint32_t u : {16u, 32u, 64u})
    {
        auto t = make_ring({u, 2048 / u, 1}); // 2 KiB
        auto p = dyn::producer::attach(t.ring, full_policy::fail);
        auto c = dyn::consumer::join(t.ring);
        REQUIRE((p && c));
        const std::size_t maxm = t.ring.max_message();
        CHECK(maxm == 2048 - 16);
        CHECK(p->max_message() == maxm);
        CHECK(p->claim(maxm + 1).error() == std::errc::message_size);
        CHECK(p->claim(std::size_t{1} << 40).error() == std::errc::message_size);
        for (int round = 0; round < 6; ++round)
        {
            if (round % 2)
                put(*p, "small"); // the next big record needs a skip first
            auto s = p->try_claim(maxm, static_cast<std::uint32_t>(round));
            if (!s)
            { // the skip is published; the record waits for the consumer to drain it
                CHECK(s.error() == std::errc::resource_unavailable_try_again);
                CHECK(round % 2 == 1);
                CHECK(drain(*c).size() == 1);
                s = p->try_claim(maxm, static_cast<std::uint32_t>(round));
            }
            REQUIRE(s);
            fill(*s, 1000u + static_cast<std::uint32_t>(round));
            REQUIRE(p->commit(maxm));
            CHECK(p->position() % (2048 / u) == 0);
            // the ring is now completely full
            CHECK(p->try_claim(0).error() == std::errc::resource_unavailable_try_again);
            std::size_t got = 0;
            c->poll([&](const dyn::message& m) {
                ++got;
                CHECK(m.data.size() == maxm);
                CHECK(m.type == static_cast<std::uint32_t>(round));
                CHECK(std::memcmp(m.data.data() + 8,
                                  pattern() + (1000u + static_cast<unsigned>(round)) % 509 + 8,
                                  maxm - 8) == 0);
            });
            CHECK(got == 1u);
        }
        CHECK(c->corrupt() == 0);
    }
}

// regression (fixed in 2.1.x; consumer.hpp peek): passing over a skip record advanced next_ but published no
// progress when nothing followed it, so the gate stayed in front of the skip. A producer waiting at position 0
// for a record larger than the free space behind the skip then waited forever while the peek()/wait() consumer
// kept returning nullopt.
TEST_CASE("ipc: peek() releases a skip record even when no message follows it", "[ipc]")
{
    auto t = make_ring({64, 16, 1});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    put(*p, "a");
    REQUIRE(c->peek());
    REQUIRE(c->release());
    CHECK(p->try_claim(t.ring.max_message()).error() == std::errc::resource_unavailable_try_again);
    CHECK(t.ring.cursor() == 16); // the skip (15 units) is published
    CHECK_FALSE(c->peek());       // only a skip to read: nothing to deliver
    CHECK(c->position() == 16);
    // the consumer has read everything, so the whole ring is free again
    CHECK(p->try_claim(t.ring.max_message()));
}

TEST_CASE("ipc: poll() releases a skip record even when no message follows it", "[ipc]")
{
    auto t = make_ring({64, 16, 1});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    put(*p, "a");
    CHECK(drain(*c).size() == 1);
    CHECK(p->try_claim(t.ring.max_message()).error() == std::errc::resource_unavailable_try_again);
    CHECK(c->poll([](const dyn::message&) {}) == 0);
    CHECK(c->position() == 16);
    CHECK(p->try_claim(t.ring.max_message()));
}

TEST_CASE("ipc: many wrap-arounds of a tiny ring with mixed sizes up to max_message()", "[ipc]")
{
    for (const std::uint32_t u : {16u, 32u, 64u})
    {
        auto t = make_ring({u, 1024 / u, 2}); // 1 KiB: almost every message wraps
        auto p = dyn::producer::attach(t.ring, full_policy::fail);
        auto c1 = dyn::consumer::join(t.ring);
        auto c2 = dyn::consumer::join(t.ring);
        REQUIRE((p && c1 && c2));
        const std::size_t maxm = t.ring.max_message();
        std::uint64_t r1 = 0, r2 = 0, bad = 0;
        const auto drain_both = [&] {
            c1->poll([&](const dyn::message& m) { bad += !verify(m, r1++, maxm); });
            c2->poll([&](const dyn::message& m) { bad += !verify(m, r2++, maxm); });
        };
        constexpr std::uint64_t n = 20000;
        for (std::uint64_t seq = 0; seq < n; ++seq)
            pump(*p, seq, drain_both);
        drain_both();
        CHECK(r1 == n);
        CHECK(r2 == n);
        CHECK(bad == 0);
        CHECK(c1->corrupt() + c2->corrupt() == 0);
        CHECK(t.ring.cursor() > 4u * t.g.units); // wrapped many times
    }
}

TEST_CASE("ipc: positions wrap across 2^32 (cursor and gates started near the end)", "[ipc]")
{
    for (const auto k : {dyn::ring_kind::spmc, dyn::ring_kind::spsc})
        for (const std::uint32_t u : {16u, 64u})
        {
            auto t = make_ring({u, 2048 / u, 1}, k);
            constexpr std::uint32_t start = 0xFFFF'FF00u;
            set_cursor(t.base(), start);
            if (k == dyn::ring_kind::spsc)
                set_gate_pos(t.base(), 0, start);
            auto p = dyn::producer::attach(t.ring, full_policy::fail);
            auto c = dyn::consumer::join(t.ring);
            REQUIRE((p && c));
            CHECK(p->position() == start);
            CHECK(c->position() == start);
            const std::size_t maxm = t.ring.max_message();
            std::uint64_t got = 0, bad = 0;
            bool crossed = false;
            const auto drain1 = [&] {
                c->poll([&](const dyn::message& m) {
                    bad += !verify(m, got++, maxm);
                    crossed |= m.position < 0x8000'0000u;
                });
            };
            for (std::uint64_t seq = 0; seq < 3000; ++seq)
                pump(*p, seq, drain1);
            drain1();
            CHECK(got == 3000);
            CHECK(bad == 0);
            CHECK(crossed);
            CHECK(t.ring.cursor() < start);
            CHECK(c->corrupt() == 0);
        }
}

TEST_CASE("ipc: types are preserved; reserved types and oversize messages are refused", "[ipc]")
{
    auto t = make_ring({64, 64, 1});
    auto p = dyn::producer::attach(t.ring);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    for (const std::uint32_t ty : {0u, 1u, 77u, 0xFFFFu, dyn::type_reserved_first - 1})
        put(*p, "t", ty);
    std::vector<std::uint32_t> types;
    c->poll([&](const dyn::message& m) { types.push_back(m.type); });
    CHECK(types == std::vector<std::uint32_t>{0u, 1u, 77u, 0xFFFFu, dyn::type_reserved_first - 1});
    for (const std::uint32_t ty :
         {dyn::type_reserved_first, dyn::type_reserved_first + 1, dyn::type_skip})
    {
        CHECK(p->claim(1, ty).error() == std::errc::invalid_argument);
        CHECK(p->try_claim(1, ty).error() == std::errc::invalid_argument);
        CHECK_FALSE(p->write(nullptr, 0, ty));
        CHECK_FALSE(p->publish(4, ty, [](std::span<std::byte>) { return std::size_t{1}; }));
    }
    CHECK(p->claim(t.ring.max_message() + 1).error() == std::errc::message_size);
    CHECK_FALSE(p->commit(0)); // the refused claims left nothing open
    CHECK(t.ring.cursor() == 5);
    CHECK(c->poll([](const dyn::message&) {}) == 0);
}

TEST_CASE("ipc: full_policy::fail when full, and try_claim under spin", "[ipc]")
{
    for (const auto k : {dyn::ring_kind::spsc, dyn::ring_kind::spmc})
    {
        auto t = make_ring({64, 32, 1}, k);
        auto p = dyn::producer::attach(t.ring, full_policy::fail);
        REQUIRE(p);
        std::optional<dyn::basic_consumer<>> c;
        if (k ==
            dyn::ring_kind::spmc) // an SPMC ring without consumer runs free; SPSC gates without one
        {
            auto j = dyn::consumer::join(t.ring);
            REQUIRE(j);
            c.emplace(std::move(*j));
        }
        const std::array<std::byte, 100> buf{};
        int written = 0;
        while (p->write(buf.data(), 100)) // 2 units each
            ++written;
        CHECK(written == 16);
        CHECK(p->claim(100).error() == std::errc::resource_unavailable_try_again);
        CHECK(p->claim(0).error() == std::errc::resource_unavailable_try_again);
        p->policy(full_policy::spin);
        CHECK(p->try_claim(0).error() == std::errc::resource_unavailable_try_again);
        if (!c)
        {
            auto j = dyn::consumer::join(t.ring);
            REQUIRE(j);
            c.emplace(std::move(*j));
        }
        CHECK(c->pending_units() == 32);
        CHECK(c->poll([](const dyn::message&) {}, 3) == 3);
        int more = 0;
        while (p->try_claim(100) && p->commit(100))
            ++more;
        CHECK(more == 3);
        CHECK(c->poll([](const dyn::message&) {}) == 16);
    }
}

TEST_CASE("ipc: cancel, abandoned claims, short commits, commit without a claim", "[ipc]")
{
    auto t = make_ring({64, 64, 1});
    auto p = dyn::producer::attach(t.ring);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    CHECK_FALSE(p->commit(0)); // no claim
    auto s = p->claim(100);
    REQUIRE(s);
    p->cancel();
    CHECK_FALSE(p->commit(100));
    CHECK(t.ring.cursor() == 0);

    auto a = p->claim(500);
    REQUIRE(a);
    auto* const first = a->data();
    auto b = p->claim(10, 3); // abandons the first claim: same place
    REQUIRE(b);
    CHECK(b->data() == first);
    CHECK(b->size() == 10);
    CHECK_FALSE(p->commit(11)); // longer than claimed: refused, the claim stays open
    std::memcpy(b->data(), "0123456789", 10);
    CHECK(p->commit(4)); // short commit: occupies record_units(4)
    CHECK_FALSE(p->commit(4));
    CHECK(t.ring.cursor() == 1);
    put(*p, "after");
    std::vector<dyn::message> ms;
    c->poll([&](const dyn::message& m) { ms.push_back(m); });
    REQUIRE(ms.size() == 2);
    CHECK(ms[0].data.size() == 4);
    CHECK(std::memcmp(ms[0].data.data(), "0123", 4) == 0);
    CHECK(ms[0].type == 3);
    CHECK(ms[1].position == 1);

    // a large claim committed short frees the rest
    auto big = p->claim(3000);
    REQUIRE(big);
    CHECK(p->commit(0));
    CHECK(t.ring.cursor() == 3);
}

TEST_CASE("ipc: deferred commits become visible with flush() or the next publishing commit",
          "[ipc]")
{
    auto t = make_ring({32, 128, 1});
    auto p = dyn::producer::attach(t.ring);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    for (int i = 0; i < 3; ++i)
    {
        auto s = p->claim(8, static_cast<std::uint32_t>(i));
        REQUIRE(s);
        std::memcpy(s->data(), "deferred", 8);
        REQUIRE(p->commit(8, false));
    }
    CHECK(t.ring.cursor() == 0);
    CHECK(c->pending_units() == 0);
    CHECK(c->poll([](const dyn::message&) {}) == 0);
    CHECK_FALSE(c->peek());
    p->flush();
    CHECK(t.ring.cursor() == 6);
    CHECK(drain(*c) == std::vector<std::string>{"deferred", "deferred", "deferred"});
    p->flush(); // nothing deferred: no change
    CHECK(t.ring.cursor() == 6);

    REQUIRE(p->claim(1));
    REQUIRE(p->commit(1, false));
    put(*p, "now"); // a publishing commit publishes the deferred record too
    CHECK(drain(*c).size() == 2);

    REQUIRE(p->claim(1));
    REQUIRE(p->commit(1, false));
    p->detach(); // detach flushes
    CHECK_FALSE(t.ring.has_producer());
    CHECK(drain(*c).size() == 1);
}

TEST_CASE("ipc: publish() with fill, abandon and overlong results", "[ipc]")
{
    auto t = make_ring({64, 64, 1});
    auto p = dyn::producer::attach(t.ring);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    const auto nested_1088 = p->publish(64, 9, [](const std::span<std::byte> d) {
        CHECK(d.size() == 64);
        std::memcpy(d.data(), "xyz", 3);
        return std::size_t{3};
    }); // Catch2 assertions inside the lambda: not nested (-Wshadow)
    CHECK(nested_1088);
    CHECK_FALSE(p->publish(64, 9, [](std::span<std::byte>) { return dyn::abandon; }));
    CHECK_FALSE(p->publish(64, 9, [](std::span<std::byte>) { return std::size_t{65}; }));
    CHECK_FALSE(p->commit(0)); // abandoned publishes leave no open claim
    CHECK_FALSE(p->publish(t.ring.max_message() + 1, 9,
                           [](std::span<std::byte>) { return std::size_t{0}; }));
    CHECK(p->publish(0, 10, [](std::span<std::byte>) { return std::size_t{0}; }));
    std::vector<dyn::message> ms;
    c->poll([&](const dyn::message& m) { ms.push_back(m); });
    REQUIRE(ms.size() == 2);
    CHECK(string_of(ms[0]) == "xyz");
    CHECK(ms[0].type == 9);
    CHECK(ms[1].data.empty());
    CHECK(ms[1].type == 10);
}

TEST_CASE("ipc: producer role — busy, detach, move", "[ipc]")
{
    auto t = make_ring({64, 64, 1});
    auto p = dyn::producer::attach(t.ring);
    REQUIRE(p);
    CHECK(t.ring.has_producer());
    CHECK(dyn::producer::attach(t.ring).error() == std::errc::device_or_resource_busy);
    dyn::producer moved = std::move(*p);
    CHECK(t.ring.has_producer());
    put(moved, "via moved");
    p->detach(); // moved-from: inert
    CHECK(t.ring.has_producer());
    moved.detach();
    CHECK_FALSE(t.ring.has_producer());
    auto q = dyn::producer::attach(t.ring);
    REQUIRE(q);
    CHECK(q->position() == t.ring.cursor()); // continues at the cursor
    auto r2 = make_ring({64, 64, 1});
    auto other = dyn::producer::attach(r2.ring);
    REQUIRE(other);
    *other = std::move(*q); // move assignment detaches the target's role first
    CHECK_FALSE(r2.ring.has_producer());
    CHECK(t.ring.has_producer());
}

TEST_CASE("ipc: peek/release, wait, poll with max and early stop", "[ipc]")
{
    auto t = make_ring({64, 64, 1});
    auto p = dyn::producer::attach(t.ring);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    CHECK_FALSE(c->peek());
    CHECK_FALSE(c->release()); // nothing peeked
    for (int i = 0; i < 6; ++i)
        put(*p, std::to_string(i));
    auto m = c->peek();
    REQUIRE(m);
    CHECK(string_of(*m) == "0");
    auto again = c->peek(); // same message until released
    REQUIRE(again);
    CHECK(again->position == m->position);
    CHECK(c->poll([](const dyn::message&) {}) == 0); // a peeked message blocks poll()
    CHECK(c->release());
    CHECK_FALSE(c->release());
    auto w = c->wait();
    REQUIRE(w);
    CHECK(string_of(*w) == "1");
    CHECK(c->release());
    std::vector<std::string> seen;
    CHECK(c->poll([&](const dyn::message& x) { seen.push_back(string_of(x)); }, 2) == 2);
    CHECK(c->poll([&](const dyn::message& x) {
        seen.push_back(string_of(x));
        return false; // stop after this one
    }) == 1);
    CHECK(c->poll([&](const dyn::message& x) { seen.push_back(string_of(x)); }) == 1);
    CHECK(seen == std::vector<std::string>{"2", "3", "4", "5"});
    CHECK(c->pending_units() == 0);
    // resync drops a backlog
    for (int i = 0; i < 4; ++i)
        put(*p, "old");
    CHECK(c->pending_units() == 4);
    CHECK(c->resync());
    CHECK(c->pending_units() == 0);
    put(*p, "new");
    CHECK(drain(*c) == std::vector<std::string>{"new"});
}

// =================================================================================================================
// SPSC
// =================================================================================================================
TEST_CASE("ipc SPSC: messages wait for a consumer; hand-over keeps the position", "[ipc][spsc]")
{
    auto t = make_ring({32, 64, 1}, dyn::ring_kind::spsc);
    CHECK(t.ring.is_spsc());
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    for (int i = 0; i < 6; ++i)
        put(*p, "m" + std::to_string(i));
    CHECK(t.ring.active_consumers() == 0);
    {
        auto a = dyn::consumer::join(t.ring);
        REQUIRE(a);
        CHECK(t.ring.active_consumers() == 1);
        CHECK(dyn::consumer::join(t.ring).error() == std::errc::device_or_resource_busy);
        std::vector<std::string> got;
        a->poll([&](const dyn::message& m) { got.push_back(string_of(m)); }, 2);
        CHECK(got == std::vector<std::string>{"m0", "m1"});
        auto pk = a->peek(); // peeked, never released: the next consumer gets it again
        REQUIRE(pk);
        CHECK(string_of(*pk) == "m2");
        CHECK_FALSE(t.ring.evict(a->id())); // a queue never drops messages
    } // leave
    CHECK(t.ring.active_consumers() == 0);
    put(*p, "m6");
    auto b = dyn::consumer::join(t.ring);
    REQUIRE(b);
    CHECK(drain(*b) == std::vector<std::string>{"m2", "m3", "m4", "m5", "m6"});
    b->leave();
    b->leave(); // idempotent
    auto c = dyn::consumer::join(t.ring);
    REQUIRE(c);
    CHECK(drain(*c).empty());
    put(*p, "m7");
    CHECK(drain(*c) == std::vector<std::string>{"m7"});
}

TEST_CASE("ipc SPSC: the gate holds the producer while no consumer is joined", "[ipc][spsc]")
{
    auto t = make_ring({64, 16, 1}, dyn::ring_kind::spsc);
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    const std::array<std::byte, 48> buf{};
    int n = 0;
    while (p->write(buf.data(), 48))
        ++n;
    CHECK(n == 16);
    CHECK(t.ring.reap_dead_consumers() == 0);
    {
        auto c = dyn::consumer::join(t.ring);
        REQUIRE(c);
        CHECK(c->poll([](const dyn::message&) {}, 5) == 5);
    }
    for (int i = 0; i < 5; ++i)
        CHECK(p->write(buf.data(), 48));
    CHECK_FALSE(p->write(buf.data(), 48));
}

// =================================================================================================================
// SPMC
// =================================================================================================================
TEST_CASE("ipc SPMC: every consumer sees every message; the slowest gates", "[ipc][spmc]")
{
    auto t = make_ring({64, 64, 3});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    put(*p, "before anyone joined"); // a broadcast ring without consumers runs free
    auto c1 = dyn::consumer::join(t.ring);
    auto c2 = dyn::consumer::join(t.ring);
    auto c3 = dyn::consumer::join(t.ring);
    REQUIRE((c1 && c2 && c3));
    CHECK(t.ring.active_consumers() == 3);
    CHECK(c1->id() != c2->id());
    CHECK(dyn::consumer::join(t.ring).error() == std::errc::device_or_resource_busy);
    const std::size_t maxm = t.ring.max_message();
    std::array<std::uint64_t, 3> got{}, bad{};
    std::array<dyn::consumer*, 3> cs{&*c1, &*c2, &*c3};
    for (std::uint64_t seq = 0; seq < 3000; ++seq)
        pump(*p, seq, [&] {
            for (std::size_t i = 0; i < 3; ++i)
                cs[i]->poll([&](const dyn::message& m) { bad[i] += !verify(m, got[i]++, maxm); });
        });
    for (std::size_t i = 0; i < 3; ++i)
    {
        cs[i]->poll([&](const dyn::message& m) { bad[i] += !verify(m, got[i]++, maxm); });
        CHECK(got[i] == 3000);
        CHECK(bad[i] == 0);
    }
    // c3 stops reading: the ring fills up; c3 reading frees it
    while (p->write(nullptr, 0))
        drain(*c1), drain(*c2);
    CHECK(c3->pending_units() == 64);
    drain(*c3);
    CHECK(p->write(nullptr, 0));
    // a consumer that leaves stops gating, and its gate is free again
    c3->leave();
    CHECK(t.ring.active_consumers() == 2);
    auto c4 = dyn::consumer::join(t.ring);
    REQUIRE(c4);
    CHECK(c4->position() == t.ring.cursor());
    CHECK(c4->pending_units() == 0);
}

TEST_CASE("ipc SPMC: evict — the evicted consumer is lapped and can never move the gate",
          "[ipc][spmc]")
{
    auto t = make_ring({64, 16, 2});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto a = dyn::consumer::join(t.ring);
    REQUIRE((p && a));
    for (int i = 0; i < 16; ++i)
        put(*p, "x" + std::to_string(i));
    CHECK_FALSE(p->write(nullptr, 0)); // a gates: full
    auto held = a->peek();             // a holds message 0 in place
    REQUIRE(held);
    const std::uint32_t id = a->id();
    CHECK_FALSE(t.ring.evict(id + 5)); // bad id
    CHECK_FALSE(t.ring.evict(1));      // free gate
    CHECK(t.ring.evict(id));
    CHECK_FALSE(t.ring.evict(id)); // already evicted
    CHECK(t.ring.active_consumers() == 0);
    // the producer is no longer gated by a
    for (int i = 0; i < 16; ++i)
        put(*p, "y" + std::to_string(i)); // overwrites everything a had
    // the evicted gate is not reused before a acknowledges: b takes another one
    CHECK(dyn::detail::state_of(gate_word(t.base(), id)) == dyn::detail::gate_evicted);
    auto b = dyn::consumer::join(t.ring);
    REQUIRE(b);
    CHECK(b->id() != id);
    const std::uint64_t bw = gate_word(t.base(), b->id());
    const std::uint32_t bpos = gate_pos(t.base(), b->id());
    CHECK_FALSE(a->release()); // what a held was overwritten: lapped
    CHECK(a->lapped());
    CHECK(gate_word(t.base(), id) == 0); // a acknowledged the eviction: its gate is free again
    CHECK(a->poll([](const dyn::message&) {}) == 0);
    CHECK_FALSE(a->peek());
    CHECK_FALSE(a->wait());
    CHECK_FALSE(a->resync());
    a->leave();
    CHECK(gate_word(t.base(), id) == 0);
    CHECK(gate_word(t.base(), b->id()) ==
          bw); // b's gate untouched by every call of the evicted consumer
    CHECK(gate_pos(t.base(), b->id()) == bpos);
    CHECK(t.ring.active_consumers() == 1);
    {
        auto again =
            dyn::consumer::join(t.ring); // the acknowledged gate is free for the next joiner
        REQUIRE(again);
        CHECK(again->id() == id);
    }
    put(*p, "for b");
    CHECK(drain(*b) == std::vector<std::string>{"for b"});
    // b gates normally
    while (p->write(nullptr, 0))
    {
    }
    CHECK(b->pending_units() == 16);
}

TEST_CASE("ipc SPMC: an evicted consumer stops before reading (owned check)", "[ipc][spmc]")
{
    auto t = make_ring({64, 16, 2});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto a = dyn::consumer::join(t.ring);
    auto other = dyn::consumer::join(t.ring);
    REQUIRE((p && a && other));
    put(*p, "one");
    put(*p, "two");
    CHECK(t.ring.evict(a->id()));
    CHECK(a->poll([](const dyn::message&) {}) == 0);
    CHECK(a->lapped());
    CHECK(drain(*other).size() == 2);
    // join again to continue
    auto again = dyn::consumer::join(t.ring);
    REQUIRE(again);
    put(*p, "three");
    CHECK(drain(*again) == std::vector<std::string>{"three"});
}

// =================================================================================================================
// corrupt headers (a buggy or hostile peer writes into the record area below the cursor)
// =================================================================================================================
TEST_CASE("ipc: corrupt record headers are counted, skipped to the cursor, never delivered",
          "[ipc][corrupt]")
{
    struct forged
    {
        const char* what;
        void (*apply)(dyn::detail::record_header&, std::size_t maxm);
    };
    const std::array<forged, 9> cases{{
        {"garbage",
         [](dyn::detail::record_header& h, std::size_t) { std::memset(&h, 0xAB, sizeof h); }},
        {"zero", [](dyn::detail::record_header& h, std::size_t) { std::memset(&h, 0, sizeof h); }},
        {"wrong seq",
         [](dyn::detail::record_header& h, std::size_t) { forge_header(h, 40, 5, 0); }},
        {"bad check", [](dyn::detail::record_header& h, std::size_t) { ++h.check; }},
        {"size > max_message",
         [](dyn::detail::record_header& h, std::size_t m) {
             forge_header(h, static_cast<std::uint32_t>(m + 1), 1, 0);
         }},
        {"size 2^32-1",
         [](dyn::detail::record_header& h, std::size_t) { forge_header(h, 0xFFFFFFFFu, 1, 0); }},
        {"past the cursor",
         [](dyn::detail::record_header& h, std::size_t) { forge_header(h, 200, 1, 0); }},
        {"short skip", [](dyn::detail::record_header& h,
                          std::size_t) { forge_header(h, 40, 1, dyn::type_skip); }},
        {"skip to end beyond cursor",
         [](dyn::detail::record_header& h, std::size_t) {
             forge_header(h, 63 * 64 - 16, 1, dyn::type_skip);
         }},
    }};
    for (const auto& fc : cases)
    {
        INFO(fc.what);
        auto t = make_ring({64, 64, 2});
        auto p = dyn::producer::attach(t.ring, full_policy::fail);
        auto c = dyn::consumer::join(t.ring);
        auto peeker = dyn::consumer::join(t.ring);
        REQUIRE((p && c && peeker));
        put(*p, std::string(40, 'A')); // position 0
        put(*p, std::string(40, 'B')); // 1  <- forged
        put(*p, std::string(40, 'C')); // 2
        fc.apply(header_at(t, 1), t.ring.max_message());
        std::vector<std::string> got;
        CHECK(c->poll([&](const dyn::message& m) { got.push_back(string_of(m)); }) == 1);
        CHECK(got == std::vector<std::string>{std::string(40, 'A')});
        CHECK(c->corrupt() == 1);
        CHECK(c->position() == 3); // skipped to the cursor (C is lost)
        CHECK_FALSE(c->lapped());
        // the same through peek()
        auto m = peeker->peek();
        REQUIRE(m);
        CHECK(peeker->release());
        CHECK_FALSE(peeker->peek());
        CHECK(peeker->corrupt() == 1);
        CHECK(peeker->position() == 3);
        // later messages are delivered, and the skip released the gate: the whole ring is writable again
        put(*p, "D");
        CHECK(drain(*c) == std::vector<std::string>{"D"});
        auto pd = peeker->peek();
        REQUIRE(pd);
        CHECK(string_of(*pd) == "D");
        CHECK(peeker->release());
        int n = 0;
        while (p->write(nullptr, 0))
            ++n;
        CHECK(n == 64);
        CHECK(c->poll([](const dyn::message&) {}) == 64);
        CHECK(c->corrupt() == 1);
    }
}

TEST_CASE("ipc: a forged header that would cross the end of the buffer is refused",
          "[ipc][corrupt]")
{
    auto t = make_ring({64, 64, 1});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    for (int i = 0; i < 62; ++i)
        put(*p, "f");
    CHECK(drain(*c).size() == 62);
    put(*p, "B"); // 62
    put(*p, "C"); // 63
    put(*p, "D"); // 64 (index 0)
    put(*p, "E"); // 65
    // 3 units from 62 cross the end (2 to the end) although 4 units are published
    forge_header(header_at(t, 62), 160, 62, 0);
    CHECK(c->poll([](const dyn::message&) {}) == 0);
    CHECK(c->corrupt() == 1);
    CHECK(c->position() == 66);
    // the reader continues with the next record
    put(*p, "F");
    CHECK(drain(*c) == std::vector<std::string>{"F"});
}

TEST_CASE("ipc: a skip record that does not reach the end of the buffer is refused",
          "[ipc][corrupt]")
{
    auto t = make_ring({32, 64, 1}); // granule 64 = 2 units
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    while (p->position() % 64 != 60)
    {
        put(*p, "f");
        drain(*c);
    }
    const std::uint32_t skip_at = p->position();
    put(*p, std::string(200, 'R')); // 8 units, 4 to the end: a 4-unit skip at 60, the record at 64
    CHECK(header_at(t, skip_at).type == dyn::type_skip);
    CHECK(header_at(t, skip_at).size == 4 * 32 - 16);
    // a consistent header (valid check) for a skip of 2 units: it would leave 2 units before the end unread
    forge_header(header_at(t, skip_at), 2 * 32 - 16, skip_at, dyn::type_skip);
    CHECK(c->poll([](const dyn::message&) {}) == 0);
    CHECK(c->corrupt() == 1);
    CHECK(c->position() == t.ring.cursor());
    put(*p, "ok");
    CHECK(drain(*c) == std::vector<std::string>{"ok"});
    // the intact case for comparison
    while (p->position() % 64 != 60)
    {
        put(*p, "f");
        drain(*c);
    }
    put(*p, std::string(200, 'R'));
    CHECK(drain(*c) == std::vector<std::string>{std::string(200, 'R')});
    CHECK(c->corrupt() == 1);
}

// =================================================================================================================
// threads: every byte verified, units 16/32/64, compile-time and run-time units
// =================================================================================================================
namespace
{
template<class Ring>
void threads_round(const std::uint32_t unit, const dyn::ring_kind k, const int consumers,
                   const std::uint64_t n)
{
    using P = dyn::basic_producer<Ring::unit_template, Ring::align, Ring::data_align>;
    using C = dyn::basic_consumer<Ring::unit_template, Ring::align, Ring::data_align>;
    auto t = make_ring<Ring>({unit, 65536 / unit, 4}, k);
    auto p = P::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    const std::size_t maxm = t.ring.max_message();
    std::vector<C> cs;
    for (int i = 0; i < consumers; ++i)
    {
        auto c = C::join(t.ring);
        REQUIRE(c);
        cs.push_back(std::move(*c));
    }
    std::vector<consumed> results(cs.size());
    std::vector<std::thread> th;
    for (std::size_t i = 0; i < cs.size(); ++i)
        th.emplace_back([&, i] { results[i] = consume(cs[i], 0, n, maxm); });
    bool produced = false;
    std::thread prod([&] { produced = produce(*p, 0, n); });
    prod.join();
    for (auto& x : th)
        x.join();
    CHECK(produced);
    for (const auto& r : results)
    {
        CHECK(r.received == n);
        CHECK(r.bad == 0);
        CHECK_FALSE(r.lapped);
        CHECK_FALSE(r.timed_out);
        CHECK(r.corrupt == 0);
    }
}
} // namespace

TEST_CASE("ipc threads: producer + 1 SPSC consumer, 200k mixed sizes, units 16/32/64",
          "[ipc][threads][spsc]")
{
    threads_round<dyn::basic_ring<16>>(16, dyn::ring_kind::spsc, 1, 200000);
    threads_round<dyn::basic_ring<32>>(32, dyn::ring_kind::spsc, 1, 200000);
    threads_round<dyn::basic_ring<64>>(64, dyn::ring_kind::spsc, 1, 200000);
    threads_round<dyn::ring>(32, dyn::ring_kind::spsc, 1, 50000);
    // payloads on their own cache line (DataAlign 64)
    threads_round<dyn::basic_ring<64, 64, 64>>(64, dyn::ring_kind::spsc, 1, 100000);
    threads_round<dyn::basic_ring<16, 64, 64>>(16, dyn::ring_kind::spsc, 1, 50000);
}

TEST_CASE("ipc threads: producer + 3 SPMC consumers, 200k mixed sizes, units 16/32/64",
          "[ipc][threads][spmc]")
{
    threads_round<dyn::basic_ring<16>>(16, dyn::ring_kind::spmc, 3, 200000);
    threads_round<dyn::basic_ring<32>>(32, dyn::ring_kind::spmc, 3, 200000);
    threads_round<dyn::basic_ring<64>>(64, dyn::ring_kind::spmc, 3, 200000);
    threads_round<dyn::basic_ring<dyn::dynamic, 16>>(16, dyn::ring_kind::spmc, 2, 50000);
}

TEST_CASE("ipc threads: consumers joining and leaving a running producer are never lapped",
          "[ipc][threads][spmc]")
{
    auto t = make_ring({32, 1024, 4});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    constexpr std::uint64_t n = 100000;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> sessions{0}, gaps{0}, lapped{0}, corrupt{0}, first_pos_bad{0};
    const auto churn = [&] {
        while (!done.load())
        {
            auto c = dyn::consumer::join(t.ring);
            if (!c)
                continue;
            const std::uint32_t start = c->position();
            std::optional<std::uint64_t> prev;
            int reads = 0;
            while (reads < 200 && !done.load())
            {
                const std::size_t got = c->poll([&](const dyn::message& m) {
                    std::uint64_t v[3];
                    std::memcpy(v, m.data.data(), sizeof v);
                    if (!prev && m.position != start)
                        first_pos_bad.fetch_add(1);
                    if ((prev && v[0] != *prev + 1) || v[1] != v[0] * 3 || v[2] != ~v[0])
                        gaps.fetch_add(1);
                    prev = v[0];
                });
                reads += static_cast<int>(got);
                if (got == 0)
                {
                    if (c->lapped())
                        break;
                    std::this_thread::yield();
                }
            }
            lapped.fetch_add(c->lapped());
            corrupt.fetch_add(c->corrupt());
            sessions.fetch_add(1);
        }
    };
    std::thread a(churn), b(churn);
    bool ok = true;
    const auto deadline = steady::now() + time_limit;
    // 2.2.1 (the 2.2.0 full run, gcc16-release): after its n messages the producer keeps writing until one churn
    // session has completed (bounded by the deadline) — the same defect the 2.1.15 run found in the eviction test:
    // an optimised build on two cores wrote all n messages before either consumer thread finished a session, so
    // `sessions` was 0 and the test had checked nothing.
    for (std::uint64_t seq = 0; ok && (seq < n || sessions.load() == 0); ++seq)
    {
        if (seq >= n && steady::now() > deadline)
            break;
        const std::uint64_t v[3] = {seq, seq * 3, ~seq};
        while (!p->write(reinterpret_cast<const std::byte*>(v), sizeof v))
        {
            if (steady::now() > deadline)
            {
                ok = false;
                break;
            }
            std::this_thread::yield();
        }
    }
    done.store(true);
    a.join();
    b.join();
    CHECK(ok);
    CHECK(sessions.load() > 0);
    CHECK(gaps.load() == 0);
    CHECK(lapped.load() == 0);
    CHECK(corrupt.load() == 0);
    CHECK(first_pos_bad.load() == 0);
    CHECK(t.ring.active_consumers() == 0);
}

TEST_CASE("ipc threads: a consumer joining a running producer sees from the cursor on, in order",
          "[ipc][threads][spmc]")
{
    auto t = make_ring({64, 256, 2});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    std::atomic<bool> stop{false};
    std::thread prod([&] {
        for (std::uint64_t seq = 0; !stop.load(); ++seq)
            while (!p->write(reinterpret_cast<const std::byte*>(&seq), sizeof seq) && !stop.load())
                std::this_thread::yield();
    });
    while (t.ring.cursor() < 5000)
        std::this_thread::yield();
    auto c = dyn::consumer::join(t.ring);
    REQUIRE(c);
    const std::uint32_t start = c->position();
    CHECK(start >= 5000);
    std::uint64_t prev = 0, count = 0, bad = 0;
    std::uint32_t first_pos = 0;
    const auto deadline = steady::now() + time_limit;
    while (count < 20000 && steady::now() < deadline)
        c->poll([&](const dyn::message& m) {
            std::uint64_t v = 0;
            std::memcpy(&v, m.data.data(), sizeof v);
            if (count == 0)
                first_pos = m.position;
            else
                bad += v != prev + 1;
            prev = v;
            ++count;
        });
    stop.store(true);
    prod.join();
    CHECK(count >= 20000);
    CHECK(bad == 0);
    CHECK(first_pos == start);
    CHECK(prev + 1 >= count);
    CHECK_FALSE(c->lapped());
}

// =================================================================================================================
// ---- regions (es::ipc::region; these cases came from the es::ipc_old suite with G-17) -----------------------------
TEST_CASE("ipc region: create/open/remove on tmpfs, and only on tmpfs or hugetlbfs",
          "[ipc][region]")
{
    const std::string name = unique_name("region");
    {
        auto a = ipc::region::create(name, 5000);
        REQUIRE(a);
        CHECK(a->owner());
        CHECK(a->size() == 8192); // rounded up to the page size
        CHECK(a->page_size() == ipc::region::system_page());
        CHECK_FALSE(a->huge());
        CHECK(ipc::region::create(name, 4096).error() == std::errc::file_exists);
        auto b = ipc::region::open("/" + name); // a leading '/' (shm_open style) is accepted
        REQUIRE(b);
        CHECK_FALSE(b->owner());
        CHECK(b->size() == 8192);
        static_cast<char*>(a->data())[100] = 'x';
        CHECK(static_cast<const char*>(b->data())[100] == 'x'); // one object, two mappings
    } // the creator unlinks
    CHECK(ipc::region::open(name).error() == std::errc::no_such_file_or_directory);
    CHECK(ipc::region::create("a/b", 64).error() == std::errc::invalid_argument);
    CHECK(ipc::region::create("", 64).error() == std::errc::invalid_argument);
    CHECK(ipc::region::create("..", 64).error() == std::errc::invalid_argument);
    CHECK(ipc::region::create(name, 0).error() == std::errc::invalid_argument);
    {
        auto k = ipc::region::create(name, 64);
        REQUIRE(k);
        k->keep();
    }
    CHECK(ipc::region::open(name));
    CHECK(ipc::region::remove(name));
    CHECK_FALSE(ipc::region::remove(name));

    // a directory on a disk filesystem is refused: that memory would be written back to disk
    struct statfs s = {};
    REQUIRE(::statfs(ES_SERREG_TEST_TMPDIR, &s) == 0);
    if (s.f_type != ipc::region::tmpfs_magic &&
        ::access(ES_SERREG_TEST_TMPDIR, W_OK) == 0) // else EACCES first
    {
        CHECK(ipc::region::create(name, 64, {.dir = ES_SERREG_TEST_TMPDIR}).error() ==
              std::errc::invalid_argument);
        CHECK(::access((std::string(ES_SERREG_TEST_TMPDIR) + "/" + name).c_str(), F_OK) !=
              0); // and unlinked
    }
    // asking for huge pages in a tmpfs directory is refused
    CHECK(
        ipc::region::create(name, 64, {.pages = ipc::pages::huge_2m, .dir = "/dev/shm"}).error() ==
        std::errc::invalid_argument);
    auto locked = ipc::region::create(name, 4096, {.lock = true});
    if (locked) // mlock may be refused by RLIMIT_MEMLOCK in a container: then the error must be reported
        CHECK(locked->size() == 4096);
    else
        CHECK((locked.error() == std::errc::operation_not_permitted ||
               locked.error() == std::errc::not_enough_memory ||
               locked.error() == std::errc::resource_unavailable_try_again));
}

TEST_CASE("ipc region: only the creating process unlinks a named region", "[ipc][region][process]")
{
    const std::string name = unique_name("forkunlink");
    auto mem = ipc::region::create(name, 4096);
    REQUIRE(mem);
    REQUIRE(in_child([&] {
                mem->close(); // the inherited handle is still the owner's copy
                return 0;
            }) == 0);
    {
        const auto again = ipc::region::open(name);
        CHECK(again.has_value()); // I-06: once ENOENT
    }
    mem->close(); // the creator unlinks
    CHECK(ipc::region::open(name).error() == std::errc::no_such_file_or_directory);
}

// I-08: /dev/shm is world-writable, so open() refuses a file another user could have planted or could rewrite —
// writable by group or others, or owned by another user — unless region_options::foreign_owner.
TEST_CASE("ipc region: open() refuses a region file writable by others unless foreign_owner",
          "[ipc][region]")
{
    const std::string name = unique_name("foreign");
    auto mem = ipc::region::create(name, 4096);
    REQUIRE(mem);
    REQUIRE(::chmod(std::string(mem->path()).c_str(), 0666) == 0);
    CHECK(ipc::region::open(name).error() == std::errc::permission_denied);
    ipc::region_options o;
    o.foreign_owner = true;
    CHECK(ipc::region::open(name, o).has_value());
    REQUIRE(::chmod(std::string(mem->path()).c_str(), 0600) == 0);
    CHECK(ipc::region::open(name).has_value());
}

TEST_CASE("ipc region: explicit 2 MiB huge pages — named (hugetlbfs) and anonymous (MAP_HUGETLB)",
          "[ipc][region][huge]")
{
    const std::string mnt = huge_2m_mount();
    const auto anon = ipc::region::anonymous(4096, {.pages = ipc::pages::huge_2m});
    if (mnt.empty() && !anon)
    {
        CHECK(anon.error() ==
              std::errc::not_enough_memory); // no pool: an error, never a silent fallback
        SKIP("no hugetlbfs mount with 2 MiB pages and no free huge pages on this machine");
    }
    if (anon)
    {
        CHECK(anon->size() == ipc::page_bytes(ipc::pages::huge_2m));
        CHECK(anon->page_size() == ipc::page_bytes(ipc::pages::huge_2m));
        CHECK(anon->huge());
    }
    if (mnt.empty())
        SKIP("no hugetlbfs mount with 2 MiB pages (anonymous huge pages checked)");
    const std::string name = unique_name("huge");
    const dyn::geometry g{64, 4096, 4};
    auto mem = ipc::region::create(name, dyn::ring::bytes_for(g), {.pages = ipc::pages::huge_2m});
    if (!mem && mem.error() == std::errc::not_enough_memory)
        SKIP("hugetlbfs is mounted but the pool has no free 2 MiB page");
    if (!mem && (mem.error() == std::errc::permission_denied ||
                 mem.error() == std::errc::operation_not_permitted))
    {
        // the error must be the truth: this user really cannot write to the mount
        CHECK(::access(mnt.c_str(), W_OK) != 0);
        SKIP("hugetlbfs mount " + mnt + " is not writable by this user (" +
             std::make_error_code(mem.error()).message() +
             "): chmod 1777 it, or mount it with uid=/gid=/mode= for this user");
    }
    INFO("region::create on " << mnt << ": "
                              << (mem ? std::string("ok")
                                      : std::make_error_code(mem.error()).message()));
    REQUIRE(mem);
    CHECK(mem->path() == mnt + "/" + name);
    CHECK(mem->page_size() == ipc::page_bytes(ipc::pages::huge_2m));
    CHECK(mem->size() % mem->page_size() == 0);
    auto r = dyn::ring::create(*mem, g);
    REQUIRE(r);
    CHECK(in_child([&] { // open() finds it without being told the page size
              auto m = ipc::region::open(name);
              if (!m || !m->huge())
                  return 10;
              auto cr = dyn::ring::attach(*m);
              return cr && cr->shape().units == 4096 ? 0 : 11;
          }) == 0);
    auto p = dyn::producer::attach(*r, full_policy::fail);
    auto c = dyn::consumer::join(*r);
    REQUIRE((p && c));
    CHECK(p->emplace<std::uint64_t>(std::uint64_t{77}));
    const auto seen = c->poll_as<std::uint64_t>([](const std::uint64_t v) { CHECK(v == 77); }) ==
                      1; // Catch2 assertions inside the lambda: not nested (-Wshadow)
    CHECK(seen);
}

// processes (fork): anonymous and named regions, dead consumers, dead producers
// =================================================================================================================
TEST_CASE("ipc processes: SPSC producer child, consumer parent, every byte verified",
          "[ipc][process][spsc]")
{
    (void)pattern(); // built before fork
    for (const std::uint32_t u : {16u, 64u})
    {
        auto t = make_ring({u, 32768 / u, 1}, dyn::ring_kind::spsc);
        constexpr std::uint64_t n = 50000;
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            auto p = dyn::producer::attach(t.ring, full_policy::fail);
            if (!p)
                std::_Exit(10);
            std::_Exit(produce(*p, 0, n) ? 0 : 11);
        }
        auto c = dyn::consumer::join(t.ring);
        REQUIRE(c);
        const consumed r = consume(*c, 0, n, t.ring.max_message());
        CHECK(wait_child(pid) == 0);
        CHECK(r.received == n);
        CHECK(r.bad == 0);
        CHECK(r.corrupt == 0);
        CHECK_FALSE(r.timed_out);
    }
}

TEST_CASE("ipc processes: SPMC producer parent, 3 consumer children (anonymous region)",
          "[ipc][process][spmc]")
{
    (void)pattern();
    auto t = make_ring({32, 1024, 4}); // 32 KiB
    constexpr std::uint64_t n = 50000;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    std::vector<pid_t> kids;
    for (int i = 0; i < 3; ++i)
    {
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            ::close(fds[0]);
            auto c = dyn::consumer::join(t.ring);
            if (!c)
                std::_Exit(10);
            (void)::write(fds[1], "r", 1);
            const consumed r = consume(*c, 0, n, t.ring.max_message());
            c->leave();
            std::_Exit(r.received == n && r.bad == 0 && r.corrupt == 0 ? 0 : 11);
        }
        kids.push_back(pid);
    }
    ::close(fds[1]);
    for (int i = 0; i < 3; ++i)
    {
        char ch = 0;
        REQUIRE(::read(fds[0], &ch, 1) == 1);
    }
    ::close(fds[0]);
    CHECK(t.ring.active_consumers() == 3);
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    CHECK(produce(*p, 0, n));
    for (const pid_t k : kids)
        CHECK(wait_child(k) == 0);
    CHECK(t.ring.active_consumers() == 0);
}

TEST_CASE("ipc processes: a named region attached by a child (SPMC)", "[ipc][process][spmc]")
{
    (void)pattern();
    const std::string name = unique_name("named");
    const dyn::geometry g{64, 512, 2};
    auto mem = ipc::region::create(name, dyn::ring::bytes_for(g));
    REQUIRE(mem);
    auto r = dyn::ring::create(*mem, g);
    REQUIRE(r);
    constexpr std::uint64_t n = 20000;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    if (pid == 0)
    { // like an unrelated program: finds the region by name and the geometry in the ring
        ::close(fds[0]);
        auto m = ipc::region::open(name);
        if (!m)
            std::_Exit(10);
        auto ring = dyn::ring::attach(*m);
        if (!ring || ring->shape().units != 512)
            std::_Exit(11);
        auto c = dyn::consumer::join(*ring);
        if (!c)
            std::_Exit(12);
        (void)::write(fds[1], "r", 1);
        const consumed res = consume(*c, 0, n, ring->max_message());
        std::_Exit(res.received == n && res.bad == 0 ? 0 : 13);
    }
    ::close(fds[1]);
    char ch = 0;
    REQUIRE(::read(fds[0], &ch, 1) == 1);
    ::close(fds[0]);
    auto p = dyn::producer::attach(*r, full_policy::fail);
    REQUIRE(p);
    CHECK(produce(*p, 0, n));
    CHECK(wait_child(pid) == 0);
}

TEST_CASE("ipc processes: a dead SPMC consumer is reaped instead of blocking the producer",
          "[ipc][process][spmc]")
{
    auto t = make_ring({64, 16, 2});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    killed_child([&](const auto ready) {
        auto c = dyn::consumer::join(t.ring);
        if (c)
            ready(); // killed here, `c` still joined
    });
    CHECK(t.ring.active_consumers() == 1);
    int n = 0;
    while (p->write(nullptr, 0))
        ++n;
    CHECK(n == 16); // gated by the dead consumer
    CHECK(t.ring.reap_dead_consumers() == 1);
    CHECK(t.ring.active_consumers() == 0);
    CHECK(p->write(nullptr, 0)); // free run again
    CHECK(t.ring.reap_dead_consumers() == 0);

    // full_policy::fail reaps by itself now and then (every 1024 refusals)
    auto c = dyn::consumer::join(t.ring); // our own gate is never reaped
    REQUIRE(c);
    killed_child([&](const auto ready) {
        auto d = dyn::consumer::join(t.ring);
        if (d)
            ready();
    });
    drain(*c);
    CHECK(t.ring.active_consumers() == 2);
    while (p->write(nullptr, 0))
    {
    }
    drain(*c);
    int refusals = 0;
    while (!p->write(nullptr, 0) && refusals < 5000)
        ++refusals;
    CHECK(refusals < 5000);
    CHECK(t.ring.active_consumers() == 1);

    // join() reaps a dead holder when every gate is taken
    auto t1 = make_ring({64, 16, 1});
    killed_child([&](const auto ready) {
        auto d = dyn::consumer::join(t1.ring);
        if (d)
            ready();
    });
    auto j = dyn::consumer::join(t1.ring);
    CHECK(j);
}

TEST_CASE("ipc processes: a dead SPSC consumer keeps the queue position for the next join",
          "[ipc][process][spsc]")
{
    auto t = make_ring({64, 64, 1}, dyn::ring_kind::spsc);
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    for (int i = 0; i < 5; ++i)
        put(*p, "q" + std::to_string(i));
    killed_child([&](const auto ready) {
        auto c = dyn::consumer::join(t.ring);
        if (c)
        {
            c->poll([](const dyn::message&) {}, 2); // consumes q0, q1
            ready();
        }
    });
    CHECK(t.ring.active_consumers() == 1);
    CHECK(in_child([&] {
              auto c = dyn::consumer::join(t.ring); // reaps the dead holder by itself
              if (!c)
                  return 10;
              const auto got = drain(*c);
              return got == std::vector<std::string>{"q2", "q3", "q4"} ? 0 : 11;
          }) == 0);
    // the child exited without leave(): its gate is reaped by the next join, and its progress (after q4) kept
    put(*p, "q5");
    auto c = dyn::consumer::join(t.ring);
    REQUIRE(c);
    CHECK(drain(*c) == std::vector<std::string>{"q5"});
}

// B4 (fixed): freeing a gate leaves the dead owner's pid in it. A joiner that has CASed its ticket into the gate but
// not yet stored its pid must never be paired with that stale pid. Since the fix a gate records which ticket its pid
// belongs to (gate::pid_ticket, stored after the pid), and a reaper acts only when it equals the word's ticket.
// Simulated here: after the reap, the gate word is set as a joiner's first CAS would set it.
TEST_CASE("ipc processes: a reaper never pairs a stale dead pid with a new ticket",
          "[ipc][process]")
{
    auto t = make_ring({64, 64, 1}, dyn::ring_kind::spsc);
    killed_child([&](const auto ready) {
        auto c = dyn::consumer::join(t.ring);
        if (c)
            ready();
    });
    CHECK(t.ring.reap_dead_consumers() == 1);
    CHECK(gate_word(t.base(), 0) == 0);
    set_gate_word(t.base(), 0,
                  dyn::detail::own_word(0x7777, dyn::detail::gate_active)); // a joiner's first step
    CHECK(t.ring.reap_dead_consumers() ==
          0); // the stale pid belongs to the old ticket, not to 0x7777
}

TEST_CASE("ipc processes: a killed producer is replaced by the next attach()", "[ipc][process]")
{
    auto t = make_ring({64, 64, 2});
    auto c = dyn::consumer::join(t.ring);
    REQUIRE(c);
    killed_child([&](const auto ready) {
        auto p = dyn::producer::attach(t.ring);
        if (p)
        {
            put(*p, "41");
            auto s = p->claim(
                2); // claimed and deferred records of the dead producer are never published
            if (s)
            {
                std::memcpy(s->data(), "xx", 2);
                (void)p->commit(2, false);
            }
            ready();
        }
    });
    CHECK(t.ring.has_producer());
    auto p = dyn::producer::attach(t.ring);
    REQUIRE(p);
    CHECK(p->position() == 1);
    put(*p, "42");
    CHECK(drain(*c) == std::vector<std::string>{"41", "42"});
    CHECK(dyn::producer::attach(t.ring).error() == std::errc::device_or_resource_busy);
    CHECK(c->corrupt() == 0);
}

TEST_CASE("ipc processes: handles copied by fork() do not give the parent's roles away",
          "[ipc][process]")
{
    auto t = make_ring({64, 64, 2});
    auto p = dyn::producer::attach(t.ring);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    const std::uint64_t w = gate_word(t.base(), c->id());
    CHECK(in_child([&] {
              p->detach();
              c->leave();
              return 0;
          }) == 0);
    CHECK(t.ring.has_producer());
    CHECK(gate_word(t.base(), c->id()) == w);
    CHECK(t.ring.active_consumers() == 1);
    put(*p, "still mine");
    CHECK(drain(*c) == std::vector<std::string>{"still mine"});
}

// =================================================================================================================
// large rings
// =================================================================================================================
namespace
{
struct mapping
{
    void* p = MAP_FAILED;
    std::size_t n = 0;
    mapping(const mapping&) = delete;
    mapping& operator=(const mapping&) = delete;
    explicit mapping(const std::size_t bytes)
        : p(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)),
          n(bytes)
    {
    }
    ~mapping()
    {
        if (p != MAP_FAILED)
            ::munmap(p, n);
    }
};
} // namespace

// regression (fixed in 2.1.x; producer.hpp commit / ring.hpp max_message): the header's size field is 32 bits,
// but max_message() of a ring above 4 GiB was larger; commit() truncated the size with
// static_cast<std::uint32_t>, so the reader saw a short message and then a seq mismatch. Only the header pages
// are touched (MAP_NORESERVE).
TEST_CASE("ipc: messages above 4 GiB in an 8 GiB ring keep their size (or are refused)",
          "[ipc][large]")
{
    const dyn::geometry g{64, 1u << 27, 1}; // 8 GiB
    const mapping m(dyn::ring::bytes_for(g));
    if (m.p == MAP_FAILED)
        SKIP("cannot reserve 8 GiB of address space");
    auto r = dyn::ring::create(m.p, m.n, g, dyn::ring_kind::spmc);
    REQUIRE(r);
    auto p = dyn::producer::attach(*r, full_policy::fail);
    auto c = dyn::consumer::join(*r);
    REQUIRE((p && c));
    const std::size_t n = (std::size_t{1} << 32) + 48;
    auto s = p->claim(n);
    if (!s)
    { // refusing is a valid fix too
        CHECK(s.error() == std::errc::message_size);
        CHECK(r->max_message() <= 0xFFFF'FFFFu);
        return;
    }
    REQUIRE(p->commit(n));
    put(*p, "after");
    auto msg = c->peek();
    REQUIRE(msg);
    CHECK(msg->data.size() == n);
    CHECK(c->release());
    const auto rest = drain(*c);
    CHECK(rest == std::vector<std::string>{"after"});
    CHECK(c->corrupt() == 0);
}

// =================================================================================================================
// basic_sink + es::serreg
// =================================================================================================================
TEST_CASE("ipc: basic_sink — serreg frames are built in the ring and read in place", "[ipc][sink]")
{
    using namespace es::serreg::literals;
    auto t = make_ring({64, 256, 1});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    dyn::sink out{*p, 42};
    CHECK(out.type() == 42);
    static_assert(es::serreg::claiming_sink<dyn::sink>);
    const auto n = es::serreg::serialize(out, "tick"_, dtest::Tick{7, -1250, 300, "XYZ"});
    REQUIRE(n);
    CHECK(*n == es::serreg::frame_size<es::serreg::lit<"tick">, dtest::Tick>());
    out.type(43);
    const dtest::Book book{"XNAS", {1, -2, 3, 1 << 20}};
    const auto n2 = es::serreg::serialize(out, book);
    REQUIRE(n2);
    // the dynamic frame equals what vector_sink writes
    std::vector<std::byte> ref;
    es::serreg::vector_sink vs{ref};
    REQUIRE(es::serreg::serialize(vs, book));
    REQUIRE(out.write(ref.data(), ref.size()));
    int seen = 0;
    const auto nested_2018 = c->poll([&](const dyn::message& m) {
        if (seen == 0)
        {
            CHECK(m.type == 42);
            CHECK(m.data.size() == *n);
            dtest::Tick tk{};
            CHECK(es::serreg::deserialize(m.data, "tick"_, tk));
            CHECK(tk.seq == 7);
            CHECK(tk.px == -1250);
            CHECK(tk.qty == 300);
            CHECK(std::string(tk.sym, 3) == "XYZ");
        }
        else
        {
            CHECK(m.type == 43);
            CHECK(m.data.size() == ref.size());
            CHECK(std::memcmp(m.data.data(), ref.data(), ref.size()) == 0);
            dtest::Book b{};
            CHECK(es::serreg::deserialize(m.data, b));
            CHECK(b.venue == "XNAS");
            CHECK(b.levels == book.levels);
        }
        ++seen;
    }) == 3; // Catch2 assertions inside the lambda: not nested (-Wshadow)
    CHECK(nested_2018);
    CHECK(seen == 3);

    // a deferred sink publishes on flush
    dyn::sink later{*p, 1, false};
    REQUIRE(es::serreg::serialize(later, "tick"_, dtest::Tick{8, 1, 2, "AB"}));
    CHECK(c->pending_units() == 0);
    p->flush();
    CHECK(c->poll([](const dyn::message&) {}) == 1);

    // full / reserved type: serialize reports the error, nothing is published
    dyn::sink reserved{*p, dyn::type_skip};
    CHECK_FALSE(reserved.claim(8));
    CHECK_FALSE(es::serreg::serialize(reserved, "tick"_, dtest::Tick{9, 1, 2, "AB"}));
    while (p->write(nullptr, 0))
    {
    }
    CHECK_FALSE(es::serreg::serialize(out, "tick"_, dtest::Tick{9, 1, 2, "AB"}));
    CHECK(&out.producer() == &*p);
    CHECK(c->corrupt() == 0);
}

// ---- Q6b (maintainer, 2026-10-02): readers are usually below 16; the producer scans only the gates ever used ----------
namespace
{
std::uint32_t gates_used(const std::byte* const base)
{
    auto* const h =
        reinterpret_cast<std::uint32_t*>(const_cast<std::byte*>(base) + dyn::detail::hwm_offset);
    return std::atomic_ref<std::uint32_t>(*h).load();
}
} // namespace

TEST_CASE("ipc: 3 readers of a 16-reader ring use gates 0..2 and the scan stops there (Q6b)",
          "[ipc][spmc]")
{
    auto t = make_ring({64, 256, 16});
    CHECK(gates_used(t.base()) == 0);
    auto a = dyn::consumer::join(t.ring);
    auto b = dyn::consumer::join(t.ring);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((a && b && c));
    CHECK((a->id() == 0 && b->id() == 1 && c->id() == 2));
    CHECK(gates_used(t.base()) == 3);
    b->leave(); // a rejoin takes the lowest free gate, so the mark does not grow with churn
    auto d = dyn::consumer::join(t.ring);
    REQUIRE(d);
    CHECK(d->id() == 1);
    CHECK(gates_used(t.base()) == 3);
    // the scan really covers those three: the slowest of them still gates the producer
    auto p = dyn::producer::attach(t.ring, dyn::full_policy::fail);
    REQUIRE(p);
    std::size_t sent = 0;
    while (p->write(nullptr, 0))
        ++sent;
    CHECK(
        sent ==
        256); // a 0-byte payload is one 64-byte unit: the ring holds exactly 256 before a reader moves
    std::size_t got = 0;
    a->poll([&](const dyn::message&) { ++got; });
    CHECK(got == 256);
    CHECK_FALSE(p->write(nullptr, 0)); // c and d have not read yet
}

TEST_CASE("ipc: deferred records are published before the producer rescans, so a joiner "
          "never loses them",
          "[ipc][spmc]")
{
    auto t = make_ring({64, 64, 2});
    auto p = dyn::producer::attach(
        t.ring, dyn::full_policy::fail); // one thread: never spin on our own reader
    REQUIRE(p);
    std::uint8_t next = 0;
    const auto put_deferred = [&] {
        auto s = p->claim(8);
        if (!s)
            return false;
        std::memset(s->data(), next, 8);
        ++next;
        return p->commit(8, false);
    };
    // no reader yet: the producer runs free with 40 deferred records (more than half the ring)
    for (int i = 0; i < 40; ++i)
        REQUIRE(put_deferred());
    auto c = dyn::consumer::join(t.ring); // starts at the published cursor
    REQUIRE(c);
    const std::uint32_t start = c->position();
    std::uint8_t expect = 0;
    bool first = true;
    std::uint64_t got = 0;
    const auto drain = [&] {
        c->poll([&](const dyn::message& m) {
            REQUIRE(m.data.size() == 8);
            const auto v = std::to_integer<std::uint8_t>(m.data[0]);
            if (!first)
                CHECK(v == static_cast<std::uint8_t>(expect + 1)); // no gap, no torn record
            first = false;
            expect = v;
            ++got;
        });
    };
    for (int i = 0; i < 400; ++i)
        if (!put_deferred())
        {
            drain(); // the fail policy flushed the deferred records before refusing
            REQUIRE(put_deferred());
        }
    p->flush();
    drain();
    CHECK(c->corrupt() == 0);
    CHECK_FALSE(c->lapped());
    CHECK(got > 300);
    CHECK(c->position() != start);
}

TEST_CASE("ipc layout 2: readers publish progress with a plain store of the gate's position",
          "[ipc][progress]")
{
    for (const auto k : {dyn::ring_kind::spsc, dyn::ring_kind::spmc})
    {
        auto t = make_ring({64, 16, 2}, k);
        auto p = dyn::producer::attach(t.ring, full_policy::fail);
        auto c = dyn::consumer::join(t.ring);
        REQUIRE((p && c));
        const std::uint64_t own = gate_word(t.base(), c->id());
        CHECK(dyn::detail::state_of(own) == dyn::detail::gate_active);
        put(*p, "a");
        put(*p, "b");
        CHECK(drain(*c) == std::vector<std::string>{"a", "b"});
        CHECK(gate_pos(t.base(), c->id()) == c->position());
        CHECK(gate_word(t.base(), c->id()) == own); // progress never touches the ownership word
        auto moved = std::move(*c);
        put(*p, "c");
        CHECK(drain(moved) == std::vector<std::string>{"c"});
        CHECK(gate_pos(t.base(), moved.id()) == moved.position());
        moved.leave();
        CHECK(gate_word(t.base(), moved.id()) == 0);
        if (k == dyn::ring_kind::spsc)
            CHECK(gate_pos(t.base(), 0) == moved.position()); // the queue position stays
    }
}

TEST_CASE("ipc SPSC: a consumer that lost its gate never stores into it", "[ipc][progress][spsc]")
{
    auto t = make_ring({64, 16, 1}, dyn::ring_kind::spsc);
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto a = dyn::consumer::join(t.ring);
    REQUIRE((p && a));
    put(*p, "one");
    put(*p, "two");
    auto held = a->peek();
    REQUIRE(held);
    set_gate_word(t.base(), 0,
                  0); // simulate a reap of a: the queue position stays, the ownership goes
    auto b = dyn::consumer::join(t.ring);
    REQUIRE(b);
    const std::uint64_t bw = gate_word(t.base(), 0);
    const std::uint32_t bpos = gate_pos(t.base(), 0);
    CHECK_FALSE(a->release()); // not ours any more: lapped, and no store
    CHECK(a->lapped());
    CHECK(a->poll([](const dyn::message&) {}) == 0);
    CHECK_FALSE(a->resync());
    a->leave();
    CHECK(gate_word(t.base(), 0) == bw);
    CHECK(gate_pos(t.base(), 0) == bpos);
    // b continues at the queue position: nothing a held was released
    CHECK(drain(*b) == std::vector<std::string>{"one", "two"});
}

TEST_CASE("ipc SPMC: an evicted gate holds the producer no longer, whatever its position says",
          "[ipc][progress][spmc]")
{
    auto t = make_ring({64, 16, 2});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto a = dyn::consumer::join(t.ring);
    REQUIRE((p && a));
    const std::uint32_t id = a->id();
    for (int i = 0; i < 16; ++i)
        put(*p, "x");
    CHECK_FALSE(p->write(nullptr, 0)); // a gates: full
    CHECK(t.ring.evict(id));
    // a late store of the evicted reader (a preemption between its check and its store) lands on the evicted gate
    set_gate_pos(t.base(), id, gate_pos(t.base(), id));
    for (int i = 0; i < 64; ++i)
        CHECK(p->write(nullptr, 0)); // not gated
    // the gate is not reused until a acknowledges; then the next joiner takes it with a fresh position
    auto b = dyn::consumer::join(t.ring);
    REQUIRE(b);
    CHECK(b->id() != id);
    CHECK(a->poll([](const dyn::message&) {}) == 0);
    CHECK(a->lapped());
    CHECK(gate_word(t.base(), id) == 0);
    auto c = dyn::consumer::join(t.ring); // two gates: b holds one, a's is free again
    REQUIRE(c);
    CHECK(c->id() == id);
    CHECK(gate_pos(t.base(), id) == t.ring.cursor()); // a fresh position, not a's
}

TEST_CASE("ipc: DataAlign 64 puts every payload on its own cache line", "[ipc]")
{
    using R = dyn::basic_ring<16, 64, 64>;
    using P = dyn::basic_producer<16, 64, 64>;
    using C = dyn::basic_consumer<16, 64, 64>;
    STATIC_REQUIRE(R::data_align == 64);
    STATIC_REQUIRE(dyn::ring::data_align == 16);
    auto t = make_ring<R>({16, 256, 2});
    CHECK(t.ring.max_message() == 16 * 256 - 64);
    CHECK(t.ring.record_units(0) == 4);  // 64 bytes
    CHECK(t.ring.record_units(64) == 8); // 128 bytes
    auto p = P::attach(t.ring, full_policy::fail);
    auto c = C::join(t.ring);
    REQUIRE((p && c));
    for (std::size_t n = 0; n < 300; n += 7)
    {
        const auto s = p->claim(n, 5);
        REQUIRE(s);
        CHECK(reinterpret_cast<std::uintptr_t>(s->data()) % 64 == 0);
        for (std::size_t i = 0; i < n; ++i)
            (*s)[i] = static_cast<std::byte>(i + n);
        REQUIRE(p->commit(n));
        std::size_t got = 0;
        c->poll([&](const dyn::message& m) {
            CHECK(reinterpret_cast<std::uintptr_t>(m.data.data()) % 64 == 0);
            CHECK(m.data.size() == n);
            CHECK(m.type == 5);
            bool same = true;
            for (std::size_t i = 0; i < n; ++i)
                same &= m.data[i] == static_cast<std::byte>(i + n);
            CHECK(same);
            ++got;
        });
        CHECK(got == 1);
    }
    CHECK(c->corrupt() == 0);
    // a record of max_message() fills the whole ring
    {
        auto s = p->claim(t.ring.max_message());
        if (!s) // a skip record to the end of the buffer was published first: the reader releases it
        {
            CHECK(c->poll([](const dyn::message&) {}) == 0);
            s = p->claim(t.ring.max_message());
        }
        REQUIRE(s);
        REQUIRE(p->commit(t.ring.max_message()));
        CHECK(drain(*c).size() == 1);
    }
    // the payload offset is part of the layout: a ring of the other DataAlign is refused
    CHECK(dyn::basic_ring<16, 64>::attach(t.mem.data(), t.mem.size()).error() ==
          std::errc::invalid_argument);
    CHECK(R::attach(t.mem.data(), t.mem.size()).has_value());
}

TEST_CASE("ipc threads: readers joining, leaving and evicted while a producer runs; every "
          "byte checked",
          "[ipc][threads][spmc]")
{
    auto t = make_ring({16, 256, 4});
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    REQUIRE(p);
    constexpr std::uint64_t n = 300000;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> delivered{0}, bad{0}, corrupt{0}, laps{0}, sessions{0};
    const auto churn = [&] {
        while (!done.load())
        {
            auto c = dyn::consumer::join(t.ring);
            if (!c)
            {
                std::this_thread::yield();
                continue;
            }
            sessions.fetch_add(1);
            std::uint64_t local_bad = 0, last_bad = 0;
            for (int polls = 0; polls < 400 && !done.load(); ++polls)
            {
                local_bad = 0;
                last_bad = 0;
                const std::size_t got = c->poll([&](const dyn::message& m) {
                    std::uint64_t v[4];
                    std::memcpy(v, m.data.data(), sizeof v);
                    last_bad = m.data.size() != sizeof v || v[1] != v[0] * 3 || v[2] != ~v[0] ||
                               v[3] != v[0] + 7;
                    local_bad += last_bad;
                });
                // only the last message of a poll that ended lapped may be torn
                if (c->lapped())
                    local_bad -= last_bad;
                bad.fetch_add(local_bad);
                delivered.fetch_add(got);
                if (c->lapped())
                    break;
                if (got == 0)
                    std::this_thread::yield();
            }
            laps.fetch_add(c->lapped());
            corrupt.fetch_add(c->corrupt());
        }
    };
    // An evicted reader may read a record while the producer overwrites it: that race is the protocol's design (the
    // read is then reported as lapped, never delivered as intact) and ThreadSanitizer reports it. Under TSan the
    // readers only join and leave; the eviction race is covered by the other builds and the stress harnesses.
#if defined(__SANITIZE_THREAD__)
    constexpr bool evicting = false;
#else
    constexpr bool evicting = true;
#endif
    std::thread a(churn), b(churn), e([&] {
        std::uint32_t i = 0;
        while (evicting && !done.load())
        {
            t.ring.evict(i++ % 4);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    bool ok = true;
    const auto deadline = steady::now() + time_limit;
    // 2.1.15: the producer starts when a reader has joined, and after its n messages keeps writing until a reader
    // has received one (both bounded by the deadline). An optimised build on two cores wrote all n messages before
    // the reader threads were first scheduled, so `sessions` and `delivered` were 0 and the test checked nothing.
    while (sessions.load() == 0 && steady::now() < deadline)
        std::this_thread::yield();
    for (std::uint64_t seq = 0; ok && (seq < n || delivered.load() == 0); ++seq)
    {
        if (seq >= n && steady::now() > deadline)
            break;
        const std::uint64_t v[4] = {seq, seq * 3, ~seq, seq + 7};
        while (!p->write(reinterpret_cast<const std::byte*>(v), sizeof v))
        {
            if (steady::now() > deadline)
            {
                ok = false;
                break;
            }
            std::this_thread::yield();
        }
    }
    done.store(true);
    a.join();
    b.join();
    e.join();
    CHECK(ok); // the producer never stalls on an evicted or abandoned gate
    CHECK(sessions.load() > 0);
    CHECK(delivered.load() > 0);
    CHECK(bad.load() == 0);
    CHECK(corrupt.load() == 0);
    CHECK(t.ring.active_consumers() == 0);
    // every gate is free again: evicted readers acknowledged, the others left
    for (std::uint32_t i = 0; i < 4; ++i)
        CHECK(gate_word(t.base(), i) == 0);
}

TEST_CASE("ipc: create and attach require memory aligned to max(64, Align)", "[ipc]")
{
    using R128 = dyn::basic_ring<64, 128>;
    using R4k = dyn::basic_ring<64, 4096>;
    STATIC_REQUIRE(dyn::ring::mem_align == 64);
    STATIC_REQUIRE(dyn::basic_ring<16, 16>::mem_align == 64);
    STATIC_REQUIRE(R128::mem_align == 128);
    STATIC_REQUIRE(R4k::mem_align == 4096);
    const dyn::geometry g{64, 256, 2};
    const std::size_t need = R4k::bytes_for(g);
    REQUIRE(need > 0);
    auto m = ipc::region::anonymous(need + 4096); // page-aligned
    REQUIRE(m);
    auto* const base = static_cast<std::byte*>(m->data());
    CHECK(R128::create(base + 64, need, g).error() == std::errc::invalid_argument);
    CHECK(R4k::create(base + 64, need, g).error() == std::errc::invalid_argument);
    CHECK(R4k::create(base + 2048, need, g).error() == std::errc::invalid_argument);
    CHECK(R128::create(base + 128, need, g));
    auto r = R4k::create(base, need, g);
    REQUIRE(r);
    CHECK(R4k::attach(base, need).has_value());
    CHECK(R4k::attach(base + 64, need).error() == std::errc::invalid_argument);
    // every record starts on an Align boundary; the payload follows the 16-byte header
    auto p = dyn::basic_producer<64, 4096>::attach(*r, full_policy::fail);
    REQUIRE(p);
    for (const std::size_t n : {0u, 1u, 100u, 5000u})
    {
        const auto s = p->claim(n);
        REQUIRE(s);
        const auto addr = reinterpret_cast<std::uintptr_t>(s->data());
        CHECK((addr - dyn::detail::record_header_bytes) % 4096 == 0);
        CHECK(p->commit(n));
    }
}

TEST_CASE("ipc: emplace<T> and poll_as<T> — typed messages in place; other lengths counted in "
          "mismatched()",
          "[ipc]")
{
    struct quote
    {
        std::uint64_t id;
        std::int64_t px;
        std::uint32_t qty;
    };
    auto t = make_ring({64, 64, 1}, dyn::ring_kind::spsc);
    auto p = dyn::producer::attach(t.ring, full_policy::fail);
    auto c = dyn::consumer::join(t.ring);
    REQUIRE((p && c));
    CHECK(p->emplace<quote>(quote{1, 100, 5}));
    CHECK(p->emplace_typed<quote>(7, quote{2, 200, 6}));
    put(*p, "not a quote");
    CHECK(p->emplace<std::uint64_t>(std::uint64_t{42}));
    std::vector<std::uint64_t> ids;
    std::int64_t px = 0;
    CHECK(c->poll_as<quote>([&](const quote& q) {
        ids.push_back(q.id);
        px += q.px;
    }) == 4); // released: two quotes, one string, one u64
    CHECK(ids == std::vector<std::uint64_t>{1, 2});
    CHECK(px == 300);
    CHECK(c->mismatched() == 2);
    // f may stop early
    CHECK(p->emplace<std::uint64_t>(std::uint64_t{1}));
    CHECK(p->emplace<std::uint64_t>(std::uint64_t{2}));
    std::uint64_t first = 0;
    CHECK(c->poll_as<std::uint64_t>([&](const std::uint64_t v) {
        first = v;
        return false;
    }) == 1);
    CHECK(first == 1);
    CHECK(c->poll_as<std::uint64_t>([&](const std::uint64_t v) { first = v; }) == 1);
    CHECK(first == 2);
}
