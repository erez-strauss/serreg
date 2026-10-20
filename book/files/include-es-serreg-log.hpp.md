# `include/es/serreg/log.hpp`

*(Format 1.7, DECISIONS A-*.)* Binary structured logging on top of `println`: a
`logger<Opts, S>` over any sink writes one **log frame** per message — an ordinary print frame whose call signature
begins with the **log envelope** (level, source file, line, an options mask) as zero-byte constants, then the
timestamp, then by policy a thread id and a sequence number, then the format literal and the arguments. Because the
envelope is part of the *call signature*, a message costs on the wire only what `println` costs plus 8
bytes of timestamp (and 4 / 8 more for a per-message thread id / sequence); the level, file, line and format text
are in the dictionary, once. Every policy is a member of the structural `log_options` passed as a template
argument, so every choice is `if constexpr`. Readers — `srdump` (`--level`, `--since`, `--until`), `tools::dump`,
`tools::render_frame`, `frame_router::on_log`, the generated Wireshark dissector and the Python reader — render a
log frame as `2026-10-07 14:03:11.123456789 WARN  main.cpp:42 book AAPL depth 7`. The header belongs to the
**serialisation layer** and is header-only; it needs reflection through `print.hpp`/`serialize.hpp`.

## Depends on / used by

Includes: `es/serreg/print.hpp` (`detail::printable_arg`, `check_format`, `no_nested_fields`),
`es/serreg/registry.hpp`, `es/serreg/serialize.hpp` (`detail::serialize_frame`, `log_tag` through
`blob_bytes.hpp`), `es/serreg/sink.hpp` (`sink`, `scoped_sink`), `<unistd.h>` (`gettid`), `<atomic>`, `<chrono>`,
`<x86intrin.h>` on x86-64 (`__rdtsc`).

Included by: `serreg.hpp`. Users: `tests/unit/test_log.cpp`, `tests/python/log_capture.cpp` (the capture the
Python parity test renders). The readers do not include it: they know the envelope from the dictionary
(`call_flag_log`, `dyn::call::is_log()`) and its fixed positions (`log_layout`, mirrored by
`internal::decode_log` in `src/formatters.cpp`, by `python/serreg/reader.py` and by the Lua runtime in
`src/wireshark.cpp`).

## What it defines

| name | kind | meaning |
|---|---|---|
| `level` | `enum class : u8` | `trace = 0`, `debug`, `info`, `warn`, `error`, `fatal = 5`, `off = 6` (a floor or minimum that disables everything) |
| `to_string(level)` | `constexpr` function | `"trace"` … `"off"` |
| `thread_policy` | `enum class : u8` | `none`; `once` (the default — one thread per sink, so the id is recorded once per file: open the capture file with `capture_options{.info_thread = true}`); `per_message` (a `u32` after the timestamp) |
| `sequence_policy` | `enum class : u8` | `none`; `per_sink` (a `u64` counted by this logger, after the thread id) |
| `clock_policy` | `enum class : u8` | `system` (ns since the epoch), `steady`, `tsc` (`__rdtsc`; steady clock on other targets), `user` (`set_clock`) |
| `dictionary_policy` | `enum class : u8` | `scoped` (the default: `S` must be a `scoped_sink`, the file's dictionary holds only that scope's calls) or `full` (any sink; the whole registry) |
| `log_options` | structural struct | `floor = trace`, `thread = once`, `sequence = none`, `clock = system`, `dictionary = scoped`; `mask()` — the envelope's options byte: bit 0 thread id follows, bit 1 sequence follows, bits 2–3 the clock (`log.hpp:105-120`) |
| `ES_SERREG_LOG_MIN_LEVEL` | macro (default 0) | a translation unit's compile-time floor, as an integer level |
| `detail::this_thread_id()`, `detail::log_now(clock, user)` | inline functions | `gettid()`; the timestamp of a clock policy |
| `logger<log_options Opts, sink S>` | class template, `requires(Opts.dictionary == full \|\| scoped_sink<S>)` | the logger (below) |
| `ES_SERREG_LOG(lg, lvl, "fmt"_, args...)` | macro | the gated call with the call site: `__FILE__`/`__LINE__` as constants, nothing after the level evaluated when it is off (`log.hpp:323-326`) |
| `log_layout::level_arg`, `file_arg`, `line_arg`, `mask_arg`, `time_arg`, `mask_thread`, `mask_sequence`, `format_arg(mask)`, `clock_of(mask)` | constants and `constexpr` functions | the fixed positions of the envelope in a log call's argument list, for readers (`log.hpp:317-329`) |

`logger` members:

| member | meaning |
|---|---|
| `options`, `floor` | `Opts`; the effective floor — the higher of `Opts.floor` and `ES_SERREG_LOG_MIN_LEVEL` |
| `logger(S& sink, level min_level = trace)` | binds the sink (by reference; it must outlive the logger) and the runtime minimum |
| `enabled(l)` | `l >= floor && l >= min_level()` — one relaxed atomic load; below the floor `false` at compile time |
| `set_level(l)`, `min_level()` | the runtime minimum (relaxed atomic) |
| `set_clock(fn)` | the clock of `clock_policy::user` (a `uint64_t (*)() noexcept`) |
| `sink()` | the sink |
| `write<L, File, Line>(lit<F>, args...) -> bool` | one log frame; below the floor compiled out (returns true); a failure is counted |
| `trace/debug/info/warn/error/fatal(lit<F>, args...) -> bool` | `write<level, "-", 0>` — no call site |
| `last_error()`, `failed()` | the last failed write's error (`error{}` when none) and the number of failed writes |

## How it works

### The envelope is the signature

```cpp
return detail::serialize_frame<log_tag, constant<static_cast<std::uint8_t>(L)>,
                               lit<File>, constant<Line>, constant<mask>,
                               std::uint64_t, lit<F>, std::remove_cvref_t<Ts>...>(
    serialize_options{}, *sink_, constant<static_cast<std::uint8_t>(L)>{}, lit<File>{},
    constant<Line>{}, constant<mask>{}, ts, lit<F>{}, vs...);
```
(`log.hpp:265-269`, the branch without thread id and sequence; the other three branches at `log.hpp:241-263` add
`std::uint32_t` and/or `std::uint64_t` after the timestamp.) `log_tag` (`blob_bytes.hpp`) is a form tag like
`println_tag`: the call string is `log(n){…}` and the call flags are `print | newline | log`. The level, the
file (a literal — its text is in the dictionary once), the line and the mask are **constants** (format 1.5):
they are in the call's ENTRY and occupy no bytes in a compact frame. A different level, file or line is a
different call signature, registered at compile time like any `serialize` call site. The one argument with bytes
in every frame is the `u64` timestamp; `thread_policy::per_message` adds a `u32`, `sequence_policy::per_sink` a
`u64`. The arguments after the format literal are exactly those of `println` — `detail::printable_arg`,
checked at compile time against the format (`detail::check_format`); nested replacement fields (`{:{}}`) are
refused because the readers do not render them.

A frame header plus an 8-byte timestamp plus the payload: `ES_SERREG_LOG(lg, warn, "book {} depth {}"_, sym,
depth)` writes 16 + 8 + the string record + the depth — the same as `println` of the same arguments plus
eight bytes.

### The gate and the floor

```cpp
#define ES_SERREG_LOG(lg, lvl, ...)                                                                \
    ((lg).enabled(::es::serreg::level::lvl) &&                                                     \
     (lg).template write<::es::serreg::level::lvl, ::es::serreg::fixed_string{__FILE__}, __LINE__>( \
         __VA_ARGS__))
```
(`log.hpp:310-313`.) `&&` short-circuits, so the arguments — the expressions after the format — are not evaluated
when the level is off; `enabled` is one relaxed load of `min_`. Below the compile-time `floor` (`log_options::floor`
or `ES_SERREG_LOG_MIN_LEVEL`, whichever is higher; `log.hpp:165-167`) `enabled` is false at compile time and
`write<L,…>` is `if constexpr (L < floor)` → `return true`: no `serialize_frame` instantiation, so **no
registration and no code** — the call site does not exist in the binary or in its dictionary. The member functions
(`lg.info(...)`) evaluate their arguments (they are function arguments); use the macro, or `if
(lg.enabled(level::debug))`, when building an argument costs something ("A log: the gate evaluates nothing below
the level; the floor compiles the call out", `tests/unit/test_log.cpp`).

### Dictionary scopes

```cpp
template<log_options Opts, sink S>
    requires(Opts.dictionary == dictionary_policy::full || scoped_sink<S>)
class logger
```
(`log.hpp:159-161`.) With the default `dictionary_policy::scoped` the sink must declare `using dictionary_scope =
Tag;` — `scoped_capture_file<Tag>` does, a user ring sink adds the line. Every log call then also registers a
scope node (`registry.hpp` `registrar_scoped`), and a scoped capture file's dictionary is exactly the scope's log
calls, their file and format literals and the types of their arguments ("A log: the scoped dictionary holds the
scope's calls, their literals and types — nothing else"). `dictionary_policy::full` lifts the requirement for a sink
that mixes log frames with other messages; its readers then need the full registry image.

### Rendering

A reader recognises a log frame by `call_flag_log` and reads the envelope from fixed positions
(`log_layout::format_arg(mask)` = 5, 6 or 7): level, file and line from the call's constants, the timestamp from the
record, the thread id and sequence when the mask says so. The text form is
`<time> <LEVEL> <file>:<line> [tid] #seq <message>`: the time as `YYYY-MM-DD HH:MM:SS.nnnnnnnnn` (UTC) for
`clock_policy::system`, the raw number for the other clocks (the mask's bits 2–3 tell); the level upper-case and
padded to five; the file's basename, omitted for the member functions' `"-"`. `srdump --level <lvl>` prints only
those lines (no dictionary, no frame headers), `--json` one `{"frame":N,"log":{…}}` object per line, and
`--since`/`--until` select by timestamp (`src/formatters.cpp`, `src/cli.cpp`); `frame_router::on_log(h)` receives
`h(level, line)` for every log frame that has no handler of its own, and `on_log(min_level, h)` the frames at or
above a level floor as a `tools::log_record` — the envelope's fields and the message without the prefix, as
`tools::render_log` returns it (`router.hpp`, `tools.hpp`).

## Maintaining it

- **The envelope layout is format.** The order level, file, line, mask, time, [thread], [sequence], format is
  checked by the dictionary parser (`src/dictionary.cpp` refuses a `log` call whose arguments do not have that
  shape) and mirrored in `internal::decode_log`, `python/serreg/reader.py` and the Lua runtime. Changing it, the
  mask bits or the level values is a `format_minor` bump.
- **Policies change call hashes.** Two loggers with different `log_options` produce different signatures for the
  same format and call site (the mask constant differs); that is intended — a reader never guesses which fields a
  frame has.
- `thread_policy::once` writes nothing per message; the writer thread's id is recorded only if the capture file is
  opened with `capture_options{.info_thread = true}` (the logger does not do it for the caller).
- A logger is not thread-safe when `sequence_policy::per_sink` is used (`seq_` is a plain counter) and is exactly as
  thread-safe as its sink otherwise; one logger per sink per thread is the model.
- Tests: `tests/unit/test_log.cpp` ("A log: the gate evaluates nothing below the level; the floor compiles the
  call out", "A log: the envelope by policy — level, file:line, time, thread id, sequence — renders in srdump",
  "A log: the scoped dictionary holds the scope's calls, their literals and types — nothing else", "A log: a
  logger over a plain sink needs dictionary_policy::full; the frames decode with the registry"), the
  `python_reader_parity` ctest over the capture of `tests/python/log_capture.cpp`, and the Wireshark parity runs.
