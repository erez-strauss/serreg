// ce-example7 — binary logging, "formatted by the reader" (slide 12 of the Core C++ 2026 talk), on Compiler
// Explorer (GCC 16, -std=c++26 -freflection). Three ES_SERREG_LOG lines (info, warn, error; a scalar, a string
// and a struct as arguments) go into a capture_file; then the bytes of one log frame are shown: the 16-byte frame
// header and the argument records only — the level, the source file, the line, the envelope mask and the format
// string are constants of the call ENTRY in the dictionary and cost 0 bytes per line. A counter shows that an
// argument of a line below the compile-time floor (ES_SERREG_LOG_MIN_LEVEL = info here) is never evaluated. Then
// every frame is read back twice: typed (the compiled signature, as frame_router::on_log does) and driven by the
// dictionary record alone (as srdump --level does, with nothing compiled in) — and the rendered line is printed.
//
// One translation unit: the library headers plus ce_registry_stub.hpp standing in for libserreg (Compiler
// Explorer links nothing else). The stub exports the whole registry as the dictionary (export_image_since), not a
// scope's closure (export_image_scope lives in src/registry.cpp), so the logger uses dictionary_policy::full; the
// frames are the same bytes either way. If the executor's sandbox refuses to create the file, the same frames go
// to an in-memory stream (write_dictionary_frame + the logger over a vector_sink) and the dump says so.
#define ES_SERREG_LOG_MIN_LEVEL 2 // level::info: trace and debug lines compile to nothing in this TU
#include "ce_registry_stub.hpp"

#include <es/serreg/capture.hpp>
#include <es/serreg/deserialize.hpp>
#include <es/serreg/log.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace sr = es::serreg;
using namespace sr::literals;

namespace corpus
{
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Order
{
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
    Side side;
    char sym[8];
};
} // namespace corpus

namespace
{
using bytes = std::span<const std::byte>;
// The policy: no thread id or sequence per message (bits 0, 1 of the mask), the system clock (ns since the epoch,
// bits 2-3), the whole registry as the dictionary (see the header comment).
constexpr sr::log_options opts{.dictionary = sr::dictionary_policy::full};
constexpr auto k_file = sr::fixed_string{__FILE__}; // what ES_SERREG_LOG puts into the signature
int trace_evaluated = 0;
int costly() // the argument of the trace line: a side effect that must not happen
{
    return ++trace_evaluated;
}

std::string hex(const bytes b)
{
    std::string s;
    for (const std::byte x : b)
        s += std::format("{:02x} ", std::to_integer<unsigned>(x));
    return s;
}
template<class T> T load(const bytes b, const std::size_t at = 0)
{
    T v{};
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}
std::vector<std::byte> slurp(const char* const path)
{
    std::vector<std::byte> v;
    if (std::FILE* const f = std::fopen(path, "rb"))
    {
        std::byte buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
            v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    }
    return v;
}

// (d) the typed read: the compiled signature of a log call — log_tag, then the envelope constants (level, file,
// line, mask), the timestamp, the format literal, the arguments. The call hash covers all of it, so a wrong
// level, file, line or format is a hash mismatch, never a wrong line. frame_router::on_log does the same with
// the dictionary instead of the types (libserreg).
template<sr::level L, std::uint32_t Line, sr::fixed_string F, class... Ts>
bool read_log(const bytes frame, std::uint64_t& ts, Ts&... outs)
{
    constexpr auto lvl = sr::constant<static_cast<std::uint8_t>(L)>{};
    return sr::detail::deserialize_frame<sr::log_tag, decltype(lvl), sr::lit<k_file>, sr::constant<Line>,
                                         sr::constant<opts.mask()>, std::uint64_t, sr::lit<F>, Ts...>(
               frame, lvl, sr::lit<k_file>{}, sr::constant<Line>{}, sr::constant<opts.mask()>{}, ts,
               sr::lit<F>{}, outs...)
        .has_value();
}

// (d) the dictionary-driven read: inspect() resolves the frame's call hash to its ENTRY (through the registry
// here; srdump, the Python reader and Wireshark through the dictionary in the file) and hands out one record per
// argument — the constants and literals come from the ENTRY and have no bytes in the frame. The envelope is
// log_layout: level, file, line, mask, timestamp, then the format and its arguments. Renders the line as
// srdump --level prints it (the time is left as the raw ns count).
std::string render(const bytes frame)
{
    const auto fv = sr::inspect(frame);
    if (!fv)
        return std::string("(") + std::string(sr::to_string(fv.error())) + ")";
    std::vector<std::string> arg; // every record as text
    for (const sr::record_view& r : fv->records())
    {
        if (r.is_literal)
            arg.push_back(std::string(sr::registry::instance().find_literal(r.hash)));
        else if (r.ak == sr::fmt::arg_string)
            arg.push_back(std::string(r.text()));
        else if (r.desc) // a struct: its fields from the dictionary record (ce-example6 decodes them in full)
        {
            const std::span<const sr::field_desc> fs = r.desc->fields;
            const char* const sym = reinterpret_cast<const char*>(r.payload.data() + fs[4].offset);
            arg.push_back(std::format("{} {{id: {} px: {} qty: {} sym: \"{}\"}}", r.desc->name,
                                      load<std::uint64_t>(r.payload, fs[0].offset),
                                      load<std::int64_t>(r.payload, fs[1].offset),
                                      load<std::uint32_t>(r.payload, fs[2].offset),
                                      std::string_view(sym, strnlen(sym, fs[4].size))));
        }
        else
            switch (r.k)
            {
            case sr::kind::u8: arg.push_back(std::to_string(load<std::uint8_t>(r.payload))); break;
            case sr::kind::i32: arg.push_back(std::to_string(load<std::int32_t>(r.payload))); break;
            case sr::kind::u32: arg.push_back(std::to_string(load<std::uint32_t>(r.payload))); break;
            case sr::kind::u64: arg.push_back(std::to_string(load<std::uint64_t>(r.payload))); break;
            case sr::kind::f64: arg.push_back(std::format("{:.2f}", load<double>(r.payload))); break;
            default: arg.push_back("?");
            }
    }
    namespace ll = sr::log_layout;
    if (arg.size() <= ll::time_arg)
        return "(short frame)";
    const std::size_t fmt_at = ll::format_arg(static_cast<std::uint8_t>(std::stoi(arg[ll::mask_arg])));
    if (arg.size() <= fmt_at)
        return "(short frame)";
    std::string line = std::format("{} {:<5} {}:{} ", arg[ll::time_arg],
                                   sr::to_string(static_cast<sr::level>(std::stoi(arg[ll::level_arg]))),
                                   arg[ll::file_arg], arg[ll::line_arg]);
    std::size_t next = fmt_at + 1; // substitute the format's {…} fields in order
    for (std::string_view f = arg[fmt_at]; !f.empty();)
        if (f.front() == '{' && f.find('}') != f.npos)
        {
            line += next < arg.size() ? arg[next++] : "?";
            f.remove_prefix(f.find('}') + 1);
        }
        else
        {
            line += f.front();
            f.remove_prefix(1);
        }
    return line;
}

// Writes the three lines (and the trace line that compiles to nothing), collects the bytes, explains them.
template<sr::sink S, class Collect> int run(S& out, Collect collect, const bool is_file)
{
    sr::logger<opts, S> lg{out, sr::level::info}; // the run-time gate; the compile-time floor is info too
    const corpus::Order o{42, 1050000, 100, corpus::Side::buy, "AAPL"};
    constexpr std::uint32_t line_info = __LINE__ + 1;
    const bool w1 = ES_SERREG_LOG(lg, info, "started with {} workers"_, 4);
    constexpr std::uint32_t line_warn = __LINE__ + 1;
    const bool w2 = ES_SERREG_LOG(lg, warn, "book {} depth {}"_, "AAPL", 7);
    constexpr std::uint32_t line_error = __LINE__ + 1;
    const bool w3 = ES_SERREG_LOG(lg, error, "rejected {} px={:.2f}"_, o, 105.0);
    // (c) below the floor: `L < floor` is an `if constexpr` in logger::write — no call ENTRY, no code, and the
    // macro's `enabled() &&` never evaluates costly()
    const bool w4 = ES_SERREG_LOG(lg, trace, "cache {} entries"_, costly());
    const int written = w1 + w2 + w3 + w4;
    std::println("{} log frames written (info, warn, error); the trace line: {} frames, costly() called {} times\n",
                 written, w4 ? 1 : 0, trace_evaluated);

    const std::vector<std::byte> f = collect();
    std::println("{} bytes: {}, then the log frames\n", f.size(),
                 is_file ? "FILE header, dictionary, CHAIN, writer-info frame" : "the dictionary frame");
    // the log frames are the call frames; a file's frames start after FILE header | RRI | pad | CHAIN
    // (ce-example6 takes those apart), a stream's after the dictionary frame
    std::vector<bytes> frames;
    std::size_t at = is_file ? ((load<std::uint64_t>(f, 24) + load<std::uint64_t>(f, 32) + 7) & ~std::size_t{7}) +
                                   sr::fmt::chain_size
                             : 0;
    while (at + sr::fmt::frame_header_size <= f.size())
    {
        const std::uint32_t len = load<std::uint32_t>(f, at);
        if (len < sr::fmt::frame_header_size || at + len > f.size())
            break;
        if (const std::uint64_t call = load<std::uint64_t>(f, at + 8);
            call != sr::fmt::info_sentinel && call != sr::fmt::dict_frame_sentinel)
            frames.push_back(bytes{f}.subspan(at, len));
        at += len;
    }

    int read = 0;
    if (frames.size() == 3)
    {
        // (b) one frame, byte by byte: the warn line
        const bytes w = frames[1];
        std::println("the warn frame, {} bytes:", w.size());
        std::println("  {} header: frame_len {} flags {:#x} n_records {} call {:#018x}", hex(w.first(16)),
                     load<std::uint32_t>(w, 0), load<std::uint16_t>(w, 4), load<std::uint16_t>(w, 6),
                     load<std::uint64_t>(w, 8));
        std::println("  {}                         timestamp (u64 ns, system_clock) = {}", hex(w.subspan(16, 8)),
                     load<std::uint64_t>(w, 16));
        std::println("  {}                         \"AAPL\": u32 len + bytes, padded to 8", hex(w.subspan(24, 8)));
        std::println("  {}                         7 (int), padded to 8", hex(w.subspan(32, 8)));
        std::println("  level warn, file \"{}\", line {}, the mask and the format \"book {{}} depth {{}}\" are "
                     "constants of the call ENTRY: 0 bytes here.\n  n_records {} counts them all: {} ARGs in the "
                     "ENTRY, three with bytes in the frame.\n",
                     k_file.view(), line_warn, load<std::uint16_t>(w, 6), load<std::uint16_t>(w, 6));

        // (d) read back: typed, then from the dictionary alone
        std::uint64_t t1 = 0, t2 = 0, t3 = 0;
        int workers = 0, depth = 0;
        std::string sym;
        corpus::Order back{};
        double px = 0;
        const bool r1 = read_log<sr::level::info, line_info, "started with {} workers">(frames[0], t1, workers);
        const bool r2 = read_log<sr::level::warn, line_warn, "book {} depth {}">(frames[1], t2, sym, depth);
        const bool r3 = read_log<sr::level::error, line_error, "rejected {} px={:.2f}">(frames[2], t3, back, px);
        read = (r1 && workers == 4) + (r2 && sym == "AAPL" && depth == 7) +
               (r3 && back.id == 42 && std::strncmp(back.sym, "AAPL", 8) == 0 && px == 105.0);
        std::println("typed (deserialize_frame<log_tag, …>, the compiled signature): {} of 3 identical; t1 < t2 < t3: {}",
                     read, t1 <= t2 && t2 <= t3);
        std::println("rendered from the dictionary record alone (what srdump --level does):");
        for (const bytes fr : frames)
            std::println("  {}", render(fr));
    }
    std::println("\nlog frames: {} written, {} read back, trace argument evaluated {} times", written, read,
                 trace_evaluated);
    return written == 3 && read == 3 && trace_evaluated == 0 ? 0 : 1;
}
} // namespace

int main()
{
    const char* const path = "app.log.ser";
    sr::capture_file file(path); // header + dictionary written now
    if (file.is_open())
    {
        std::println("app.log.ser written by capture_file and read back:");
        return run(file, [&] {
            static_cast<void>(file.close());
            std::vector<std::byte> f = slurp(path);
            std::remove(path); // leave nothing behind (srdump --level warn app.log.ser works on it too)
            return f;
        }, true);
    }
    // no writable directory in this sandbox: the stream form, same frames, dictionary as a frame
    std::vector<std::byte> stream;
    sr::vector_sink out{stream};
    if (!sr::write_dictionary_frame(out))
        return 1;
    std::println("(no writable file here) in-memory stream: dictionary frame + log frames:");
    return run(out, [&] { return stream; }, false);
}

// ---- the same frames through srdump (libserreg's dictionary-driven reader; not on Compiler Explorer) -----------
// Built locally with the `std::remove(path)` line taken out, GCC 16.2, 2026-10-19; build/gcc16-debug/srdump:
//
// $ srdump --level warn app.log.ser
// 2026-10-19 21:51:49.045708007 WARN  main.cpp:174 book AAPL depth 7
// 2026-10-19 21:51:49.045708568 ERROR main.cpp:176 rejected corpus::Order{id: 42, px: 1050000, qty: 100, side: corpus::Side::buy (1), sym: "AAPL\0\0\0\0"} px=105.00
//
// $ srdump --level trace app.log.ser          (--level info gives the same three lines; the trace line never existed)
// 2026-10-19 21:51:49.045680857 INFO  main.cpp:172 started with 4 workers
// 2026-10-19 21:51:49.045708007 WARN  main.cpp:174 book AAPL depth 7
// 2026-10-19 21:51:49.045708568 ERROR main.cpp:176 rejected corpus::Order{id: 42, px: 1050000, qty: 100, side: corpus::Side::buy (1), sym: "AAPL\0\0\0\0"} px=105.00
//
// $ srdump --dict-only app.log.ser | grep ^log  — the three call ENTRYs: level, file, line, mask as constants
// log 0x65010e4ba0d51eeb  (const u8=0x0000000000000002, lit, const u32=0x00000000000000ac, const u8=0x0000000000000000, u64, lit, i32)
// log 0x0f13a2ecc4050d59  (const u8=0x0000000000000003, lit, const u32=0x00000000000000ae, const u8=0x0000000000000000, u64, lit, string, i32)
// log 0x6a5a2def39423ce2  (const u8=0x0000000000000004, lit, const u32=0x00000000000000b0, const u8=0x0000000000000000, u64, lit, corpus::Order, f64)
//
// $ srdump --json --level warn app.log.ser | head -1
// {"frame":2,"log":{"level":"warn","time":1792446709045708007,"file":"main.cpp","line":174,"message":"book AAPL depth 7"}}
//
// $ srdump --verify app.log.ser
// ok: 3 frames, 23 records, 136 bytes
