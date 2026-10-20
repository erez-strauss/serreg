# `include/es/serreg/literal.hpp`

Compile-time string literals as serialisable arguments (spec §4.4, §6.6, FR-L1..L6): the `lit<S>` type, the
four spellings that produce it, the literal ENTRY blob, and the `is_literal_v` trait. A literal is an *interned*
argument — its text is registered once per process and, since format 1.1, nothing at all crosses the wire for it
in a compact frame; the call hash names it. This header sits between the **wire-format layer** and the
**registry** (it builds the ENTRY blob but does not register it — `registry.hpp::registrar_lit` does). It has
**no reflection dependency**.

The header also defines (format 1.5, FR-A5) **compile-time constants**: `constant<V>`, the variable
template `k<V>` and a numeric literal operator `_` (`42_`, `-7_`, `1.5_`). A constant is to a number what `lit<S>`
is to a string: the value is in the type, so it is part of the call signature and costs no bytes in a compact
frame. This header only builds the type; `arg.hpp` classifies it and `detail/blob_bytes.hpp` writes it into the
call string and the call ENTRY.

## Depends on / used by

Includes: `<array>`, `<cstddef>`, `<cstdint>`, `<string_view>`, `<type_traits>`, `<vector>`, `es/serreg/descriptor.hpp` (for
`ES_SERREG_HIDDEN` only), `es/serreg/format.hpp` (`cbuf`, `to_blob_array`, `tag_literal`, `literal_text_offset`),
`es/serreg/hash.hpp` (`fixed_string`, `fingerprint64`).

Included by: `arg.hpp` (`is_literal_v` selects the `arg_literal` category, `is_constant_v` the `arg_const` one), `blob.hpp` (`literal_blob_v`),
`detail/blob_bytes.hpp` (`is_literal_v`), `detail/embed_elf.hpp` (`literal_blob_copy_v`) and `detail/embed_none.hpp`
(each includes it itself), and through them `registry.hpp` (`registrar_lit<S>`), `serialize.hpp`, `deserialize.hpp`,
`print.hpp` (the format string is a `lit<F>`).

## What it defines

| name | kind | meaning |
|---|---|---|
| `lit<fixed_string S>` | struct template | the literal type: `str` (the NTTP), `size`, `id = fingerprint64(S.view())`, `text()` |
| `is_literal_v<T>` | variable template | `true` for `lit<S>`, `false` otherwise |
| `literals::operator""_lit` | `consteval` UDL (inline namespace) | `"text"_lit` → `lit<"text">{}` |
| `literals::operator""_` | `consteval` UDL | `"text"_` → the same |
| `constant<auto V>` | struct template | a compile-time constant argument: `value_type` (`decltype(V)` without `const`), `value` |
| `is_constant_v<T>` | variable template | `true` for `constant<V>`, `false` otherwise |
| `k<V>` | `inline constexpr constant<V>` | the spelling for every constant a numeric literal cannot write: `k<true>`, `k<'x'>`, `k<42u>`, `k<1.5f>`, `k<Side::buy>` |
| `operator-(constant<V>)`, `operator+(constant<V>)` | `constexpr` | `-7_` is `constant<-7>`; minus for signed and floating `V` except `char`, plus for arithmetic `V` except `bool` |
| `literals::operator""_` (numeric) | `consteval` UDL, `template<char... Cs>` | `42_`, `0x10_`, `0b101_`, `017_`, `1'000'000_`, `1.5_`, `2.5e-3_` → `constant<V>` |
| `ES_SERREG_LIT(s)` | macro | `lit<fixed_string{s}>{}` for contexts where a UDL is awkward |
| `detail::literal_blob_bytes<S>()` | `consteval` function | builds the literal ENTRY as a byte vector |
| `detail::literal_blob_p<S>` | struct | the `P` for `to_blob_array` |
| `detail::literal_blob_bytes_v<S>` | `ES_SERREG_HIDDEN inline constexpr std::array` | the frozen ENTRY blob, COMDAT, one per image |
| `detail::numeric<Cs...>()` | `consteval` | the numeric literal's characters → `constant<V>` of the right type |
| `detail::numeric_is_hex`, `numeric_is_float`, `numeric_uint`, `numeric_double` | `consteval` | the digit parsers behind `numeric` |
| `detail::constant_error(const char*)` | declared, never defined | calling it during constant evaluation is the compile error; its argument is the message |

The remaining spellings live elsewhere: `serialize<"text">(out, ...)` and `serialize_with<"text">` are overloads
in `serialize.hpp` that prepend `lit<L>{}`; `print<"fmt">` likewise in `print.hpp`.

## How it works

### Identity must be in the type

```cpp
// The literal's identity must be in the *type* (or a template argument): that is the only channel that reaches
// static-initialisation-time registration, so a runtime `const char*`/`string_view` cannot be a literal.
```
(`literal.hpp:9`). Registration happens by instantiating `detail::registrar_lit<S>::g` (a static guard) from
inside `serialize_frame`; a template can only be instantiated from a compile-time value. That is why `"new"` (no
suffix) is a *runtime string* argument (category `string`, bytes on the wire, FR-L5) while `"new"_` is a literal.

### The four spellings are one literal

```cpp
template<fixed_string S> struct lit
{
    static constexpr auto str = S; // the NTTP itself (for registrar_lit<lit<S>::str>)
    static constexpr std::size_t size = S.size();
    static constexpr std::uint64_t id = fingerprint64(S.view());
    static std::string_view text() noexcept
    {
        return {reinterpret_cast<const char*>(detail::literal_blob_bytes_v<S>.data()) + fmt::literal_text_offset, size};
    }
};
```
(`literal.hpp:58`). `fixed_string` is structural, so `"x"_lit`, `"x"_`, `lit<"x">{}`, `ES_SERREG_LIT("x")` and
the `serialize<"x">` overload all name the same specialisation `lit<fixed_string<2>{"x"}>`; same type → same
`registrar_lit`, same blob, same `id`, same call string item `lit:0x…`. `tests/unit/test_roundtrip.cpp` "FR-L1 the
four literal spellings are one literal" checks exactly this.

`id` is the XXH3-64 fingerprint of the text bytes (FR-H7). It is the ARG hash in the call ENTRY, the `u64` payload of a
full-records literal record, and the key of `registry::find_literal` / `dyn::dictionary::literals`.

### The footprint: one static object per image

```cpp
template<fixed_string S> consteval std::vector<std::byte> literal_blob_bytes()
{
    cbuf b;
    b.begin_entry(fmt::tag_literal);
    b.put(fingerprint64(S.view()));
    b.put(static_cast<std::uint32_t>(S.size()));
    b.put(std::uint32_t{0});
    for (const char c : S.view())
        b.v.push_back(static_cast<std::byte>(c));
    b.end_entry();
    return b.v;
}
```
(`literal.hpp:34`). The ENTRY is `u32 total_len, u8 3, u8 1, u16 0 | u64 id | u32 len | u32 0 | text | pad8`, so
the text starts at byte 24 (`fmt::literal_text_offset`) and `"evt"` gives a 32-byte blob (locked in
`tests/golden/blobs.cpp`: hash `0x52155f6f3763912f`, size 32).

The text is stored *only* inside this blob: `lit<S>::text()` views into it, and `registry::push`
sets `reg_node::name` to the same bytes when a literal node arrives (`src/registry.cpp`, reading `len` at
`literal_len_offset`). The template-parameter object `S` is never odr-used (only `S.view()`/`S.size()` in
constant expressions), so the compiler emits no separate copy of the characters. `text()` is not `constexpr`
because a `std::byte* → const char*` view needs `reinterpret_cast`.

Per image there are exactly two copies of a literal's ENTRY: `literal_blob_bytes_v<S>` (COMDAT `.rodata`, hidden
visibility, what descriptors and registry nodes point at) and the TU-local section copy `literal_blob_copy_v<S>`
declared in `detail/embed_elf.hpp`, which is what `srdump --elf` reads from `es_serreg_dict`. `blob.hpp::literal_blob_span<S>()`
touches the section copy and returns the COMDAT span; `literal_blob_v<S>` is that span and `registrar_lit<S>::node`
is initialised from it.

### The blob of `"evt"`, byte by byte

| offset | field | value |
|---|---|---|
| 0 | `u32 total_len` | 32 |
| 4 | `u8 tag` | 3 (`tag_literal`) |
| 5 | `u8 version` | 1 |
| 6 | `u16 reserved` | 0 |
| 8 | `u64 id` | `fingerprint64("evt")` = `0x198e4c6da0847235` |
| 16 | `u32 len` | 3 (`fmt::literal_len_offset`) |
| 20 | `u32 rsv` | 0 |
| 24 | text | `'e' 'v' 't'` (`fmt::literal_text_offset`) |
| 27 | pad | 5 zero bytes |

`lit<"evt">::text()` is `{blob + 24, 3}`; `registry::push` computes the same view with `std::min(len, size - 24)`
as a guard against a malformed blob. The frozen hash of these 32 bytes is `0x52155f6f3763912f`.

### Where a literal shows up in a frame

| mode | bytes for the literal | who knows it was there |
|---|---|---|
| compact (1.1+) | none | the call hash → call ENTRY → ARG `lit:<id>` at that position; `n_records` still counts it |
| compact (1.0 stream, historical) | `u64 id` | format 1.0 wrote the id; readers accept only minors 1.6 and 1.7, so this form is never read |
| full records | `u64 id (hash) , u32 8, u16 0, u16 0, u64 id` | self-describing; `get_record` compares the payload id with `U::id` |

### 0-byte compact records (FR-L6, format 1.1)

`serialize.hpp::record_size<lit<S>>(full)` is `0` in compact mode and `16 + 8` in full-records mode;
`put_value_record` (which `put_record` calls for everything but a constant) returns early for a compact literal.
The reader side mirrors this: `frame_view::compact_size` returns 0 for `arg_literal`, `get_record` skips the id check
in compact frames because the call hash already matched. `n_records` still counts the literal. `serialize(out,
"heartbeat"_)` is therefore a bare 16-byte header.

### Where a literal is allowed

`serializable_arg` (`arg.hpp`) admits `lit<S>`; `deserialize(bytes, "x"_, o)` accepts the literal as a const
value (the `static_assert` in `deserialize_frame` exempts literals from the non-const-lvalue rule); `dispatch`
delivers literal records as `literal_view{id, text}` where `text` is empty when the id is unknown to the registry.
`print.hpp` requires its format string to be a `lit<F>`. A literal is never a *member* — `kind::literal_` exists in
the dictionary schema but no FIELD carries it.

### Compile-time constants (format 1.5)

```cpp
template<auto V> struct constant
{
    using value_type = std::remove_const_t<decltype(V)>;
    static constexpr value_type value = V;
};
template<class T> constexpr bool is_constant_v = false;
template<auto V> constexpr bool is_constant_v<constant<V>> = true;
template<auto V> inline constexpr constant<V> k{};
```
(`literal.hpp:84-91`). `constant<V>` is an empty type whose template argument is the value. `V` is a scalar
(`bool`, `char`, the integers, `float`, `double`, `std::byte`) or an enum; this header does not check that —
`arg.hpp`'s `is_constant_arg_ok_v` does, so a class-type constant fails `serializable_arg`. `42` and `42u` are
different template arguments, so `constant<42>` and `constant<42u>` are different types and different calls.

A numeric literal with the suffix `_` reaches the *literal operator template*, which receives the literal's
characters as a pack:

```cpp
template<char... Cs>
consteval auto operator""_() // 2.1.15: 42_  0x10_  1.5_  5'000'000'000_  (-7_ with unary minus)
{
    return detail::numeric<Cs...>();
}
```
(`literal.hpp:237-244`). It lives next to the string form `"text"_` in the inline namespace `literals`; the two do
not conflict, because the language sends a string literal to the `fixed_string` operator and a numeric literal to
this one (`"42"_` is still `lit<"42">`). `detail::numeric` picks the type from the characters:

```cpp
template<char... Cs> consteval auto numeric()
{
    constexpr char s[]{Cs..., 0};
    constexpr std::size_t n = sizeof...(Cs);
    if constexpr (numeric_is_float(s, n))
        return constant<numeric_double(s, n)>{};
    else if constexpr (numeric_uint(s, n) <= 0x7fffffffull)
        return constant<static_cast<int>(numeric_uint(s, n))>{};
    else if constexpr (numeric_uint(s, n) <= 0x7fffffffffffffffull)
        return constant<static_cast<long long>(numeric_uint(s, n))>{};
    else
        return constant<numeric_uint(s, n)>{};
}
```
(`literal.hpp:212-224`).

- **Integers** (`numeric_uint`, `literal.hpp:128-156`): decimal, `0x`/`0X` hexadecimal, `0b`/`0B` binary, a leading
  `0` for octal, digit separators `'` skipped. The type is `int` when the value fits, else `long long`, else
  `unsigned long long`. Unlike the language rule for an unsuffixed hexadecimal literal, unsigned types are not tried
  before `long long`: `0xFFFFFFFF_` is a `long long`. A value above 64 bits or a bad digit calls `constant_error`.
- **Floating literals** (`numeric_double`, `literal.hpp:160-211`): a literal with a `.` or a decimal exponent is a
  `double`. The digits are accumulated as an integer (at most 15 significant digits, so the integer is exact in a
  `double`) and scaled by one power of ten whose exponent is within ±22 (exact up to `1e22`): one correctly rounded
  multiplication or division, which gives the value the compiler gives the same spelling without the suffix.
  More than 15 significant digits, a decimal exponent outside ±22 after the fraction digits are counted, or a
  hexadecimal floating literal is a compile error whose message says to write `k<value>`.
- **The sign** is not part of the literal: the operator sees `7` for `-7_`. The unary minus on `constant<V>`
  (`literal.hpp:94-100`) returns `constant<-V>`; it exists for signed and floating `V` and not for `char`. Unary plus
  (`literal.hpp:101-107`) returns the same constant. `-2147483648_` is therefore `constant<-2147483648ll>` (the
  operator saw `2147483648`, a `long long`).
- **`k<V>`** spells what the literal cannot: `k<true>`, `k<'x'>`, `k<42u>`, `k<1.5f>`, `k<Side::buy>`,
  `k<std::byte{7}>`. A character literal with `_` (`'x'_`) is not supported: the language passes a character
  literal to a literal operator as a run-time value, so its value cannot become a template argument.
- **Errors.** `detail::constant_error` (`literal.hpp:113`) is declared and never defined. It is not `constexpr`, so
  a `consteval` function that reaches a call to it is not a constant expression, and the compiler's diagnostic
  shows the call with its message string. The library is built without exceptions, so `throw` is not used.

What a constant costs and where its value goes is decided elsewhere: the call-string item `const:<name>=<value>`
and the 24-byte ARG in `detail/blob_bytes.hpp`, the record sizes in `serialize.hpp`, the reader's view in
`deserialize.hpp`. The golden test "FR-A5 the numeric _ literal and k<V> build constant<V>"
(`tests/golden/hashes.cpp`) fixes the type of every spelling above.

## Maintaining it

- **Constants are hash-relevant too.** The type `numeric` picks for a spelling decides the call-string
  item (`const:i32=42` or `const:i64=42`), so changing the int / long long / unsigned long long rule, or the value
  `numeric_double` computes, changes call hashes: a `format_minor` bump. `numeric_double` must keep returning
  exactly the compiler's value for the same spelling; that is why it refuses what it cannot round correctly
  instead of approximating.
- **Hash-relevant:** `id = fingerprint64(S.view())` (no terminator), the blob layout, and the call-string item
  `lit:<hex id>` (`detail/blob_bytes.hpp::append_call_item`). Locked by `tests/golden/hashes.cpp` ("FR-H4/FR-S11 hash relations",
  "1.1 argument categories and print forms") and `tests/golden/blobs.cpp` (literal and call blob hashes/sizes). The
  fixtures `tests/fixtures/golden/*.rri` contain literal ENTRYs read by the canary with `n_literals` counts.
- **The text offset is a contract** shared with `registry::push` and `lit<S>::text()` through
  `fmt::literal_len_offset`/`literal_text_offset`; if the literal body ever changes, change the constants, the
  layout comment in `format.hpp`, and `tests/unit/test_roundtrip.cpp` "FR-L2 literal text lives inside the ENTRY
  blob, once (v1.0.3 footprint)".
- **Do not add a `constexpr` text accessor that reads `S.data`.** It would odr-use the NTTP object and bring back
  the second copy per literal per TU that storing the text in the blob removed.
- **Length limits.** `len` is a `u32`; the call-string item and ENTRY size are otherwise unbounded, but a literal
  is meant to be a short message id. A very long literal costs a long constant evaluation in `to_blob_array` (two
  passes) — nothing enforces a cap today.
- Long-literal ambiguity: two texts with the same 64-bit fingerprint would collide in the registry (same `hash`, same
  `node_kind::literal`, treated as aliases). The library does not detect this; the probability is that of a 64-bit
  collision among the literals of one program.
- **UDL namespace.** `literals` is an inline namespace of `es::serreg`, so `using namespace es::serreg::literals;`
  brings only the literal operators: `_lit`, the string `_` and the numeric `_`. `constant`, `k` and
  the unary operators are in `es::serreg` itself (the operators are found by argument-dependent lookup; `k` needs
  `es::serreg::k<…>` or a using-declaration). `operator""_` (a bare underscore) is a reserved-looking but legal UDL
  name; some style checkers flag it — it is intentional ("the short form").
- Tests: `tests/unit/test_roundtrip.cpp` ("FR-L1 literals mixed with objects round-trip and are id-checked",
  "FR-L1 the four literal spellings…", "FR-L2 literal text lives inside the ENTRY blob, once"),
  `tests/unit/test_print.cpp` ("FR-L6 a compact literal occupies no bytes; full records keep the id", "format 1.0
  captures (8-byte literal records) still decode"), `tests/unit/test_registry.cpp` ("FR-R5 lookups: … find_literal"),
  `tests/unit/test_tools.cpp` ("gk 1.4 a flipped literal id makes tools::verify report a problem"), the dlopen
  plugins (literals registered from a DSO and retained after its `dlclose`). Constants:
  `tests/golden/hashes.cpp` ("FR-A5 the numeric _ literal and k<V> build constant<V>", "FR-A5 a constant's type and
  value are in the call string and the call hash"), `tests/unit/test_roundtrip.cpp` ("FR-A5 constants: no bytes in
  a compact frame, the value's record in a full one (1.5)").
