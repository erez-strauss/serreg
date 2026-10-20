# `src/internal.hpp`

The private header shared by the `src/*.cpp` files of `libserreg` (spec §6.4, §6.9). It is not installed and
nothing in it is public API. It declares the library's write loop (`internal::write_all`), the input cap
(`max_input_bytes`, and `input_limit()` over `decode_limits::input_bytes`), small text helpers used by every renderer (`hex64`, `hex8`, `dec`, `append_quoted`,
`parse_u64`), the capture-walking contract between `reader.cpp` and its clients (`frame_info`, `record_info`,
`frame_fn`, `record_fn`, `walk_result`, `for_each_record`, `for_each_record_in_file`, `record_source`
and `add_problem`, `decode_problem`, the FILE header parser and `abi_text`) and the 1.1 print-rendering
primitive `render_message` implemented in `formatters.cpp`. It has no reflection dependency and defines no
state: everything is an inline function, a struct, or a declaration whose definition lives in one `.cpp`.

## Depends on / used by

Includes: `<charconv>`, `<cstddef>`, `<cstdint>`, `<functional>`, `<optional>`, `<span>`, `<string>`,
`<string_view>`, `<vector>`, `es/serreg/dictionary.hpp`, `es/serreg/error.hpp`, `es/serreg/format.hpp`,
`es/serreg/limits.hpp` and `es/serreg/tools.hpp` (`get_decode_limits`, `tools::block_options`).

Included by: `src/cli.cpp`, `src/decoder.cpp`, `src/dictionary.cpp`, `src/diff.cpp`, `src/elf_reader.cpp`,
`src/file_io.cpp`, `src/formatters.cpp`, `src/reader.cpp`, `src/wireshark.cpp` (`dec`, `hex64`). (`src/registry.cpp` does not include it; it uses the public
`tools::write_all` from `tools.hpp` instead.) Definitions: `internal::write_all` in `file_io.cpp`;
`for_each_record`, `for_each_record_in_file`, `add_problem` and `decode_problem` in `reader.cpp`; `render_message` in `formatters.cpp`.

## Structure

- **`write_all(fd, data, size)`** (`internal.hpp:26`): declaration of the write loop — retries `EINTR`,
  treats a zero-length write as `io_error`; same function as `tools::write_all`.
- **`max_input_bytes = 1 << 30`**, **`input_limit()`** (`internal.hpp:30-35`): the `read_file`/`slurp` cap (gk 1.15:
  `min(get_decode_limits().input_bytes, max_input_bytes)`).
- **`hex64(v)`** (`internal.hpp:38-45`): `"0x"` + 16 lower-case digits, the canonical rendering of every 64-bit
  wire id; **`hex8(v)`** (`internal.hpp:47-51`): two digits.
- **`dec(v)`** (`internal.hpp:53-58`): `std::to_chars` into a 32-byte buffer → `std::string`.
- **`abi_text(tag)`** (`internal.hpp:61-70`): a 1.3 ABI tag as `ptr8 long8 wchar4 char-signed max-align16
  ldbl16 le` (or `unknown(version N)`), for the `abi` line of a dump.
- **`utf8_sequence(s, i)`** (`internal.hpp:73-95`): the length of the well-formed UTF-8 sequence at
  `s[i]` (RFC 3629: no overlong forms, no surrogates, at most U+10FFFF), or 0.
- **`append_quoted(out, s)`** (`internal.hpp:100-149`): JSON string quoting that is also a valid TOML basic
  string; its output is valid UTF-8.
- **`parse_u64(s, out)`** (`internal.hpp:152-164`): `0x…` or decimal, whole-string, `false` on garbage.
- **`file_header`, `parse_file_header(first_48_bytes)`** (`internal.hpp:169-175`): the FILE header parsed
  and checked once for both capture walkers and `tools::info` — magic, format 1.6 or 1.7, the
  little-endian flag, `hash_algo` 2 —
  returning `program_hash`, `dict_offset`/`dict_len` and the ABI tag.
- **Capture walking** (`internal.hpp:177-258`): `frame_info`, `record_info`, `frame_fn`, `record_fn`,
  `walk_result` (`program_hash`, `problems_omitted`), `for_each_record`; (A6)
  `for_each_record_in_file` (the same walk over a file or `"-"`, window by window), `record_source` (a
  `std::function` over either walk, what `verify` and `dump` are written against) and `add_problem` (append a
  problem within the limit, or count it); (review G2-9) `frame_class` + `classify_frame` (one reading of a frame
  header for both walkers: `ok`, `bad_len`, `unknown_flags`) and `dictionary_problems` (schema, literal and call
  conflicts and mixed ABIs of a merged dictionary, as problem text); `decode_problem`. Format 1.7:
  `frame_info::has_crc`, `is_checkpoint`, `is_info`, `crc_ok` (`internal.hpp:186-187`), and `check_crc()` (`internal.hpp:255`), the
  trailer check both walkers call.
- **Log frames** (`internal.hpp:270-285`): `log_view` (level, file, line, mask, time, thread, sequence, the
  index of the format literal), `decode_log(d, k, records)` (the envelope at `log.hpp`'s `log_layout` positions),
  `level_name(level)` (`"trace"` … `"fatal"`), `time_text(view)` (`YYYY-MM-DD HH:MM:SS.nnnnnnnnn` UTC for the system
  clock, the number otherwise); implemented in `src/formatters.cpp`.
- **`render_message`** (`internal.hpp:286-287`): the print-frame renderer; `envelope` (default true) puts a
  log frame's `<time> <LEVEL> <file>:<line> [tid] #seq` prefix before the message (JSON output passes false and
  writes the envelope as fields).

## How it works

### The text helpers

```cpp
// "0x" + 16 lower-case hex digits (canonical rendering of every 64-bit wire ID).
inline std::string hex64(const std::uint64_t v)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s("0x0000000000000000");
    for (int i = 0; i < 16; ++i)
        s[static_cast<std::size_t>(17 - i)] = digits[(v >> (4 * i)) & 0xf];
    return s;
}
```

(`internal.hpp:37-45`.) Fixed width, lower case, always prefixed — this is what appears in `to_json`,
`to_toml`, `dict_text`, `diff_report::to_text`, every problem string and `srdump --program-hash`, and what
`parse_u64` accepts back for `--type 0x…` and `--frames`. `dec` is the integer counterpart (no locale, no
`std::to_string`).

```cpp
default:
    if (u < 0x20 || u == 0x7f)
    {
        out += "\\u00";
        out += hex8(u);
    }
    else
        out += c;
```

(`internal.hpp:100-149`.) `append_quoted` escapes `"`, `\`, `\n`, `\r`, `\t`, and every other control
character (and DEL) as `\u00NN`. Because the same escapes are legal in a TOML basic string, one function serves
both canonical renderings (FR-W5).

Bytes from 0x80 up are checked (D-08/T-06), so the output is always valid UTF-8:

```cpp
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
```
(`internal.hpp:109-120`.) A well-formed sequence (`utf8_sequence` returns its length, 2 to 4) is copied whole; any
other byte — a stray continuation byte, a truncated or overlong sequence, a surrogate, 0xF5..0xFF — is written as
`\u00XX`, the byte's value as a code point, so nothing is lost. Before, every byte ≥ 0x80 passed through, and a
name or literal that was not UTF-8 made the JSON (and TOML) invalid. `runtime_selftest.cpp`
`review_2_1_10_decoders()` renders `"caf\xc3\xa9 \xff\xfe d\xc3\x28"` as `café \u00ff\u00fe d\u00c3(`.
The text visitor's `quote_chars` in `formatters.cpp` is a different, human-oriented quoting (`\0`, `\xNN`)
and is deliberately not this function.

### `frame_info` and `record_info`

```cpp
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
```

(`internal.hpp:188-206`.) `frame_info` carries what the frame header says (`index`, `offset` of the header in
the capture, `frame_len`, `flags`, `n_records`, `call_hash`) plus the derived `is_full`/`is_dict`.
`record_info` is the walker's per-record hand-off: the 1.1 argument category `ak` (`arg_type`, `arg_literal`,
`arg_scalar`, `arg_string`), the scalar `kind` for `arg_scalar`, the dictionary `type` pointer (valid for the
life of the `walk_result::dict` / the walker's dictionary that produced it), and `payload` with every wire
prefix already stripped — for a string record the `u32 len`, for a v2 dynamic record the `u32 len, u32 0`
image prefix — so consumers see just the bytes. `text()` is the string view for `arg_string` records.
`is_const` (format 1.5) marks a compile-time constant of the call. Such a record never has
`ak == arg_const`: the walker sets `ak` to `arg_scalar` (with `k`) or `arg_type` (with `type`, an enum), and in a
compact frame `payload` views the `u64 value` of the `dyn::call_arg` in the walker's dictionary instead of bytes of
the capture. Consumers that only render or count need not look at the flag.

### Callbacks and `walk_result`

```cpp
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
```

(`internal.hpp:208-221`.) The dictionary passed to a callback is the walker's *current* dictionary — a
dictionary frame later in the capture may add to it. `walk_result::dict` is the final state, which is why
`tools::dump` does a first pass just to obtain it. `problems` are human strings, `first_error` the code of
the first; the list is capped at `decode_limits::problems` and `problems_omitted` counts the rest.
`for_each_record` returns `std::unexpected` only for container-level failures (see
`src/reader.cpp`).

### `for_each_record`, `for_each_record_in_file`, `record_source`, `add_problem`, `decode_problem`

```cpp
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
```

(`internal.hpp:225-240`.) The entry points to capture walking: over a span, and over a file read
window by window. `extra` is merged over the embedded
dictionary (a bare stream needs it, or a leading dictionary frame); `stop_on_problem` selects `verify`'s
"report everything" (`false`) or `dump`/`render_frame`'s "stop at the first" (`true`). `record_source` is the
shape both fit, so `verify_source` (`reader.cpp`) and `dump_source` (`formatters.cpp`) are written once and
instantiated for a span or a path. One difference a client sees: over a file, `on_frame` never sees a dictionary
frame (`is_dict` is always false) and a record's `payload` is valid only during the callback. `add_problem` is
the one way a problem line enters a list, so the cap holds everywhere. `decode_problem` (`internal.hpp:262`) keeps
the wording of decode failures identical between `verify` and `dump`.

### `render_message`

```cpp
// 1.1 (FR-P4, formatters.cpp): renders a print frame's message. `k` must be a print call, `records` the
// frame's records in order (records[0] is the format literal). Each placeholder is formatted by std::vformat
// with the one argument, whose C++ type is picked at run time from the record's category/kind. Appends '\n'
// for println. Errors: unknown_literal (format text not in `d`), length_mismatch (fewer arguments than
// placeholders), invalid_argument (not a print call, malformed format string, spec rejected by std::format).
result<std::string> render_message(const dyn::dictionary& d, const dyn::call& k,
                                   std::span<const record_info> records, bool envelope = true);
```

(`internal.hpp:263-287`; the log declarations sit between the comment and the function.) Shared by `tools::dump` (print frames in text/JSON) and `tools::render_frame`;
see `src/formatters.cpp` for the algorithm. The public primitive is `tools::render_frame` and this internal function is its core (FR-P4: no public per-record
variant is needed).

## Maintaining it

- **This header is the contract between `reader.cpp` and everything that consumes records.** A new record
  category (a new `arg_kind`) or a new prefix means a `record_info` field or a stripping rule here, plus both
  cutters in `reader.cpp` and the renderers in `formatters.cpp`; and it is a wire-format change.
- **Do not add state.** No globals, no `static` objects; the header must stay safe to include from any TU
  and free of initialisation order.
- **Do not make it public.** Anything an embedder needs goes through `tools.hpp`; `record_info` and
  `walk_result` are allowed to change between versions.
- **`hex64`/`dec`/`append_quoted` output is frozen by the golden fixtures** (`orders.json`, `orders.toml`,
  `orders.text`, `dynamic.*`, and the corpus captures). Changing case, width or escapes breaks them. The golden
  text fixtures are ASCII, so the UTF-8 check does not touch them.
- **`write_all` has one definition** (`file_io.cpp`); keep this declaration and `tools::write_all` in
  sync, both `noexcept`.
- **Tests:** indirectly everything in `test_tools.cpp`, `test_print.cpp`, `test_dynamic.cpp`,
  `runtime_selftest.cpp`, the fixture-compare ctests and `fuzz_capture`/`fuzz_rri`/`fuzz_config`;
  `parse_u64` is exercised by `srdump --type 0x…` and `--frames`.
- **Allocation / threading:** the helpers allocate `std::string`s (tools layer); all are pure functions;
  `std::function` callbacks are invoked on the walking thread only.
