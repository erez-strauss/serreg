# `include/es/serreg/print.hpp`

`print` / `println` (FR-P1..P6): binary structured logging on top of
`serialize()`. The format string is a literal (only its id is implied by the call hash; its text lives in the
dictionary), the arguments go out as ordinary records, and the *reader* does the formatting
(`tools::render_frame`, `srdump`). Nothing is formatted or allocated on the writer. The header's own
contribution is small: a compile-time check of the format string against the argument categories, and six
thin wrappers around `detail::serialize_frame` with a `print_tag`/`println_tag` at the front of the signature.
It is part of the **serialisation layer** and is reflection-dependent only through `serialize.hpp`.

## Depends on / used by

Includes: `<format>`, `<string_view>`, `<type_traits>`, `es/serreg/serialize.hpp` (which brings `arg.hpp`,
`registry.hpp`, `blob.hpp` — `print_tag`/`println_tag` are defined in `detail/blob_bytes.hpp`, reached through `blob.hpp`).

Included by: `serreg.hpp` and `router.hpp` (for `on_print`/`on_println`); used by `tests/unit/test_print.cpp`, `tests/unit/test_dynamic.cpp` (print with a
dynamic argument), `tests/golden/hashes.cpp` (`print_call_hash_v`), `tests/golden/make_fixtures.cpp`,
`tests/negative/neg_print_bad_spec.cpp`, `tests/negative/neg_print_arg_count.cpp`,
`tests/negative/neg_print_nested_field.cpp`. The typed readers
`deserialize_print`/`deserialize_println` are in `deserialize.hpp`; the renderer is `tools::render_frame`.

## What it defines

| name | kind | meaning |
|---|---|---|
| `print(S& out, lit<F>, const Ts&...)` | function template | one frame with call string `print(n){lit:<F id>,…}` |
| `println(S& out, lit<F>, const Ts&...)` | function template | the same with `println(`; the reader appends `'\n'` |
| `print_with(serialize_options, S&, lit<F>, const Ts&...)` / `println_with(...)` | function templates | with options (`full_records`, `scrub_padding`, `crc`, `canonical_unordered`) |
| `print<F>(S& out, const Ts&...)` / `println<F>(...)` | function templates | template-argument spelling: `print<"x = {}">(out, x)` |
| `print_call_hash_v<F, Ts...>`, `println_call_hash_v<F, Ts...>` | variable templates | `call_hash_v<print_tag, lit<F>, Ts...>` / `println_tag` |
| `detail::render_type<T>` / `detail::render_t<T>` | trait | the type the *reader* formats each argument as; for a constant, the type of its value |
| `detail::printable_arg<T>` | concept | `serializable_arg<T>` minus `std::byte`, `char8_t`, `char16_t`, `char32_t`, `wchar_t` (also as the value type of a constant) |
| `detail::check_format<F, Ts...>()` | `consteval` | constructs `std::format_string<render_t<Ts>...>{F.view()}` |
| `detail::no_nested_fields<F>()` | `consteval` | false when a `{` appears inside a replacement field (`{:{}}`, `{:>{}}`) |

All entry points are `noexcept(detail::sink_noexcept<S>)` and return `result<std::size_t>` (bytes written), like
`serialize`.

**The names mirror `std::print` / `std::println` on purpose.** With `namespace sr = es::serreg;`,
`sr::println(out, "px={:.2f}"_, px)` reads as what it is, and an unqualified `println(out, "…"_, px)` is found by
argument-dependent lookup through the sink argument (a `capture_file`, `span_sink`, … lives in `es::serreg`). The
overload sets never collide with the standard's: the first parameter is constrained by the `sink` concept, and a
`FILE*` or a format string is not a sink, so `std::println("{}", x)` and `println(out, "{}"_, x)` never compete even
with `using namespace std;` in scope. ("FR-P1 print/println: sr::println, println<…> and ADL println write the
same frame", `tests/unit/test_print.cpp`: the three spellings produce identical bytes.)

## How it works

### A print frame is a plain frame with a different call string

```cpp
template<sink S, fixed_string F, detail::printable_arg... Ts>
result<std::size_t> print_with(
    const serialize_options o, S& out, lit<F>,
    const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    static_assert(detail::check_format<F, Ts...>());
    static_assert(detail::no_nested_fields<F>(),
                  "print: nested replacement fields ({:{}}) are not supported by the readers; write the "
                  "width and precision into the format string");
    return detail::serialize_frame<print_tag, lit<F>, std::remove_cvref_t<Ts>...>(o, out, lit<F>{},
                                                                                  vs...);
}
```
(`print.hpp:84-95`.) `serialize_frame<Sig..., S, Ts...>` takes the *signature* pack separately from the *record*
pack: here `Sig = print_tag, lit<F>, Ts...` and the records are `lit<F>{}, vs...`. `detail/blob_bytes.hpp::call_flags<Sig...>()`
sees the form tag first and yields `call_flag_print` (or `| call_flag_newline`); `call_string` writes
`print(n){…}` / `println(n){…}` with `n = call_arity` (the tag is not counted); `make_call_args` skips the tag. The
frame bytes are therefore exactly those of `serialize(out, lit, args...)` — a compact literal record costs 0 bytes
(FR-L6), so `print(out, "hello {} {} {}"_, "new", 100, "worlds"_)` is 32 bytes: header, `string` record
(8), `i32` record (8). The call ENTRY carries the flags, so `println` costs nothing per frame (FR-P3).

`print(…)` and `println(…)` and `call(…)` over the same arguments are three different call strings, hence three
call hashes and no dictionary conflict (`tests/golden/hashes.cpp`: `print_call_hash_v<"x {}", int> ==
0x062ff32816c78e7d`, `println_call_hash_v<"x {}", int> == 0x90a1eb79c6e2eda5`).

### `render_t`: what the check is done against

```cpp
template<class T> struct render_type { using type = std::string_view; };  // literal, string, enum, struct
template<class T>
    requires(is_scalar_arg_v<T> && !std::is_same_v<T, std::byte> && !std::is_same_v<T, char8_t> &&
             !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t> && !std::is_same_v<T, wchar_t>)
struct render_type<T> { using type = T; };
// 1.5: a constant renders as the value it stands for
template<class T> using render_t = typename render_type<arg_value_t<std::remove_cvref_t<T>>>::type;
```
(`print.hpp:30-43`). Scalars render as themselves (so `{:.2f}` on a `double` and `{:#x}` on an `int` are
validated as such); everything else — literal text, string bytes, an enumerator name, a struct's one-line text
form, a dynamic type's text form — renders as a `std::string_view`. This is the contract the reader honours
in `tools::render_frame` (`src/formatters.cpp`): it splits the format string at its placeholders and formats each
one with `std::vformat` using the type the dictionary kind implies (`i64`/`u64`/`double`/`bool`/`char`/`string_view`).
Integer specs are width-independent, so a spec accepted for `int` is valid when rendered as `int64_t`.

**Constants.** `render_t` goes through `arg_value_t` (`arg.hpp`), so a `constant<V>` is checked and
rendered as a run-time `V`: `print(out, "n={} side={} px={:.2f} ok={}"_, 42_, k<Side::buy>, 2.5_,
k<false>)` compiles like the same call with run-time values, writes a 16-byte frame (the format literal and four
constants occupy no bytes) and renders as `n=42 side=buy px=2.50 ok=false`: the reader takes the values from the
call's dictionary entry. `printable_arg` applies its five exclusions to the value type as well, so `k<std::byte{7}>`
is serialisable but not printable.

The five excluded scalars are not renderable by `std::format<char>`; they are still *serialisable*
(`serializable_arg` accepts them), just not through `print` — `printable_arg` rejects them at overload
resolution.

### The compile-time check

```cpp
// FR-P2: constructing a std::format_string in a constant expression runs libstdc++'s format-string checker.
template<fixed_string F, class... Ts> consteval bool check_format()
{
    const std::format_string<render_t<Ts>...> f{F.view()};
    static_cast<void>(f);
    return true;
}
```
(`print.hpp:54`). `std::format_string<Args...>`'s constructor is `consteval` and runs the standard's checker
over placeholder count, positional indices (`{1}`), escaped braces (`{{`, `}}`) and every `{:spec}` against the
argument type. A mismatch is a hard error at the call site (the `static_assert(detail::check_format<…>())` in each
wrapper). The library wrote no checker of its own; `tests/negative/neg_print_bad_spec.cpp` (`{:.2f}` with an `int`)
and `neg_print_arg_count.cpp` (wrong placeholder count) are the try-compile guards.

Nested replacement fields — a width or precision taken from an argument, `{:>{}}` — pass `std::format_string`, but
no reader renders them: `srdump` reports `invalid_argument` and the Wireshark dissector shows the field as written.
A second `static_assert` (T-05) in `print_with` and
`println_with` rejects them at compile time:

```cpp
template<fixed_string F> consteval bool no_nested_fields()
{
    const std::string_view s = F.view();
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '{')
            continue;
        if (i + 1 < s.size() && s[i + 1] == '{')
        { // an escaped brace
            ++i;
            continue;
        }
        for (std::size_t j = i + 1; j < s.size() && s[j] != '}'; ++j)
            if (s[j] == '{')
                return false;
    }
    return true;
}
```
(`print.hpp:64-81`.) It skips escaped braces (`{{`) and fails when a `{` occurs before the `}` that closes a
replacement field. The diagnostic reads "print: nested replacement fields ({:{}}) are not supported by the readers;
write the width and precision into the format string"; `tests/negative/neg_print_nested_field.cpp` (ctest
`negative_neg_print_nested_field`) is its guard.

### Worked example: the golden print frame

`print(out, "hello {} {} {}"_, "new", 100, "worlds"_)`, compact (frame 6 of the golden `orders.ser`,
chapter 7 §7.5, `frame_size` 32):

| offset | bytes | meaning |
|---|---|---|
| 0 | `20 00 00 00` | `frame_len` = 32 |
| 4 | `00 00` | flags: compact |
| 6 | `04 00` | `n_records` = 4 (two literals at 0 bytes each) |
| 8 | 8 bytes | `print_call_hash_v<"hello {} {} {}", const char*, int, decltype("worlds"_lit)>` = `0xe97f49911b00565e` |
| 16 | `03 00 00 00 6e 65 77 00` | string: `u32 3`, `"new"`, 1 pad byte |
| 24 | `64 00 00 00 00 00 00 00` | `i32 100`, 4 pad bytes |

The call ENTRY in the dictionary (72 bytes, frozen for the smaller `"x {}"` case) says: flags = print, ARG 0
`lit:<id of "hello {} {} {}">`, ARG 1 `string`, ARG 2 `i32` (kind byte 3), ARG 3 `lit:<id of "worlds">`.
`render_frame` looks up ARG 0's text, splits it at the three `{}`, and formats `"new"` as `string_view`, `100`
as `i64`, and `"worlds"` (looked up by id) as `string_view`: `hello new 100 worlds`.

### Reader side, for orientation

- `deserialize_print(bytes, "fmt"_, outs...)` / `deserialize_println` (`deserialize.hpp`) are the typed readers:
  same `Sig = print_tag, lit<F>, Outs...`, same `validate_frame`.
- `tools::render_frame(frame, dict)` returns the message; `error::invalid_argument` if the call is not a print call
  or a spec fails at render time; `unknown_call`/`unknown_literal` when the dictionary lacks the entries.
- `srdump` prints `frame N  print 0x…` + `  print: <message>`; `--raw` shows the records; JSON adds
  `{"frame":N,"message":"…"}`.
- `dispatch`/`inspect` see a print frame as an ordinary frame; `record_view::ak` tells the category.

## Maintaining it

- **Hash-relevant:** the call-string prefixes `print(`/`println(` (`detail/blob_bytes.hpp::call_string`), `call_flags`, the
  `flags` position in the call ENTRY. Locked by `tests/golden/hashes.cpp` ("1.1 argument categories and print forms":
  three frozen hashes and the plan §3 example `0xe97f49911b00565e`) and `tests/golden/blobs.cpp` (the print call
  ENTRY: 72 bytes, hash `0x39c7bd272ba4951b`, `pb[18] == call_flag_print`). Fixture `tests/fixtures/golden/orders.ser`
  holds print/println frames rendered in `orders.text`/`orders.json`.
- **Changing `render_t`** changes what the compile-time check accepts and must be mirrored in
  `src/formatters.cpp`'s per-kind rendering — the two are a pair; a spec the writer accepted must never throw on
  the reader (the reader catches nothing; it returns `invalid_argument`, but a *mismatch* would be a silent
  wrong rendering). Add a unit case to "FR-P4 tools::render_frame formats every argument category" for any new
  kind.
- The two `static_assert`s in the `_with` overloads are what produce the diagnostics; keep `check_format` `consteval`
  (not merely `constexpr`) so it cannot be deferred to run time when a caller passes it through a non-constant
  path.
- The form tags live in `detail/blob_bytes.hpp` because `call_string`/`call_flags` need them; `touch_arg<print_tag>()` is a
  no-op (`registry.hpp:296`) — do not give tags a registry entry.
- `print` with a dynamic argument works because `render_t<DynMsg>` is `string_view` and
  `serialize_frame` accepts `dynamic_type` records; the reader renders it as the one-line text form.
- Tests: `tests/unit/test_print.cpp` ("FR-P1 print frames are plain frames with a print call hash", "FR-P4
  tools::render_frame formats every argument category", "FR-P5 srdump text renders print frames, --raw shows
  records, JSON adds message", "FR-P1 deserialize_print / deserialize_println are the typed readers"),
  "FR-P1 print/println: sr::println, println<…> and ADL println write the same frame"),
  `tests/unit/test_dynamic.cpp` ("2.0 tools: dump text/JSON of dynamic values and print with a dynamic argument"),
  `tests/negative/neg_print_*.cpp` (ctest `negative_neg_print_bad_spec`, `negative_neg_print_arg_count`,
  `negative_neg_print_nested_field`).
