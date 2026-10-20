# `include/es/serreg/arg.hpp`

The call-argument model (FR-A1..A5): every argument of `serialize()`, `deserialize()`,
`dispatch<>()` and `print()` falls into one of five **categories** — type, literal, scalar, string and
(format 1.5) constant — each with a canonical name (what the call string is built from), a 64-bit ARG hash (what the call ENTRY and the
full-record header carry) and a record layout (`format.hpp`). This header classifies a C++ type into its
category and produces the name and hash; it belongs to the **wire-format / call-signature layer**. It includes
`reflect.hpp` for `flat_type`, `dynamic_type`, `is_scalar_v`, `scalar_kind` and `schema_hash_v`, so it is
**reflection-dependent** through that include.

## Depends on / used by

Includes: `<bit>` (`std::bit_cast`), `<cstddef>`, `<cstdint>`, `<cstring>` (`strnlen`), `<string>`, `<string_view>`, `<type_traits>`,
`es/serreg/format.hpp` (`fmt::arg_kind`), `es/serreg/hash.hpp`, `es/serreg/literal.hpp` (`is_literal_v`, `is_constant_v`, `constant<V>`),
`es/serreg/reflect.hpp`.

Included by: `detail/blob_bytes.hpp` (call strings, `call_arg_desc`), `serialize.hpp` (record sizes and payloads), and through
them `deserialize.hpp`, `print.hpp`, `capture.hpp`, `registry.hpp` (`touch_arg` uses `is_string_arg_v`,
`is_scalar_arg_v` and, for a constant, `arg_value_t`).

## What it defines

Public (`namespace es::serreg`):

| name | kind | meaning |
|---|---|---|
| `arg_kind_v<T>` | `inline constexpr fmt::arg_kind` | category of `remove_cvref_t<T>` |
| `arg_hash_v<T>` | `inline constexpr std::uint64_t` | ARG hash of `remove_cvref_t<T>` |

`namespace detail`:

| name | kind | meaning |
|---|---|---|
| `is_string_arg_v<T>` | variable template | `true` for `const char*`, `char*`, `char[N]`, `const char[N]`, `std::string_view`, and `std::basic_string<char, std::char_traits<char>, A>` for any allocator (`std::string`, `std::pmr::string`) |
| `is_scalar_arg_v<T>` | variable template | `is_scalar_v<remove_cv_t<T>>` (arithmetic except `long double`, plus `std::byte`) |
| `arg_value<T>`, `arg_value_t<T>` | trait | the type a `constant<V>` stands for (`constant<V>::value_type`); `T` itself for every other argument |
| `is_constant_arg_ok_v<T>` | variable template | a `constant<V>` whose value type is a scalar or a flat enum |
| `const_bits(v)` | `consteval` | the 8 value bytes of a constant's ARG as a `u64`: the object's bytes, zero-extended |
| `arg_kind_of<T>()` | `consteval` | literal → `arg_literal`, constant → `arg_const`, string → `arg_string`, scalar → `arg_scalar`, else `arg_type` |
| `string_arg_name`, `string_arg_hash` | constants | `"string"`, `fingerprint64("string")` = `0xd8ad194648e774f1` |
| `arg_name<T>()` | `consteval` | `"string"` or the scalar kind name (`to_string(scalar_kind<T>())`) |
| `arg_hash<T>()` | `consteval` | `T::id` / `string_arg_hash` / `fingerprint64(arg_name<T>())` / `schema_hash_v<T>`; for a constant, the hash of its value type |
| `arg_scalar_kind<T>()` | `consteval` | the `kind` for a scalar argument or a scalar constant, `kind{}` otherwise (the ARG `kind` byte) |
| `arg_const_value<T>()` | `consteval` | `const_bits(T::value)` for a constant, 0 otherwise (the `u64` after a constant's ARG) |
| `string_bytes(...)` | 4 inline overloads | bytes+length of a runtime string: `const char*` (NUL, null → empty), `char (&)[N]` (`strnlen(a, N)`), `string_view`, `std::string` |
| `serializable_arg<T>` | concept | literal ∨ constant of a scalar or enum ∨ `flat_type` ∨ `dynamic_type` ∨ scalar ∨ string, on `remove_cvref_t<T>` |

## How it works

### The categories

From the header comment (`arg.hpp:5`):

| category | example arguments | canonical name | compact record |
|---|---|---|---|
| type | any `flat_type` T (struct, enum, `as_flat`) | `<hex schema hash>` | `sizeof(T)` bytes |
| type | any `dynamic_type` T (v2: string/vector/… members; a container itself — `std::vector<E>`, `std::optional<E>`, `std::variant<…>`, a map or set — through its transparent type, `is_container_arg_v`) | `<hex schema hash>` | `u32 len, u32 0, image` |
| literal | `"text"_lit`, `"text"_`, `lit<"text">{}` | `lit:<hex id>` | nothing (1.1); `u64 id` in full records |
| scalar | `bool`, `char`, `int`, `std::uint64_t`, `double`, `std::byte`, `char8/16/32_t`, `wchar_t` | `i32`, `f64`, `bool`, … | `sizeof` bytes |
| string | `const char*`, `"text"`, `std::string_view`, `std::string` | `string` | `u32 len, bytes` |
| constant (1.5) | `42_`, `-7_`, `1.5_`, `k<true>`, `k<Side::buy>` (`literal.hpp`; a scalar or an enum) | `const:i32=42`, `const:<enum schema hash>=1` | nothing; the scalar or enum record of its value in full records |

Names are **category names, not C++ type names**: `serialize(out, "abc", 1)` and
`deserialize(bytes, std::string&, int&)` build the same call string (`call(2){string,i32}`), and
`call_hash_v<const char*, int> == call_hash_v<std::string_view&, int&>` is a golden `STATIC_REQUIRE`. A dynamic
type shares the `type` category with flat types; its schema string starts with `dyn<` so the hash differs from a
flat type of the same name, but the argument category and ARG kind (`arg_type`) are the same.

### Classification order

```cpp
template<class T> consteval fmt::arg_kind arg_kind_of()
{
    if constexpr (is_literal_v<T>)          return fmt::arg_literal;
    else if constexpr (is_constant_v<T>)    return fmt::arg_const;
    else if constexpr (is_string_arg_v<T>)  return fmt::arg_string;
    else if constexpr (is_scalar_arg_v<T>)  return fmt::arg_scalar;
    else                                    return fmt::arg_type;
}
```
(`arg.hpp:83`). The order matters in one place: `char` is a scalar and `char[N]` is a string, so a
`char buf[8]` *argument* is NUL-terminated text, whereas a `char sym[8]` *member* of a flat type is an array of
8 `char` (the header says so: "fixed buffers are members of flat types, not top-level arguments"). `char*` (non-const)
is accepted as a string too. Pointers other than these fail `serializable_arg` and, via `check_supported`, produce
the FR-S7 diagnostic.

### The ARG hash

```cpp
template<class T> consteval std::uint64_t arg_hash()
{
    if constexpr (is_literal_v<T>)          return T::id;
    else if constexpr (is_constant_v<T>)    return arg_hash<arg_value_t<T>>();
    else if constexpr (is_string_arg_v<T>)  return string_arg_hash;
    else if constexpr (is_scalar_arg_v<T>)  return fingerprint64(arg_name<T>());
    else                                    return schema_hash_v<T>;
}
```
(`arg.hpp:111`). This one value is written in three places: the ARG entry of the call ENTRY (`detail/blob_bytes.hpp::
call_blob_bytes`), the `u64 hash` of a full-records record header (`serialize.hpp::put_record_header`), and it is
what `dispatch` compares `record_view::hash` against (`r.hash != arg_hash_v<T>`). The scalar hashes are hashes of
the *kind names* (`fingerprint64("i32")`), which makes `int` and `std::int32_t` and `long` on LP64 (`i64`) interchangeable
by width, not by spelling. `arg_scalar_kind<T>()` additionally puts the `kind` byte in the ARG so a reader without
the C++ type still knows the record size (`frame_view::compact_size` → `kind_size`).

### Constants (format 1.5)

A `constant<V>` (`literal.hpp`) is described as the value it stands for. `arg_value_t<constant<V>>` is `V`'s type,
and three functions forward to it: `arg_hash` gives `fingerprint64("i32")` for `constant<42>` and the enum's schema
hash for `constant<Side::buy>`; `arg_scalar_kind` gives the scalar kind, or `kind{}` (0) for an enum; and in a
full-records frame the record is the one a run-time value of that type would be. Only the category differs:
`arg_kind_of` returns `arg_const`, so the call string and the call ENTRY say that the value is fixed.

```cpp
template<class V> consteval std::uint64_t const_bits(const V v)
{
    if constexpr (std::is_enum_v<V> && !std::is_same_v<V, std::byte>)
        return const_bits(static_cast<std::underlying_type_t<V>>(v));
    else if constexpr (std::is_same_v<V, std::byte>)
        return std::to_integer<std::uint64_t>(v);
    else if constexpr (std::is_same_v<V, bool>)
        return v ? 1 : 0;
    else if constexpr (std::is_same_v<V, double>)
        return std::bit_cast<std::uint64_t>(v);
    else if constexpr (std::is_same_v<V, float>)
        return std::bit_cast<std::uint32_t>(v);
    else
        return static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<V>>(v));
}
```
(`arg.hpp:66-80`). This is the `u64` that follows a constant's ARG in the call ENTRY (`arg_const_value<T>()`,
`arg.hpp:133-139`): the bytes of the object, zero-extended to 8. An integer goes through its own unsigned type
first, so `-7` as an `int` is `0x00000000fffffff9`, not sign-extended to 64 bits; a `float` is its 32-bit pattern;
an enum is its underlying value. On a little-endian host the first `sizeof(V)` bytes of that `u64` are the bytes a
run-time record of the value would have, which is what lets a reader present the constant as a record
(`deserialize.hpp`, `resolver::arg_value`).

`is_constant_arg_ok_v<T>` (`arg.hpp:61-64`) admits a constant whose value type is a scalar (`is_scalar_v`) or an
enum that is a `flat_type`. `serializable_arg` uses it, and excludes a `constant` from its `flat_type` branch, so
a constant of any other type (a class-type template argument) is not an argument at all.

### Runtime string bytes

```cpp
template<std::size_t N> inline std::string_view string_bytes(const char (&a)[N]) noexcept
{
    return {a, ::strnlen(a, N)};
}
```
(`arg.hpp:147`). A char array is scanned up to `N`, so an unterminated buffer is not over-read; a null
`const char*` becomes an empty string rather than UB. These overloads are what `record_payload_bytes` and
`put_record` call; there is no owning copy anywhere on the writer side (FR-P6).

### Worked examples: call strings

`detail/blob_bytes.hpp::call_string` concatenates the items this header names:

| call | call string | notes |
|---|---|---|
| `serialize(out, "evt"_, order)` | `call(2){lit:0x<id of "evt">,0x367f8cd8c6441dd2}` | literal first, then the schema hash |
| `serialize(out, "abc", 1)` | `call(2){string,i32}` | frozen hash `0xef7e3878972feec1` |
| `deserialize(bytes, std::string& s, int& i)` | `call(2){string,i32}` | identical — the reader and writer agree by construction |
| `serialize(out, 3.5, true, std::byte{1})` | `call(3){f64,bool,byte}` | |
| `print(out, "x {}"_, 7)` | `print(2){lit:0x61127b53167a9c45,i32}` | the form tag is not an item |
| `serialize(out, dyn_msg)` | `call(1){0xb487de0c417a37e8}` | a dynamic type is an ordinary `type` item |
| `serialize(out, "heartbeat"_)` | `call(1){lit:0x…}` | 16-byte frame, one 0-byte record |
| `serialize(out, "evt"_, 42_, "abc")` | `call(3){lit:0x198e4c6da0847235,const:i32=42,string}` | 1.5: the constant's type and value are in the string; frozen hash `0xc4d099fc7bd16c9c` |
| `serialize(out, 42)` / `serialize(out, 42_)` | `call(1){i32}` / `call(1){const:i32=42}` | a run-time value and a constant are different calls |

### What the reader gets per category

| category | `record_view::ak` | `record_view::payload` | `deserialize` output | `dispatch` delivery |
|---|---|---|---|---|
| type (flat) | `arg_type` | the object bytes | `T&` (through `as_flat::from` if mapped) | `const T&` (zero-copy when aligned) |
| type (dynamic) | `arg_type` | the image (8-byte prefix stripped) | `T&` decoded | `const T&` of a temporary |
| literal | `arg_literal` | empty (compact) / `u64 id` (full) | `lit<S>` by value, id checked in full mode | `literal_view{id, text}` |
| scalar | `arg_scalar`, `k` set | `sizeof` bytes | `T&` of the same width | `T` by value |
| string | `arg_string` | the bytes (length prefix stripped) | `std::string&` or `std::string_view&` | `std::string_view` |
| constant (1.5) | `arg_scalar` (with `k`) or `arg_type`, and `is_const` set | the value's bytes: from the call's entry (compact) or from the frame (full) | the same `constant<V>` by value; another value is `hash_mismatch` | as the scalar or enum it stands for |

### `serializable_arg`

The concept is what constrains `serialize`, `serialize_with`, `deserialize`, `deserialize_print*`; it accepts
`dynamic_type` and a `constant<V>` of a scalar or an enum (`arg.hpp:162-167`). Note that `dynamic_type` deliberately admits *any* non-trivially-copyable class that is
not itself a dyn member or `string_view` — so an unsupported class still passes overload resolution and reaches
`check_supported<T>()` inside `touch<T>()`, which emits the intended `static_assert` message instead of a
"no matching function" wall.

## Maintaining it

- **Every name and hash here is a wire contract.** `tests/golden/hashes.cpp` "1.1 argument categories and print
  forms" locks `arg_hash_v` for the string spellings, `int`, `uint64_t`, `double`, `bool`, `arg_kind_v` for each
  category, `fingerprint64("string") == 0xd8ad194648e774f1` and `call_hash_v<const char*, int> == 0xef7e3878972feec1`.
  Adding a category means a new `fmt::arg_kind` value (format minor bump), a new `append_call_item` branch in
  `detail/blob_bytes.hpp`, new `record_size`/`put_record`/`stream_record`/`get_record` branches, `compact_size`/`decode` in
  `frame_view`, `dyn::dictionary::record_size`, the decoder in `src/reader.cpp`, and fixtures. The constant
  category (`arg_const`, format 1.5) touched exactly these, plus `parse_call` and `to_rri` in
  `src/dictionary.cpp`, `router.hpp` and the Wireshark generator.
- **`const_bits` is a wire contract**: its result is stored in the call ENTRY and compared by readers
  byte for byte. "FR-A5 a constant's type and value are in the call string and the call hash"
  (`tests/golden/hashes.cpp`) and the constant call ENTRY in `tests/golden/blobs.cpp` lock it.
- **Adding a string spelling** (e.g. `std::u8string`) is *not* a format change if it maps to `arg_string` with
  the same bytes — add the `is_string_arg_v` specialisation and a `string_bytes` overload; `deserialize` outputs
  are separately restricted to owning strings (`std::string`, any allocator) and `std::string_view` by `string_out` in
  `deserialize.hpp`.
- **Adding a scalar** is bounded by `scalar_kind` in `reflect.hpp` (there is no kind for `long double` or
  `__int128`; both are rejected with a message). A new scalar kind is a new `kind` value → format change.
- `is_scalar_arg_v` is defined over `remove_cv_t`, the public aliases over `remove_cvref_t`; keep call sites
  passing the stripped type (all `detail::` functions take "an already remove_cvref'd argument type").
- The `char*` → string rule means a `char*` *cannot* be used to serialise a pointer value — that is intended.
- Tests: `tests/unit/test_print.cpp` ("FR-A1 every scalar kind round-trips as a top-level argument", "FR-A2
  runtime strings: every spelling in, std::string / string_view out", "FR-A4 inspect/dispatch see string and scalar
  records"), `tests/golden/hashes.cpp` (frame sizes with scalars/strings, "FR-S3 frame sizes"),
  `tests/golden/blobs.cpp` (the print call ENTRY: `pb[40] == arg_string`, `pb[56] == arg_scalar`,
  `pb[57] == kind::i32`).
