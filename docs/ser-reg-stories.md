# ser-reg — Stories: use cases and their code (`es::serreg`)

**Library:** 2.2.6 · **Wire format:** 1.7 · **Date:** 2026-10-07 · **Companion of:** `docs/ser-reg-requirements.md`
(the FR-IDs cited below), `docs/DECISIONS.md` (the R/P/A/C/G/T-IDs), `docs/ser-reg-architecture.md`, `book/`.

This document describes **how the library is used today**, one story per situation: who has the problem, what the
problem is, the complete program that solves it, what the command line shows afterwards, and which decisions and
requirements the solution rests on. It is written from the current state only; history is `CHANGELOG.md`.

Every program below was compiled and run on 2026-10-07 with GCC 16.2 (`-std=c++26 -freflection`) against the
`gcc16-debug` build of 2.2.1; the output blocks are pasted from those runs, trimmed where marked `…`. The one
exception is marked in its own section (§5: the *schema conflict* needs a hand-built dictionary entry, so that part
is run from the test suite's binaries rather than from the story's program). §13 is a preview of 2.3 and contains no
code beyond what compiles today.

**The build line used for every story** (an installed package would use `find_package(serreg 2.2 CONFIG REQUIRED)`
and `target_link_libraries(app PRIVATE es::serreg es::serreg_tools)`, which carry the flags below):

```sh
g++-16 -std=c++26 -freflection -O1 -Wall -Wextra -Wshadow -Werror -I<ser-reg>/include story.cpp -o story \
       -L<build> -lserreg_tools -lserreg -Wl,-rpath,<build>          # -lpthread / -ldl where a story says so
export LD_LIBRARY_PATH=/opt/gcc-16/lib64                             # when g++-16 is not the system compiler
srdump=<build>/srdump
```

`-lserreg_tools` is needed only by stories that include `to_string.hpp` or call `tools::` functions (§1, §2, §3,
§4, §5, §6, §7, §9, §11 do; a program that only writes frames links `-lserreg` alone, A-4).

---

## 1. The first message

**Who.** A developer who has a struct, wants it in a file, and wants to be sure the file can be read back — by the
program, and by someone with no access to the program.

**The problem.** Serialisation libraries ask for schemas, macros, code generation or a registration call per type.
Any of these can drift from the struct it describes.

**The solution.** `serialize(sink, "tag"_, value)` is the whole API. The struct is described by C++26 reflection at
compile time; the description (an ENTRY blob) is placed in the binary's `es_serreg_dict` section and registered in
the process registry **before `main()`**, as a side effect of instantiating `serialize` (R-5, FR-R1). A capture
file starts with that dictionary, so `srdump` decodes it knowing nothing about `shop::Order`.

```cpp
// s01_first.cpp — one struct, one frame, one capture file, read back typed.
#include <es/serreg/serreg.hpp>

#include <print>

namespace shop
{
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Order
{
    std::uint64_t id;
    std::int64_t px; // price in 1/10000
    std::uint32_t qty;
    Side side;
    char sym[8];
};
} // namespace shop

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

class first
{
public:
    int run() const
    {
        // Everything serialize() below names is already registered: shop::Order, shop::Side, the literal
        // "new order" and the call signature (literal, Order). Registration ran before main().
        std::println("registry: {} entries, program hash {:#x}", registry::instance().size(),
                     registry::instance().program_hash());
        std::println("schema hash of shop::Order {:#x}, layout hash {:#x}", schema_hash_v<shop::Order>,
                     layout_hash_v<shop::Order>);

        capture_file out("first.ser"); // FILE header + dictionary + CHAIN + writer-info frame
        if (!out.is_open())
            return 1;
        const shop::Order o{42, 1'050'000, 100, shop::Side::sell, "AAPL"};
        const auto n = serialize(out, "new order"_, o); // one compact frame
        if (!n)
            return 1;
        std::println("frame of {} bytes (16-byte header + {}-byte Order; the literal costs 0 bytes)", *n,
                     sizeof(shop::Order));
        if (!out.close())
            return 1;

        // Read it back, typed: the call hash and the lengths are validated before anything is copied.
        const auto bytes = tools::read_file("first.ser");
        if (!bytes)
            return 1;
        int seen = 0;
        const auto walk = tools::for_each_frame(*bytes, [&](const dyn::dictionary&, const tools::frame_ref& f) {
            shop::Order back{};
            if (const auto r = deserialize(f.bytes, "new order"_, back); !r)
            {
                std::println("frame {}: {} (expected {} items, decoded {}, failed at {})", f.index,
                             to_string(r.error().code), r.error().expected, r.error().actual,
                             r.error().failed);
                return true;
            }
            ++seen;
            std::println("read back: id={} px={} qty={} side={} sym={}", back.id, back.px, back.qty,
                         back.side == shop::Side::sell ? "sell" : "buy", std::string_view(back.sym));
            // the wrong signature is refused, not misread
            shop::Order wrong{};
            const auto bad = deserialize(f.bytes, "cancel"_, wrong);
            std::println("as \"cancel\": {}", to_string(bad.error().code));
            return true;
        });
        return walk && seen == 1 ? 0 : 1;
    }
};
} // namespace app

int main()
{
    return app::first{}.run();
}
```

**What to expect.**

```
$ ./s01_first
registry: 6 entries, program hash 0x28ea563706d4de96
schema hash of shop::Order 0xedd0bcf95fbf101d, layout hash 0x661bb3e388c01fe
frame of 48 bytes (16-byte header + 32-byte Order; the literal costs 0 bytes)
read back: id=42 px=1050000 qty=100 side=sell sym=AAPL
as "cancel": hash_mismatch

$ srdump first.ser
dictionary  program_hash 0x28ea563706d4de96  (2 types, 2 literals, 2 calls)
enum shop::Side : u8  schema 0x5c7468d4207da4a2  layout 0xfdccdf0b5609277b  size 1  align 1
  buy = 1
  sell = 2
struct shop::Order  schema 0xedd0bcf95fbf101d  layout 0x0661bb3e388c01fe  size 32  align 8  flat
  id: u64 @0 [8,8]
  px: i64 @8 [8,8]
  qty: u32 @16 [4,4]
  side: enum<shop::Side> @20 [1,1]
  sym: array<char,8> @21 [8,1]
  /* tail: 3 bytes padding */
  /* size 32: payload 29 (90.6%), padding 3 (9.4%) = holes 0/0 B, tail 3 B */
literal 0x21b6f8be4e3aeb6d "new order"
literal 0xf63c0e6b24e8aa21 "cancel"
call 0x29b999ac2a24b6a6  (lit, shop::Order)
call 0xf77edd036a1ce4cd  (lit, shop::Order)

frame 1  call 0x29b999ac2a24b6a6  (2 records)
  [0] "new order"
  [1] shop::Order {
        id: 42
        px: 1050000
        qty: 100
        side: shop::Side::sell (2)
        sym: "AAPL\0\0\0\0"
      }

$ srdump first.ser --hex | tail -2
        hex 0: 2a 00 00 00 00 00 00 00 90 05 10 00 00 00 00 00
        hex 16: 64 00 00 00 02 41 41 50 4c 00 00 00 00 00 00 00
$ srdump first.ser --verify
ok: 1 frames, 2 records, 48 bytes
```

Three things to notice. The registry has six entries although the program serialises one call: the reader's
`deserialize(f.bytes, "cancel"_, …)` is also an instantiation, so the literal `"cancel"` and the second call
signature are registered too — **readers register what they read** (FR-R1, A-1 of the `register_types`
note). The frame is 48 bytes: a 16-byte header (`u32 frame_len | u16 flags | u16 n_records | u64 call_hash`) and
the 32 object bytes; the literal is in the call hash and costs nothing (R-6, FR-L6); padding is scrubbed, the
`--hex` line shows the three zero bytes after `sym` (R-4, FR-S6). And `frame 1`: frame 0 is the writer-info frame
of format 1.7, consumed by the readers and counted in the numbering (P-13, FR-W14).

**Two hashes per type.** The *schema* hash covers names and metadata (`struct<shop::Order>[32,8]{id:u64@0[8,8];…}`);
the *layout* hash covers the bytes only. The call hash of `("new order", Order)` is derived from the literal id
and the schema hash, so reading with the wrong literal is a `hash_mismatch`, never a misread (P-1, FR-H1, FR-H2,
FR-S4). A `decode_error` says how far the reader got: items expected, decoded, and the first that failed (R-8).

---

## 2. Market-data capture for replay

**Who.** A team recording a feed handler's output for replay in a simulator; several message kinds, millions of
frames, and the replayer is a different program built months later.

**The problem.** Replay needs each frame routed to the right handler with no per-frame string compares, and the
file must remain verifiable without the recorder's source.

**The solution.** One literal per message kind, feed ids as compile-time constants (`7_`, `k<Side::ask>`: part of
the call signature, zero bytes on the wire, A-6, FR-A5), and a `frame_router` on the replay side: the handler's
parameter list spells the signature, the frame's call hash picks the handler with one hash-table lookup (A-12's
sibling, `router.hpp`).

```cpp
// s02_mdcap.cpp — a market-data capture: several message types, literals and constants, replayed with a
// frame_router.   s02_mdcap write md.ser | s02_mdcap replay md.ser
#include <es/serreg/serreg.hpp>

#include <print>
#include <string_view>

namespace md
{
enum class Side : std::uint8_t
{
    bid,
    ask
};
struct Quote
{
    std::uint64_t ts;
    char sym[8];
    std::int64_t px; // 1/10000
    std::uint32_t size;
    Side side;
};
struct Trade
{
    std::uint64_t ts;
    char sym[8];
    std::int64_t px;
    std::uint32_t size;
};
struct Status
{
    std::uint64_t ts;
    char sym[8];
    std::uint8_t halted;
};
} // namespace md

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

constexpr lit<"quote"> k_quote;
constexpr lit<"trade"> k_trade;

class writer
{
public:
    static int run(const std::string_view path)
    {
        capture_file out(path, capture_options{.program = "s02_mdcap"});
        if (!out.is_open())
            return 1;
        std::uint64_t ts = 1'700'000'000'000'000'000ull;
        for (int i = 0; i < 5; ++i, ts += 1000)
        {
            const std::int64_t px = 2'281'500 + i * 100;
            // a literal tag, the message, and a feed id as a compile-time constant: 16-byte header + 32 bytes
            if (!serialize(out, k_quote, md::Quote{ts, "AAPL", px, 100u * (i + 1), md::Side::bid}, 7_))
                return 1;
            if (!serialize(out, k_quote, md::Quote{ts + 1, "AAPL", px + 100, 200, md::Side::ask}, 7_))
                return 1;
            if (i % 2 == 1 && !serialize(out, k_trade, md::Trade{ts + 2, "AAPL", px + 50, 50}, 7_))
                return 1;
        }
        // a status change, with the venue as a constant enum and a reason literal
        if (!serialize(out, "status"_, md::Status{ts, "AAPL", 1}, k<md::Side::ask>, "LULD pause"_))
            return 1;
        // and a human-readable line that the reader formats
        if (!println(out, "session {} done after {} quotes, feed {}"_, "AAPL", 10, 7_))
            return 1;
        if (!out.close())
            return 1;
        std::println("wrote {} ({} data frames, {} bytes)", path, out.data_frames(), out.bytes_written());
        return 0;
    }
};

class replay
{
public:
    static int run(const std::string_view path)
    {
        frame_router r;
        std::uint64_t quotes = 0, trades = 0, volume = 0;
        std::int64_t last = 0;
        // a handler's parameters spell the signature; the constants are named exactly as the writer named them
        r.on<lit<"quote">, md::Quote, constant<7>>([&](const md::Quote& q) {
             ++quotes;
             last = q.px;
         })
            .on<lit<"trade">, md::Trade, constant<7>>([&](const md::Trade& t) {
                ++trades;
                volume += t.size;
            })
            .on<lit<"status">, md::Status, constant<md::Side::ask>, lit<"LULD pause">>(
                [&](const md::Status& s) {
                    std::println("status: {} halted={} (venue ask side, reason LULD pause)",
                                 std::string_view(s.sym, 4), s.halted);
                })
            .otherwise([](const dyn::dictionary& d, const tools::frame_ref& f) {
                if (const auto text = tools::render_frame(f.bytes, d); text)
                    std::print("log: {}", *text); // the println frame, formatted here
            });
        const auto w = tools::for_each_frame(path, r.callback());
        if (!w)
            return 1;
        std::println("{} quotes (last px {}), {} trades (volume {}); {} frames handled, {} unhandled, {} failed",
                     quotes, last, trades, volume, r.handled(), r.unhandled(), r.failed());
        return w->problems.empty() ? 0 : 1;
    }
};
} // namespace app

int main(const int argc, char** const argv)
{
    if (argc != 3)
        return 2;
    const std::string_view cmd = argv[1];
    return cmd == "write" ? app::writer::run(argv[2]) : app::replay::run(argv[2]);
}
```

**What to expect.**

```
$ ./s02_mdcap write md.ser
wrote md.ser (14 data frames, 2200 bytes)
$ ./s02_mdcap replay md.ser
status: AAPL halted=1 (venue ask side, reason LULD pause)
log: session AAPL done after 10 quotes, feed 7
10 quotes (last px 2282000), 2 trades (volume 100); 13 frames handled, 1 unhandled, 0 failed

$ srdump md.ser --verify
ok: 14 frames, 44 records, 648 bytes
$ srdump md.ser --dict-only | tail -4
call 0x3cfb562ad7cf4358  (lit, md::Status, const md::Side=0x0000000000000001, lit)
call 0xbaa9dd65926129eb  (lit, md::Quote, const i32=0x0000000000000007)
println 0xe087f6f0dfbbc303  (lit, string, i32, const i32=0x0000000000000007)
call 0xf3b0fd368d1c1a61  (lit, md::Trade, const i32=0x0000000000000007)
$ srdump md.ser --frames 3..4            # the dictionary, then only frames 3 and 4
…
frame 3  call 0xbaa9dd65926129eb  (3 records)
  [0] "quote"
  [1] md::Quote {
        ts: 1700000000000001000
        sym: "AAPL\0\0\0\0"
        px: 2281600
        size: 200
        side: md::Side::bid (0)
      }
  [2] i32 = 7
frame 4  call 0xbaa9dd65926129eb  (3 records)
…
$ srdump md.ser --type md::Trade         # only the Trade records (the dictionary is still printed first)
…
frame 5  call 0xf3b0fd368d1c1a61  (3 records)
  [1] md::Trade {
        ts: 1700000000000001002
…
```

The 2200 bytes the writer reports include the header, the dictionary (about 1.4 KB) and the writer-info frame;
`--verify` counts data frames only: 10 quotes and 2 trades of 48 bytes, the status frame (40) and the println
frame (32). The constant `7` shows as `[2] i32 = 7` although no byte of it is in the frame: its value is stored in
the call ENTRY (`const i32=0x…07`) and every reader shows it as the scalar it stands for.

**A trap worth knowing.** A named literal (`constexpr lit<"quote"> k_quote;`) is fine as a *value* in `serialize`,
but `decltype(k_quote)` is `const lit<"quote">`, and a `const`-qualified literal in `on<Sig...>` is not recognised
as a literal: spell the signature with `lit<"quote">` as above. `srdump --cxx --cxx-router` writes the signatures
correctly (§8).

**Rests on.** FR-S1 (one frame per call), FR-L1/FR-L6 (literal spellings, zero bytes), FR-A5 (constants), FR-P1/FR-P4
(print frames rendered by the reader), FR-D1/FR-D2 (`srdump` views), R-12 (`for_each_frame` reads a file of any
size in 10 MiB windows). Decisions A-6 (constants), A-7 (`decode_limits`: a frame above 64 MiB is a problem, never
a crash), P-13 (meta frames consumed).

---

## 3. Binary logging

**Who.** A latency-sensitive service that wants `log.info("book {} depth {}", sym, depth)` to cost a few stores,
and an operations team that wants `grep`-able text afterwards.

**The problem.** Text logging formats on the hot path. Binary logging usually loses the file:line, the level and
the thread, or needs a separate schema for every log site.

**The solution.** A log frame **is a print frame** whose signature starts with an envelope of zero-byte constants
— level, source file, line, an options mask — then the timestamp, then by policy a thread id and a sequence number,
then the format literal and the arguments (P-15, FR-G3). The level gate is one relaxed load (FR-G1); a compile-time
floor removes calls entirely (FR-G2); the policies are an NTTP, so every choice is `if constexpr` (A-8, FR-G4). The
log file is a `scoped_capture_file<Scope>`: its dictionary holds only the calls made into sinks of that scope
(A-9, P-16, FR-G5, FR-R15), not the service's thousand archive types.

```cpp
// s03_log.cpp — binary logging: a scoped capture file, a logger, the gate, policies, and reading it back.
//   s03_log write app.log.ser | s03_log read app.log.ser
#include <es/serreg/serreg.hpp>

#include <format>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

struct Pos
{
    std::int32_t id;
    double qty;
};

// The dictionary scope: a tag type. Only calls serialised into sinks of this scope land in the file's dictionary.
struct log_scope
{
};
using log_file = scoped_capture_file<log_scope>;

// Two policies, two loggers over the same file: the plain one (timestamp only), the traced one (thread id and a
// per-sink sequence number after the timestamp).
constexpr log_options plain{};
constexpr log_options traced{.thread = thread_policy::per_message, .sequence = sequence_policy::per_sink};

class writer
{
public:
    static int run(const std::string_view path)
    {
        log_file f(path, capture_options{.program = "s03_log", .info_thread = true, .log_clock = "system"});
        if (!f.is_open())
            return 1;
        logger<plain, log_file> lg{f, level::info}; // run-time level: info and above
        logger<traced, log_file> tr{f};

        int evaluated = 0;
        const auto expensive = [&] { return ++evaluated; };
        ES_SERREG_LOG(lg, debug, "never built: {}"_, expensive()); // below the level: nothing evaluated
        ES_SERREG_LOG(lg, info, "started with {} workers"_, 4);
        ES_SERREG_LOG(lg, warn, "book {} depth {}"_, "AAPL", 7);
        lg.error("pos {}"_, Pos{17, 2.5});             // no call site: file "-", line 0
        if (lg.enabled(level::debug))                  // the gate by hand
            lg.debug("not reached {}"_, expensive());
        std::thread t([&] { ES_SERREG_LOG(tr, info, "worker {} up"_, 1); });
        t.join();
        ES_SERREG_LOG(tr, error, "px={:.2f} order={}"_, 1.25, 42);
        std::println("expensive() evaluated {} times; {} frames failed", evaluated, lg.failed() + tr.failed());

        return f.close() ? 0 : 1;
    }
};

class reader
{
public:
    static int run(const std::string_view path)
    {
        frame_router r;
        r.on_log([](const std::uint8_t lvl, const std::string_view line) {
            std::println("[{}] {}", to_string(static_cast<level>(lvl)), line);
        });
        const auto w = tools::for_each_frame(path, r.callback());
        if (!w)
            return 1;
        std::println("{} log frames; dictionary: {} types, {} calls", r.handled(), w->dict.types.size(),
                     w->dict.calls.size());
        // what each line cost on the wire
        std::string sizes;
        static_cast<void>(tools::for_each_frame(path, [&](const dyn::dictionary&, const tools::frame_ref& f) {
            sizes += std::format("{} ", f.bytes.size());
            return true;
        }));
        std::println("frame sizes: {}", sizes);
        return 0;
    }
};
} // namespace app

int main(const int argc, char** const argv)
{
    if (argc != 3)
        return 2;
    return std::string_view(argv[1]) == "write" ? app::writer::run(argv[2]) : app::reader::run(argv[2]);
}
```

Build with `-lpthread`. (The file is a `capture_file`, not thread-safe: the thread in this story only shows the
thread id in the envelope — the file was idle while it ran. A sink shared by threads is an `es::ipc` ring, §9.)

**What to expect.**

```
$ ./s03_log write app.log.ser
expensive() evaluated 0 times; 0 frames failed
$ ./s03_log read app.log.ser
[info] 2026-10-08 00:50:38.107862287 INFO  s03_log.cpp:47 started with 4 workers
[warn] 2026-10-08 00:50:38.107863962 WARN  s03_log.cpp:48 book AAPL depth 7
[error] 2026-10-08 00:50:38.107864568 ERROR pos app::Pos{id: 17, qty: 2.5}
[info] 2026-10-08 00:50:38.107993778 INFO  s03_log.cpp:52 [1687] #1 worker 1 up
[error] 2026-10-08 00:50:38.108081424 ERROR s03_log.cpp:54 [1686] #2 px=1.25 order=42
5 log frames; dictionary: 1 types, 7 calls
frame sizes: 32 40 40 48 56

$ srdump app.log.ser --level warn
2026-10-08 00:50:38.107863962 WARN  s03_log.cpp:48 book AAPL depth 7
2026-10-08 00:50:38.107864568 ERROR pos app::Pos{id: 17, qty: 2.5}
2026-10-08 00:50:38.108081424 ERROR s03_log.cpp:54 [1686] #2 px=1.25 order=42
$ srdump app.log.ser --level info --since 2026-10-08T00:50:38.1078 --until 2026-10-08T00:50:38.108
…the four lines in that window…
$ srdump app.log.ser --level error --json
{"frame":3,"log":{"level":"error","time":1791420638107864568,"file":"-","line":0,"message":"pos app::Pos{id: 17, qty: 2.5}"}}
{"frame":5,"log":{"level":"error","time":1791420638108081424,"file":"s03_log.cpp","line":54,"thread":1686,"sequence":2,"message":"px=1.25 order=42"}}
$ srdump app.log.ser --dict-only | grep -c '^log '
7
$ srdump app.log.ser --dict-only | grep 'px='
log 0x943942ded609d102  (const u8=0x0000000000000004, lit, const u32=0x0000000000000036, const u8=0x0000000000000003, u64, u32, u64, lit, f64, i32)
$ srdump app.log.ser --info | grep -E 'dictionary|thread|clock'
dictionary  1 types, 9 literals, 7 calls; 1 block(s) at 48
writer      thread = 1686
writer      clock = system
writer      dictionary = scoped:app::log_scope
```

**What a log line costs on the wire (FR-G8).** Records are 8-aligned inside a frame, so with the 16-byte header:
`started with {} workers`, one `int` = 16 + 8 (timestamp) + 4 → **32 bytes**; `book {} depth {}` with a runtime
string and an int = 16 + 8 + (4 + 4) + 4 → **40**; `pos {}` with a 16-byte struct → **40**; the traced policy adds a
`u32` thread id and a `u64` sequence: `worker {} up` → **48**, `px={:.2f} order={}` → **56**. Level, file, line
and mask are constants of the call and cost nothing per message; the text of the format string is in the
dictionary once. The dictionary says what each call is (`log 0x… (const u8=4, lit, const u32=0x36, const u8=3,
u64, u32, u64, lit, f64, i32)`: level error, the file literal, line 54, mask 3 = thread + sequence, the clock,
then the message's literal and arguments); `log_layout` in `log.hpp` gives readers the fixed positions.

Seven calls in the dictionary for five frames: `"never built: {}"` and `"not reached {}"` were never written, but
their call sites were compiled, so their signatures are registered (the compile-time floor `log_options::floor`
or `-DES_SERREG_LOG_MIN_LEVEL=…` is what removes a site entirely, FR-G2).

**Rests on.** R-11 (binary logging is a primary use), A-8, A-9, A-12 (`on_log`: log frames no typed handler took,
rendered), P-15, P-16, FR-G1…FR-G8, FR-T3 (`--level`, `--since`, `--until`, JSON log objects), FR-W13 (the
writer-info frame carries `thread` for `thread_policy::once` and `clock` for readers).

---

## 4. A crash-safe multi-TB archive

**Who.** The owner of a capture that runs for a trading week, several terabytes, on a machine that may lose power;
and the analyst who must read what was written up to the last second — and seek into the middle of it.

**The problem.** A file whose header is rewritten at close is unreadable after a crash; a file without
checksums hides disk corruption; a file without index points cannot be entered in the middle; and a plugin
loaded on day three adds types the opening dictionary does not know.

**The solution.** Format 1.7 (P-9…P-12, all opt-in through `capture_options`, A-11): the dictionary is a forward
chain of blocks and **nothing is ever rewritten** — a registry change writes a *delta* dictionary frame inline
before the first frame that needs it, and the previous block's `next` pointer is patched once with an 8-byte
`pwrite` (FR-W12); a **CRC32C** trailer per frame (FR-W10); **checkpoint frames** by size and/or time, in their own
chain, with a final one at `close()` (FR-W11, P-17 replaced the spec's footer with them); a writer-info frame
records how the file was written (FR-W13). The readers consume these meta frames (P-13) and keep bounded memory
whatever the file size (R-12).

```cpp
// s04_archive.cpp — a crash-safe archive (format 1.7): CRC per frame, checkpoints, a plugin loaded mid-file.
//   s04_archive write archive.ser ./plugin_tick.so
#include <es/serreg/serreg.hpp>

#include <dlfcn.h>

#include <chrono>
#include <print>
#include <string_view>

namespace arch
{
struct Quote
{
    std::uint64_t ts;
    char sym[8];
    std::int64_t px;
    std::uint32_t size;
};
} // namespace arch

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;
using namespace std::chrono_literals;

class archive_writer
{
public:
    static int run(const std::string_view path, const char* const plugin)
    {
        const capture_options opts{
            .crc = true,                   // a CRC32C trailer on every data frame (8 bytes)
            .checkpoint_bytes = 4096,      // a checkpoint frame every 4 KiB of frames ...
            .checkpoint_every = 1s,        // ... or every second, whichever comes first
            .sync = sync_policy::checkpoint, // fdatasync at every checkpoint and dictionary block
            .program = "s04_archive",
        };
        capture_file out(path, opts);
        if (!out.is_open())
            return 1;
        std::uint64_t ts = 1'700'000'000'000'000'000ull;
        const auto burst = [&](const int n) {
            for (int i = 0; i < n; ++i, ts += 500)
                if (!serialize(out, "quote"_, arch::Quote{ts, "MSFT", 4'120'000 + i, 100}))
                    return false;
            return true;
        };
        if (!burst(200))
            return 1;
        std::println("registry version before dlopen: {}", registry::instance().version());
        // A plugin arrives with a type this program never heard of. Its registration moves the registry version;
        // the next frame written to the file is preceded by a dictionary *delta* holding only the new entries.
        void* const h = ::dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
        if (!h)
        {
            std::println("dlopen: {}", ::dlerror());
            return 1;
        }
        std::println("registry version after dlopen:  {}", registry::instance().version());
        const auto write = reinterpret_cast<bool (*)(capture_file&)>(::dlsym(h, "plugin_write"));
        if (!write || !write(out)) // the plugin's frame: delta first, then the frame
            return 1;
        if (!burst(200))
            return 1;
        if (!out.close()) // a final checkpoint, fflush, fdatasync — nothing is rewritten
            return 1;
        std::println("wrote {}: {} data frames, {} bytes", path, out.data_frames(), out.bytes_written());
        return 0;
    }
};
} // namespace app

int main(const int argc, char** const argv)
{
    if (argc != 4)
        return 2;
    return app::archive_writer::run(argv[2], argv[3]);
}
```

The plugin is the one of §5 (`plugin_tick.so`, built with `-shared -fPIC -fno-gnu-unique`); the host is built with
`-ldl`. In production the archive writer would use a larger `checkpoint_bytes` (a checkpoint is 80 bytes; one per
64 MiB or per second is typical) and would leave `sync` at its default `dictionary`.

**What to expect.**

```
$ ./s04_archive write archive.ser ./plugin_tick.so
registry version before dlopen: 3
registry version after dlopen:  6
wrote archive.ser: 401 data frames, 23800 bytes

$ srdump archive.ser --info                       # header, chains and writer info: seeks only, no frame walk
format 1.7  program_hash 0xd48d3f187b593fda  size 23800  flags crc checkpoints info
dictionary  2 types, 2 literals, 2 calls; 2 block(s) at 48 11920
checkpoints 6
  @4704  frames 74  bytes 4144  dictionary @48  wall 1791420747072340763
  @8928  frames 148  bytes 8288  dictionary @48  wall 1791420747072721464
  @13456  frames 222  bytes 12424  dictionary @11920  wall 1791420747073647685
  @17680  frames 296  bytes 16568  dictionary @11920  wall 1791420747073920186
  @21904  frames 370  bytes 20712  dictionary @11920  wall 1791420747074177567
  @23720  frames 401  bytes 22448  dictionary @11920  wall 1791420747074386243
writer      library = 2.2.1
writer      format = 1.7
writer      program = s04_archive
writer      host = vm
writer      pid = 1753
writer      start_wall_ns = 1791420747070888563
writer      crc = 1
writer      checkpoint_bytes = 4096
writer      checkpoint_ns = 1000000000
writer      dictionary = full
writer      sync = checkpoint
$ srdump archive.ser --verify
ok: 401 frames, 802 records, 22448 bytes
$ srdump archive.ser --type feed::Tick | tail -6
frame 204  call 0x3240e03378a59332  (2 records)
  [1] feed::Tick {
        ts: 1700000000000
        px: 101.25
        size: 300
      }
```

The second dictionary block at offset 11920 is the delta: one struct, one literal, one call — the plugin's. Each
checkpoint names the dictionary block in force at that point (`dictionary @48`, later `@11920`), so a reader that
seeks to a checkpoint knows which blocks to load before decoding from there.

**Crash: truncate the file in the middle of a frame.**

```
$ head -c 15000 archive.ser > cut.ser
$ srdump cut.ser --verify
problem: frame 253 @14992: 8 stray bytes after last frame
FAILED (1 problems): 248 frames, 496 records, 13880 bytes
$ srdump cut.ser --info | head -3
format 1.7  program_hash 0xd48d3f187b593fda  size 15000  flags crc checkpoints info
dictionary  2 types, 2 literals, 2 calls; 2 block(s) at 48 11920
checkpoints 3
```

Everything up to the cut reads — including the plugin's frame and the delta, because the chain was written
forward, not at close. The one problem is the 8-byte stub of the frame that was being written.

**Corruption: one payload byte flipped.**

```
$ srdump bad.ser --verify             # byte 6153 of the file XOR 0x20
problem: frame 100 @6128: crc mismatch (frame skipped)
FAILED (1 problems): 400 frames, 800 records, 22392 bytes
```

A frame whose CRC fails is reported and skipped: never decoded, never delivered to a handler; the typed readers
return `error::crc_mismatch` for it (P-10). Without `.crc = true` the flipped byte would have decoded as a
plausible price.

**Corruption: a smashed `frame_len`, and `--resync`.**

```
$ srdump smashed.ser --verify                       # frame_len of data frame 100 set to 0x7ffffff8
problem: frame 101 @6184: bad frame_len 2147483640
FAILED (1 problems): 99 frames, 198 records, 5544 bytes
$ srdump smashed.ser --verify --resync
problem: frame 101 @6184: bad frame_len 2147483640: skipped 2744 bytes to the next checkpoint
FAILED (1 problems): 352 frames, 704 records, 19704 bytes
```

Without a checkpoint a bad length ends the walk (the next frame cannot be found); with `--resync` the reader scans
8 bytes at a time for the next checkpoint sentinel and continues from there (FR-T2). The skipped range stays a
problem: the file is not pretended whole.

**Rests on.** R-2 (never crash on wrong data), R-12, P-9…P-14, P-17, A-11, FR-W10…FR-W14, FR-T1 (`--info`,
`tools::info`), FR-T2 (`--resync`), FR-R11/FR-R12 (the plugin's entries). `sync_policy` and the clock hook are
§14 defaults of the requirements.

---

## 5. Plugins and reload

**Who.** A host process that loads strategy or feed plugins with `dlopen`, unloads and reloads them while it runs,
and must keep decoding what an unloaded plugin wrote.

**The problem.** A type that lives in a shared object is gone when the object is unmapped. A registry that points
into the plugin's `.rodata` would dangle; one that refuses `dlclose` prevents reloads; and two plugins may disagree
about one type.

**The solution.** Each registration is a heap entry (main-program entries are *viewed* in place, C-11, FR-R14;
plugin entries are *copied* at load). `dlclose` never aborts: the image is unmapped, its entries are **retained**
with `refs == 0`, old descriptors stay valid, and a reload revives the same entries instead of adding new ones
(A-3, FR-R10, FR-R11). Retained entries stay in exported dictionaries so old captures remain readable (A-14,
FR-R16). A type with the same schema hash but a different layout in two loaded images is a **conflict**: the first
definition is kept, one line goes to stderr, `conflicts()` lists it, and `ES_SERREG_CONFLICT=warn|abort|silent`
sets the policy (G-16, FR-R4).

The plugin (built twice: `plugin_tick.so`, and `plugin_tick_v2.so` with `-DV2`):

```cpp
// plugin_tick.cpp — a plugin whose type the host never saw. Build: -shared -fPIC (and -DV2 for the rebuilt one).
#include <es/serreg/serreg.hpp>

namespace feed
{
struct Tick
{
    std::uint64_t ts;
    double px;
    std::uint32_t size;
#ifdef V2
    std::uint32_t venue; // the field the next build of the plugin adds
#endif
};
} // namespace feed

extern "C" [[gnu::visibility("default")]] bool plugin_write(es::serreg::capture_file& out)
{
    using namespace es::serreg::literals;
    return es::serreg::serialize(out, "tick"_, feed::Tick{1'700'000'000'000ull, 101.25, 300
#ifdef V2
                                                       ,
                                                       7
#endif
                                                       })
        .has_value();
}
extern "C" [[gnu::visibility("default")]] bool plugin_write_buf(std::vector<std::byte>& buf)
{
    using namespace es::serreg::literals;
    es::serreg::vector_sink s{buf};
    return es::serreg::serialize(s, "tick"_, feed::Tick{1'700'000'000'000ull, 101.25, 300
#ifdef V2
                                                     ,
                                                     7
#endif
                                                     })
        .has_value();
}
```

```sh
g++-16 -std=c++26 -freflection -O1 -Wall -Wextra -Wshadow -Werror -fno-gnu-unique -fPIC -shared \
       -I<ser-reg>/include plugin_tick.cpp -o plugin_tick.so -L<build> -lserreg -Wl,-rpath,<build>
g++-16 … -DV2 plugin_tick.cpp -o plugin_tick_v2.so …
```

`-fno-gnu-unique` matters: an `STB_GNU_UNIQUE` symbol makes glibc mark the DSO `NODELETE`, and the plugin would
never really unload (the exported CMake target `es::serreg` carries the flag). The host:

```cpp
// s05_plugins.cpp — types that arrive with a shared object: dlopen, dlclose, reload, a rebuilt plugin.
//   s05_plugins ./plugin_tick.so ./plugin_tick_v2.so
#include <es/serreg/serreg.hpp>

#include <dlfcn.h>

#include <print>
#include <vector>

namespace app
{
using namespace es::serreg;

class plugin_host
{
public:
    static int run(const char* const v1, const char* const v2)
    {
        registry& reg = registry::instance();
        reg.on_change([](const registry&) { std::println("  (registry changed)"); });
        status("start");
        std::vector<std::byte> frame;
        {
            void* const h = ::dlopen(v1, RTLD_NOW | RTLD_LOCAL);
            if (!h)
                return 1;
            status("after dlopen(v1)"); // + feed::Tick, "tick", the call
            const auto write = reinterpret_cast<bool (*)(std::vector<std::byte>&)>(::dlsym(h, "plugin_write_buf"));
            if (!write || !write(frame))
                return 1;
            render(frame, "while loaded");
            ::dlclose(h); // never aborts; the image is unmapped, its entries are retained
            status("after dlclose(v1)");
            render(frame, "after dlclose"); // the frame still decodes: the dictionary kept the entries
        }
        {
            void* const h = ::dlopen(v1, RTLD_NOW | RTLD_LOCAL); // a reload revives the retained entries
            status("after reload(v1)");
            ::dlclose(h);
        }
        {
            void* const h = ::dlopen(v2, RTLD_NOW | RTLD_LOCAL); // the rebuilt plugin: a new Tick, a new call
            status("after dlopen(v2)");
            if (const auto* t = reg.find_type_by_name("feed::Tick"))
                std::println("  find_type_by_name(feed::Tick) -> schema {:#x}, size {} (the loaded one wins)",
                             t->schema_hash, t->size);
            ::dlclose(h);
        }
        std::println("conflicts: {}", reg.conflicts().size());
        return 0;
    }

private:
    static void status(const char* const when)
    {
        const registry& reg = registry::instance();
        std::println("{:<20} size {:>2}  retained {}  images {}  version {}", when, reg.size(), reg.retained(),
                     reg.images().size(), reg.version());
    }
    static void render(const std::vector<std::byte>& frame, const char* const when)
    {
        // a dictionary-driven dump of the plugin's frame, with the process's own dictionary
        std::string text;
        if (tools::dump(frame, &tools::process_dictionary(), tools::dump_options{}, text))
            std::print("  {}: {}", when, text.substr(text.find("frame")));
        else
            std::println("  {}: cannot dump", when);
    }
};
} // namespace app

int main(const int argc, char** const argv)
{
    return argc == 3 ? app::plugin_host::run(argv[1], argv[2]) : 2;
}
```

**What to expect** (build with `-ldl`).

```
$ ./s05_plugins ./plugin_tick.so ./plugin_tick_v2.so
start                size  0  retained 0  images 0  version 0
  (registry changed)
after dlopen(v1)     size  3  retained 0  images 1  version 3
  while loaded: frame 0  call 0x3240e03378a59332  (2 records)
  [0] "tick"
  [1] feed::Tick {
        ts: 1700000000000
        px: 101.25
        size: 300
      }
  (registry changed)
after dlclose(v1)    size  3  retained 3  images 0  version 6
  after dlclose: frame 0  call 0x3240e03378a59332  (2 records)
  [0] "tick"
  [1] feed::Tick {
        …the same…
      }
  (registry changed)
after reload(v1)     size  3  retained 0  images 1  version 9
  (registry changed)
after dlopen(v2)     size  5  retained 2  images 1  version 15
  find_type_by_name(feed::Tick) -> schema 0x5f73350a0465a291, size 24 (the loaded one wins)
  (registry changed)
conflicts: 0

$ srdump --elf plugin_tick.so                   # the plugin's own embedded dictionary
dictionary  program_hash 0x0000000000000000  (1 types, 1 literals, 1 calls)
struct feed::Tick  schema 0x30283aa959ed1ce9  layout 0x32d5af65aef384a2  size 24  align 8  flat
…
$ srdump --diff plugin_tick.so plugin_tick_v2.so
breaking        feed::Tick  0x30283aa959ed1ce9 -> 0x5f73350a0465a291  breaking: layout differs: frames of either side cannot be read by the other
call removed    call 0x3240e03378a59332  0x3240e03378a59332 -> -  breaking: removed: a reader built from the new side cannot read old frames of this call typed
call added      call 0x68fdc323cc1c45ed  - -> 0x68fdc323cc1c45ed
1 types: 0 identical, 0 names-only, 0 metadata-only, 1 breaking, 0 added, 0 removed, 0 duplicate-name; …  BREAKING (direction old-data: 1 changed, 1 removed, 0 added)
```

The host program has no types of its own (`size 0` at start: nothing in it instantiates `serialize`). The rebuilt
plugin's `Tick` is a **different type** (new schema hash), not a conflict: the registry holds both, `feed::Tick` by
name resolves to the loaded one, and the old frames still decode through the retained entry. `size 5, retained 2`
after `dlopen(v2)`: v2 shares the literal `"tick"` with the retained v1 (an alias: `refs` goes back to 1), so the
retained set is v1's `Tick` and its call.

**The conflict case (run from the test suite, not from the program above).** Reflection cannot produce two
layouts for one schema hash from source — the schema string is a superset of the layout string — so the test
plugin `tests/dlopen/plugin_conf_b.cpp` hand-builds an ENTRY whose layout column is patched. With the built test
binaries:

```
$ tests/dlopen/dlopen_host ./serreg_plugin_conf_a.so conflict ./serreg_plugin_conf_b.so
stderr during dlopen(b): es::serreg: schema conflict: 'conf::Msg' (0x0faf44adb39aaea4) is defined with a different layout in ./serreg_plugin_conf_a.so and ./serreg_plugin_conf_b.so
ok: conflicting second image warned, recorded once, first definition kept; it took over after the first unloaded
$ ES_SERREG_CONFLICT=abort tests/dlopen/dlopen_host ./serreg_plugin_conf_a.so conflict ./serreg_plugin_conf_b.so
Aborted (exit 134)
$ srdump --elf serreg_plugin_conf_a.so --elf serreg_plugin_conf_b.so | tail -2
conflict 0x0faf44adb39aaea4  kept conf::Msg (layout 0x01f2815a1ce3a685, size 16)  dropped conf::Msg (layout 0x01f2815a1ce3a684, size 16)
srdump: schema conflict: conf::Msg 0x0faf44adb39aaea4
```

**Rests on.** A-3, A-14, C-11, G-16, FR-R2 (registration at image load), FR-R3 (one registry per process — build
the library shared when plugins serialise), FR-R4, FR-R6 (one published index per registration burst, `on_change`
once per `dlopen`/`dlclose`), FR-R10…FR-R12, FR-R16, FR-D3 (`--diff`), FR-D6 (`--elf` on a `.so`). The runtime hooks
`ES_SERREG_DUMP_ON=change` and `ES_SERREG_DUMP_SIGNAL` (FR-R9) export the merged dictionary of a long-running host
without code.

---

## 6. Debugging with `to_string`, `to_json` and `formattable<T>`

**Who.** Anyone who wants to print a value in a test, a log line or a debugger session without writing an
`operator<<` for every struct.

**The problem.** A `std::formatter` per type is boilerplate that rots; a generic one would hijack `std::format` for
types that already have theirs.

**The solution.** `to_string(v)` / `to_json(v)` / `render(v, format)` work for any value the library can serialise:
they encode the value as the compact record a frame would carry and render it through the **process's own
dictionary** (`tools::process_dictionary()`, the registry image parsed once per registry version) — exactly what
`srdump` and `print` would show (A-10, FR-A7…FR-A11). Instantiating them registers the type, like a
`serialize` call. `formattable<T>` is the opt-in to `std::format` / `std::print`; `{:j}` selects JSON.

```cpp
// s06_debug.cpp — printing any serialisable value: to_string, to_json, formattable<T>, process_dictionary().
#include <es/serreg/serreg.hpp>
#include <es/serreg/to_string.hpp> // links es::serreg_tools

#include <map>
#include <print>
#include <string>
#include <vector>

namespace risk
{
enum class Kind : std::uint8_t
{
    limit = 1,
    market = 2
};
struct Leg
{
    std::int32_t qty;
    double px;
};
struct Order
{
    std::uint64_t id;
    Kind kind;
    std::string account;
    std::vector<Leg> legs;
    std::map<std::string, std::int32_t> tags;
};
} // namespace risk

// opt in: std::format / std::print know risk::Order through to_string; "{:j}" is the JSON form
template<> struct es::serreg::formattable<risk::Order> : std::true_type
{
};

namespace app
{
using namespace es::serreg;

class debug_demo
{
public:
    static int run()
    {
        const risk::Order o{7, risk::Kind::limit, "ACC-1", {{100, 228.15}, {-50, 228.20}}, {{"desk", 3}, {"algo", 1}}};
        // No serialize() call names risk::Order in this program: instantiating to_string<risk::Order> is what
        // registered it — before main(), like a serialize call would. It is in the ELF dictionary too.
        std::println("{}", to_string(o));
        std::println("{}", to_json(o));
        std::println("{}", o);   // formattable<risk::Order>
        std::println("{:j}", o); // the JSON spec
        std::println("{} / {} / {}", to_string(risk::Kind::market), to_json(risk::Kind::market),
                     to_json(std::uint64_t{1} << 60)); // enums by name; big integers as strings
        // the dictionary these strings were rendered from: the registry's image, parsed once per version
        const dyn::dictionary& d = tools::process_dictionary();
        std::println("process dictionary: {} types, {} calls; registry version {}", d.types.size(),
                     d.calls.size(), registry::instance().version());
        if (const auto* t = d.find_type(schema_hash_v<risk::Order>))
            std::println("risk::Order: {} fields, {} bytes fixed part, flat={}", t->fields.size(), t->size,
                         t->is_flat);
        return 0;
    }
};
} // namespace app

int main()
{
    return app::debug_demo::run();
}
```

**What to expect.**

```
$ ./s06_debug
risk::Order{id: 7, kind: risk::Kind::limit (1), account: "ACC-1", legs: [risk::Leg{qty: 100, px: 228.15}, risk::Leg{qty: -50, px: 228.2}], tags: {"algo": 1, "desk": 3}}
{"id":"7","kind":{"name":"limit","value":1},"account":"ACC-1","legs":[{"qty":100,"px":228.15},{"qty":-50,"px":228.2}],"tags":[{"key":"algo","value":1},{"key":"desk","value":3}]}
risk::Order{id: 7, kind: risk::Kind::limit (1), account: "ACC-1", legs: [...], tags: {"algo": 1, "desk": 3}}
{"id":"7","kind":{"name":"limit","value":1},"account":"ACC-1","legs":[...],"tags":[...]}
market / {"name":"market","value":2} / "1152921504606846976"
process dictionary: 3 types, 0 calls; registry version 3
risk::Order: 5 fields, 40 bytes fixed part, flat=false

$ srdump --elf s06_debug | grep -A6 'struct risk::Order'
struct risk::Order  schema 0x88ad155f041afbc1  layout 0x808b1b599e0c244d  size 40  align 8  dynamic
  id: u64 @0 [8,8]
  kind: enum<risk::Kind> @8 [1,1]
  account: string @12 [8,4]
  legs: vector<struct<risk::Leg>> @20 [8,4]
  tags: map<string,i32> @28 [8,4]
```

The JSON is `srdump --json`'s value: 64-bit integers are strings (JSON numbers are doubles in most parsers), enums
are `{"name","value"}`, maps are `[{"key","value"}]` (FR-A8, FR-D8). The `std::map` was entered as
`{"desk",3},{"algo",1}` and renders sorted by key — the canonical image writes maps in key order (P-7). The binary's
dictionary has three types and **no call**: nothing serialised them. This is a debugging convenience, not a hot
path (an allocation for the record and the string, a shared lock, a dictionary-driven walk, FR-A11); on the hot
path, serialise the value and let a reader render it.

**Print frames and `--raw`.** The hot-path form of "print a value" is `println(out, "fmt {}"_, v)`: the
format string's id and the arguments' records go out, nothing is formatted, and a reader runs `std::format` with
the dictionary's text (FR-P1, FR-P4). `srdump` shows such a frame as the rendered line; `--raw` shows the records it
was rendered from, `--hex` their bytes (§2's capture, frame 14):

```
$ srdump md.ser --frames 14..14 | sed -n '/^frame/,$p'
frame 14  println 0xe087f6f0dfbbc303  (4 records)
  print: session AAPL done after 10 quotes, feed 7
$ srdump md.ser --frames 14..14 --raw --hex | sed -n '/^frame/,$p'
frame 14  println 0xe087f6f0dfbbc303  (4 records)
  [0] "session {} done after {} quotes, feed {}"
  [1] string = "AAPL"
        hex 0: 41 41 50 4c
  [2] i32 = 10
        hex 0: 0a 00 00 00
  [3] i32 = 7
        hex 0: 07 00 00 00
  print: session AAPL done after 10 quotes, feed 7
$ srdump md.ser --json --frames 14..14 | tail -1
{"frame":14,"message":"session AAPL done after 10 quotes, feed 7\n"}
```

The format literal and the constant `7` occupy no bytes (the frame is 32 bytes: header, `u32 len` + `"AAPL"`, the
`i32`); the format string was checked against the argument types at compile time by `std::format_string`
(FR-P2), so a reader can never meet a placeholder its arguments do not fit. A struct argument renders as its
`to_string` form, an enum as its enumerator name, a `std::formatter<T>` of the writer is never consulted — the
reader need not have `T`.

**Rests on.** A-10, A-4 (the renderer lives in `es::serreg_tools`, so `-lserreg_tools`), FR-A7…FR-A11, FR-P1, FR-P2,
FR-P4, FR-P5 (`--raw`), FR-D1, R-5.

---

## 7. Dynamic members: strings, vectors, optionals, variants, maps, sets

**Who.** A developer whose messages are not flat: symbols as `std::string`, legs as `std::vector`, an optional
note, a `variant` of order types, a price map, a venue set.

**The problem.** Pointers cannot cross a process boundary, and "identical values give identical bytes" must hold
for containers whose in-memory layout is allocation-dependent (an `unordered_map` in particular).

**The solution.** A class with dynamic members is written as a **canonical image** (P-5): a fixed part with every
member at a canonical offset, each `string` / `vector` / `optional` / `variant` / `map` / `set` member an 8-byte
`{u32 off, u32 n}` slot, and a tail the slots point into. Maps and sets are wire *shapes* (`map<K,V>`, `set<K>`):
`std::map`, `std::unordered_map` and `std::flat_map` of the same `K, V` are one shape, entries written in key order
(P-7, FR-W9). Any of these containers is also accepted as an argument of its own — the record of a *transparent*
type (FR-A6). The same `serialize` and `deserialize`; no macros.

```cpp
// s07_dynamic.cpp — strings, vectors, optionals, variants, maps and sets as members and as arguments.
#include <es/serreg/serreg.hpp>

#include <map>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dyn_demo
{
struct Msg
{
    std::uint32_t id;
    std::string name;
    std::vector<std::int16_t> xs;
};
struct Limit
{
    double px;
    bool operator==(const Limit&) const = default;
};
struct Market
{
    std::uint16_t slippage_bp;
    bool operator==(const Market&) const = default;
};
struct Order
{
    std::uint64_t id;
    std::variant<Limit, Market> type;
    std::optional<std::string> note;
    std::unordered_map<std::string, double> px; // written sorted by key: identical values, identical bytes
    std::set<std::uint16_t> venues;
    bool operator==(const Order&) const = default;
};
} // namespace dyn_demo

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

class dynamic_demo
{
public:
    static int run()
    {
        capture_file out("dynamic.ser", capture_options{.info_host = false});
        if (!out.is_open())
            return 1;
        // the README's canonical image: 28 bytes for Msg{7, "ab", {1, -2, 3}}
        const auto n1 = serialize(out, dyn_demo::Msg{7, "ab", {1, -2, 3}});
        const dyn_demo::Order o{42, dyn_demo::Market{15}, std::string("resting"), {{"bid", 228.1}, {"ask", 228.2}},
                                {4, 1, 9}};
        const auto n2 = serialize(out, "order"_, o);
        // containers as arguments of their own: the record of a transparent type
        const std::vector<std::int32_t> ids{3, 1, 2};
        const std::map<std::string, std::int32_t> counts{{"b", 2}, {"a", 1}};
        const auto n3 = serialize(out, "ids"_, ids, counts);
        if (!n1 || !n2 || !n3 || !out.close())
            return 1;
        std::println("frames: Msg {} bytes, Order {} bytes, (vector, map) {} bytes", *n1, *n2, *n3);

        // read back typed: vectors resized, optionals engaged, variants emplaced, maps and sets rebuilt
        int ok = 0;
        static_cast<void>(tools::for_each_frame("dynamic.ser", [&](const dyn::dictionary&, const tools::frame_ref& f) {
            dyn_demo::Order back;
            if (deserialize(f.bytes, "order"_, back))
                ok += back == o;
            std::vector<std::int32_t> v;
            std::map<std::string, std::int32_t> m;
            if (deserialize(f.bytes, "ids"_, v, m))
                ok += v == ids && m == counts;
            return true;
        }));
        std::println("typed reads that matched: {}", ok);
        return ok == 2 ? 0 : 1;
    }
};
} // namespace app

int main()
{
    return app::dynamic_demo::run();
}
```

**What to expect.**

```
$ ./s07_dynamic
frames: Msg 56 bytes, Order 136 bytes, (vector, map) 96 bytes
typed reads that matched: 2

$ srdump dynamic.ser --hex
…
struct dyn_demo::Order  schema 0x5ccba34ead66ef21  layout 0x4e7311083e36613f  size 40  align 8  dynamic
  id: u64 @0 [8,8]
  type: variant<struct<dyn_demo::Limit>|struct<dyn_demo::Market>> @8 [8,4]
  note: optional<string> @16 [8,4]
  px: map<string,f64> @24 [8,4]
  venues: set<u16> @32 [8,4]
struct dyn_demo::Msg  schema 0x8f846ba731715a3c  layout 0x7835cb0574f155da  size 20  align 4  dynamic
  id: u32 @0 [4,4]
  name: string @4 [8,4]
  xs: vector<i16> @12 [8,4]
struct std::map<std::string,i32>  schema 0x9ba49a614d8d0569  layout 0x208d9fccdfc63567  size 8  align 4  dynamic  transparent
  : map<string,i32> @0 [8,4]
…
frame 1  call 0x5409e7ec365fbaea  (1 records)
  [0] dyn_demo::Msg {
        id: 7
        name: "ab"
        xs: [1, -2, 3]
      }
        hex 0: 07 00 00 00 14 00 00 00 02 00 00 00 16 00 00 00
        hex 16: 03 00 00 00 61 62 01 00 fe ff 03 00
frame 2  call 0x8dd0f1a20d7570d1  (2 records)
  [0] "order"
  [1] dyn_demo::Order {
        id: 42
        type: variant<1:dyn_demo::Market> {
          value: dyn_demo::Market {
            slippage_bp: 15
          }
        }
        note: "resting"
        px: {"ask": 228.2, "bid": 228.1}
        venues: {1, 4, 9}
      }
        hex 0: 2a 00 00 00 00 00 00 00 28 00 00 00 02 00 00 00
        hex 16: 2c 00 00 00 01 00 00 00 40 00 00 00 02 00 00 00
        hex 32: 66 00 00 00 03 00 00 00 0f 00 00 00 34 00 00 00
        hex 48: 07 00 00 00 72 65 73 74 69 6e 67 00 00 00 00 00
        hex 64: 60 00 00 00 03 00 00 00 66 66 66 66 66 86 6c 40
        hex 80: 63 00 00 00 03 00 00 00 33 33 33 33 33 83 6c 40
        hex 96: 61 73 6b 62 69 64 01 00 04 00 09 00
frame 3  call 0x579139c01877b52c  (3 records)
  [0] "ids"
  [1] [3, 1, 2]
        hex 0: 08 00 00 00 03 00 00 00 03 00 00 00 01 00 00 00
        hex 16: 02 00 00 00
  [2] {"a": 1, "b": 2}
        hex 0: 08 00 00 00 02 00 00 00 20 00 00 00 01 00 00 00
        hex 16: 01 00 00 00 21 00 00 00 01 00 00 00 02 00 00 00
        hex 32: 61 62
```

Reading the `Msg` image: `07 00 00 00` is `id`; `14 00 00 00 02 00 00 00` the `name` slot (offset 20, 2 bytes);
`16 00 00 00 03 00 00 00` the `xs` slot (offset 22, 3 elements); then the tail `61 62` and `01 00 fe ff 03 00`. 28
bytes, always these bytes for this value — the README's promise. A frame carrying a dynamic record prefixes it
with `u32 len | u32 0` (P-5), so the `Msg` frame is 16 + 8 + 28 = 52 → 56 bytes with the 8-byte alignment. In the
`Order` image the variant slot is `{off 0x28, index 2}` (index + 1: 0 is "valueless"), the `unordered_map` came out
as `{"ask", "bid"}` in key order, and the set's `u16` elements end the tail. The containers passed as arguments are
each one slot plus their tail — `std::vector<int32_t>{3,1,2}` is 20 bytes: a slot `{8, 3}` and the three ints.

**Rests on.** P-5, P-7, FR-S4 (typed decode validates every slot against the image bounds before writing to the
outputs), FR-S7 (`std::multimap`, `std::list`, `std::deque`, pointers and `string_view` members are rejected by
name at compile time), FR-W8, FR-W9, FR-A6, FR-D5 (a hostile image cannot make the decoder read outside the frame;
`decode_limits` bounds depth and work, A-7).

---

## 8. Schema tools: `--elf`, `--diff`, `--cxx`, `--wireshark`

**Who.** A release engineer who must say whether build N+1 can still read build N's captures; a support engineer
with a binary and a capture but no source; a network engineer who lives in Wireshark.

**The problem.** The schema is in the code, and the code is in several places. The question "what changed?" needs
an answer that does not depend on reading diffs of C++.

**The solution.** Every binary carries its dictionary in the `es_serreg_dict` section (R-5, FR-D6). `srdump --elf`
prints it, `--rri-out` saves it, `--diff a b` compares two dictionaries with a verdict per type and per call and a
`--direction` that says which reader the verdict is for (A-5, FR-D3), `--cxx` regenerates self-checking C++ from any
dictionary (T-7, FR-D10), `--wireshark` writes a Lua dissector for it (T-5).

For this story, `s08_mdcap_v2` is §2's program rebuilt after two edits: `md::Quote` gains
`[[=es::serreg::since(2)]] std::uint8_t flags;` and `md::Status::halted` is renamed `is_halted`.

```
$ srdump --elf ./s02_mdcap --rri-out md-v1.rri        # the shipped build's dictionary, kept with the release
$ srdump --diff md-v1.rri ./s08_mdcap_v2               # --direction old-data (the default): can the new build read old files?
breaking        md::Quote  0xae7fd351d0d22a6f -> 0x4edafecca774eb95  breaking: layout differs: frames of either side cannot be read by the other
identical       md::Side  0x685160c3f1658c58 -> 0x685160c3f1658c58
names-only      md::Status  0x917af3246f33b5ff -> 0x8873fb336b5f7f9b
identical       md::Trade  0x2660f28a3e647f1f -> 0x2660f28a3e647f1f
call removed    call 0x3cfb562ad7cf4358  0x3cfb562ad7cf4358 -> -  breaking: removed: a reader built from the new side cannot read old frames of this call typed
call removed    call 0xbaa9dd65926129eb  0xbaa9dd65926129eb -> -  breaking: removed: a reader built from the new side cannot read old frames of this call typed
call added      call 0x86b0bec2ead6278e  - -> 0x86b0bec2ead6278e
call added      call 0xc5634ed1a0a9cd7e  - -> 0xc5634ed1a0a9cd7e
4 types: 2 identical, 1 names-only, 0 metadata-only, 1 breaking, 0 added, 0 removed, 0 duplicate-name; 4 calls: 2 added, 2 removed, 0 changed; 0 literals: 0 added, 0 removed, 0 changed  BREAKING (direction old-data: 1 changed, 2 removed, 0 added)
$ echo $?
1
$ srdump --diff md-v1.rri ./s08_mdcap_v2 --direction new-data | tail -1   # can the OLD build read NEW files?
4 types: … BREAKING (direction new-data: 1 changed, 0 removed, 2 added)
$ srdump --diff md-v1.rri ./s02_mdcap | tail -1                           # the same build against itself
4 types: 4 identical, 0 names-only, … 0 calls: 0 added, 0 removed, 0 changed; …  (direction old-data)
```

The vocabulary is `identical | names-only | metadata-only | breaking | added | removed | duplicate-name` for
types, and `added | removed | changed` for calls and literals. `md::Status` is *names-only*: same bytes, a
different schema hash — today that still changes the call hash, so the typed read of old frames fails
(`hash_mismatch`); 2.3's projection by name is what will make a names-only change readable (§13). The exit code is
the verdict, so this is a CI step.

**Regenerating the types.** With only the binary (or an `.rri`, or a capture) in hand:

```
$ srdump --cxx md_types.hpp --cxx-router --elf ./s02_mdcap
srdump: wrote md_types.hpp: 4 types, 4 call signatures, 5 literals
$ sed -n '/^struct Quote/,/^};/p;/install_handlers/,/^}/p' md_types.hpp
struct Quote
{
    std::uint64_t ts;
    std::array<char, 8> sym;
    std::int64_t px;
    std::uint32_t size;
    md::Side side;
};
inline void install_handlers(es::serreg::frame_router& r)
{
    using namespace es::serreg::literals;
    r.on<es::serreg::lit<"status">, md::Status, es::serreg::constant<md::Side::ask>, es::serreg::lit<"LULD pause">>(
        []([[maybe_unused]] const md::Status& a1) { /* TODO */ }); // call 0x3cfb562ad7cf4358
    r.on<es::serreg::lit<"quote">, md::Quote, es::serreg::constant<7>>(
        []([[maybe_unused]] const md::Quote& a1) { /* TODO */ }); // call 0xbaa9dd65926129eb
    r.on<es::serreg::println_tag, es::serreg::lit<"session {} done after {} quotes, feed {}">, std::string_view, std::int32_t, es::serreg::constant<7>>(
        []([[maybe_unused]] std::string_view a1, [[maybe_unused]] std::int32_t a2) { /* TODO */ }); // call 0xe087f6f0dfbbc303
    r.on<es::serreg::lit<"trade">, md::Trade, es::serreg::constant<7>>(
        []([[maybe_unused]] const md::Trade& a1) { /* TODO */ }); // call 0xf3b0fd368d1c1a61
}
```

The header ends in `static_assert`s for every size, offset, canonical name and hash, so compiling it proves the
regenerated types are the writer's. A reader built from it alone:

```cpp
// s08_regen.cpp — a reader built from the generated header only (no access to the writer's source).
#include "md_types.hpp"

#include <print>

namespace app
{
class regen_reader
{
public:
    static int run(const char* const path)
    {
        es::serreg::frame_router r;
        install_handlers(r); // the generated skeleton: one handler per call signature, printing each
        const auto w = es::serreg::tools::for_each_frame(path, r.callback());
        if (!w)
            return 1;
        std::println("{} handled, {} unhandled, {} failed", r.handled(), r.unhandled(), r.failed());
        return 0;
    }
};
} // namespace app

int main(const int argc, char** const argv)
{
    return argc == 2 ? app::regen_reader::run(argv[1]) : 2;
}
```

```
$ ./s08_regen md.ser
14 handled, 0 unhandled, 0 failed
```

**Wireshark.** The dissector is generated from the capture's dictionary (or from `--elf`/`--rri`, or from a
program's own registry through `cli::app`); Wireshark does not run Lua as root, hence `runuser` here:

```
$ srdump --wireshark serreg_md.lua md.ser
srdump: wrote serreg_md.lua: 4 types, 4 call signatures, 5 literals
$ runuser -u nobody -- tshark -X lua_script:$PWD/serreg_md.lua -r $PWD/md.ser | head -4
    1   →   SERREG 1416 file header, format 1.7; dictionary: 13 entries known, 0 unknown
    2   →   SERREG 136 writer info
    3   →   SERREG 48 "quote", md::Quote, 7
    4   →   SERREG 48 "quote", md::Quote, 7
$ runuser -u nobody -- tshark -X lua_script:$PWD/serreg_md.lua -r $PWD/md.ser \
      -Y 'serreg.md_Trade.size >= 50' -T fields -e frame.number -e serreg.md_Trade.px
7	2281650
12	2281850
```

Filter fields are `serreg.<Type with :: as _>.<member>`; print and log frames expose `serreg.message`
(`docs/wireshark.md`).

**On the wire.** The same dissector reads three kinds of input: a `.ser` capture or a pcap of link type USER0
(frames as packets, dictionary frames included), **UDP** datagrams and **TCP** streams. UDP and TCP are recognised
automatically: a datagram, or the first segment of a stream, whose first 16 bytes are a frame header with a call
hash in the dissector's tables (or one of the 1.7 meta sentinels) is SERREG from then on; frames split across TCP
segments are reassembled. A false positive would take a random match of 64 bits; the heuristics are
`serreg_udp_heur` / `serreg_tcp_heur` in Analyze → Enabled Protocols, and fixed ports go in the SERREG preferences
(`-o serreg.udp_port:N`, `-o serreg.tcp_port:N`). A stream that carries no dictionary frame is decoded from the
dissector's own tables alone (generate it with `--elf ./producer`), and entries the capture knows but the dissector
does not are expert infos, not errors.

**Log frames** (§3's capture, a dissector generated from it) render with the `LEVEL file:line [tid] #seq` prefix in
the Info column, and every piece of the envelope is a field:

```
$ srdump --wireshark serreg_log.lua app.log.ser
srdump: wrote serreg_log.lua: 1 types, 7 call signatures, 9 literals
$ runuser -u nobody -- tshark -X lua_script:$PWD/serreg_log.lua -r $PWD/app.log.ser
    1   →   SERREG 1800 file header, format 1.7; dictionary: 17 entries known, 0 unknown
    2   →   SERREG 168 writer info
    3   →   SERREG 32 INFO s03_log.cpp:47 started with 4 workers
    4   →   SERREG 40 WARN s03_log.cpp:48 book AAPL depth 7
    5   →   SERREG 40 ERROR -:0 pos app::Pos{id: 17, qty: 2.5}
    6   →   SERREG 48 INFO s03_log.cpp:52 [1687] #1 worker 1 up
    7   →   SERREG 56 ERROR s03_log.cpp:54 [1686] #2 px=1.25 order=42
$ runuser -u nobody -- tshark -X lua_script:$PWD/serreg_log.lua -r $PWD/app.log.ser -V -Y 'frame.number==7' | sed -n '/es::serreg frame/,$p'
es::serreg frame
    Frame length: 56
    Flags: 0x0000
    Records: 10
    Call hash: 0x943942ded609d102
    Signature: log(const u8=0x0000000000000004, "s03_log.cpp", const u32=0x0000000000000036, const u8=0x0000000000000003, u64, u32, u64, "px={:.2f} order={}", f64, i32)
    Kind: compact, log
    [u8: 4]
    Literal: s03_log.cpp
    [u32: 54]
    [u8: 3]
    u64: 1791420638108081424
    u32: 1686
    u64: 2
    Literal: px={:.2f} order={}
    f64: 1.25
    i32: 42
    Message: ERROR s03_log.cpp:54 [1686] #2 px=1.25 order=42
$ runuser -u nobody -- tshark -X lua_script:$PWD/serreg_log.lua -r $PWD/app.log.ser -Y 'serreg.message contains "depth"' -T fields -e frame.number -e serreg.message
4	WARN s03_log.cpp:48 book AAPL depth 7
```

The bracketed values (`[u8: 4]`, `[u32: 54]`, `[u8: 3]`) are the zero-byte constants of the signature — level,
line, mask — shown where the readers put them; the file name and the format string are literals of the call. The
dissector is generated from the dictionary, so a new log site needs a regenerated `.lua` only when the capture
itself carries no dictionary (a UDP feed); `.ser` files and dictionary-bearing streams bring their own.

**Rests on.** R-5, A-5, T-5, T-7, FR-D3, FR-D6, FR-D7 (`--rri`), FR-D10, FR-H1/FR-H3 (the program hash in the header
is the order-independent hash of the dictionary, so `--program-hash` on a capture and on the binary agree).

---

## 9. Streams, IPC and the hot path

**Who.** Two threads or two processes on one machine: a producer that must not block on a disk, a consumer that
decodes in place; or a pipe between two programs. And the trading system's hot path, where the question is what one
`serialize` call costs and whether the consumer can read without copying.

**The problem.** A stream has no header to put the dictionary in, and a shared-memory ring must take variable-size
frames without a copy on either side.

**The solution.** On a stream the dictionary travels as **dictionary frames** (frame flag bit 1): a
`dictionary_tracker` writes the full image the first time and a delta whenever `registry::version()` moved — one
relaxed load otherwise (FR-W7). `es::ipc` (G-4; header-only C++23, no dependency on `es::serreg`) gives SPSC
queues and SPMC broadcast rings in anonymous or named shared memory; `es::ipc::sink` is a *claiming sink*, so
`serialize()` builds the frame directly in the ring record (FR-S2), and the consumer routes the record's span in
place.

```cpp
// s09_streams.cpp — frames on streams: (A) a pipe between two threads with dictionary frames in band,
// (B) an es::ipc SPSC ring between two threads, the consumer decoding in place with a frame_router.
#include <es/ipc/ipc.hpp>
#include <es/serreg/serreg.hpp>

#include <unistd.h>

#include <format>
#include <print>
#include <string>
#include <thread>

namespace feed
{
struct Fill
{
    std::uint64_t order;
    std::int32_t qty;
    double px;
};
struct Tick
{
    std::uint64_t seq;
    std::int64_t bid;
    std::int64_t ask;
};
} // namespace feed

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

frame_router& handlers(frame_router& r, std::uint64_t& fills, std::uint64_t& ticks, std::string& last_line)
{
    return r.on("fill"_, [&](const feed::Fill&) { ++fills; })
        .on([&](const feed::Tick&) { ++ticks; })
        .on_println("tick {} spread {}"_, [&](const std::uint64_t seq, const std::int64_t spread) {
            last_line = std::format("tick {} spread {}", seq, spread);
        });
}

class pipe_demo // A: a bare frame stream over a pipe; the dictionary travels as frames
{
public:
    static int run()
    {
        int fds[2];
        if (::pipe(fds) != 0)
            return 1;
        std::thread producer([wfd = fds[1]] {
            fd_sink out{wfd};
            dictionary_tracker dict; // the first update() writes the full dictionary frame; later ones a delta
            static_cast<void>(dict.update(out)); // (only when registry::version() moved: one relaxed load)
            for (std::uint64_t i = 0; i < 1000; ++i)
            {
                static_cast<void>(dict.update(out));
                if (i % 10 == 0)
                    static_cast<void>(serialize(out, "fill"_, feed::Fill{i, 100, 228.15}));
                else
                    static_cast<void>(serialize(out, feed::Tick{i, 2281500, 2281600}));
                if (i % 100 == 99)
                    static_cast<void>(println(out, "tick {} spread {}"_, i, std::int64_t{100}));
            }
            ::close(wfd);
        });
        std::uint64_t fills = 0, ticks = 0;
        std::string last;
        frame_router r;
        handlers(r, fills, ticks, last);
        // the consumer side reads the pipe in windows ("-" would be stdin); the dictionary frame it meets first
        // is what lets it verify and, if it wanted to, render frames it has no handler for
        const auto w = tools::for_each_frame(std::format("/proc/self/fd/{}", fds[0]), r.callback());
        producer.join();
        ::close(fds[0]);
        if (!w)
        {
            std::println("pipe: {}", to_string(w.error()));
            return 1;
        }
        std::println("pipe: {} fills, {} ticks, last line \"{}\"; dictionary from the stream: {} types, {} calls; "
                     "{} problems",
                     fills, ticks, last, w->dict.types.size(), w->dict.calls.size(), w->problems.size());
        return fills == 100 && ticks == 900 ? 0 : 1;
    }
};

class ring_demo // B: an es::ipc SPSC ring in anonymous shared memory; zero-copy both ways
{
public:
    static int run()
    {
        namespace ipc = es::ipc;
        const auto g = ipc::ring::for_capacity(64 * 1024, 64, 1);
        auto mem = ipc::region::anonymous(ipc::ring::bytes_for(g));
        if (!mem)
            return 1;
        auto q = ipc::ring::create(*mem, g, ipc::ring_kind::spsc);
        if (!q)
            return 1;
        constexpr std::uint64_t n = 100000;
        std::thread producer([&] {
            auto p = ipc::producer::attach(*q); // claim() spins while the ring is full
            ipc::sink out{*p};                  // a claiming sink: serialize() builds the frame in the ring record
            for (std::uint64_t i = 0; i < n; ++i)
                if (i % 10 == 0)
                    static_cast<void>(serialize(out, "fill"_, feed::Fill{i, 100, 228.15}));
                else
                    static_cast<void>(serialize(out, feed::Tick{i, 2281500, 2281600}));
            static_cast<void>(println(out, "tick {} spread {}"_, n, std::int64_t{0}));
        });
        auto c = ipc::consumer::join(*q);
        std::uint64_t fills = 0, ticks = 0;
        std::string last;
        frame_router r;
        handlers(r, fills, ticks, last);
        while (last.empty())
            if (!c->poll([&](const ipc::message& m) { static_cast<void>(r.route(m.data)); })) // in place
                std::this_thread::yield();
        producer.join();
        std::println("ring: {} fills, {} ticks, then \"{}\"; {} handled, {} failed", fills, ticks, last, r.handled(),
                     r.failed());
        return fills == n / 10 && ticks == n - n / 10 ? 0 : 1;
    }
};
} // namespace app

int main()
{
    const int a = app::pipe_demo::run();
    const int b = app::ring_demo::run();
    return a | b;
}
```

**What to expect** (build with `-lpthread`).

```
$ ./s09_streams
pipe: 100 fills, 900 ticks, last line "tick 999 spread 100"; dictionary from the stream: 2 types, 3 calls; 0 problems
ring: 10000 fills, 90000 ticks, then "tick 100000 spread 0"; 100001 handled, 0 failed
```

The same stream captured to a file is read by `srdump stream.bin` (it begins with a dictionary frame); a stream
without one needs `--dict app.rri` or `--elf ./producer` (FR-W3). For two *processes*, replace
`region::anonymous` with `region::create("orders.q", bytes)` on one side and `region::open("orders.q")` +
`ring::attach` on the other — `examples/ipc_ring.cpp` (fork) and `examples/ipc_log/` (two programs, the dictionary
sent as the first ring record, the consumer rendering messages it was not compiled with) are the full versions,
and `book/11-ipc-and-latency.md` covers the protocol, huge pages, `mlock` and the half-RTT benchmarks.

**Zero-copy on the consumer side: `view<T>` and `dispatch`.** `frame_router` decodes into storage it owns (a copy
per value). When the consumer wants a pointer into the ring record instead, it validates the frame once with
`inspect` (style 2) and takes a typed view (style 5), or lets `dispatch<Ts...>` (style 3) hand it a `const T&` that
points into the record whenever the bytes are aligned — a ring record's payload is 16-byte aligned (`es::ipc`
`DataAlign`) and a frame header is 16 bytes, so the first record of every frame is aligned for any flat type:

```cpp
// s09_view.cpp — the consumer's zero-copy readers on one ring record: inspect() + view<T>, and dispatch<Ts...>.
#include <es/ipc/ipc.hpp>
#include <es/serreg/serreg.hpp>

#include <cstring>
#include <print>

namespace feed
{
struct Tick
{
    std::uint64_t seq;
    std::int64_t bid;
    std::int64_t ask;
};
struct Fill
{
    std::uint64_t order;
    std::int32_t qty;
    double px;
};
} // namespace feed

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

template<class... F> struct overloaded : F...
{
    using F::operator()...;
};

class view_demo
{
public:
    static int run()
    {
        namespace ipc = es::ipc;
        const auto g = ipc::ring::for_capacity(4096, 64, 1);
        auto mem = ipc::region::anonymous(ipc::ring::bytes_for(g));
        auto q = ipc::ring::create(*mem, g, ipc::ring_kind::spsc);
        auto p = ipc::producer::attach(*q);
        ipc::sink out{*p};
        if (!serialize(out, feed::Tick{7, 2281500, 2281600}) || !serialize(out, "fill"_, feed::Fill{42, 100, 228.15}))
            return 1;

        auto c = ipc::consumer::join(*q);
        int n = 0;
        c->poll([&](const ipc::message& m) {
            ++n;
            // style 2 + 5: validate the frame once, then a typed pointer into the record — no copy. The payload
            // of a ring record is 16-byte aligned (es::ipc DataAlign) and a frame header is 16 bytes, so the
            // first record's bytes are aligned for any flat type.
            const auto fv = inspect(m.data);
            if (!fv)
                return;
            for (const record_view r : fv->records())
                if (const auto t = view<feed::Tick>(r))
                    std::println("view<Tick>: seq {} spread {} (pointer into the ring at offset {})", (*t)->seq,
                                 (*t)->ask - (*t)->bid,
                                 reinterpret_cast<const std::byte*>(*t) - m.data.data());
                else if (r.is_literal)
                    std::println("literal {:#x} ({}), zero bytes", r.hash,
                                 registry::instance().find_literal(r.hash));
                else
                    std::println("view<Tick> on a {}-byte record of another type: {}", r.payload.size(),
                                 to_string(t.error()));
            // style 3: compile-time match of each record against Ts..., zero-copy when aligned
            static_cast<void>(dispatch<feed::Tick, feed::Fill>(
                m.data, overloaded{[](const feed::Fill& f) {
                                       std::println("dispatch: Fill order {} qty {} px {}", f.order, f.qty, f.px);
                                   },
                                   [](const feed::Tick& t) { std::println("dispatch: Tick seq {}", t.seq); },
                                   [](const literal_view l) { std::println("dispatch: literal \"{}\"", l.text); }}));
        });
        // what happens when the bytes are not aligned: a copy of the frame at an odd address
        alignas(16) std::byte buf[128];
        span_sink s{buf};
        static_cast<void>(serialize(s, feed::Tick{1, 2, 3}));
        std::byte odd[129];
        std::memcpy(odd + 1, buf, s.used);
        const auto fv = inspect(std::span<const std::byte>(odd + 1, s.used));
        const auto t = view<feed::Tick>(*fv->begin());
        std::println("view<Tick> at an odd address: {} (dispatch would copy to the stack instead)",
                     t ? "ok" : to_string(t.error()));
        return n == 2 ? 0 : 1;
    }
};
} // namespace app

int main()
{
    return app::view_demo::run();
}
```

```
$ ./s09_view
view<Tick>: seq 7 spread 100 (pointer into the ring at offset 16)
dispatch: Tick seq 7
literal 0xe1c90c19f0ff66a4 (fill), zero bytes
view<Tick> on a 24-byte record of another type: hash_mismatch
dispatch: literal "fill"
dispatch: Fill order 42 qty 100 px 228.15
view<Tick> at an odd address: misaligned (dispatch would copy to the stack instead)
```

`view<T>` refuses rather than guesses: a wrong type is `hash_mismatch`, a wrong length `length_mismatch`, an
unaligned payload `misaligned`, a `bool` byte other than 0/1 `invalid_value` (a view cannot repair in place; the
copying readers do). `dispatch` matches records against up to four types with inlined compares and above that with
a compile-time perfect hash — never a walk over the registry.

**What a `serialize` call costs (NFR-1).** For a flat struct into a claiming sink the generated code is the 16-byte
header store, a `memcpy` of the object and zero to two stores for the padding gaps; no registry access, no atomic,
no allocation, and `serialize()` leaves no call at `-O2` (ctest `serialize_inlines_at_O2`, C-5). A literal or a
constant argument costs nothing (it is in the call hash); a run-time string one `size()` plus `u32 len` and the
bytes; a dynamic argument one measure pass and one encode pass. The gating ceilings of `tests/bench/bench.cpp` are
**< 50 ns** mean for a 32-byte struct into a `span_sink` and **< 500 ns** for `inspect` of a one-record frame
(release library; 4× under Debug). On the ring, `book/11-ipc-and-latency.md` §11.6 reports — for orientation only,
a sandbox port pinned to two P-cores with hyperthreading off, not a reference-box result — a half-RTT p50/p99 of
115/136 ns for raw 64-byte records and 141/175 ns for the same fields as a reflected struct through `serialize`
and a typed read: the frame costs some 25 ns over a hand-written copy in that loop. Numbers for the record come
only from `scripts/bench-hrtt-*.sh` on the reference box (T-9).

The writer-side knobs for the hot path are compile-time: a `capture_file` with
`capture_policy{.dictionary_updates = false}` drops the per-frame relaxed load of `registry::version()` when the
program never `dlopen`s (§11), a `scoped_capture_file` keeps the file's dictionary to the hot messages, and
`es::ipc::basic_ring<64, 64>` is the lowest-latency ring layout (§11.5 of the book).

**Rests on.** G-4, FR-S2 (claiming sinks), requirements §7.1 styles 2, 3 and 6 (`inspect`, `dispatch`, `view<T>`;
R-8: validation before any output), FR-W1 (stream container), FR-W7
(dictionary completeness on streams, deltas since 1.7), FR-W3, FR-D11 (the pipe is read in windows), NFR-1, C-5,
T-9, `docs/ipc/ipc-design.md` §4–§5 (record headers, gates, never-crash validation of ring records).

---

## 10. Reading from Python

**Who.** A quant or an operations engineer who has a capture and a notebook, not a C++ toolchain.

**The problem.** A binary format that only its own library can open is a liability for the people who read it
most.

**The solution.** `python/serreg` is a stand-alone reader — standard library only, Python ≥ 3.10 — that opens
FILE 1.6/1.7 captures, bare streams and `.rri` dictionaries, decodes flat records and the canonical image, and
renders print and log frames with a `std::format` subset; ctest `python_reader_parity` compares it semantically
with `srdump --json` on every fixture (T-6, FR-T4). It never raises on corrupt input (R-2).

```
$ export PYTHONPATH=<ser-reg>/python            # or pip install <ser-reg>/python → `serreg-dump`
$ python3 -m serreg md.ser --json | grep '"frame":1,'
{"frame":1,"record":0,"type":"literal","value":{"lit":"quote","id":"0x410e03af668cacc1"}}
{"frame":1,"record":1,"type":"md::Quote","value":{"ts":"1700000000000000000","sym":"AAPL\u0000\u0000\u0000\u0000","px":"2281500","size":100,"side":{"name":"bid","value":0}}}
{"frame":1,"record":2,"type":"i32","value":7}
$ python3 -m serreg md.ser --json | tail -1
{"frame":14,"message":"session AAPL done after 10 quotes, feed 7\n"}
$ python3 -m serreg archive.ser --verify          # §4's archive, CRC and checkpoints included
ok: 401 frames, 802 records, 22448 bytes
$ python3 -m serreg bad.ser --verify              # the flipped byte
problem: frame 100 @6128: crc mismatch (frame skipped)
FAILED (1 problems): 400 frames, 800 records, 22392 bytes
$ python3 -m serreg archive.ser --info | head -3
format 1.7  program_hash 0xd48d3f187b593fda  size 23800  flags crc checkpoints info
dictionary  2 types, 2 literals, 2 calls; 2 block(s) at 48 11920
checkpoints 6
$ python3 -m serreg app.log.ser --level warn      # §3's log
2026-10-08 00:50:38.107863962 WARN  s03_log.cpp:48 book AAPL depth 7
2026-10-08 00:50:38.107864568 ERROR pos app::Pos{id: 17, qty: 2.5}
2026-10-08 00:50:38.108081424 ERROR s03_log.cpp:54 [1686] #2 px=1.25 order=42
```

The counts, the problem texts and the rendered lines are `srdump`'s. As a library:

```python
# s10_read.py — the Reader API in a few lines.    python3 -I s10_read.py md.ser
import sys, serreg

with open(sys.argv[1], "rb") as fh:
    r = serreg.Reader(fh.read())                 # bytes, bytearray or an mmap
by_type = {}
for frame in r.frames():                         # data frames only; meta frames are consumed on the way
    for rec in frame.records:
        by_type[rec.type_label] = by_type.get(rec.type_label, 0) + 1
    if frame.call and frame.call.is_print:       # print / println / log frames: render the message
        print("message:", serreg.render_message(r.dictionary, frame.call, frame.records))
print(by_type)
print(r.frame_count, "frames,", r.record_count, "records,", r.byte_count, "bytes,", len(r.problems), "problems")
first = next(r.frames())
print(first.records[1].type_label, serreg.value_text(first.records[1].value))
print(serreg.value_json(first.records[1].value))
```

```
$ python3 -I s10_read.py md.ser
message: session AAPL done after 10 quotes, feed 7

{'literal': 15, 'md::Quote': 10, 'i32': 14, 'md::Trade': 2, 'md::Status': 1, 'md::Side': 1, 'string': 1}
14 frames, 44 records, 648 bytes, 0 problems
md::Quote md::Quote{ts: 1700000000000000000, sym: "AAPL\0\0\0\0", px: 2281500, size: 100, side: md::Side::bid (0)}
{'ts': '1700000000000000000', 'sym': 'AAPL\x00\x00\x00\x00', 'px': '2281500', 'size': 100, 'side': {'name': 'bid', 'value': 0}}
```

Decoded values are plain Python where they can be and small dataclasses where structure matters (`Struct`,
`Array`, `Enum`, `Variant`, `Map`, `Set`, `Lit`, `Transparent`); `value_json` gives `srdump`'s JSON shape.

**Where it stops.** It does not recompute hashes — there is no XXH3 in it — so it trusts the schema hashes, literal
ids and call hashes in the file and can only say that a hash is *unknown*, not that it is forged. No `--elf`
(ELF reading), no `--diff`, no `--wireshark`/`--cxx`, no `--resync` scan, no `--window` (the file is mapped whole;
pipes are read whole). `--json` is the interface to rely on; the text dump is a close relative of `srdump`'s, not a
copy (`python/README.md`).

**Rests on.** T-6, FR-T4, R-2, D-5 (the Python `format.py` is the draft of the 3.0 wire specification).

---

## 11. Registry at scale

**Who.** The owner of a service with hundreds of message types and thousands of log sites, who cares about
start-up time, the size of every capture file, and the per-frame cost.

**The problem.** Every type, literal and call signature is an entry; log sites are the largest source of them
(R-11). Entries are pushed before `main()`, published as one index, and exported into the opening dictionary of
every capture file.

**The solution.** The budgets are decided (G-5, FR-R13): 10 000 entries / 10 MB of blobs → push + publish ≤ 10 ms,
100 000 / 100 MB → ≤ 100 ms (release builds, reference box), measured by `tests/bench/registry_measure`
(FR-T6). Main-program entries are viewed in place, never copied (C-11, FR-R14). And a program decides *per file*
what its dictionary holds: a `scoped_capture_file<Scope>` writes only the scope's calls, closed over their types and
literals (P-16); `capture_policy{.dictionary_updates = false}` removes the per-frame version compare for a writer
whose registry cannot move; `checkpoint_dictionary = false` (the default) keeps checkpoints at 80 bytes.

```cpp
// s11_scale.cpp — a program with many message types: what the registry costs and the knobs that keep files small.
#include <es/serreg/serreg.hpp>

#include <chrono>
#include <print>
#include <utility>

namespace many
{
// 96 distinct message types (a template instance per index), each with its own call signature and literal.
template<int N> struct Msg
{
    std::uint64_t ts;
    std::int64_t value;
    std::uint32_t id;
    std::uint16_t venue;
    std::uint8_t kind;
};
} // namespace many

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

struct hot_scope // the few messages the latency-critical file carries
{
};
// a file whose registry cannot move at run time (no dlopen): the per-frame version compare disappears
using hot_file = basic_capture_file<hot_scope, capture_policy{.dictionary_updates = false}>;

template<int... Is> bool write_all(capture_file& out, std::integer_sequence<int, Is...>)
{
    return (serialize(out, "many"_, many::Msg<Is>{1, 2, 3, 4, 5}).has_value() && ...);
}

class scale_demo
{
public:
    static int run()
    {
        using clock = std::chrono::steady_clock;
        const registry& reg = registry::instance();
        std::println("registry: {} entries (types, literals and calls), program hash {:#x}", reg.size(),
                     reg.program_hash());
        auto t0 = clock::now();
        capture_file all("all.ser", capture_options{.info_host = false});
        const auto open_all = clock::now() - t0;
        if (!all.is_open() || !write_all(all, std::make_integer_sequence<int, 96>{}) || !all.close())
            return 1;

        t0 = clock::now();
        hot_file hot("hot.ser", capture_options{.info_host = false});
        const auto open_hot = clock::now() - t0;
        if (!hot.is_open())
            return 1;
        for (int i = 0; i < 96; ++i)
            if (!serialize(hot, "fill"_, many::Msg<0>{1, 2, 3, 4, 5}))
                return 1;
        if (!hot.close())
            return 1;

        const auto ia = tools::info("all.ser"), ih = tools::info("hot.ser");
        if (!ia || !ih)
            return 1;
        std::println("all.ser: dictionary {} types, {} calls; file {} bytes; open() {} us", ia->dict.types.size(),
                     ia->dict.calls.size(), ia->file_size,
                     std::chrono::duration_cast<std::chrono::microseconds>(open_all).count());
        std::println("hot.ser: dictionary {} types, {} calls; file {} bytes; open() {} us (scope {})",
                     ih->dict.types.size(), ih->dict.calls.size(), ih->file_size,
                     std::chrono::duration_cast<std::chrono::microseconds>(open_hot).count(),
                     ih->writer_item(fmt::info_dictionary));
        return 0;
    }
};
} // namespace app

int main()
{
    return app::scale_demo::run();
}
```

**What to expect** (a debug library on a 2-vCPU sandbox; `open()` includes `fopen` and an `fdatasync`):

```
$ ./s11_scale
registry: 196 entries (types, literals and calls), program hash 0x5ef7b51912b8c4ed
all.ser: dictionary 96 types, 97 calls; file 32480 bytes; open() 2168 us
hot.ser: dictionary 1 types, 1 calls; file 4360 bytes; open() 1604 us (scope scoped:app::hot_scope)
$ srdump hot.ser --dict-only
dictionary  program_hash 0x5ef7b51912b8c4ed  (1 types, 1 literals, 1 calls)
struct many::Msg<0>  schema 0x4ea1b1d3ec544a1a  layout 0x0d6b90835b4a0f74  size 24  align 8  flat
  …
literal 0xe1c90c19f0ff66a4 "fill"
call 0xef8e623db4da7039  (lit, many::Msg<0>)
```

Both files hold 96 frames of 40 bytes (3840 bytes of data). The full dictionary costs about 28 KB per file; the
scoped one about 300 bytes. Note the 96 `Msg<N>` share one layout hash (`0x0d6b…`): same bytes, different names.

**`registry_measure`** synthesises N entries of B bytes and pushes them as the guards do (this sandbox, **debug**
library — the facts sheet's release numbers on the same VM are push 7.7 ms + publish 1.9 ms at 10k/10 MB, lookup
100 ns, export 14 ms, parse 12 ms; and 111 ms + 29 ms at 100k/100 MB, over budget on this VM, reference box
pending):

```
$ tests/bench/registry_measure --entries 10000 --bytes 10000000
registry_measure: 10000 entries, 9.5 MB of ENTRY blobs (1000 bytes each), descriptors in this image (as the main program's)
  push          14.023 ms   (1.40 µs per entry)
  publish        7.139 ms   (first snapshot after the burst)
  lookup         4.729 ms   (472.9 ns per find_type, random order)
  export       291.262 ms   (9.5 MB image)
  parse         44.575 ms   (dyn::dictionary::add_rri of that image)
  rss             51.7 MB   growth over the run
```

`--assert` enforces the budgets (ctests `registry_measure_10k/100k`, label `bench`, non-gating, T-9); `--plugin`
measures the deep-copy path of a `dlopen`'d image; `--elf <bin>` the reader side.

**The options a large program has, in order of effect.** Fewer, larger message types (one struct with a `kind`
beats fifty structs); scoped capture files per purpose (the log file, the order file, the market-data file each
carry their own dictionary); `capture_policy{.dictionary_updates = false}` where nothing is `dlopen`'d;
`checkpoint_dictionary = false`; and remember that the ELF dictionary section is read by tools, never by the
program itself — it costs file size, not start-up time.

**Rests on.** G-5, C-11, P-16, A-11, A-13 (every published index lives until exit — about 80 bytes per entry per
publish — so publish in bursts, which `dlopen` does naturally), FR-R13…FR-R15, FR-T6, T-9, NFR-1 (hot-path
ceilings: nothing on the serialise path allocates or locks, C-5).

---

## 12. Deploying: the package, the three libraries and what the binary carries

**Who.** The engineer who installs the library on the build hosts and the production hosts, and must know what a
version number promises, which library an application needs, and what a shipped binary reveals about its messages.

**The problem.** Three things change at different speeds — the wire format, the library ABI and the tools — and
an application that links the wrong one, or a host that upgrades the wrong one, finds out at run time.

**The solution.** One package, three libraries, one rule (A-4, G-2, FR-V1…FR-V3). `es::serreg` is the runtime
(registry, readers, decoder, the dictionary parser) and is what every program that serialises links; `es::serreg_tools`
is `srdump` as a library (the CLI, the text/JSON/TOML renderers, `to_string`, Wireshark, `--diff`, `--cxx`) and is
linked by programs that *render* or embed the tool surface; `es::ipc` is the header-only C++23 ring library with no
dependency on the other two. Until 3.0 **the minor is the compatibility unit**: a wire-format or ABI change bumps
the library minor, so the SONAME is `major.minor` and `find_package(serreg 2.2)` accepts 2.2.x only.

```cmake
# CMakeLists.txt of a consumer. The package carries -std=c++26 -freflection -fno-gnu-unique, Threads and dl.
cmake_minimum_required(VERSION 3.28)
project(deploy_story CXX)
find_package(serreg 2.2 CONFIG REQUIRED COMPONENTS serreg tools ipc) # or no COMPONENTS: everything installed
add_executable(app s01_first.cpp)
target_link_libraries(app PRIVATE es::serreg es::serreg_tools es::ipc) # es::serreg does not bring es::ipc
```

```sh
cmake --install <build> --prefix /opt/serreg                 # bin/srdump, include/es/{serreg,ipc}, lib/, lib/cmake/serreg, lib/pkgconfig, share/serreg/serreg.magic
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/serreg && cmake --build build
pkg-config --cflags --libs serreg serreg-tools              # the same without CMake
```

**What to expect.**

```
$ pkg-config --cflags --libs serreg serreg-tools
-I<prefix>/include -std=c++26 -freflection -fno-gnu-unique -L<prefix>/lib -lserreg_tools -lserreg -ldl -lpthread
$ readelf -d <prefix>/lib/libserreg.so.2.2.1 | grep SONAME
 0x000000000000000e (SONAME)             Library soname: [libserreg.so.2.2]
$ readelf -d build/app | grep -E 'NEEDED.*serreg|RUNPATH'
 0x0000000000000001 (NEEDED)             Shared library: [libserreg_tools.so.2.2]
 0x0000000000000001 (NEEDED)             Shared library: [libserreg.so.2.2]
 0x000000000000001d (RUNPATH)            Library runpath: [<prefix>/lib]
$ srdump --version
srdump 2.2.1 (format 1.7, hash_algo 2)

$ sed -i 's/find_package(serreg 2.2/find_package(serreg 2.3/' CMakeLists.txt && cmake -S . -B b23 -DCMAKE_PREFIX_PATH=<prefix>
CMake Error at CMakeLists.txt:3 (find_package):
  Could not find a configuration file for package "serreg" that is compatible
  with requested version "2.3".
  The following configuration files were considered but not accepted:
    <prefix>/lib/cmake/serreg/serregConfig.cmake, version: 2.2.1
      The version found is not compatible with the version requested.
```

A 2.2.x patch never changes bytes or ABI, so `libserreg.so.2.2` can be replaced under a running fleet; a 2.3 (format
1.8) is a new SONAME and a relink. Readers accept the current format minor and the previous one (`format_minor = 7`,
`format_minor_oldest = 6`): a 2.3 reader opens 1.7 files, a 2.2 reader refuses 1.8 with `bad_version` rather than
guess (FR-V3). The one exception on record is format 1.7 itself, which shipped in a patch release while the
library was still inside its development tree (FR-V4).

**What the binary carries.** Every image that instantiates `serialize`, `deserialize`, `to_string` or a logger call
has an `es_serreg_dict` section: the ENTRY blobs of its types, literals and calls, read by tools and never by the
program itself (the program's registry views the same blobs in place, C-11). So the binary *is* the schema:

```
$ readelf -S -W build/app | grep es_serreg_dict
  [17] es_serreg_dict    PROGBITS        0000000000454dd0 054dd0 000210 00  Ao  0   0  8
$ srdump --elf build/app | head -3
dictionary  program_hash 0x0000000000000000  (2 types, 2 literals, 2 calls)
enum shop::Side : u8  schema 0x5c7468d4207da4a2  layout 0xfdccdf0b5609277b  size 1  align 1
  buy = 1
$ srdump --elf build/app --program-hash; srdump first.ser --program-hash      # the binary and its capture agree
0x28ea563706d4de96
0x28ea563706d4de96
$ file -m <prefix>/share/serreg/serreg.magic first.ser md-v1.rri
first.ser:  es::serreg capture (FILE container), format 1.7, XXH3-64 hashes, abi ptr8/long8/wchar4, program hash 0x28ea563706d4de96
md-v1.rri:  es::serreg registry image (RRI), format 1.7, 13 entries, program hash 0x0000000000000000, XXH3-64 hashes, abi ptr8/long8/wchar4
```

528 bytes of section for §1's program (two types, two literals, two calls); a service with thousands of log sites
carries a few hundred kilobytes, which costs file size and nothing at start-up. `srdump --elf app --rri-out app.rri`
at release time is what §8's `--diff` and §13's compatibility checks run on later; `srdump --elf app.so` works on a
plugin the same way (§5). The program hash in a capture's header is the order-independent hash of the dictionary,
so `--program-hash` on the binary and on any of its captures agree (FR-H3).

**Build options that matter in deployment.** `ES_SERREG_BUILD_SHARED` (default ON) is required when `dlopen`ed
libraries serialise — one registry per process lives in the shared `libserreg` (FR-R3); a static build is for a
single-image program. `ES_SERREG_IMAGE_FORMAT=none` (needs the static build) omits the section and the loader
integration for targets without ELF. `-fno-gnu-unique` comes with the `es::serreg` target on GCC and must stay (a
`STB_GNU_UNIQUE` symbol makes glibc mark a DSO `NODELETE`, and plugins would never unload). Installed targets carry
an `$ORIGIN`-relative RPATH; `cmake --install` never relinks. The registry budget a program with thousands of
types must fit — push + publish ≤ 10 ms for 10 000 entries — and how to measure it on the target host with
`registry_measure` are §11.

**Rests on.** A-4, G-2, C-11, FR-V1…FR-V4, FR-V6 (versioned deliverables, `scripts/bump-version.sh`), FR-R2, FR-R3,
FR-D6, FR-H3, FR-R13 (the budget), requirements §11.1 (loader), ctest `soname_major_minor` and the install +
`find_package` consumer stage of `scripts/run-all-tests.sh`.

---

## 13. Evolution preview (2.3 — planned)

**Who.** Everyone above, the day the struct changes.

**What happens today.** The typed readers accept a frame only under the identical call signature: the schema hash
is over names, types, offsets and metadata, and the call hash is over the schema hashes. Renaming `halted` to
`is_halted` (§8) is *names-only* for the type but a different call hash, so a reader built from the new source
gets `hash_mismatch` on old frames; `srdump` and the dictionary-driven readers, which never needed the source,
read both. The dictionary already carries what evolution will need:

```cpp
// s12_evo.cpp — what the dictionary already records for schema evolution: since / deprecated.
#include <es/serreg/serreg.hpp>

#include <print>

namespace evo
{
struct Quote
{
    std::uint64_t ts;
    std::int64_t px;
    std::uint32_t size;
    [[=es::serreg::since(2)]] std::uint32_t venue;      // added in schema version 2
    [[=es::serreg::deprecated(3)]] std::uint16_t flags; // to be removed after version 3
    [[=es::serreg::skip]] std::uint16_t cache;          // never on the wire (scrubbed to zero)
};
} // namespace evo

namespace app
{
using namespace es::serreg;
using namespace es::serreg::literals;

class evo_demo
{
public:
    static int run()
    {
        std::vector<std::byte> buf;
        vector_sink out{buf};
        if (!serialize(out, "quote"_, evo::Quote{1, 2, 3, 4, 5, 6}))
            return 1;
        std::println("schema {:#x} (names, since/deprecated in it), layout {:#x} (bytes only)",
                     schema_hash_v<evo::Quote>, layout_hash_v<evo::Quote>);
        // today: a frame is read typed only by the identical signature (the layout hash is the fast path)
        evo::Quote back{};
        const auto r = deserialize(buf, "quote"_, back);
        std::println("round trip: {} (cache={} — the skipped member reads back as zero)", r ? "ok" : "failed",
                     back.cache);
        return 0;
    }
};
} // namespace app

int main()
{
    return app::evo_demo::run();
}
```

```
$ ./s12_evo
schema 0x69ba890524e86e85 (names, since/deprecated in it), layout 0x11f6541b6f3ff9f6 (bytes only)
round trip: ok (cache=0 — the skipped member reads back as zero)
$ srdump --elf s12_evo | sed -n '/^struct/,/^literal/p'
struct evo::Quote  schema 0x69ba890524e86e85  layout 0x11f6541b6f3ff9f6  size 32  align 8  flat
  ts: u64 @0 [8,8]
  px: i64 @8 [8,8]
  size: u32 @16 [4,4]
  venue: u32 @20 [4,4] since 2
  flags: u16 @24 [2,2] deprecated 3
$ srdump --elf s12_evo --json | jq '.types[0].fields[3]'
{"align":4,"deprecated":0,"elem":[],"extent":0,"kind":"u32","name":"venue","offset":20,"since":2,"size":4,"type_hash":"0x0000000000000000"}
$ srdump --diff ./s12_evo ./s12_evo_b | head -1          # the same type with since(3) instead of since(2)
metadata-only   evo::Quote  0x69ba890524e86e85 -> 0xf6bf8d3808970585
```

Every field entry carries its name, kind, offset, size, alignment, element shape, `since` and `deprecated`
(`uint16_t`, §14 default) — and the schema hash changes with them (P-6: `since`/`deprecated` are in the schema
hash, not the layout hash), which is why a metadata-only edit is visible to `--diff` and why a `renamed_from` can
be added without a wire change.

**What 2.3.0 will mean** (R-9, G-15, requirements §15.2; no byte of the format changes):

- **Renaming a member** (`halted` → `is_halted`): `[[=es::serreg::renamed_from("halted")]]` on the member; the
  reader projects by member name, old frames decode into the new struct, new frames into the old one. Without the
  annotation the rename is reported (`projection_report`, `srdump --diff --compat`), not blocking, for nested types
  that still match by structure.
- **Widening** (`u16 size` → `u32 size`, `f32` → `f64`): allowed in the reading direction that widens; **narrowing
  never**. R3 (unsigned → strictly wider signed) and R12 (an array that grew) stay.
- **Adding a field** (`[[=since(2)]] std::uint8_t flags`): an old frame projects into the new struct with the new
  member value-initialised; a new frame projects into the old struct by dropping it. `since`/`deprecated` gain
  their meaning: a reader can say which schema version a frame predates.
- **Variants** match alternatives by canonical name (`std::string` regardless of allocator; a renamed alternative
  class needs `renamed_from` on the class); **enums** match by underlying kind; **nested types** project by
  structure when the member name matches.
- The typed readers, `dispatch` and `frame_router` take a projection built once per (writer type, reader type)
  pair from the dictionary; the layout hash remains the O(1) "same bytes" fast path, so a frame whose layout
  matches costs what it costs today.
- Levels 2 (structural changes beyond these) and 3 (evolved reads without a dictionary) are unscheduled and not
  planned respectively (R-10): every stream carries its dictionary.

Until then the rule is the one the tools already enforce: keep `md-v1.rri` with every release, run `srdump --diff
old.rri ./new-app` in CI, and treat `BREAKING` as a request for a new literal (`"quote2"_`) or a new type name — a
reader can then serve both signatures with two `on(...)` handlers (§2), and the capture files never needed the
source.

**Rests on.** R-9, R-10, G-15, P-6, FR-S11 (`since`/`deprecated` today), A-5 (`--diff --direction`), the planned
FR-IDs of requirements §15.2.

---

## Appendix — the files behind this document

| Story | Program(s) | Built and run | Notes |
|---|---|---|---|
| 1 | `s01_first.cpp` | yes | |
| 2 | `s02_mdcap.cpp` | yes | `on<Sig...>` needs `lit<"x">`, not `decltype(k_x)` |
| 3 | `s03_log.cpp` | yes (`-lpthread`) | frame sizes measured from the file |
| 4 | `s04_archive.cpp` + `plugin_tick.so` | yes (`-ldl`) | truncation / byte flip / smashed length done on copies of the file |
| 5 | `plugin_tick.cpp` (×2), `s05_plugins.cpp` | yes (`-ldl`) | the conflict part is `tests/dlopen/dlopen_host` with the test plugins |
| 6 | `s06_debug.cpp` | yes | |
| 7 | `s07_dynamic.cpp` | yes | |
| 8 | `s08_mdcap_v2.cpp`, `md_types.hpp` (generated), `s08_regen.cpp` | yes | tshark 4.6 as `nobody` |
| 9 | `s09_streams.cpp` | yes (`-lpthread`) | |
| 10 | `s10_read.py` | yes (Python 3, `-I`) | |
| 11 | `s11_scale.cpp`, `registry_measure` | yes | debug-library numbers; release numbers from the facts sheet |
| 12 | `s12_evo.cpp`, `s12_evo_b.cpp` | yes | the 2.3 bullets are a plan, not code |
