// tests/unit/test_limits.cpp — 2.1.16: the reader limits (A3, decode_limits, FR-D5) and the windowed capture
// readers (A6): a capture of any size is read `window_bytes` at a time; the output of every window size equals
// the whole-file output; a frame larger than the window grows it; a frame above frame_bytes is a problem; the
// problem list is capped; memory stays bounded while a 64 MiB capture is walked with a 1 MiB window.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace
{

// Restores the process-wide limits when a test leaves.
struct limits_scope
{
    decode_limits saved = get_decode_limits();
    ~limits_scope()
    {
        set_decode_limits(saved);
    }
};

std::string tmp(const char* const name)
{
    return std::string(ES_SERREG_TEST_TMPDIR) + "/" + name;
}

const char* golden(const char* const name)
{
    static std::string p;
    p = std::string(ES_SERREG_TEST_SOURCE_DIR) + "/../fixtures/golden/" + name;
    return p.c_str();
}

std::string dump_text(const std::string& path, const std::size_t window, const tools::format fmt)
{
    tools::dump_options opt;
    opt.fmt = fmt;
    std::string out;
    tools::block_options o;
    o.block_bytes = window;
    REQUIRE(tools::dump(path, nullptr, opt, out, o).has_value());
    return out;
}

long maxrss_kib()
{
    rusage u{};
    ::getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss;
}

} // namespace

TEST_CASE("2.1.16 A3 decode_limits: defaults, process-wide set/get, and what each one bounds",
          "[limits][FR-D5]")
{
    limits_scope scope;
    const decode_limits d = get_decode_limits();
    CHECK(d.frame_bytes == (std::uint64_t{64} << 20));
    CHECK(d.dictionary_bytes == (std::uint64_t{256} << 20));
    CHECK(d.dictionary_entries == 1'000'000);
    CHECK(d.image_depth == 64);
    CHECK(d.input_bytes == (std::uint64_t{1} << 30));
    CHECK(d.window_bytes == (std::size_t{10} << 20));
    CHECK(d.problems == 1000);
    decode_limits l = d;
    l.frame_bytes = 4096;
    set_decode_limits(l);
    CHECK(get_decode_limits() == l);
    CHECK(get_decode_limits().frame_bytes == 4096);
}

TEST_CASE("2.1.16 A3 a frame above decode_limits::frame_bytes is a problem, not an allocation",
          "[limits][FR-D5]")
{
    limits_scope scope;
    // a FILE capture (header, dictionary) with two honest frames, then a frame header claiming 200 MiB
    const std::string honest_path = tmp("limits_honest.ser");
    {
        capture_file f(honest_path);
        REQUIRE(f.is_open());
        REQUIRE(serialize(f, "evt"_, corpus::sample_order()));
        REQUIRE(serialize(f, corpus::sample_cancel(), "why"_));
        REQUIRE(f.close());
    }
    const auto whole = tools::read_file(honest_path);
    REQUIRE(whole.has_value());
    const std::vector<std::byte> cap = *whole;
    ::unlink(honest_path.c_str());
    const std::size_t honest = cap.size();
    std::vector<std::byte> liar(cap);
    liar.resize(cap.size() + 16);
    const std::uint32_t claimed = 200u << 20;
    std::memcpy(liar.data() + honest, &claimed, 4); // frame_len
    // span walker: the default limit (64 MiB) rejects it as a bad length; the two frames before it are counted
    const auto rep = tools::verify(liar);
    REQUIRE(rep.has_value());
    CHECK(rep->frames == 2);
    REQUIRE(rep->problems.size() == 1);
    CHECK(rep->problems[0].find("bad frame_len 209715200") != std::string::npos);
    // a limit below an honest frame makes the honest frame the problem
    decode_limits l = get_decode_limits();
    l.frame_bytes = 32;
    set_decode_limits(l);
    const auto strict = tools::verify(cap);
    REQUIRE(strict.has_value());
    CHECK(strict->frames == 0);
    REQUIRE(strict->problems.size() == 1);
    CHECK(strict->problems[0].find("bad frame_len") != std::string::npos);
    // the windowed reader applies the same limit to a file and to a pipe
    const std::string path = tmp("limits_liar.ser");
    REQUIRE(tools::write_file(path, liar));
    set_decode_limits(scope.saved);
    const auto file_rep = tools::verify(path);
    REQUIRE(file_rep.has_value());
    CHECK(file_rep->frames == 2);
    REQUIRE(file_rep->problems.size() == 1);
    CHECK(file_rep->problems[0].find("bad frame_len 209715200") != std::string::npos);
    ::unlink(path.c_str());
}

TEST_CASE("2.1.16 A3 the problem list is capped at decode_limits::problems; the rest are counted",
          "[limits][FR-D5]")
{
    limits_scope scope;
    decode_limits l = get_decode_limits();
    l.problems = 3;
    set_decode_limits(l);
    // 10 frames whose call hash nobody knows: ten "unknown call" problems
    std::vector<std::byte> cap;
    for (int i = 0; i < 10; ++i)
    {
        std::byte frame[32]{};
        const std::uint32_t len = 32;
        const std::uint16_t flags = 0, n = 1;
        const std::uint64_t call = 0x0bad0bad0bad0badull + static_cast<unsigned long long>(i);
        std::memcpy(frame, &len, 4);
        std::memcpy(frame + 4, &flags, 2);
        std::memcpy(frame + 6, &n, 2);
        std::memcpy(frame + 8, &call, 8);
        cap.insert(cap.end(), frame, frame + 32);
    }
    const auto rep = tools::verify(cap);
    REQUIRE(rep.has_value());
    CHECK(rep->frames == 10);
    CHECK(rep->problems.size() == 3);
    CHECK(rep->problems_omitted == 7);
    CHECK_FALSE(rep->ok());
    // the windowed path caps the same way
    const std::string path = tmp("limits_problems.ser");
    REQUIRE(tools::write_file(path, cap));
    const auto file_rep = tools::verify(path);
    REQUIRE(file_rep.has_value());
    CHECK(file_rep->problems.size() == 3);
    CHECK(file_rep->problems_omitted == 7);
    ::unlink(path.c_str());
}

TEST_CASE("2.1.16 A3 decode_limits::dictionary_entries bounds what a dictionary may declare",
          "[limits][FR-D5]")
{
    limits_scope scope;
    const auto rri = registry::instance().export_image();
    REQUIRE(tools::load_dictionary_rri(rri).has_value());
    decode_limits l = get_decode_limits();
    l.dictionary_entries = 2;
    set_decode_limits(l);
    const auto refused = tools::load_dictionary_rri(rri);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == error::corrupt_dictionary);
}

TEST_CASE("2.1.16 A6 every window size gives the whole-file output, byte for byte",
          "[limits][window]")
{
    // The golden captures have frames of 16 bytes to a few hundred bytes, compact and full, print frames, and
    // dynamic images; a 64-byte window makes almost every frame straddle a window boundary, and the 4 KiB and the
    // default windows take the other paths. Text, JSON and TOML must not depend on how the file was read.
    for (const char* const name : {"orders.ser", "dynamic.ser"})
    {
        const std::string path = golden(name);
        const auto whole = tools::read_file(path);
        REQUIRE(whole.has_value());
        for (const auto fmt : {tools::format::text, tools::format::json, tools::format::toml})
        {
            tools::dump_options opt;
            opt.fmt = fmt;
            std::string span_out;
            REQUIRE(tools::dump(*whole, nullptr, opt, span_out).has_value());
            for (const std::size_t window : {std::size_t{0}, std::size_t{4096}, std::size_t{64}})
            {
                INFO(name << " window " << window << " format " << static_cast<int>(fmt));
                // a 64-byte window is below the 4 KiB floor of the reader: it rounds up to 4 KiB, which is still
                // smaller than the fixture's dictionary and larger than any of its frames
                CHECK(dump_text(path, window, fmt) == span_out);
            }
        }
        // verify agrees too
        const auto v_span = tools::verify(*whole);
        tools::block_options small;
        small.block_bytes = 64;
        const auto v_file = tools::verify(path, nullptr, small);
        REQUIRE(v_span.has_value());
        REQUIRE(v_file.has_value());
        CHECK(v_span->ok());
        CHECK(v_file->ok());
        CHECK(v_span->frames == v_file->frames);
        CHECK(v_span->records == v_file->records);
        CHECK(v_span->bytes == v_file->bytes);
        CHECK(v_file->program_hash != 0);
    }
}

TEST_CASE("2.1.16 A6 a frame larger than the window grows it; a 64 MiB capture walks within a "
          "bounded RSS",
          "[limits][window]")
{
    // 1024 frames of a 64 KiB string: 64 MiB on disk; a 1 MiB window; the frame of 4 MiB in the middle is
    // larger than the window (it grows for that frame and shrinks back to the window afterwards).
    const std::string path = tmp("limits_big.ser");
    {
        capture_file f(path);
        REQUIRE(f.is_open());
        const std::string chunk(std::size_t{64} << 10, 'x');
        const std::string big(std::size_t{4} << 20, 'y');
        for (int i = 0; i < 1024; ++i)
        {
            if (i == 512)
                REQUIRE(serialize(f, "big"_, big));
            REQUIRE(serialize(f, "chunk"_, i, chunk));
        }
        REQUIRE(f.close());
    }
    const long before = maxrss_kib();
    std::uint64_t frames = 0, big_frames = 0;
    tools::file_options o;
    o.block_bytes = std::size_t{1} << 20;
    const auto w = tools::for_each_frame(
        path,
        [&](const dyn::dictionary&, const tools::frame_ref& fr) {
            ++frames;
            if (fr.bytes.size() > (std::size_t{1} << 20))
                ++big_frames;
            return true;
        },
        o);
    REQUIRE(w.has_value());
    CHECK(frames == 1025);
    CHECK(big_frames == 1);
    CHECK(w->problems.empty());
    tools::block_options bo;
    bo.block_bytes = std::size_t{1} << 20;
    const auto rep = tools::verify(path, nullptr, bo);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 1025);
    const long after = maxrss_kib();
    // the high-water mark may grow by the window, the 4 MiB frame and the dictionary — not by the file; under
    // AddressSanitizer the freed windows sit in its quarantine (256 MiB by default) and the shadow grows with
    // them, so the bound there is "far below the file", not "the window plus the frame"
#if defined(__SANITIZE_ADDRESS__)
    CHECK(after - before < 48 * 1024);
#else
    CHECK(after - before < 24 * 1024);
#endif
    ::unlink(path.c_str());
}

TEST_CASE("2.1.16 A6 srdump reads a capture in windows: --window and --frame-limit",
          "[limits][window][cli]")
{
    // the rendered output of the windowed CLI path is compared with the fixtures by the golden_srdump_* ctests;
    // here the flags and their exit codes
    limits_scope scope;
    const std::string path = golden("orders.ser");
    const auto run = [&](std::vector<std::string> args) {
        args.insert(args.begin(), "srdump");
        std::vector<char*> argv;
        for (auto& s : args)
            argv.push_back(s.data());
        std::FILE* const saved = stdout;
        stdout = std::fopen("/dev/null", "w"); // cli::run writes the dump to stdout
        const int rc = cli::run(static_cast<int>(argv.size()), argv.data());
        std::fclose(stdout);
        stdout = saved;
        return rc;
    };
    CHECK(run({"--window", "4096", "--verify", path}) == 0);
    CHECK(run({"--window", "10M", "--json", path}) == 0);
    CHECK(run({"--window", "12", path}) == 2);                  // below the 4 KiB floor
    CHECK(run({"--window", "ten", path}) == 2);                 // not a size
    CHECK(run({"--frame-limit", "32", "--verify", path}) == 1); // every fixture frame is above it
    CHECK(get_decode_limits().frame_bytes == 32); // the flag sets the process-wide limit
    CHECK(run({"--frame-limit", "64M", "--verify", path}) == 0);
}

// ---- 2.1.16, decision D (second half): enums without a fixed underlying type are range-checked ----------------
namespace limits_test
{
enum Plain
{
    plain_a,
    plain_b,
    plain_c
}; // values 0..3 are inside the hypothetical 2-bit type; 7 is not
struct HasPlain
{
    std::uint32_t id;
    Plain p;
    std::uint32_t tail;
};
} // namespace limits_test
using limits_test::HasPlain;
using limits_test::plain_c;

TEST_CASE("2.1.16 decision D: an out-of-range value of an enum without a fixed underlying type "
          "reads as 0 and "
          "is reported as invalid_value",
          "[limits][enum][decision-D]")
{
    std::vector<std::byte> buf;
    {
        vector_sink s{buf};
        REQUIRE(serialize(s, HasPlain{1, plain_c, 9}, std::uint32_t{5}).has_value());
    }
    HasPlain out{};
    std::uint32_t u = 0;
    REQUIRE(deserialize(buf, out, u).has_value());
    CHECK((out.id == 1 && out.p == plain_c && out.tail == 9 && u == 5));
    // 3 is inside the range (0..3) although no enumerator names it: accepted, as any flag-style value would be
    buf[16 + 4] = std::byte{3};
    REQUIRE(deserialize(buf, out, u).has_value());
    CHECK(static_cast<int>(out.p) == 3);
    // 7 is outside: the frame still decodes, the leaf reads as 0, the error names the argument
    buf[16 + 4] = std::byte{7};
    out = HasPlain{};
    u = 0;
    const auto r = deserialize(buf, out, u);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == error::invalid_value);
    CHECK(r.error().expected == 3);
    CHECK(r.error().actual == 2); // the frame and the u32; HasPlain was read but is not complete
    CHECK(r.error().failed == 1);
    CHECK((out.id == 1 && static_cast<int>(out.p) == 0 && out.tail == 9 && u == 5));
    // a zero-copy view refuses it, dispatch does not deliver it, deserialize_raw reports it
    const auto fv = inspect(buf);
    REQUIRE(fv.has_value());
    const record_view rec = *fv->records().begin();
    CHECK(view<HasPlain>(rec).error() == error::invalid_value);
    int delivered = 0;
    const auto dr = dispatch<HasPlain>(buf, [&](const HasPlain&) { ++delivered; });
    CHECK(dr.error() == error::invalid_value);
    CHECK(delivered == 0);
    HasPlain raw{};
    CHECK(deserialize_raw(std::span<const std::byte>(buf).subspan(16, sizeof(HasPlain)), raw)
              .error() == error::invalid_value);
}
