// src/internal.hpp — private helpers shared by the runtime/tools .cpp files (spec §6.4, §6.9).
// Not installed; nothing here is part of the public API. No reflection dependency.
#pragma once
#include <es/serreg/dictionary.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/limits.hpp>
#include <es/serreg/tools.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace es::serreg::internal
{

// The library's single write loop (gk 1.9 / cc F20): retries EINTR, treats a zero-length write as io_error.
// Same function as the public tools::write_all, with a (pointer, size) signature for callers inside src/.
result<void> write_all(int fd, const void* data, std::size_t size) noexcept;

// Largest input read_file/slurp accept (gk 1.15: a hostile pipe must not exhaust memory). Since 2.1.16 the
// process-wide decode_limits::input_bytes (1 GiB by default); this is its ceiling.
inline constexpr std::size_t max_input_bytes = std::size_t{1} << 30;
inline std::size_t input_limit() noexcept
{
    const std::uint64_t l = get_decode_limits().input_bytes;
    return static_cast<std::size_t>(std::min<std::uint64_t>(l, max_input_bytes));
}

// "0x" + 16 lower-case hex digits (canonical rendering of every 64-bit wire ID).
inline std::string hex64(const std::uint64_t v)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s("0x0000000000000000");
    for (int i = 0; i < 16; ++i)
        s[static_cast<std::size_t>(17 - i)] = digits[(v >> (4 * i)) & 0xf];
    return s;
}

inline std::string hex8(const std::uint8_t v)
{
    static constexpr char digits[] = "0123456789abcdef";
    return std::string{digits[v >> 4], digits[v & 0xf]};
}

template<class Int> std::string dec(const Int v)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

// "ptr8 long8 wchar4 char-signed max-align16 ldbl16 le" — the fields of a 1.3 ABI tag, for dumps and problems.
inline std::string abi_text(const std::uint64_t tag)
{
    const fmt::abi_tag t = fmt::unpack_abi(tag);
    if (t.version != 1)
        return "unknown(version " + dec(t.version) + ")";
    return "ptr" + dec(t.ptr_size) + " long" + dec(t.long_size) + " wchar" + dec(t.wchar_size) +
           (t.char_signed ? " char-signed" : " char-unsigned") + " max-align" + dec(t.max_align) +
           " ldbl" + dec(t.long_double_size) + (t.endian ? " be" : " le");
}

// Length of the well-formed UTF-8 sequence at s[i] (RFC 3629: no overlong forms, no surrogates, at most
// U+10FFFF), or 0 when the bytes there are not one.
inline std::size_t utf8_sequence(const std::string_view s, const std::size_t i)
{
    const auto b = [&](const std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    const unsigned char c = b(0);
    const std::size_t left = s.size() - i;
    const auto cont = [&](const std::size_t k) { return k < left && (b(k) & 0xC0) == 0x80; };
    if (c >= 0xC2 && c <= 0xDF)
        return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF)
    {
        if (!cont(1) || !cont(2) || (c == 0xE0 && b(1) < 0xA0) || (c == 0xED && b(1) > 0x9F))
            return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4)
    {
        if (!cont(1) || !cont(2) || !cont(3) || (c == 0xF0 && b(1) < 0x90) ||
            (c == 0xF4 && b(1) > 0x8F))
            return 0;
        return 4;
    }
    return 0;
}

// Appends s as a quoted JSON string (also valid as a TOML basic string). The output is always valid UTF-8
// (2.1.12, review D-08/T-06): well-formed UTF-8 sequences pass through, control characters and every byte that
// is not part of one are written as \u00XX (the byte's value as a code point, so no information is lost).
inline void append_quoted(std::string& out, const std::string_view s)
{
    out += '"';
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80)
        {
            if (const std::size_t n = utf8_sequence(s, i))
            {
                out.append(s.substr(i, n));
                i += n - 1;
            }
            else
            {
                out += "\\u00";
                out += hex8(u);
            }
            continue;
        }
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (u < 0x20 || u == 0x7f)
            {
                out += "\\u00";
                out += hex8(u);
            }
            else
                out += c;
        }
    }
    out += '"';
}

// Parses "0x..." or decimal into a u64; false on garbage.
inline bool parse_u64(std::string_view s, std::uint64_t& out)
{
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        s.remove_prefix(2);
        base = 16;
    }
    if (s.empty())
        return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, base);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// ---- capture walking (reader.cpp) -----------------------------------------------------------------------
// The 48-byte FILE header (format.hpp), parsed and checked once for both capture walkers (the in-memory one and
// the block reader): magic, major 1 with a minor between format_minor_oldest (6) and format_minor (7), the
// little-endian flag, hash_algo 2.
struct file_header
{
    std::uint16_t major{}, minor{};
    std::uint32_t flags{}, hash_algo{};
    std::uint64_t program_hash{}, dict_offset{}, dict_len{}, abi{};
};
result<file_header> parse_file_header(std::span<const std::byte> first_48_bytes) noexcept;

struct frame_info
{
    std::uint64_t index{};
    std::size_t offset{}; // byte offset of the frame header inside the capture
    std::uint32_t frame_len{};
    std::uint16_t flags{};
    std::uint16_t n_records{};
    std::uint64_t call_hash{};
    bool is_full{}, is_dict{};
    bool has_crc{}, is_checkpoint{}, is_info{}; // 1.7
    bool crc_ok{true}; // 1.7: false when has_crc and the trailer did not match
    bool
        is_opening{}; // 2.2.6: a FILE's opening dictionary block, delivered once before frame 0 (is_dict too)
};

struct record_info
{
    std::uint32_t index{};
    bool is_literal{};
    std::uint64_t literal_id{};  // is_literal
    std::uint64_t schema_hash{}; // !is_literal (0 if unknown)
    const dyn::type* type{};     // !is_literal; nullptr when not in the dictionary
    std::span<const std::byte>
        payload;        // object | u64 id | scalar bytes | string bytes (prefix stripped)
    fmt::arg_kind ak{}; // 1.1: category
    kind k{};           // 1.1: scalar kind when ak == arg_scalar
    bool
        is_const{}; // 1.5: a constant of the call, shown as the scalar or enum record it stands for (in a
                    // compact frame the payload is the value in the call's dictionary entry)
    std::string_view text() const noexcept
    {
        return {reinterpret_cast<const char*>(payload.data()), payload.size()};
    }
};

// Called once per frame (before its records) and once per record; return false to stop the walk.
using frame_fn = std::function<bool(const dyn::dictionary&, const frame_info&)>;
using record_fn =
    std::function<bool(const dyn::dictionary&, const frame_info&, const record_info&)>;

struct walk_result
{
    std::uint64_t frames{}, records{}, bytes{};
    std::uint64_t program_hash{};   // 2.1.16: the FILE header's, 0 for a bare stream
    std::size_t problems_omitted{}; // 2.1.16: beyond decode_limits::problems
    std::vector<std::string> problems;
    std::optional<error> first_error; // code of problems[0]; empty when there are none
    dyn::dictionary dict; // embedded + dictionary frames + `extra`, as of the end of the walk
};

// Walks a FILE or bare frame stream. Structural errors of the container itself are returned as an error;
// per-frame/per-record inconsistencies are appended to `problems` (and stop the walk if stop_on_problem).
result<walk_result> for_each_record(std::span<const std::byte> capture,
                                    const dyn::dictionary* extra, bool stop_on_problem,
                                    const frame_fn& on_frame, const record_fn& on_record);
// 2.1.16 (A6): the same walk over a file or "-", read in windows (tools::for_each_frame_in_file underneath), so
// verify, dump and srdump handle a capture of any size with bounded memory. Dictionary frames are consumed by the
// windowed reader and never reach on_frame.
result<walk_result> for_each_record_in_file(std::string_view path, const dyn::dictionary* extra,
                                            bool stop_on_problem, const frame_fn& on_frame,
                                            const record_fn& on_record,
                                            const tools::block_options& o);
// A walk over either source: what verify and dump are written against (2.1.16).
using record_source =
    std::function<result<walk_result>(bool stop_on_problem, const frame_fn&, const record_fn&)>;
// 2.1.16: append a problem line within the limit, or count it.
void add_problem(std::vector<std::string>& problems, std::size_t& omitted, std::size_t limit,
                 std::string what);

// G2-9 (2.1.3): one classification of a 16-byte frame header for both walkers. Fills frame_len, flags, n_records,
// call_hash, is_dict, is_full; `room` is the most bytes the frame may span.
enum class frame_class
{
    ok,
    bad_len,      // frame_len < 16, not a multiple of 8, or larger than `room`: the walk stops
    unknown_flags // a flag bit this reader does not know: recorded and skipped
};
frame_class classify_frame(std::span<const std::byte> header, std::uint64_t room,
                           frame_info& f) noexcept;
// 1.7: true when the frame's CRC32C trailer matches its bytes (the frame has frame_flag_crc)
bool check_crc(std::span<const std::byte> frame) noexcept;
// The problems of a merged dictionary (schema, literal and call conflicts; mixed ABIs), appended in order;
// `first` receives the code of the first one when it is still empty.
void dictionary_problems(const dyn::dictionary& d, std::vector<std::string>& problems,
                         std::optional<error>& first);

// "frame N @off: record M: decode error: <code>" — the problem text for a visit_record failure (verify and dump).
std::string decode_problem(const frame_info& f, const record_info& r, error e);

// 1.1 (FR-P4, formatters.cpp): renders a print frame's message. `k` must be a print call, `records` the
// frame's records in order (records[0] is the format literal). Each placeholder is formatted by std::vformat
// with the one argument, whose C++ type is picked at run time from the record's category/kind. Appends '\n'
// for println. Errors: unknown_literal (format text not in `d`), length_mismatch (fewer arguments than
// placeholders), invalid_argument (not a print call, malformed format string, spec rejected by std::format).
// 2.2.1 (1.7): the envelope of a log frame, decoded from its records (constants come from the call entry).
struct log_view
{
    std::uint8_t level{};
    std::string_view file; // the writer's __FILE__ (or "-")
    std::uint32_t line{};
    std::uint8_t mask{};
    std::uint64_t time{}; // the raw timestamp
    std::uint32_t thread{};
    std::uint64_t sequence{};
    bool has_thread{}, has_sequence{};
    std::size_t format_index{}; // the record index of the format literal
};
result<log_view> decode_log(const dyn::dictionary& d, const dyn::call& k,
                            std::span<const record_info> records);
std::string level_name(std::uint8_t level); // "trace" … "fatal", else the number
std::string time_text(
    const log_view& v); // "2026-10-07 14:03:11.123456789" for the system clock, else the number
result<std::string> render_message(const dyn::dictionary& d, const dyn::call& k,
                                   std::span<const record_info> records, bool envelope = true);

} // namespace es::serreg::internal
