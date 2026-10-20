// es/serreg/error.hpp — error codes for es::serreg (spec §6.4). No exceptions anywhere in the library;
// every fallible operation returns std::expected<T, error>.
//
// `error` is only ever an error (there is no "ok" enumerator, v1.1.0). "Ok or an error" is a `status`
// (= std::expected<void, error>): what a sticky sink reports, what a fold over many steps accumulates.
#pragma once
#include <cstdint>
#include <expected>
#include <string_view>

namespace es::serreg
{

enum class error : std::uint8_t
{
    sink_full = 1, // sink refused bytes / claim() returned nullptr
    truncated,     // buffer shorter than a header/record claims
    bad_magic,
    bad_version,
    endianness_mismatch,
    hash_mismatch,   // call/schema hash differs from the expected one
    length_mismatch, // record/frame length inconsistent with dictionary or buffer
    unknown_type,    // hash not present in registry/dictionary
    unknown_literal,
    unknown_call,
    corrupt_dictionary, // RRI/ENTRY stream malformed
    misaligned,         // zero-copy view impossible at this address
    schema_conflict,    // FR-R4: same schema hash, different layout hash in the process
    io_error,
    not_found,   // ELF section / file missing
    unsupported, // feature not available in this build/version (e.g. dynamic members in v1)
    invalid_argument,
    abi_mismatch, // 1.3: dictionaries of different writer ABIs mixed in one capture (FR-W4)
    invalid_value, // 2.1.3: bytes the C++ type cannot hold (a valueless variant without std::monostate, a
    // non-canonical bool through a zero-copy view); the rest of the frame is still decoded
    crc_mismatch, // 1.7 (2.2.1): a frame's CRC32C trailer does not match its bytes; the frame is skipped
};

template<class T> using result = std::expected<T, error>;
using status = result<void>; // status{} is ok; std::unexpected(e) is the error e

constexpr std::string_view to_string(const error e) noexcept
{
    switch (e)
    {
    case error::sink_full:
        return "sink_full";
    case error::truncated:
        return "truncated";
    case error::bad_magic:
        return "bad_magic";
    case error::bad_version:
        return "bad_version";
    case error::endianness_mismatch:
        return "endianness_mismatch";
    case error::hash_mismatch:
        return "hash_mismatch";
    case error::length_mismatch:
        return "length_mismatch";
    case error::unknown_type:
        return "unknown_type";
    case error::unknown_literal:
        return "unknown_literal";
    case error::unknown_call:
        return "unknown_call";
    case error::corrupt_dictionary:
        return "corrupt_dictionary";
    case error::misaligned:
        return "misaligned";
    case error::schema_conflict:
        return "schema_conflict";
    case error::io_error:
        return "io_error";
    case error::not_found:
        return "not_found";
    case error::unsupported:
        return "unsupported";
    case error::invalid_argument:
        return "invalid_argument";
    case error::abi_mismatch:
        return "abi_mismatch";
    case error::invalid_value:
        return "invalid_value";
    case error::crc_mismatch:
        return "crc_mismatch";
    }
    return "unknown_error";
}

// ---- what a typed reader reports on failure (2.1.15, decision C) -----------------------------------------
// deserialize / deserialize_print / deserialize_println decode in one pass and say how far they got. Items are
// counted from the frame itself: item 0 is the frame (header, call hash, number of records), item i is argument i
// (1-based; literals and constants are arguments).
//   expected  1 + the number of arguments of the call
//   actual    0 when the frame itself was refused; otherwise 1 + the number of arguments decoded into their
//             outputs. A success would be actual == expected (success returns the bytes consumed instead).
//   failed    the first item that failed: 0 = the frame, i = argument i
// When the frame is accepted but a record's length or hash is wrong, nothing has been written (actual == 1,
// failed == that record). A failure inside a record (a dynamic image that does not decode) leaves the arguments
// before it complete, that one valid but unspecified, the later ones untouched. `invalid_value` does not stop
// decoding: that output keeps its old value, the later ones are still read, and `failed` names the first one.
// A decode_error converts to and compares with its `error` code, so `r.error() == error::truncated` and
// `return std::unexpected(r.error())` from a function returning result<T> keep working.
struct decode_error
{
    error code{};
    std::uint16_t expected = 0;
    std::uint16_t actual = 0;
    std::uint16_t failed = 0;
    constexpr operator error() const noexcept // NOLINT(google-explicit-constructor): see above
    {
        return code;
    }
    friend constexpr bool operator==(const decode_error&, const decode_error&) noexcept = default;
    friend constexpr bool operator==(const decode_error& a, const error b) noexcept
    {
        return a.code == b;
    }
};
template<class T> using decode_result = std::expected<T, decode_error>;
constexpr std::string_view to_string(const decode_error& e) noexcept
{
    return to_string(e.code);
}

// ---- warnings (2.1.3, review G2-12) -------------------------------------------------------------------------
// A warning is input that a reader repairs instead of rejecting. Today there is one: a bool byte other than 0/1
// (only a corrupt file or a writer that skipped scrubbing produces it) is read as `true`. Nothing is reported
// unless the application installs a handler; it runs on the decoding thread, once per decoded value that needed a
// repair, with the value's schema hash (0 for a top-level bool argument) and how many bytes were repaired. It
// must not throw. tools::verify reports the same condition in verify_report::warnings.
enum class warning : std::uint8_t
{
    noncanonical_bool = 1,
};
constexpr std::string_view to_string(const warning w) noexcept
{
    switch (w)
    {
    case warning::noncanonical_bool:
        return "noncanonical_bool";
    }
    return "unknown_warning";
}
using warning_handler = void (*)(warning w, std::uint64_t schema_hash,
                                 std::uint32_t count) noexcept;
// Installs `h` (nullptr: none, the default) process-wide and returns the previous handler. Thread-safe.
warning_handler set_warning_handler(warning_handler h) noexcept;
namespace detail
{
// Called by the decoders; a no-op without a handler. Out of line and cold: only corrupt input reaches it.
void report_warning(warning w, std::uint64_t schema_hash, std::uint32_t count) noexcept;
} // namespace detail

} // namespace es::serreg
