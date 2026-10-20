# `include/es/serreg/error.hpp`

The error vocabulary of the library: one `enum class error`, the `result<T>` alias every fallible function
returns, the `status` alias for "ok or an error", and `to_string(error)`. It also declares the
**warnings**: input a reader repairs instead of rejecting, reported to an optional process-wide handler, and it
defines `decode_error`, what the typed readers return on failure: the code plus how far the reader got. The
library throws nothing and has no RTTI; this header is how failure travels. It is shared by every layer (wire format, registry, readers, tools),
has no reflection dependency, and includes nothing from the library.

## Depends on / used by

Includes: `<cstdint>`, `<expected>`, `<string_view>`.

Included by: `sink.hpp` (`status` returned by `detail::write_fd` and `fd_sink::state()`), `serialize.hpp`,
`deserialize.hpp`, `registry.hpp`, `dictionary.hpp`, `tools.hpp`, `src/internal.hpp`, and transitively by every
other header. `capture.hpp` and `print.hpp` get it through `serialize.hpp`.

## What it defines

| name | kind | meaning |
|---|---|---|
| `error` | `enum class : std::uint8_t` | the 19 failure codes, values 1…19 (the last, `crc_mismatch`, is format 1.7's) (no zero, no "ok") |
| `result<T>` | alias template | `std::expected<T, error>` |
| `status` | alias | `result<void>`; `status{}` is ok, `std::unexpected(e)` is the error `e` |
| `to_string(error)` | `constexpr` function | the enumerator's name; `"unknown_error"` for a value outside the enum |
| `decode_error` | struct | `error code; std::uint16_t expected, actual, failed` — what `deserialize`, `deserialize_print` and `deserialize_println` return on failure; converts implicitly to `error`, compares with a `decode_error` and with an `error` |
| `decode_result<T>` | alias template | `std::expected<T, decode_error>` |
| `to_string(const decode_error&)` | `constexpr` function | `to_string(e.code)` |
| `warning` | `enum class : std::uint8_t` | `noncanonical_bool = 1`: a `bool` byte other than 0/1 was read as `true` (review G2-12) |
| `to_string(warning)` | `constexpr` function | `"noncanonical_bool"`; `"unknown_warning"` otherwise |
| `warning_handler` | function pointer | `void (*)(warning, std::uint64_t schema_hash, std::uint32_t count) noexcept` |
| `set_warning_handler(h)` | function (in `libserreg`, `src/decoder.cpp`) | installs `h` process-wide (atomic; `nullptr` = none, the default) and returns the previous one |
| `detail::report_warning(w, hash, n)` | function (in `libserreg`) | what the decoders call; cold, a no-op without a handler |

The codes, with the situations the code base uses them for:

| code | raised when |
|---|---|
| `sink_full = 1` | a plain sink's `write()` returned false, or `claim()` returned `nullptr`, and the sink has no `state()` error to report instead |
| `truncated` | buffer shorter than a header/record claims (`read_frame_header`, `inspect`, `get_record`, `deserialize_raw`, `add_rri`) |
| `bad_magic` | FILE or RRI magic mismatch |
| `bad_version` | major differs, minor outside `fmt::format_minor_oldest`…`fmt::format_minor` (1.6 and 1.7; older minors carry other names or FNV-1a ids, newer ones are not guessed at), or unknown frame flags |
| `endianness_mismatch` | a FILE header without `file_flag_le` (`src/reader.cpp:600`); no writer produces one — the static_assert in `format.hpp` limits the library to little-endian hosts |
| `hash_mismatch` | call hash (compact) or per-record hash (full) differs from the expected signature; `view<T>` on the wrong type; a literal id that does not match in a full record; `deserialize` handed a dictionary frame; a full-records constant whose value is not the one the reader names (`deserialize`) or not the one in the call's entry (`tools::verify`) |
| `length_mismatch` | frame_len not a multiple of 8 or below the header; `n_records` ≠ arity; record length ≠ `sizeof`; a corrupt dynamic image (a slot that breaks `fmt::image_budget`'s rules — outside the image, into the fixed part, `n = 0` with `off ≠ 0`, over the byte budget — depth > 64, bad optional/variant tag) |
| `unknown_type` | a compact type record whose schema hash is in neither the registry nor the side dictionary (its size is unknown, the frame cannot be walked); an enum constant whose enum is unknown |
| `unknown_literal` | `render_frame` with a format literal that is not in the dictionary |
| `unknown_call` | compact frame whose call hash resolves nowhere |
| `corrupt_dictionary` | ENTRY stream malformed (`dyn::dictionary::add_entries`/`add_rri`); more than `decode_limits::dictionary_entries` entries declared |
| `misaligned` | `view<T>` when the payload address is not `alignof(U)`-aligned |
| `schema_conflict` | FR-R4 code; the hot path never returns it. The walkers use it as the `first_error` of a capture whose merged dictionary has two bodies for one schema hash, literal id or call hash |
| `io_error` | `write(2)`/`fwrite`/`fopen`/`fseek` failure (`fd_sink`, `capture_file`, `write_file`, `export_image(fd)`); also `tools::dump` when the `write_fn` callback returns false |
| `not_found` | ELF section or file missing |
| `unsupported` | feature not in this build: `hash_algo` ≠ 2 in a FILE or RRI header, `read_file` above `decode_limits::input_bytes` (1 GiB), a FILE's dictionary above `decode_limits::dictionary_bytes`, a back-patched FILE on a pipe, `dictionary::record_size` of a string argument (its size is per record) |
| `invalid_argument` | a > 4 GiB string or image (frame would overflow `u32 frame_len`), `render_frame` on a frame whose call is not a print call, or a bad format spec at render time |
| `abi_mismatch` | format 1.3 (FR-W4): dictionaries from two different writer ABIs in one capture — the `first_error` of a walk that found them, reported by `verify` as an `abi:` problem |
| `invalid_value` | bytes the C++ type cannot hold — a valueless variant (`n = 0`) read into a variant without `std::monostate` (`deserialize`, `dispatch`, `walk_values`' typed half: the rest of the frame is still read), or a `view<T>` over a non-canonical `bool` byte (a view cannot repair in place); with the range check (decision D): a leaf of an enum *without* a fixed underlying type outside the range of its enumerators' minimal-width integer — the leaf reads as 0; `deserialize` continues and names the argument in `decode_error`, `dispatch` does not deliver the value, `view` refuses it, `deserialize_raw` and `walk_values` report it |
| `crc_mismatch` | format 1.7: a frame with the CRC flag whose trailer does not match `crc32c` of its bytes. Typed readers (`deserialize`, `inspect`, `view`, `dispatch`) return it before reading anything; the walkers and `verify` report the problem `crc mismatch (frame skipped)` and continue with the next frame — the frame is never decoded nor delivered (`error.hpp:36`) |

### Warnings

A warning never changes a result: the value is repaired and the call succeeds. Only one exists. A `bool`
member, array element or argument whose byte is not 0 or 1 (a corrupt file, or a writer that skipped scrubbing)
would be an invalid object representation if copied as is, so every typed read (`get_object`, the flat leaves of
`decode_image`, `deserialize` of a `bool` argument) sets it to 1 and calls `detail::report_warning` once per
decoded value with the number of repaired bytes and the value's schema hash (0 for a top-level `bool`). The
dictionary-driven tools report the same thing to their visitor (`decoded_value_visitor::noncanonical_bool`), and
`tools::verify` lists it in `verify_report::warnings`, which does not make the report fail. `view<T>` cannot
repair bytes it does not own and returns `invalid_value` instead; `dispatch` then falls back to a repaired copy.

## How it works

### `error` vs `status`

`error` has no `ok` enumerator; the header comment states the rule:

```cpp
// `error` is only ever an error (there is no "ok" enumerator, v1.1.0). "Ok or an error" is a `status`
// (= std::expected<void, error>): what a sticky sink reports, what a fold over many steps accumulates.
```
(`error.hpp:4`). The consequences are visible in three places:

1. **Sticky sink state.** `fd_sink::state()` and `capture_file::state()` return `status`; an ok sink is `st_` with
   `has_value()`, a failed one holds the code. `serialize.hpp`'s `sink_failure(s)` reads it: an ok state together
   with a refused write is plain `sink_full`; anything else is the sink's own error:

   ```cpp
   const status st = s.state();
   return st ? error::sink_full : st.error(); // ok state + refused write = plain sink_full
   ```
   (`serialize.hpp:107`).

2. **Fold accumulators.** Record loops in `deserialize.hpp` (`validate_frame`, `deserialize_frame`, `dispatch`)
   declare `status e;` and run each step only `if (e)`, so the first failure wins and later steps are skipped
   without an early return from a fold expression.

3. **`detail::write_fd`** (`sink.hpp:108`) returns `status` and is the single write loop of the header-only part;
   `tools::write_all` is its counterpart in `libserreg`.

`result<T>` is used for everything with a value: `result<std::size_t>` (bytes written) from every
`serialize*` and (bytes consumed) from `deserialize_raw` and `dispatch`, `result<frame_view>` from `inspect`, `result<const U*>` from `view<T>`,
`result<dyn::dictionary>` from the loaders, `result<verify_report>` from `verify`. The typed readers
`deserialize`, `deserialize_print` and `deserialize_println` return `decode_result<std::size_t>`
(next section).

### `decode_error`: expected versus actual (decision C)

```cpp
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
```
(`error.hpp:101-117`). A typed reader decodes in one pass and, when it fails, says how far it got. Items are
counted from the frame itself: item 0 is the frame (its header, call hash and number of records), item *i* is
argument *i*, counted from 1. Literals and constants are arguments, and for the print readers the format literal
is argument 1.

| field | meaning |
|---|---|
| `code` | the `error` |
| `expected` | 1 + the number of arguments of the call |
| `actual` | 0 when the frame itself was refused; otherwise 1 + the number of arguments decoded into their outputs |
| `failed` | the first item that failed: 0 = the frame, *i* = argument *i* |

The four cases (`deserialize_frame`, `files/include-es-serreg-deserialize.hpp.md`):

| what happened | `{code, expected, actual, failed}` | outputs |
|---|---|---|
| the frame is refused (header, call hash, number of records) | `{code, expected, 0, 0}` | nothing written |
| the frame is accepted, record *i* has the wrong length or hash, or runs past the frame | `{code, expected, 1, i}` | nothing written: this is found before any output is touched |
| record *i* fails while it is decoded (a dynamic image that is well-formed outside and not inside) | `{code, expected, i, i}` | arguments before *i* complete, argument *i* valid but unspecified, later ones untouched |
| `invalid_value` on one or more arguments | `{invalid_value, expected, expected − their number, first of them}` | those outputs keep their old values, all others are read |

Success is unchanged: the bytes consumed. A success would be `actual == expected`; the struct is not returned then.

The implicit conversion and the mixed `operator==` keep existing code compiling: `r.error() == error::truncated`,
`to_string(r.error())` and `return std::unexpected(r.error());` from a function that returns `result<T>` work as
before. Code that spelled the return type as `result<std::size_t>` or `std::expected<std::size_t, error>` must
write `decode_result<std::size_t>` or `auto`. `deserialize_raw`, `dispatch`, `inspect` and `frame_router` still
report a plain `error`.

### Error flow through a write

`serialize(out, …)` returns `result<std::size_t>`. The only run-time failures on the writer side are the sink
refusing bytes and an oversized runtime payload:

```cpp
std::byte* const p = out.claim(total);
if (!p)
    return std::unexpected(detail::sink_failure(out));   // sink_full, or out.state().error()
...
if (frame_size(serialize_options{.full_records = true}, vs...) > 0xFFFFFFFFull)
    return std::unexpected(error::invalid_argument);     // a > 4 GiB string or image
```
(`serialize.hpp:448`, `:364`). Everything else that could go wrong with a *type* is a compile-time
`static_assert` (FR-S7), which is why the writer path needs no other codes.

### Error flow through a read

`deserialize` checks in a fixed order and returns the first failure: `read_frame_header` (`truncated`,
`length_mismatch`, `bad_version`) → `validate_frame` (`hash_mismatch` for a dictionary frame or a wrong signature,
`length_mismatch` for arity/size, `truncated` for a record that runs past the frame; in a full-records frame also
`hash_mismatch` for a constant whose value differs) → `get_record` per
output (`truncated` for a string/image running past the frame, `length_mismatch` for a corrupt image,
`hash_mismatch` for a full-mode literal whose id differs, `invalid_value` for a valueless variant the output
type cannot hold). The fold accumulator pattern, with the counters:

```cpp
status e, soft;
std::uint16_t item = 0, done = 1; // done: the frame, then every argument decoded completely
failed = 0;
auto one = [&](auto& out) noexcept(decode_noexcept<Ts...>) {
    if (!e)
        return;
    ++item;
    e = detail::get_record(p, end, out, h->full());
    if (e)
    {
        ++done;
        return;
    }
    if (failed == 0)
        failed = item;
    if (e.error() == error::invalid_value)
    {
        if (soft)
            soft = e;
        e = {};
    }
};
```
(`deserialize.hpp:768-789`). `e` starts ok; the first failing step stores its error and every later step is
skipped. `done` becomes `decode_error::actual` and `failed` the first item whose `get_record` did not succeed.
`invalid_value` is the one soft error: it is kept in `soft`, the remaining outputs are still read,
and it is returned at the end. The same shape appears in `validate_frame` and `dispatch` (`bad`; `dispatch`
already went on with the next record after any failure).

### How the tools surface errors

`srdump` maps codes to exit status: 0 ok, 1 for verify problems or any `tools::` error (printed with
`to_string(e)` under the `app::name` prefix), 2 for usage. `tools::verify` does not return most codes — it
collects them as text `problems` and returns `ok()` false; only container-level failures (`bad_magic`,
`bad_version`, `truncated` on the header, `corrupt_dictionary`) come back as `std::unexpected`. A `dump` whose
`write_fn` returns false stops with `io_error`.

### `to_string`

A `switch` with a `default`-less body and a trailing `return "unknown_error";` — so a value that is not an
enumerator (possible after a memcpy from the wire) does not fall through undefined. Used by `capture_file`'s
destructor diagnostic, `srdump`'s messages and the tests' `CHECK` output.

## Maintaining it

- **Appending a code is safe; renumbering is not.** The values are `uint8_t` and are printed by name, never
  written to the wire, so adding at the end changes nothing on disk. Reordering changes the numeric value a
  test or an embedder may have logged, and `to_string` must be extended in the same commit (the switch has no
  default on purpose: `-Wswitch` will tell you).
- **Keep `error` free of an ok value.** Code written after 1.1 assumes `status` for "maybe ok"; reintroducing an
  `ok` enumerator would silently make `if (st)` checks wrong in `sink_failure`.
- **Do not widen the type.** `status` sits inside `fd_sink` and `capture_file`; `std::expected<void, uint8_t-enum>`
  is two bytes, which keeps those sinks small.
- **Which code where.** The table above is the de-facto contract tested by name in the unit tests; when a new
  check is added pick the existing code that the reader tests already expect for that class of problem
  (`truncated` for "not enough bytes", `length_mismatch` for "bytes present but inconsistent", `bad_version` for
  "well-formed but from a newer writer") rather than a new one.
- **`decode_error` must stay convertible to `error`**: callers forward it into `result<T>` and compare it
  with a code. Do not make the conversion `explicit`, and do not add a constructor that would stop
  `decode_error{code, expected, actual, failed}` from being an aggregate initialisation (the tests compare against
  such values).
- Tests that pin codes: `tests/unit/test_roundtrip.cpp` ("FR-D5 truncated buffers are rejected", "FR-S4 deserialize
  with the wrong signature fails before touching outputs", "FR-S4 a failure reports expected items, decoded items
  and the first that failed", "FR-S2 span_sink exhaustion reports sink_full and writes
  nothing"), `tests/unit/test_sinks.cpp` ("gk 1.9 fd_sink: io_error is sticky until reset(), never sink_full",
  "gk 1.8 capture_file: open failure is sticky…"), `tests/unit/test_readers.cpp` ("FR-S4 view<T>() … misaligned
  otherwise", "gk 1.1 dispatch rejects short full records"), `tests/unit/test_dynamic.cpp` ("2.0 corrupt images are
  rejected, never over-read"; 2.1.3: "G2-6 … both reject it", "G2-6 a valueless variant: std::monostate when the
  variant has one, else invalid_value", "G2-12 a bool byte other than 0/1 reads as true, with an optional
  warning", "G2-10 dump returns the walker's error code, not always length_mismatch"; 2.1.15: "FR-S4 a record that fails
  while it is decoded: expected, actual and the failed item"),
  `tests/unit/test_print.cpp` ("FR-H5 captures of an older minor or hash algorithm are refused, not guessed at"),
  and the fuzz targets
  (`tests/fuzz/fuzz_targets.cpp`), which require every code path to return one of these instead of crashing.
