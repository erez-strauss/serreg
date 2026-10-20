# `include/es/serreg/detail/blob_bytes.hpp`

The compile-time ENTRY blob bytes (`format.hpp`) of types and call signatures, the in-memory call descriptors
`call_arg_desc`/`call_desc`, the print form tags and the call string. Split out of `<es/serreg/blob.hpp>` for
the include rules: the executable-format backends `detail/embed_elf.hpp` and `detail/embed_none.hpp` copy these
bytes, and with the split each backend includes what it uses and compiles on its own. Format 1.5 added the
`const:` call-string item, `call_arg_desc::value` and the `u64` value after a constant's ARG; format 1.6
added the struct flag `transparent`, the enum flags byte (`fixed`) and `n_elem` 2 for a `map_` FIELD. Users include `<es/serreg/blob.hpp>` (or `<es/serreg/serreg.hpp>`),
never this header. It is part of the **compile-time reflection layer** and is **reflection-dependent** (it consumes
`reflect.hpp`'s `make_type_desc_base<T>()`). Literal blobs are not here; they are built in `literal.hpp`.

## Depends on / used by

Includes: `es/serreg/arg.hpp` (`arg_kind_of`, `arg_hash`, `arg_name`, `arg_scalar_kind`, `is_string_arg_v`,
`is_scalar_arg_v`; `arg_value_t`, `const_bits`, `arg_const_value`), `es/serreg/descriptor.hpp` (`field_desc`, `type_desc`, `ES_SERREG_HIDDEN`), `es/serreg/format.hpp`
(`cbuf`, `to_blob_array`, tags, flags), `es/serreg/literal.hpp` (`is_literal_v`, `is_constant_v`), `es/serreg/reflect.hpp`
(`make_type_desc_base`, `schema_hash_v`, `mapped_t`); then `<array>`, `<bit>`, `<cstddef>`, `<cstdint>`, `<span>`,
`<string_view>`, `<tuple>`, `<vector>`.

Included by: `blob.hpp` (first), `detail/embed_elf.hpp`, `detail/embed_none.hpp`; its own generated translation
unit in `tests/headers` (`serreg_headers_public`). Through `blob.hpp` its names reach `registry.hpp`
(`touch_arg` uses `is_form_tag_v`), `serialize.hpp`, `print.hpp` (`print_tag`/`println_tag`), `router.hpp`
(`is_form_tag_v`). `tests/golden/blobs.cpp` reads `type_blob_bytes_v<T>` and `call_blob_bytes_v<Ts...>` directly.

## What it defines

Public (`namespace es::serreg`):

| name | kind | meaning |
|---|---|---|
| `call_arg_desc` | struct | `is_literal, hash, ak, k, value` — one ARG of a call signature (in-memory form of the ARG bytes); `value` (1.5) is a constant's bytes, zero-extended, else 0 |
| `call_desc` | struct | `hash, span<const call_arg_desc> args, span<const byte> blob, uint16_t flags` |
| `print_tag`, `println_tag` | empty structs | form markers at the front of a signature pack (1.1) |
| `log_tag` | empty struct | the logger's form marker: call flags `print \| newline \| log`, call string `log(n){…}`; the log envelope follows it in the pack (`blob_bytes.hpp:48-52`) |

`namespace detail`:

| name | kind | meaning |
|---|---|---|
| `put_field(cbuf&, const field_desc&)` | `constexpr` | encodes one FIELD (recursive over `elem`; 1.6: `n_elem` 1 for a set, 2 for a map) |
| `type_blob_bytes<T>()` | `consteval` | struct or enum ENTRY as a byte vector |
| `is_form_tag_v<T>` | trait | `print_tag`, `println_tag` or `log_tag` |
| `call_flags<Ts...>()`, `call_arity<Ts...>()` | `consteval` | flags from a leading form tag; arity excludes it |
| `call_arg<T>()` | `consteval` | one `call_arg_desc` |
| `append_call_item<T>(std::string&)` | `consteval` | `lit:0x…` / `const:<name>=<value>` (1.5) / `string` / scalar name / `0x<schema hash>` |
| `call_string<Ts...>()` | `consteval` | `call(n){…}` / `print(n){…}` / `println(n){…}` / `log(n){…}` |
| `make_call_args<Ts...>()`, `call_args_v<Ts...>` | `consteval` / hidden constexpr array | the ARG table |
| `call_blob_bytes<Ts...>()` | `consteval` | call ENTRY as a byte vector |
| `type_blob_p<T>`, `call_blob_p<Ts...>` | structs | the `P` for `to_blob_array` |
| `type_blob_bytes_v<T>`, `call_blob_bytes_v<Ts...>` | `ES_SERREG_HIDDEN inline constexpr std::array` | the frozen blobs (COMDAT) |

## Structure

- **Public structs** (`blob_bytes.hpp:24-52`): `call_arg_desc`, `call_desc`, `print_tag`, `println_tag`, `log_tag`.
- **FIELD and type ENTRY encoders** (`blob_bytes.hpp:52-120`): `put_field()`, `type_blob_bytes<T>()`.
- **Call signatures** (`blob_bytes.hpp:129-251`): `is_form_tag_v`, `call_flags`, `call_arity`, `call_arg`,
  `append_call_item`, `call_string`, `make_call_args`, `call_args_v`, `call_blob_bytes`.
- **Frozen blobs** (`blob_bytes.hpp:240-262`): `type_blob_p`, `call_blob_p`, `type_blob_bytes_v`,
  `call_blob_bytes_v`.

## How it works

### Two-pass consteval blobs

A blob is built into a `std::vector<std::byte>` by a `consteval` function using `detail::cbuf` (`format.hpp`), then
frozen into a `std::array` by `to_blob_array<P>()`, which calls `P::bytes()` once for the size and once for the
content. The struct/enum builder mirrors the ENTRY layout exactly:

```cpp
b.begin_entry(fmt::tag_struct);
b.put(d.schema_hash);
b.put(d.layout_hash);
b.put(d.size);
b.put(d.align);
b.put(static_cast<std::uint8_t>((d.is_flat ? fmt::struct_flag_flat : 0) |
                                (d.transparent ? fmt::struct_flag_transparent : 0)));
b.put(std::uint8_t{0});
b.put(d.since);
b.put(d.deprecated);
b.put(static_cast<std::uint16_t>(d.name.size()));
b.put(static_cast<std::uint16_t>(d.fields.size()));
for (const char c : d.name)
    b.v.push_back(static_cast<std::byte>(c));
for (const auto& f : d.fields)
    put_field(b, f);
```
(`blob_bytes.hpp:103-118`). The enum branch (`blob_bytes.hpp:82-99`) writes `underlying` and then (1.6) the
flags byte `d.enum_fixed ? fmt::enum_flag_fixed : 0` where a zero `rsv` byte was. `put_field` writes `n_elem` as
1 for `array_`/`vector_`/`optional_`/`set_`, 2 for `map_` (1.6: the key, then the value), `extent` for
`variant_`, 0 otherwise, then recurses into `f.elem[i]` — so a 2-D array, a `vector<vector<…>>` or a
`map<string, vector<…>>` inside a struct nests FIELDs. `d` comes from `make_type_desc_base<T>()` (`reflect.hpp`), *without* the blob span (which would be
circular); `blob.hpp`'s `make_type_desc<T>()` adds the span to complete `descriptor_v<T>`.

### Call signatures

```cpp
// call(n){item,…} | print(n){…} | println(n){…}  — n counts arguments, not the form tag.
template<class... Ts> consteval std::string call_string()
{
    constexpr std::uint16_t flags = call_flags<Ts...>();
    std::string s = flags == 0                        ? "call("
                    : (flags & fmt::call_flag_log)     ? "log("
                    : (flags & fmt::call_flag_newline) ? "println("
                                                       : "print(";
    append_uint(s, call_arity<Ts...>());
    s += "){";
    ...   // items: lit:0x<id> | const:… | string | i32… | 0x<schema hash>, comma-separated, form tag skipped
    s += '}';
    return s;
}
```
(`blob_bytes.hpp:200-219`). A call led by `log_tag` is spelled `log(n){…}` and carries
`call_flag_log` in addition to `print | newline` (`call_flags`, `blob_bytes.hpp:132-149`), so a log call and a
`println` with the same arguments are different calls. The call hash is the XXH3-64 fingerprint of this string (`blob.hpp`'s `call_hash_v`);
it is the frame header's `call_hash`, the key of `registry::find_call` and `dyn::dictionary::calls`, and — for
compact frames — the only thing that tells a reader how many records follow and what each one is. A signature is a
pack of `remove_cvref_t` argument types with an optional leading form tag; `call_flags` inspects
`tuple_element_t<0>` and `call_arity` subtracts the tag. `make_call_args` fills the ARG table with
`arg_kind_of/arg_hash/arg_scalar_kind/arg_const_value` from `arg.hpp`; `call_blob_bytes` writes tag 4, hash,
`n_args`, `flags`, `u32 0`, then one 16-byte ARG per argument (`u8 ak, u8 kind, rsv[6], u64 hash`), followed for a
constant by its `u64 value`.

### Constants in the call string and the call ENTRY (format 1.5)

```cpp
else if constexpr (is_constant_v<T>)
{ // 1.5: const:<scalar name | enum schema hash>=<value> — integers in decimal, bool as true/false, floating
    // values by their bit pattern (0x…, so -0.0, NaN payloads and rounding are all part of the identity)
    using V = arg_value_t<T>;
    s += "const:";
```
(`blob_bytes.hpp:166-170`). The item of a `constant<V>` names the type and the value, so both are in the call hash:

| argument | item |
|---|---|
| `42_`, `-7_` | `const:i32=42`, `const:i32=-7` (signed integers in decimal) |
| `5'000'000'000_` | `const:i64=5000000000` |
| `k<42u>` | `const:u32=42` (unsigned integers and `std::byte` in decimal) |
| `k<'x'>` | `const:char=120` (the character's number) |
| `k<true>` | `const:bool=true` |
| `1.5_`, `k<1.5f>` | `const:f64=0x3ff8000000000000`, `const:f32=0x000000003fc00000` (the bit pattern, 16 hex digits) |
| `k<Side::sell>` | `const:0x074e267cca5188e1=2` (the enum's schema hash, then the underlying value in decimal) |

A floating value is written by bit pattern so that the string does not depend on a decimal rendering: `0.0` and
`-0.0` are two calls. `42_`, `43_`, `k<42u>`, `k<42ll>` and a run-time `int` (`i32`) give five different call
strings. All of these are frozen in "FR-A5 a constant's type and value are in the call string and the call hash"
(`tests/golden/hashes.cpp`).

In the call ENTRY the constant's ARG is 24 bytes:

```cpp
b.put(static_cast<std::uint8_t>(a.ak));
b.put(static_cast<std::uint8_t>(
    a.ak == fmt::arg_scalar || a.ak == fmt::arg_const ? a.k : kind{}));
for (int i = 0; i < 6; ++i)
    b.put(std::uint8_t{0});
b.put(a.hash);
if (a.ak == fmt::arg_const)
    b.put(a.value); // 1.5
```
(`blob_bytes.hpp:240-247`). `ak` is 4 (`arg_const`), `kind` the scalar kind or 0 for an enum, `hash` the scalar
name's hash or the enum's schema hash, `value` what `const_bits` returns (`arg.hpp`). The ENTRY of
`("evt"_, 42_, Order, k<Side::sell>)` is 8 + 16 + 16 + 24 + 16 + 24 = 104 bytes; `tests/golden/blobs.cpp` checks
its size and the bytes of both constant ARGs.

### Worked example: the ENTRY of `corpus::Cancel { u64 id; u32 reason; }`

`tests/golden/blobs.cpp` freezes this blob at 128 bytes. Counting from the layout:

| part | bytes | running total |
|---|---|---|
| ENTRY header `u32 total_len, u8 tag=1, u8 version=1, u16 0` | 8 | 8 |
| `u64 schema_hash, u64 layout_hash` | 16 | 24 |
| `u32 size=16, u32 align=8` | 8 | 32 |
| `u8 flags=1 (flat), u8 rsv, u16 since=0, u16 deprecated=0` | 6 | 38 |
| `u16 name_len=14, u16 n_fields=2` | 4 | 42 |
| `"corpus::Cancel"` | 14 | 56 |
| FIELD `id`: 32 fixed (`u16 2, u8 u64, u8 0, u32 0, u32 8, u16 8, u16 0, u16 0, u16 0, u64 0, u32 0`) + `"id"` | 34 | 90 |
| FIELD `reason`: 32 fixed (`offset 8, size 4, align 4, kind u32`) + `"reason"` | 38 | 128 |
| `pad8` | 0 | 128 |

Byte 32 is the flags byte the golden test inspects (`blob_of<corpus::Order>()[32] & struct_flag_flat`). The literal
`"evt"` ENTRY (built in `literal.hpp`) is 8 + 8 + 4 + 4 + 3 = 27 → 32; the call ENTRY of `("evt"_, Order)` is
8 + (8 + 2 + 2 + 4) + 2 × 16 = 56; the print call `(print_tag, lit<"x {}">, const char*, int)` is 24 + 3 × 16 = 72 —
all three sizes are the frozen values. A call with constants adds 8 bytes per constant (previous section).

### Per-image blobs

```cpp
template<class T>
ES_SERREG_HIDDEN inline constexpr auto type_blob_bytes_v = to_blob_array<type_blob_p<T>>();
template<class... Ts>
ES_SERREG_HIDDEN inline constexpr auto call_blob_bytes_v = to_blob_array<call_blob_p<Ts...>>();
```
(`blob_bytes.hpp:272-275`). Ordinary COMDAT `.rodata`, deduplicated across the TUs of one image; hidden
visibility keeps them out of the dynamic symbol table, so no `STB_GNU_UNIQUE` binding marks a DSO `DF_1_NODELETE`,
and every image carries its own copy. They are what `descriptor_v<T>.blob`, `call_desc_v<Ts...>.blob` and
`reg_node::blob` point at; the registry copies the bytes into its heap entry in `push()`, and the ELF
backend makes a TU-local section copy of them (`detail/embed_elf.hpp`).

### The dynamic-type case and the 1.6 flags

Nothing in this header is dynamic-specific except `put_field`'s `n_elem` rule and the flags byte:
a dynamic type's descriptor arrives from `reflect.hpp` with `is_flat = false`, `size`/`align` = fixed part, and
FIELDs whose `offset` are canonical image offsets and whose kinds may be `string_`…`set_` with element
descriptors. A container argument's transparent type arrives with `transparent = true` and one unnamed
FIELD at 0; its ENTRY is 128 bytes for `std::vector<i32>` (8 + 34 + 16 name + 32 + 32, padded) with byte 32 = `02`.
`tests/golden/blobs.cpp` checks `blob_of<corpus::DynMsg>()[32] & struct_flag_flat == 0`, the transparent bit of
`std::vector<std::int32_t>`'s blob, and byte 33 of every enum blob (`fixed` set for `corpus::Side`, clear for an
enum without a fixed underlying type).

## Maintaining it

- **Every byte here is golden.** `tests/golden/blobs.cpp` freezes the XXH3-64 and size of
  `type_blob_bytes_v<T>` for every corpus type (flat and dynamic), the literal blob of `"evt"` (32 bytes), the call
  blob of `(lit "evt", Order)` (56 bytes), the print call blob (72 bytes, with byte-position asserts on the
  flags and ARG kinds), the call blob with two constants (104 bytes, with asserts on each
  constant's `arg_kind`, `kind`, hash and value bytes), the enum flag byte and the transparent struct flag. `tests/canary/rri_canary.cpp` decodes `orders.rri` from literal offsets. Any encoding
  change: update `format.hpp`'s comment, the canary, the fixtures (`golden_fixtures_generate`), bump `format_minor`,
  CHANGELOG. `[golden-print]` (`ctest -R golden -V` with the tag) prints refreeze values.
- **Call strings are hashes.** `call(`/`print(`/`println(` prefixes, the arity, item spellings and the comma
  separator are locked by `tests/golden/hashes.cpp` ("1.1 argument categories and print forms": three frozen
  call hashes and the plan §3 example; "FR-A5 a constant's type and value are in the call string and the call
  hash": the `const:` items and nine call hashes computed outside the library). Adding a form tag or an argument category changes `call_string`,
  `call_flags`, `make_call_args`, `call_blob_bytes` and `registry.hpp::touch_arg` together.
- **Keep `ES_SERREG_HIDDEN`** on `call_args_v`, `type_blob_bytes_v` and `call_blob_bytes_v` (see "Per-image
  blobs"); `dlopen_I_plugin_without_flags_unloads` fails when a per-image symbol becomes `STB_GNU_UNIQUE`.
- **Constant-evaluation cost.** Each blob's builder runs twice (`to_blob_array`) plus once per hash memo; a type
  with thousands of fields or deep element nesting can hit the compiler's constexpr operation limit. Field count is
  capped at 65535 by a `static_assert` in `reflect.hpp`; element nesting has no compile-time cap, but the
  dictionary parser refuses FIELD nesting deeper than 32 (`max_field_depth`, `src/dictionary.cpp:28`) — a
  33-deep array-of-array member would produce a blob no reader accepts.
- **Self-contained, no backend.** The header must compile alone (`tests/headers`) and must not include
  `blob.hpp` or an `embed_*` header: the backends include it, not the other way round.
- Tests: `tests/golden/blobs.cpp`, `tests/golden/hashes.cpp`, `tests/canary/`, `tests/headers/` (compiled alone,
  ctest `headers_include_rules`).
- **Allocation/threading:** none at run time; compile-time only.
