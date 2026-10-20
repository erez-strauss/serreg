// tests/unit/test_sinks.cpp — v1.0.2 serialization-path items (plan §1.2): bounded stack for large objects on
// claiming and plain sinks (gk 1.3 / cc F22 / cc S2), stop-at-first-failure and state() propagation, bool
// normalisation (gk 1.7), conditional noexcept (cc F6), capture_file sticky errors (gk 1.8), fd_sink EINTR/zero
// handling surface (gk 1.9) and dictionary_tracker (FR-W7).
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace big
{
struct Blob
{
    std::uint8_t data[8u << 20];
    std::uint64_t seq;
}; // 8 MiB + 8: far above stack_frame_limit
struct Medium
{
    std::uint8_t data[1u << 20];
    std::uint64_t seq;
}; // 1 MiB: streams as 64 chunks of 16 KiB
} // namespace big
namespace flags
{
struct Flags
{
    bool a;
    std::uint8_t x;
    bool arr[3];
    std::uint16_t y;
};
// > 64 KiB so a plain sink streams it in 16 KiB chunks: b is the last byte of chunk 0, c the first of chunk 1
struct Boundary
{
    std::uint8_t pad[stream_chunk_size - 1];
    bool b;
    bool c;
    std::uint8_t tail[3 * stream_chunk_size];
};
static_assert(frame_size<Boundary>(serialize_options{.full_records = true}) > stack_frame_limit);
} // namespace flags

namespace
{

std::string tmp_path(const char* const name)
{
    return std::string(ES_SERREG_TEST_TMPDIR) + "/" + name;
}

// Runs fn on a thread with exactly `stack_bytes` of stack.
template<class F> void on_small_stack(const std::size_t stack_bytes, F&& fn)
{
    struct ctx
    {
        F* f;
    };
    ctx c{&fn};
    pthread_attr_t attr;
    REQUIRE(pthread_attr_init(&attr) == 0);
    REQUIRE(pthread_attr_setstacksize(&attr, stack_bytes) == 0);
    pthread_t th;
    REQUIRE(pthread_create(
                &th, &attr,
                [](void* const p) -> void* {
                    (*static_cast<ctx*>(p)->f)();
                    return nullptr;
                },
                &c) == 0);
    REQUIRE(pthread_join(th, nullptr) == 0);
    pthread_attr_destroy(&attr);
}

std::unique_ptr<big::Blob> make_blob()
{
    auto p = std::make_unique<big::Blob>();
    for (std::size_t i = 0; i < sizeof p->data; i += 4093)
        p->data[i] = static_cast<std::uint8_t>(i * 7);
    p->data[sizeof p->data - 1] = 0xEE;
    p->seq = 0x0102030405060708ull;
    return p;
}

std::vector<std::byte> read_all(const int fd)
{
    std::vector<std::byte> out;
    std::byte buf[65536];
    for (;;)
    {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

// Locate the first data frame of a FILE capture (header + dictionary, both 8-aligned; 1.7: then the CHAIN and
// the writer-info frame, which the walkers consume).
std::span<const std::byte> first_frame(const std::vector<std::byte>& file)
{
    std::span<const std::byte> found;
    const auto w = tools::for_each_frame(
        std::span<const std::byte>(file), [&](const dyn::dictionary&, const tools::frame_ref& fr) {
            found = std::span<const std::byte>(file).subspan(fr.offset);
            return false;
        });
    REQUIRE(w.has_value());
    return found;
}

// Plain sinks used below (all write-only; see test_support.hpp for the KNOWN-BUG specialisations).
struct counting_sink
{
    std::size_t calls = 0, bytes = 0, fail_at; // write #fail_at (1-based) returns false
    explicit counting_sink(const std::size_t f) noexcept : fail_at(f)
    {
    }
    bool write(const std::byte*, const std::size_t n) noexcept
    {
        ++calls;
        if (calls >= fail_at)
            return false;
        bytes += n;
        return true;
    }
};
struct counting_sink_with_error : counting_sink
{
    using counting_sink::counting_sink;
    status state() const noexcept
    {
        return calls >= fail_at ? status{std::unexpected(error::io_error)} : status{};
    }
};
struct throwing_sink
{
    std::vector<std::byte> v;
    bool write(const std::byte* const p, const std::size_t n)
    {
        v.insert(v.end(), p, p + n);
        return true;
    }
};

} // namespace
ES_SERREG_TEST_PLAIN_SINK(counting_sink, true);
ES_SERREG_TEST_PLAIN_SINK(counting_sink_with_error, true);
ES_SERREG_TEST_PLAIN_SINK(throwing_sink, false);

// ---- gk 1.3: 8 MiB object, 1 MiB stack ----------------------------------------------------------------------
TEST_CASE("gk 1.3 8 MiB struct serialised to capture_file under a 1 MiB stack",
          "[sinks][large][gk1.3]")
{
    const auto blob = make_blob();
    const std::string path = tmp_path("big.ser");
    result<std::size_t> r = std::unexpected(error::invalid_argument); // overwritten by the thread
    result<void> closed = std::unexpected(error::invalid_argument);
    on_small_stack(1u << 20, [&] {
        capture_file f(path);
        if (!f.is_open())
            return;
        r = serialize(
            f, *blob); // claiming sink: written in place into the file's buffer, no stack copy
        closed = f.close();
    });
    REQUIRE(r.has_value());
    CHECK(*r == frame_size<big::Blob>());
    REQUIRE(closed.has_value());
    const auto file = tools::read_file(path);
    REQUIRE(file.has_value());
    const auto rep = tools::verify(*file);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->records == 1);
    auto back = std::make_unique<big::Blob>();
    REQUIRE(deserialize(first_frame(*file), *back));
    CHECK(std::memcmp(back.get(), blob.get(), sizeof(big::Blob)) == 0);
}

TEST_CASE("gk 1.3 8 MiB struct serialised to fd_sink(pipe) under a 1 MiB stack",
          "[sinks][large][gk1.3]")
{
    const auto blob = make_blob();
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    { // child: the writer thread has 1 MiB of stack; the streaming path must fit in it
        ::close(fds[0]);
        int rc = 3;
        on_small_stack(1u << 20, [&] {
            fd_sink s{fds[1]};
            const auto w = serialize(s, *blob);
            rc = w && *w == frame_size<big::Blob>() ? 0 : 2;
        });
        ::close(fds[1]);
        ::_exit(rc);
    }
    ::close(fds[1]);
    const std::vector<std::byte> bytes = read_all(fds[0]);
    ::close(fds[0]);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    if (WIFSIGNALED(status))
        FAIL("child died with signal " << WTERMSIG(status)
                                       << ": the streaming path is not stack-bounded");
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    REQUIRE(bytes.size() == frame_size<big::Blob>());
    auto back = std::make_unique<big::Blob>();
    REQUIRE(deserialize(bytes, *back));
    CHECK(std::memcmp(back.get(), blob.get(), sizeof(big::Blob)) == 0);
}

TEST_CASE("gk 1.3 a failing plain sink stops serialize() after the first refused write",
          "[sinks][gk1.3]")
{
    const auto med = std::make_unique<big::Medium>();
    med->seq = 9;
    SECTION("streaming path (frame > 64 KiB): header + chunks, stop at the failing chunk")
    {
        counting_sink s{3}; // header ok, chunk 1 ok, chunk 2 refused
        const auto r = serialize(s, *med);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == error::sink_full);
        CHECK(s.calls == 3); // nothing after the failure
        CHECK(s.bytes == fmt::frame_header_size + stream_chunk_size);
        counting_sink ok{1000};
        REQUIRE(serialize(ok, *med));
        CHECK(ok.calls == 1 + (sizeof(big::Medium) + stream_chunk_size - 1) / stream_chunk_size);
        CHECK(ok.bytes == frame_size<big::Medium>());
    }
    SECTION("a sink with state() reports that error instead of sink_full")
    {
        counting_sink_with_error s{2};
        const auto r = serialize(s, *med);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == error::io_error);
        CHECK(s.calls == 2);
    }
    SECTION("multi-record streaming frame: later records are not attempted after a failure")
    {
        counting_sink s{2}; // header ok, first chunk of record 1 refused
        const auto r = serialize(s, *med, corpus::sample_order());
        REQUIRE_FALSE(r.has_value());
        CHECK(s.calls == 2);
    }
    SECTION("stack path (frame <= 64 KiB): exactly one write; a refusal is sink_full")
    {
        counting_sink s{1};
        const auto r = serialize(s, corpus::sample_order(), "x"_lit);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == error::sink_full);
        CHECK(s.calls == 1);
        counting_sink ok{2};
        REQUIRE(serialize(ok, corpus::sample_order(), "x"_lit));
        CHECK(ok.calls == 1);
        CHECK(ok.bytes == frame_size<corpus::Order, decltype("x"_lit)>());
    }
    SECTION("serialize_raw of a large object streams too and stops on failure")
    {
        counting_sink s{2};
        const auto r = serialize_raw(s, *med);
        REQUIRE_FALSE(r.has_value());
        CHECK(s.calls == 2);
        counting_sink ok{1000};
        REQUIRE(serialize_raw(ok, *med));
        CHECK(ok.bytes == sizeof(big::Medium));
    }
}

TEST_CASE("gk 1.9 fd_sink: io_error is sticky until reset(), never sink_full", "[sinks][fd]")
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    ::close(fds[0]); // no reader: write() fails with EPIPE
    ::signal(SIGPIPE, SIG_IGN);
    fd_sink s{fds[1]};
    CHECK(s.state().has_value());
    const auto r = serialize(s, corpus::sample_order());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::io_error);
    CHECK(s.state().error() == error::io_error);
    CHECK_FALSE(s.write(nullptr, 0)); // sticky: even an empty write is refused
    s.reset();
    CHECK(s.state().has_value());
    CHECK(s.write(nullptr, 0));
    ::close(fds[1]);
    // a working pipe: the frame arrives intact
    REQUIRE(::pipe(fds) == 0);
    fd_sink w{fds[1]};
    REQUIRE(serialize(w, "evt"_lit, corpus::sample_order()));
    ::close(fds[1]);
    const auto bytes = read_all(fds[0]);
    ::close(fds[0]);
    corpus::Order o{};
    REQUIRE(deserialize(bytes, "evt"_lit, o));
    CHECK(o == corpus::sample_order());
}

// ---- gk 1.7: bool normalisation --------------------------------------------------------------------------
TEST_CASE("gk 1.7 a bool holding 0xFF is 0x01 on the wire with scrub on and 0xFF with scrub off",
          "[sinks][bool][gk1.7]")
{
    flags::Flags f{};
    f.x = 0x55;
    f.y = 0x6677;
    std::memset(&f.a, 0xFF, 1);
    std::memset(&f.arr[1], 0xFF, 1);
    std::memset(&f.arr[2], 0x02, 1);
    f.arr[0] = true;
    STATIC_REQUIRE(descriptor_v<flags::Flags>.bool_offsets.size() == 4); // a + arr[0..2]
    STATIC_REQUIRE(descriptor_v<flags::Flags>.bool_offsets[0] == 0);
    STATIC_REQUIRE(descriptor_v<flags::Flags>.bool_offsets[1] == 2 &&
                   descriptor_v<flags::Flags>.bool_offsets[3] == 4);
    const auto byte_at = [](const std::vector<std::byte>& v, const std::size_t off) {
        return std::to_integer<unsigned>(v[16 + off]);
    };
    SECTION("claiming sink, scrub on (default)")
    {
        std::vector<std::byte> v;
        vector_sink s{v};
        REQUIRE(serialize(s, f));
        CHECK(byte_at(v, 0) == 0x01);
        CHECK(byte_at(v, 2) == 0x01);
        CHECK(byte_at(v, 3) == 0x01);
        CHECK(byte_at(v, 4) == 0x01);
        CHECK(byte_at(v, 1) == 0x55);
        CHECK(byte_at(v, 5) == 0x00); // padding gap before y
        // logically equal objects give identical bytes
        flags::Flags g{};
        g.a = true;
        g.x = 0x55;
        g.arr[0] = g.arr[1] = g.arr[2] = true;
        g.y = 0x6677;
        std::vector<std::byte> v2;
        vector_sink s2{v2};
        REQUIRE(serialize(s2, g));
        CHECK(v == v2);
    }
    SECTION("claiming sink, scrub off: the raw byte leaks through")
    {
        std::vector<std::byte> v;
        vector_sink s{v};
        REQUIRE(serialize_with(serialize_options{.scrub_padding = false}, s, f));
        CHECK(byte_at(v, 0) == 0xFF);
        CHECK(byte_at(v, 3) == 0xFF);
        CHECK(byte_at(v, 4) == 0x02);
    }
    SECTION("plain sink (stack path) and serialize_raw normalise too")
    {
        throwing_sink s;
        REQUIRE(serialize(s, f));
        CHECK(byte_at(s.v, 0) == 0x01);
        throwing_sink raw;
        REQUIRE(serialize_raw(raw, f));
        CHECK(std::to_integer<unsigned>(raw.v[0]) == 0x01);
        CHECK(std::to_integer<unsigned>(raw.v[3]) == 0x01);
    }
    SECTION("streaming path: a bool at a chunk boundary is normalised per chunk")
    {
        auto p = std::make_unique<flags::Boundary>();
        std::memset(p.get(), 0, sizeof(flags::Boundary));
        std::memset(&p->b, 0xFF, 1); // last byte of chunk 0
        std::memset(&p->c, 0x80, 1); // first byte of chunk 1
        throwing_sink s;
        REQUIRE(serialize(s, *p)); // frame > 64 KiB → streaming
        REQUIRE(s.v.size() == frame_size<flags::Boundary>());
        CHECK(std::to_integer<unsigned>(s.v[16 + stream_chunk_size - 1]) == 0x01);
        CHECK(std::to_integer<unsigned>(s.v[16 + stream_chunk_size]) == 0x01);
        throwing_sink off;
        REQUIRE(serialize_with(serialize_options{.scrub_padding = false}, off, *p));
        CHECK(std::to_integer<unsigned>(off.v[16 + stream_chunk_size - 1]) == 0xFF);
        CHECK(std::to_integer<unsigned>(off.v[16 + stream_chunk_size]) == 0x80);
    }
}

// ---- cc F6: noexcept follows the sink -------------------------------------------------------------------
TEST_CASE("cc F6 serialize is noexcept exactly when the sink is", "[sinks][noexcept]")
{
    std::byte buf[128];
    span_sink sp{buf};
    std::vector<std::byte> v;
    vector_sink vs{v};
    fd_sink fs{1};
    throwing_sink ts;
    const corpus::Order o = corpus::sample_order();
    STATIC_REQUIRE(noexcept(serialize(sp, o)));
    STATIC_REQUIRE(noexcept(serialize(fs, o)));
    STATIC_REQUIRE_FALSE(noexcept(serialize(vs, o))); // vector_sink allocates
    STATIC_REQUIRE_FALSE(noexcept(serialize(ts, o))); // write() not noexcept
    STATIC_REQUIRE(noexcept(serialize_raw(sp, o)));
    STATIC_REQUIRE_FALSE(noexcept(serialize_raw(vs, o)));
    constexpr auto x = "x"_lit; // (the consteval UDL call itself is not a noexcept expression)
    STATIC_REQUIRE(noexcept(deserialize(std::span<const std::byte>{}, x)));
    STATIC_REQUIRE(noexcept(inspect(std::span<const std::byte>{})));
    SUCCEED();
}

// ---- 2.1.1 (T0): G2-7 — noexcept is false exactly where the library may allocate -------------------------------
TEST_CASE("G2-7 decoding into a dynamic type or std::string, or a dynamic record to a plain sink, "
          "is not noexcept",
          "[sinks][noexcept]")
{
    const std::span<const std::byte> none{};
    corpus::Order o{};
    corpus::DynMsg d{};
    std::string s;
    std::string_view sv;
    int i = 0;
    STATIC_REQUIRE(noexcept(deserialize(none, o)));
    STATIC_REQUIRE(noexcept(deserialize(none, sv, i)));
    STATIC_REQUIRE_FALSE(noexcept(deserialize(none, d)));    // decode_image allocates
    STATIC_REQUIRE_FALSE(noexcept(deserialize(none, s, i))); // std::string copy allocates
    STATIC_REQUIRE_FALSE(noexcept(deserialize(none, o, d)));
    STATIC_REQUIRE(noexcept(dispatch<corpus::Order>(none, [](const auto&) noexcept {})));
    STATIC_REQUIRE_FALSE(noexcept(dispatch<corpus::DynMsg>(none, [](const auto&) noexcept {})));
    std::byte buf[256];
    span_sink sp{buf};
    fd_sink fs{1};
    const corpus::DynMsg dm = corpus::sample_dyn_msg();
    STATIC_REQUIRE(
        noexcept(serialize(sp, dm))); // claiming sink: the image is built in claimed memory
    STATIC_REQUIRE_FALSE(
        noexcept(serialize(fs, dm))); // plain sink: a large image goes through a heap buffer
    STATIC_REQUIRE(noexcept(serialize(fs, o)));
    SUCCEED();
}

// ---- 2.2.6 (review B1): the canonical sort of an unordered container may allocate on every sink -----------------
TEST_CASE("B1 serialize of a type that may sort an unordered container is not noexcept on any sink",
          "[sinks][noexcept][B1]")
{
    using um = std::unordered_map<int, int>;
    STATIC_REQUIRE(detail::may_sort_v<um>);
    STATIC_REQUIRE(detail::may_sort_v<std::unordered_set<std::string>>);
    STATIC_REQUIRE(detail::may_sort_v<std::unordered_multimap<int, int>>);
    STATIC_REQUIRE(
        detail::may_sort_v<std::multimap<int, int, std::greater<int>>>); // sorted by the writer
    STATIC_REQUIRE_FALSE(
        detail::may_sort_v<std::multimap<int, int>>); // std::less: the container's order
    STATIC_REQUIRE(detail::may_sort_v<std::map<int, int, std::greater<int>>>);
    STATIC_REQUIRE_FALSE(detail::may_sort_v<std::map<int, int>>);
    STATIC_REQUIRE(detail::may_sort_v<std::vector<um>>);
    STATIC_REQUIRE(detail::may_sort_v<std::optional<um>>);
    STATIC_REQUIRE(detail::may_sort_v<std::variant<int, um>>);
    STATIC_REQUIRE(detail::may_sort_v<std::map<int, std::vector<um>>>);
    STATIC_REQUIRE(detail::may_sort_v<std::pair<int, um>>);
    STATIC_REQUIRE(detail::may_sort_v<corpus::DynMap>); // names: std::unordered_map<i32, string>
    STATIC_REQUIRE_FALSE(detail::may_sort_v<corpus::DynMsg>);
    STATIC_REQUIRE_FALSE(detail::may_sort_v<corpus::DynNested>);
    STATIC_REQUIRE_FALSE(detail::may_sort_v<corpus::Order>);
    STATIC_REQUIRE_FALSE(detail::may_sort_v<std::string>);
    // the specification follows the trait on claiming and plain sinks alike
    STATIC_REQUIRE_FALSE(detail::serialize_noexcept<span_sink, um>);
    STATIC_REQUIRE_FALSE(detail::serialize_noexcept<span_sink, corpus::DynMap>);
    STATIC_REQUIRE_FALSE(detail::serialize_noexcept<span_sink, corpus::Order, um>);
    STATIC_REQUIRE_FALSE(detail::serialize_noexcept<fd_sink, um>);
    STATIC_REQUIRE(detail::serialize_noexcept<span_sink, corpus::Order>);
    STATIC_REQUIRE(detail::serialize_noexcept<span_sink, corpus::DynMsg>);
    STATIC_REQUIRE(detail::serialize_noexcept<span_sink, std::map<int, int>>);
    STATIC_REQUIRE(detail::serialize_noexcept<span_sink, std::multimap<int, int>>);
    std::byte buf[256];
    span_sink sp{buf};
    const um m;
    const corpus::DynMap dm{};
    STATIC_REQUIRE_FALSE(noexcept(serialize(sp, m)));
    STATIC_REQUIRE_FALSE(noexcept(serialize(sp, dm)));
    STATIC_REQUIRE_FALSE(
        noexcept(serialize_with(serialize_options{.canonical_unordered = false}, sp, m)));
    SUCCEED();
}

// ---- gk 1.8: capture_file error surface -------------------------------------------------------------------
TEST_CASE("gk 1.8 capture_file: open failure is sticky, write fails with it, close reports it",
          "[sinks][capture][gk1.8]")
{
    SECTION("unopenable path")
    {
        capture_file f("/nonexistent-dir/x/y.ser");
        CHECK_FALSE(f.is_open());
        CHECK(f.state().error() == error::io_error);
        const auto r = serialize(f, corpus::sample_order());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == error::io_error);
        const auto c = f.close();
        REQUIRE_FALSE(c.has_value());
        CHECK(c.error() == error::io_error);
    }
    SECTION("open() twice closes the first file cleanly; close() on a fresh object is ok")
    {
        capture_file f;
        REQUIRE(f.close());
        REQUIRE(f.open(tmp_path("twice_a.ser")));
        REQUIRE(serialize(f, corpus::sample_cancel()));
        REQUIRE(f.open(tmp_path("twice_b.ser")));
        REQUIRE(serialize(f, corpus::sample_order()));
        REQUIRE(f.close());
        CHECK_FALSE(f.is_open());
        const auto a = tools::read_file(tmp_path("twice_a.ser"));
        const auto b = tools::read_file(tmp_path("twice_b.ser"));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        const auto ra = tools::verify(*a), rb = tools::verify(*b);
        REQUIRE((ra.has_value() && rb.has_value()));
        CHECK((ra->ok() && ra->records == 1));
        CHECK((rb->ok() && rb->records == 1));
    }
    SECTION(
        "close() is [[nodiscard]] and an I/O failure (/dev/full) is reported by open() or close()")
    {
        STATIC_REQUIRE(
            std::is_same_v<decltype(std::declval<capture_file&>().close()), result<void>>);
        capture_file full;
        const auto o = full.open(
            "/dev/full"); // ENOSPC surfaces at the first stdio flush: in open() or at close()
        if (o)
        {
            REQUIRE(serialize(full, corpus::sample_order()));
            const auto c = full.close();
            REQUIRE_FALSE(c.has_value());
            CHECK(c.error() == error::io_error);
        }
        else
        {
            CHECK(o.error() == error::io_error);
        }
        CHECK(full.state().error() == error::io_error);
        CHECK_FALSE(full.is_open());
        const auto again = full.close(); // still reports the sticky error, does not crash
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error() == error::io_error);
    }
}

// ---- FR-W7: dictionary_tracker ------------------------------------------------------------------------------
TEST_CASE("FR-W7 dictionary_tracker writes a dictionary frame once and not again while the version "
          "is unchanged",
          "[sinks][tracker]")
{
    std::vector<std::byte> stream;
    vector_sink out{stream};
    dictionary_tracker dt;
    CHECK(dt.last_version == 0);
    const auto first = dt.update(out);
    REQUIRE(first.has_value());
    CHECK(*first);
    CHECK(dt.last_version == registry::instance().version());
    const std::size_t after_first = stream.size();
    CHECK(after_first == fmt::frame_header_size +
                             fmt::align8(registry::instance().export_image().size()) +
                             fmt::dict_frame_link_size); // 1.7: the u64 next_dictionary link
    for (int i = 0; i < 5; ++i)
    {
        const auto again = dt.update(out);
        REQUIRE(again.has_value());
        CHECK_FALSE(*again);
    }
    CHECK(stream.size() == after_first);
    REQUIRE(serialize(out, "evt"_lit, corpus::sample_order()));
    const auto rep = tools::verify(stream);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 1); // the data frame; 2.2.1: dictionary frames are consumed, not counted
    // a tracker that believes an older version writes again exactly once
    dt.last_version = 1;
    const auto rewritten = dt.update(out);
    REQUIRE(rewritten.has_value());
    CHECK(*rewritten);
    CHECK(dt.last_version == registry::instance().version());
    // a failing sink propagates the error and leaves last_version alone
    counting_sink_with_error bad{1};
    dictionary_tracker dt2;
    const auto e = dt2.update(bad);
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error() == error::io_error);
    CHECK(dt2.last_version == 0);
}

// ---- cc review 2.1.10 (2.1.12), C-09: a refused commit is a failed serialize() ---------------------------------
namespace c09
{
struct refusing_sink
{
    std::vector<std::byte> buf = std::vector<std::byte>(4096);
    bool accept = true;
    bool write(const std::byte*, std::size_t) noexcept
    {
        return accept;
    }
    std::byte* claim(const std::size_t n) noexcept
    {
        return n <= buf.size() ? buf.data() : nullptr;
    }
    bool commit(std::size_t) noexcept
    {
        return accept;
    }
};
} // namespace c09
static_assert(claiming_sink<c09::refusing_sink>);

TEST_CASE("2.1.12 C-09 a claiming sink whose commit() returns false makes serialize() fail",
          "[sinks]")
{
    c09::refusing_sink s;
    CHECK(serialize(s, corpus::Order{}));
    s.accept = false;
    CHECK_FALSE(serialize(s, corpus::Order{}));
}

TEST_CASE(
    "2.1.12 C-09 capture_file: a frame the file system refuses is reported by that serialize()",
    "[sinks][capture]")
{
    // In a child with a 64 KiB file-size limit (SIGXFSZ ignored): the header and dictionary fit, a 1 MiB frame
    // does not, and fwrite fails with EFBIG. 2.1.11 returned success for that frame and failed only at close().
    const std::string path = std::string(ES_SERREG_TEST_TMPDIR) + "/c09_fsize.ser";
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        ::signal(SIGXFSZ, SIG_IGN);
        const rlimit lim{64u << 10, 64u << 10};
        if (::setrlimit(RLIMIT_FSIZE, &lim) != 0)
            std::_Exit(3);
        capture_file f;
        if (!f.open(path))
            std::_Exit(4);
        const std::string big(1u << 20,
                              'x'); // larger than any stdio buffer: fwrite reaches the file
        const bool refused = !serialize(f, big);
        const bool closed = static_cast<bool>(f.close());
        std::_Exit(refused && !closed ? 0 : 5);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    ::unlink(path.c_str());
    CHECK(WIFEXITED(st));
    CHECK(WEXITSTATUS(st) == 0);
}
