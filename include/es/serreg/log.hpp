// es/serreg/log.hpp — binary structured logging with an envelope (2.2.1, format 1.7; DECISIONS A-*).
//
//   struct app_log_scope {};                                  // a dictionary scope: the log's own dictionary
//   using log_file = es::serreg::scoped_capture_file<app_log_scope>;   // or any sink with `dictionary_scope`
//   log_file file("app.log.ser", es::serreg::capture_options{.crc = true});
//
//   es::serreg::logger<{}, log_file> lg{file, es::serreg::level::info};
//   ES_SERREG_LOG(lg, warn, "book {} depth {}"_, sym, book.depth());   // file:line as zero-byte constants
//   if (lg.enabled(level::debug)) lg.debug("px={:.2f}"_, px);          // no file:line; nothing built when off
//
// What a log frame carries: a `log` call (println with the flag `log`) whose signature begins with the envelope —
// level, source file, line and an options mask as zero-byte constants, then the timestamp (u64, 8 bytes per
// message), then by policy a thread id (u32) and a sequence number (u64) — followed by the format literal and the
// arguments, exactly as println. Readers (srdump, --print, frame_router::on_log, Wireshark) render
// `2026-10-07 14:03:11.123456789 WARN  main.cpp:42 book AAPL depth 7`.
//
// log_options is a structural type used as a template parameter: every policy is an `if constexpr` inside
// write<L>(), and two loggers with different policies are two different call signatures for the same format.
//   floor      — calls below it compile to nothing (no registration, no code)
//   thread     — none | once (the writer thread's id goes into the sink's writer-info frame, by the caller) |
//                per_message (a u32 argument)
//   sequence   — none | per_sink (a u64 argument, counted by this logger)
//   clock      — system (system_clock, ns since the epoch) | steady | tsc (rdtsc) | user (set_clock())
//   dictionary — scoped (the sink type's dictionary_scope: the only calls in the file's dictionary are this
//                sink type's) | full (the whole registry; for a sink that mixes logs and archived messages)
// A runtime level (`min_level`, one relaxed atomic load) gates on top of the compile-time floor.
#pragma once
#include <es/serreg/print.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/serialize.hpp>
#include <es/serreg/sink.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>
#include <type_traits>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace es::serreg
{

enum class level : std::uint8_t
{
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    fatal = 5,
    off = 6 // a floor or min_level that disables everything
};
constexpr std::string_view to_string(const level l) noexcept
{
    switch (l)
    {
    case level::trace:
        return "trace";
    case level::debug:
        return "debug";
    case level::info:
        return "info";
    case level::warn:
        return "warn";
    case level::error:
        return "error";
    case level::fatal:
        return "fatal";
    case level::off:
        return "off";
    }
    return "?";
}
// thread id: none; once — the sink is one thread's, so the id is recorded once per file (open the capture file
// with capture_options{.info_thread = true}: the writer-info frame carries it); per_message — a u32 after the
// timestamp (a sink shared by threads).
enum class thread_policy : std::uint8_t
{
    none,
    once,
    per_message
};
enum class sequence_policy : std::uint8_t
{
    none,
    per_sink
};
enum class clock_policy : std::uint8_t
{
    system,
    steady,
    tsc,
    user
};
enum class dictionary_policy : std::uint8_t
{
    scoped,
    full
};
struct log_options
{
    level floor = level::trace;
    thread_policy thread = thread_policy::once;
    sequence_policy sequence = sequence_policy::none;
    clock_policy clock = clock_policy::system;
    dictionary_policy dictionary = dictionary_policy::scoped;
    // the envelope mask constant in every log signature: bit0 thread id follows, bit1 sequence follows,
    // bits 2-3 the clock (so a reader renders the timestamp right without the writer's source)
    constexpr std::uint8_t mask() const noexcept
    {
        return static_cast<std::uint8_t>((thread == thread_policy::per_message ? 1 : 0) |
                                         (sequence == sequence_policy::per_sink ? 2 : 0) |
                                         (static_cast<unsigned>(clock) << 2));
    }
};

// The compile-time floor for every logger in a translation unit (an integer level; default trace = 0).
#ifndef ES_SERREG_LOG_MIN_LEVEL
#define ES_SERREG_LOG_MIN_LEVEL 0
#endif

namespace detail
{
inline std::uint32_t this_thread_id() noexcept
{
    return static_cast<std::uint32_t>(::gettid());
}
inline std::uint64_t log_now(const clock_policy c, std::uint64_t (*user)() noexcept) noexcept
{
    switch (c)
    {
    case clock_policy::system:
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    case clock_policy::steady:
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    case clock_policy::tsc:
#if defined(__x86_64__)
        return __rdtsc();
#else
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    case clock_policy::user:
        return user ? user() : 0;
    }
    return 0;
}
} // namespace detail

// A logger over a sink. `S` must be a scoped_sink unless Opts.dictionary == full.
template<log_options Opts, sink S>
    requires(Opts.dictionary == dictionary_policy::full || scoped_sink<S>)
class logger
{
public:
    static constexpr log_options options = Opts;
    static constexpr level floor = Opts.floor > static_cast<level>(ES_SERREG_LOG_MIN_LEVEL)
                                       ? Opts.floor
                                       : static_cast<level>(ES_SERREG_LOG_MIN_LEVEL);

    explicit logger(S& sink, const level min_level = level::trace) noexcept
        : sink_(&sink), min_(min_level)
    {
    }
    // The gate: one relaxed load. Below the compile-time floor it is `false` at compile time.
    [[nodiscard]] bool enabled(const level l) const noexcept
    {
        return l >= floor && l >= min_.load(std::memory_order_relaxed);
    }
    void set_level(const level l) noexcept
    {
        min_.store(l, std::memory_order_relaxed);
    }
    level min_level() const noexcept
    {
        return min_.load(std::memory_order_relaxed);
    }
    void set_clock(std::uint64_t (*clock)() noexcept) noexcept // for clock_policy::user
    {
        user_clock_ = clock;
    }
    S& sink() noexcept
    {
        return *sink_;
    }

    // The frame. File and Line are the call site (the ES_SERREG_LOG macro passes __FILE__ and __LINE__; the
    // member functions below pass "-" and 0). Everything of the envelope that is constant travels as a
    // zero-byte constant in the signature; the timestamp (and by policy the thread id and the sequence) are the
    // only bytes beyond println's.
    // Returns true when the frame reached the sink; a failure is counted and kept for last_error() (a logger
    // call is not the place for error handling, so the result is not [[nodiscard]]).
    template<level L, fixed_string File, std::uint32_t Line, fixed_string F,
             detail::printable_arg... Ts>
    bool write(const lit<F> f, const Ts&... vs)
    {
        if constexpr (L < floor)
        {
            static_cast<void>(f);
            (static_cast<void>(vs), ...);
            return true; // compiled out: no registration, no code
        }
        else
            return note(write_frame<L, File, Line>(f, vs...));
    }
    // The last failed write's error (error{} when none failed) and the number of failed writes.
    ::es::serreg::error last_error() const noexcept // (error() below is the level-named write)
    {
        return last_error_;
    }
    std::uint64_t failed() const noexcept
    {
        return failed_;
    }

private:
    bool note(const result<std::size_t> r) noexcept
    {
        if (r)
            return true;
        last_error_ = r.error();
        ++failed_;
        return false;
    }
    template<level L, fixed_string File, std::uint32_t Line, fixed_string F,
             detail::printable_arg... Ts>
    result<std::size_t> write_frame(const lit<F>, const Ts&... vs)
    {
        {
            static_assert(detail::check_format<F, Ts...>());
            static_assert(
                detail::no_nested_fields<F>(),
                "log: nested replacement fields ({:{}}) are not supported by the readers");
            const std::uint64_t ts = detail::log_now(Opts.clock, user_clock_);
            constexpr std::uint8_t mask = Opts.mask();
            if constexpr (Opts.thread == thread_policy::per_message &&
                          Opts.sequence == sequence_policy::per_sink)
                return detail::serialize_frame<log_tag, constant<static_cast<std::uint8_t>(L)>,
                                               lit<File>, constant<Line>, constant<mask>,
                                               std::uint64_t, std::uint32_t, std::uint64_t, lit<F>,
                                               std::remove_cvref_t<Ts>...>(
                    serialize_options{}, *sink_, constant<static_cast<std::uint8_t>(L)>{},
                    lit<File>{}, constant<Line>{}, constant<mask>{}, ts, detail::this_thread_id(),
                    ++seq_, lit<F>{}, vs...);
            else if constexpr (Opts.thread == thread_policy::per_message)
                return detail::serialize_frame<log_tag, constant<static_cast<std::uint8_t>(L)>,
                                               lit<File>, constant<Line>, constant<mask>,
                                               std::uint64_t, std::uint32_t, lit<F>,
                                               std::remove_cvref_t<Ts>...>(
                    serialize_options{}, *sink_, constant<static_cast<std::uint8_t>(L)>{},
                    lit<File>{}, constant<Line>{}, constant<mask>{}, ts, detail::this_thread_id(),
                    lit<F>{}, vs...);
            else if constexpr (Opts.sequence == sequence_policy::per_sink)
                return detail::serialize_frame<log_tag, constant<static_cast<std::uint8_t>(L)>,
                                               lit<File>, constant<Line>, constant<mask>,
                                               std::uint64_t, std::uint64_t, lit<F>,
                                               std::remove_cvref_t<Ts>...>(
                    serialize_options{}, *sink_, constant<static_cast<std::uint8_t>(L)>{},
                    lit<File>{}, constant<Line>{}, constant<mask>{}, ts, ++seq_, lit<F>{}, vs...);
            else
                return detail::serialize_frame<log_tag, constant<static_cast<std::uint8_t>(L)>,
                                               lit<File>, constant<Line>, constant<mask>,
                                               std::uint64_t, lit<F>, std::remove_cvref_t<Ts>...>(
                    serialize_options{}, *sink_, constant<static_cast<std::uint8_t>(L)>{},
                    lit<File>{}, constant<Line>{}, constant<mask>{}, ts, lit<F>{}, vs...);
        }
    }

public:
    // Without a call site (file "-", line 0). Gate with enabled() when the arguments cost something to build.
    template<fixed_string F, detail::printable_arg... Ts>
    bool trace(const lit<F> f, const Ts&... vs)
    {
        return write<level::trace, "-", 0>(f, vs...);
    }
    template<fixed_string F, detail::printable_arg... Ts>
    bool debug(const lit<F> f, const Ts&... vs)
    {
        return write<level::debug, "-", 0>(f, vs...);
    }
    template<fixed_string F, detail::printable_arg... Ts> bool info(const lit<F> f, const Ts&... vs)
    {
        return write<level::info, "-", 0>(f, vs...);
    }
    template<fixed_string F, detail::printable_arg... Ts> bool warn(const lit<F> f, const Ts&... vs)
    {
        return write<level::warn, "-", 0>(f, vs...);
    }
    template<fixed_string F, detail::printable_arg... Ts>
    bool error(const lit<F> f, const Ts&... vs)
    {
        return write<level::error, "-", 0>(f, vs...);
    }
    template<fixed_string F, detail::printable_arg... Ts>
    bool fatal(const lit<F> f, const Ts&... vs)
    {
        return write<level::fatal, "-", 0>(f, vs...);
    }

private:
    S* sink_;
    std::atomic<level> min_;
    std::uint64_t seq_ = 0;
    std::uint64_t (*user_clock_)() noexcept = nullptr;
    ::es::serreg::error last_error_{};
    std::uint64_t failed_ = 0;
};

// The gated call with the call site: nothing after the level is evaluated when the level is off.
#define ES_SERREG_LOG(lg, lvl, ...)                                                                \
    ((lg).enabled(::es::serreg::level::lvl) &&                                                     \
     (lg).template write<::es::serreg::level::lvl, ::es::serreg::fixed_string{__FILE__},           \
                         __LINE__>(__VA_ARGS__))

// The fixed positions of the envelope in a log call's argument list (readers): level, file, line, mask,
// timestamp; then by the mask's bits a thread id and a sequence; then the format literal and its arguments.
namespace log_layout
{
inline constexpr std::size_t level_arg = 0, file_arg = 1, line_arg = 2, mask_arg = 3, time_arg = 4;
inline constexpr std::uint8_t mask_thread = 1, mask_sequence = 2;
constexpr std::size_t format_arg(const std::uint8_t mask) noexcept
{
    return time_arg + 1 + ((mask & mask_thread) ? 1 : 0) + ((mask & mask_sequence) ? 1 : 0);
}
constexpr clock_policy clock_of(const std::uint8_t mask) noexcept
{
    return static_cast<clock_policy>((mask >> 2) & 3);
}
} // namespace log_layout

} // namespace es::serreg
