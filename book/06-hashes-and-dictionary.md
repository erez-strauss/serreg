# 6. Hashes, canonical strings and the dictionary

## 6.1 Four hashes

(Five rows: the program hash is derived from the schema hashes. All are XXH3-64. The CRC32C of a 1.7 frame
trailer is an integrity check of bytes, not an identity, and appears nowhere in this table.)

| hash | over | identifies | where it appears |
|---|---|---|---|
| **schema hash** `schema_hash_v<T>` | the schema string: kind, canonical name, size/align, every field with name, kind, offset, size, align, `since`/`deprecated`; nested types by their schema hash | a type, as the writer saw it | type ENTRY, call ARG, full-record header, `record_view.hash` |
| **layout hash** `layout_hash_v<T>` | the same without names and annotations | bit-compatibility (two names for one layout share it) | type ENTRY; conflict detection (FR-R4) |
| **literal id** | the literal's bytes | a literal | literal ENTRY, call ARG |
| **call hash** `call_hash_v<Ts...>` | the call string `call(n){cat,…}` / `print(n){…}` / `println(n){…}` / `log(n){…}` | a `serialize()` signature | frame header, call ENTRY |
| **program hash** | sorted type schema hashes, order-independent (FR-H3) | a build's type set | FILE and RRI headers; `srdump --program-hash` |

All are **XXH3-64** with seed 0 and the default secret (`hash_algo` 2 in every header; format 1.3 replaced
FNV-1a-64, `hash_algo` 1, and such older files are refused). XXH3 is not collision-resistant against an adversary — it is a
fingerprint, not a MAC — but it passes the SMHasher quality tests FNV-1a fails, is constant-evaluable (one
`constexpr` function in `hash.hpp` serves compile time and run time), and accidental collisions across a program's
types are not a practical concern (about 3·10⁻¹² for 10⁴ types). Every hash is computed at
compile time from a canonical string, never from compiler spellings (`__PRETTY_FUNCTION__`, `display_string_of`),
so two compilers that agree on the layout agree on the hash.

The strings themselves are public: `schema_string_v<T>` and `layout_string_v<T>` (`reflect.hpp`) are the exact
texts the two hashes are computed from, as `constexpr std::string_view`s —
`fingerprint64(schema_string_v<T>) == schema_hash_v<T>` is a `STATIC_REQUIRE` of `tests/golden/hashes.cpp`. They
are `define_static_string`s: one copy per type per executable, emitted only when a program reads them at run
time, so printing `schema_string_v<Order>` next to a dictionary's hash is free for every type that does not.

## 6.2 Canonical strings

```
struct<corpus::Order>[32,8]{id:u64@0[8,8];px:i64@8[8,8];qty:u32@16[4,4];side:enum<corpus::Side,u8>@20[1,1];sym:array<char,8>@21[8,1];}
struct[32,8]{u64@0[8,8];i64@8[8,8];u32@16[4,4];enum<u8>@20[1,1];array<char,8>@21[8,1];}
enum<corpus::Side,u8>[1,1]{buy=1;sell=2;}
dyn<corpus::DynMsg>[20,4]{id:u32@0[4,4];name:string@4[8,4];xs:vector<i16>@12[8,4];}
dyn<corpus::DynMap>[36,4]{id:u32@0[4,4];by_name:map<string,i32>@4[8,4];ids:set<u16>@12[8,4];names:map<i32,string>@20[8,4];msgs:map<u8,dyn<0xb487de0c417a37e8>>@28[8,4];}
dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}
```

Grammar: `struct|dyn|enum` `<name>` (schema only) `[size,align(;sN;dN)]` `{` member `;` … `}`; a member is
`name:` (schema only) + kindspec `@offset[size,align(;sN;dN)]`. Kindspecs: scalar kind names (`i8…u64 f32 f64
bool char char8 char16 char32 wchar byte`), `enum<name,u8>` / `enum<u8>`, `array<kindspec,N>`,
`struct<0xhash>` / `struct<L:0xhash>` (schema/layout hash of the nested type), `dyn<0x…>` for nested dynamic
classes, `string`, `vector<kindspec>`, `optional<kindspec>`, `variant<k0|k1|…>`, and (format 1.6)
`map<key,value>` and `set<key>`, and `multimap<key,value>` (schema string only: a `std::multimap` or
`std::unordered_multimap` member, whose layout kindspec is the `vector<dyn<L:0x…>>` of `std::pair<K,V>` it is on
the wire). `std::deque`, `std::list`, `std::forward_list` and `std::stack` members spell `vector<kindspec>` like a
`std::vector`; a `std::pair`/`std::tuple` member is a nested dynamic class, `dyn<0x…>`, whose own string names the
parts (`dyn<std::pair<i32,f64>>[16,8]{first:i32@0[4,4];second:f64@8[8,8];}`). Bases are
pseudo-members `<base:Name>:kindspec@off[…]`.

**The kindspec of a map or set is the shape, not the container.** `std::map<std::string,int>`,
`std::unordered_map<std::string,int>` and `std::flat_map<std::string,int>` members all spell `map<string,i32>`
(the sets `set<K>`), as every `std::basic_string<char>` spells `string`: the comparator, hasher and allocator are
properties of the reader's object, not of the data, and the bytes are the same for all of them (chapter 8 §8.1b).
Two structs that differ only in `std::map` versus `std::unordered_map` for one member therefore have equal schema
*and* layout hashes. **A container passed as an argument** has a type of its own, the transparent type: its
schema string is `dyn<` + the container's vocabulary name + `>[8,4]{:` + the member kindspec + `@0[8,4];}` — the
one place where the container's name (and a non-default comparator, hasher or equality, never the allocator)
enters a hash: `std::vector<i32>` (`0x02c87ef3a015eb45`), `std::map<std::string,i32>` (`0x9ba49a614d8d0569`),
`std::unordered_map<i32,std::string>` (`0xc0d088a8ef8a5703`), `std::set<u64>` (`0xcfbc288f1d543208`). The last
two strings above are the type `corpus::DynMap` of the golden `dynamic.ser` and the transparent type of
its top-level vector frames; `tests/golden/hashes.cpp` "2.2.0 golden strings and hashes of the map/set shapes and
the transparent types (format 1.6)" locks them, with hashes computed outside the library. Names are qualified by walking `parent_of` (`ns::Outer::Inner`, and for a template
parent `ns::Outer<i32>::Inner`); template arguments render canonically (`W<i32,3>`, `true`, `120`, `enum(7)`, std
types by their frozen vocabulary such as `std::vector<i32>`), class-type NTTPs are rejected, unnamed types are
rejected, and a type in an unnamed namespace or local to a function must be named explicitly
(`[[=es::serreg::name("…")]]` or `es::serreg::type_properties<T>`). See `files/include-es-serreg-reflect.hpp.md`.

**The kind of an enum (decision D).** The kind inside `enum<Name,kind>` — in a member's kindspec and in
the enum's own string — is the kind of the fixed underlying type when the enum has one (`enum class`, `enum E :
T`). An unscoped enum *without* one (`enum Color { red, green };`) has an implementation-defined underlying type,
so its kind is derived from the source instead of the compiler's trait: the unsigned kind of `sizeof(E)`, or the
signed kind when an enumerator is negative (`detail::enum_underlying_kind`; detection
`detail::has_fixed_underlying_type_v<E>` = `requires { E{0}; }`). `enum Plain { a, b }` is `u32`, `enum Neg { m =
-1 }` is `i32`, an enumerator above 32 bits makes it `u64`. That is what GCC and Clang choose for
`std::underlying_type_t`, so the rule gives the hashes those compilers always gave; a compiler that picks `int` for
every such enum (MSVC) produces the same strings as GCC. The descriptor's `underlying` and the ENTRY's kind byte follow the same rule.
Such enums also hold only the values of the minimal-width integer that represents their enumerators, which is what
the typed readers' optional range check is about (chapter 4 §4.1). Locked by `tests/golden/hashes.cpp` "2.1.16 decision D: an
enum without a fixed underlying type takes its kind from its enumerators" (the derived kind equals the trait's
kind for the test enums and `corpus::Venue` on GCC).

**Call strings.** A call string is `call(n){item,…}`, `print(n){…}` or `println(n){…}`, where `n` is the number
of arguments and each item names one argument by its category:

| argument | item |
|---|---|
| a type (flat or dynamic; a container argument, by its transparent type) | `0x<schema hash>` |
| a literal | `lit:0x<id>` |
| a scalar | its kind name: `i32`, `f64`, `bool`, … |
| a runtime string | `string` |
| a constant (format 1.5) | `const:<scalar name>=<value>`, or for an enum `const:0x<enum schema hash>=<underlying value>` |

```
call(2){lit:0x198e4c6da0847235,0x367f8cd8c6441dd2}                         serialize(out, "evt"_, order)
call(3){lit:0x198e4c6da0847235,const:i32=42,string}                        serialize(out, "evt"_, 42_, "abc")
call(5){lit:0xe1c90c19f0ff66a4,const:i32=42,0x367f8cd8c6441dd2,const:0x074e267cca5188e1=2,const:f64=0xbff8000000000000}
                                                       serialize(out, "fill"_, 42_, order, k<Side::sell>, -1.5_)
println(3){lit:0xf3d0e4e70162e1f6,const:i64=5000000000,const:bool=true}    println(out, "limit={} on={}"_, 5'000'000'000_, k<true>)
```

The value of a constant is written in a fixed form: integers in decimal (`const:i32=-7`, `const:u32=42`,
`const:char=120` for `k<'x'>`), `bool` as `true` or `false`, floating values by their bit pattern as 16 hex digits
(`const:f64=0x3ff8000000000000` is 1.5, `const:f32=0x000000003fc00000` is `1.5f`), an enum as its schema hash and
underlying value. A constant's type and value are therefore in the call hash: `42_`, `43_`, `k<42u>` and a
run-time `int` are four calls, and `0.0` and `-0.0` are two. The last two strings above are the calls
`0x0ef1cf0a2766bb0e` and `0x9557b837cf09db95` of the golden `orders.ser` (chapter 7).

**Why offsets are in the string:** the schema hash promises "these bytes decode as this"; for flat types the
bytes are the native layout, so the layout is part of the identity. For dynamic types the offsets are the
*canonical* ones (chapter 8), which do not depend on the compiler.

`[golden-print]` in `serreg_tests` prints every frozen value; `tests/golden/hashes.cpp` and `blobs.cpp` lock them.
A change to any string is a wire-format change (CHANGELOG + `format_minor` bump + fixture refreeze).

## 6.3 Annotations and evolution hooks

`[[=es::serreg::since(N)]]` / `[[=es::serreg::deprecated(N)]]` on members or types enter the schema string
(`[4,4;s2;d0]`) but not the layout string: adding an annotation changes the schema hash and keeps the layout
hash — `srdump --diff` classifies that as "metadata-only". `[[=es::serreg::skip]]` removes a member from the
description and scrubs its bytes as padding. The hashes tell you *that* two schemas differ and `--diff` tells you
*how*. `[[=es::serreg::renamed_from("old")]]` on a member or a class is the third annotation, and the one that
enters **neither** string: it is reader-side metadata for schema evolution level 1 (chapter 4 §4.1d) — a reader with
the writer's dictionary projects a flat type's frames member by member, by name, across widenings, added, removed
and renamed members — so declaring it changes no hash and no dictionary.

## 6.4 The dictionary at run time: `dyn::dictionary`

```cpp
struct dyn::dictionary {
    std::map<std::uint64_t, dyn::type> types;      // name, hashes, size/align, is_enum, is_flat, transparent, enum_fixed (1.6), fields (recursive; a map's key and value, a set's key as elem), enumerators
    std::map<std::uint64_t, dyn::literal> literals; // id → text
    std::map<std::uint64_t, dyn::call> calls;       // hash → {flags, args: {ak, k, hash, value}}; value: a constant's u64 (1.5)
    std::uint64_t program_hash;  std::uint64_t abi; bool abi_known, abi_mixed;  std::vector<type_conflict> conflicts;
    result<void> add_rri(span); add_entries(span); void merge(const dictionary&);
    find_type / find_type_by_name / find_literal / find_call; record_size(arg); scalar_size(kind); same_abi(); to_json(); to_toml();
};
std::vector<std::byte> dyn::to_rri(const dictionary&);
```

Built by `tools::load_dictionary_rri` (an RRI in memory), `load_dictionary_file` (an `.rri`),
`extract_embedded_dictionary` (`extract_registry_from_elf` is the older name: the `es_serreg_dict` section
of an ELF binary or object, copied there by `detail/embed_elf.hpp`), or by the capture walker
from the file's embedded RRI and any dictionary frames. Parsing is bounds-checked everywhere (`count_fits`,
depth limits, field geometry validated against the type size, v2 slot arities); a second definition of a known
schema hash is kept out of `types` and recorded in `conflicts` if it differs. `add_rri` accepts the format minors
1.6 and 1.7 (a 1.7 delta RRI is parsed like a full one and merged) and records the writer's ABI tag (`abi`); it
keeps no per-source minor, and (FR-D5) refuses more than `decode_limits::dictionary_entries` (1 000 000) declared entries as
`corrupt_dictionary` — the readers refuse a dictionary above `decode_limits::dictionary_bytes` (256 MiB) before
its bytes reach `add_rri`.

A constant argument (format 1.5) is a `dyn::call_arg` with `ak == fmt::arg_const`: `k` is the scalar kind
of the value or `kind{}` for an enum, `hash` the scalar name's hash or the enum's schema hash, and `value` the
constant's bytes as a `u64`, zero-extended. The parser refuses a scalar constant whose value has bits above the
scalar's size. `record_size` answers 0 for it, as for a literal. `to_json()` and `to_toml()` show
`"kind": "const"` with `"scalar"` for a scalar and (B7) the value typed as what it stands for
plus the exact bits — in the golden `orders.json`:

```json
{"hash": "0x5c23eefcac97a25f", "kind": "const", "scalar": "i32", "value": 42, "bits": "0x000000000000002a"},
{"hash": "0x074e267cca5188e1", "kind": "const", "value": "sell", "bits": "0x0000000000000002"},
{"hash": "0xc89f07cc0c489ed1", "kind": "const", "scalar": "f64", "value": -1.5, "bits": "0xbff8000000000000"}
```

and in `orders.toml` `value = 42` / `bits = "0x000000000000002a"` under `[[calls.args]]`. Integers and finite
floating-point values are numbers, `bool` is `true`/`false`, characters and enumerator names are quoted
strings, and a value no enumerator names or a non-finite float is a quoted string, so every spelling is valid
JSON and valid TOML; a consumer that needs the exact value reads `bits`. `dyn::to_rri` writes the value back after the ARG. The byte layout is in chapter 7 §7.4.

Two type flags arrived with format 1.6: `dyn::type::transparent` — the type is a container argument's
synthetic type (one unnamed slot field at 0, size 8; the parser refuses anything else under that flag) — and
`dyn::type::enum_fixed` — the enum has a fixed underlying type (`enum class`, `enum E : T`), so a dictionary-driven
reader can tell which enums carry a value range. The text dump shows `transparent` after `dynamic` and `unfixed`
after an enum's line when the flag is clear; JSON has `"transparent": true` only when set and `"fixed": true|false`
for every enum; TOML `transparent = true` / `fixed = …`; `to_rri` writes both flags back. A `map`/`set` field's
`elem` holds its key and value descriptors (`"offset"` inside one entry) and `"extent"` the entry stride. In the
golden `dynamic.json`:

```json
{"align": 4, …, "name": "std::vector<i32>", "schema_hash": "0x02c87ef3a015eb45", "since": 0, "size": 8, "transparent": true}
{"align": 1, …, "fixed": true, "is_enum": true, "is_flat": true, …, "name": "corpus::Side", …, "underlying": "u8"}
```

## 6.5 The program hash and `--diff`

`srdump --program-hash capture.ser` prints the header's; `srdump --program-hash --elf ./app` recomputes it from
the section's type table (an ELF dictionary carries no header) — the two agree for the binary that wrote the
capture. `srdump --diff a.rri ./b` compares two dictionaries type by type: identical / names-only /
metadata-only / **breaking** (layout differs), added, removed, duplicate-name, plus call and literal changes;
exit code 1 when anything is breaking. The verdicts describe identity, not decodability; `--direction old-data | new-data | both | none`
(decision E) says whose reader a removal or an addition breaks — the default `old-data` (a reader built from the
new side replays old data) makes removals breaking, so a removed type makes `--diff` exit 1 unless
`--direction none` (chapter 10 §10.1b).
