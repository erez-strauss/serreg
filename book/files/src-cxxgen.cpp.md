# `src/cxxgen.cpp`

`tools::cxx_header()` from `include/es/serreg/tools.hpp` (plan A5; `srdump --cxx`, chapter 10.4b): C++26
type definitions generated from one `dyn::dictionary`. The output is one header that declares every enum and struct
of the dictionary so that, compiled with this library, each regenerated type has exactly the schema hash and layout
hash the dictionary records — and checks that itself with `static_assert`s at the end of the file. Everything comes
from the dictionary model: kinds become fixed-width scalars, the `elem` descriptors become `std::array` /
`std::vector` / `std::optional` / `std::variant`, `<base:…>` pseudo-fields become base classes, `since` /
`deprecated` are carried, the std vocabulary (`std::string`, `std::pair<…>`, `std::monostate`, …) is spelled back
and never defined, and a flat type's recorded offsets are reproduced with `[[=es::serreg::skip]]` padding members
(absent from the canonical strings and scrubbed on the wire — what a gap is). A name a plain declaration cannot
spell (a template specialisation, a class nested in a class, a namespace moved by `--namespace`) is declared under
a sanitized identifier with `[[=es::serreg::name("<canonical>")]]`, which makes the library use the recorded name.
With `cxx_options::router` a `frame_router` skeleton follows, one handler per call signature. The same dictionary
always gives the same text (every container walked is ordered). This is the **tools layer**: no reflection
dependency, nothing on a hot path, allocation is free.

## Depends on / used by

Includes: `internal.hpp` (`dec`, `hex64`, `hex8`, `abi_text`), `es/serreg/tools.hpp` (`cxx_options`),
`<algorithm>`, `<bit>`, `<charconv>`, `<limits>`, `<map>`, `<set>`. From `dictionary.hpp`: `dyn::dictionary`
(`types`, `literals`, `calls`, `find_type`, `find_literal`, `program_hash`, `computed_program_hash()`, `abi`,
`same_abi()`), `dyn::type`, `dyn::field` (`elem`, `extent`, `since`, `deprecated`), `dyn::call`, `dyn::call_arg`
(`ak`, `k`, `hash`, `value`). From `descriptor.hpp`: `kind`, `kind_size`, `is_scalar_kind`, `to_string(kind)`.
From `format.hpp`: `arg_kind`, `format_major`/`format_minor`, `hash_algo_xxh3_64`, `host_abi_tag`.

Library: `es::serreg_tools` (`libserreg_tools`).

Used by: `src/cli.cpp` (`write_cxx()`, the `--cxx <file.hpp|->`, `--cxx-router` and `--namespace old=new`
options and the config keys `cxx`, `cxx_router`, `namespace`), the ctests of `tests/cxxgen/` (label `cxxgen`:
`cxxgen_golden_compiles`, `cxxgen_golden_dynamic_compiles`, `cxxgen_golden_namespace_compiles`,
`cxxgen_golden_roundtrip`, `cxxgen_golden_dynamic_roundtrip`) and `tests/unit/test_tools.cpp` (`[tools][cxx]`).
The generated header is compiled by the user's program together with `<es/serreg/serreg.hpp>`.

## Structure

- **Header comment** (`cxxgen.cpp:1-16`): what the output is and the rules below in one paragraph.
- **Canonical-name parsing** (`cxxgen.cpp:46-162`): `split_scopes()` (components outside `<…>`, split on `::`),
  `split_args()` (template arguments, split on top-level commas), `ident_char()`, `is_number()`,
  `annotatable()` (the character set `[[=es::serreg::name]]` accepts — `reflect.hpp`'s `valid_explicit_name`),
  `sanitize()` (an identifier from any component: `Box<i32,3>` → `Box_i32_3`), `join()`.
- **Spellings** (`cxxgen.cpp:165-280`): `scalar_spelling()` (kind → `std::int32_t`, `double`, `char8_t`,
  `std::byte`, …), `kind_from_name()` (the reverse, for template arguments), `is_signed_kind()`, `cxx_str()` (a
  C++ string literal of arbitrary bytes: `\xNN""` so a following hex digit does not extend the escape),
  `int_literal()` (`INT64_MIN` has no literal), `sign_extend()` (a constant's low bytes), `float_literal()`
  (`std::to_chars` shortest round trip, with a `.0` when it has no point or exponent; empty for a non-finite
  value), `align_up()`.
- **`struct plan`** (`cxxgen.cpp:282-291`): where and how one dictionary type is declared — its namespace
  components (after `--namespace`), its identifier, how the rest of the header names it, whether the name
  annotation is needed, whether the declaration reproduces the canonical name at all, and whether it is emitted
  (the std vocabulary is not).
- **`struct generator`** (`cxxgen.cpp:293-979`): the dictionary, the options, the plans, `by_name` (canonical
  name → schema hash), the standard headers the output needs, the banner notes and the emission order.
  - `remap()` (`cxxgen.cpp:310-328`): the first `--namespace old=new` rule whose `old` components prefix the
    type's namespace replaces them.
  - `plan_types()` (`cxxgen.cpp:330-385`): one plan per type. A type whose canonical name starts with `std::` — the
    vocabulary, and the transparent type of a container argument (`std::vector<i32>`) — is never
    defined, only spelled on demand. The namespace part of a name ends at the first
    component that is a class — one with template arguments, or a prefix the dictionary knows as a type (a nested
    class is declared beside its parent, not inside it, under its full canonical name). Two names that sanitize
    alike get the schema hash appended. A canonical name the annotation cannot carry (an enum template argument
    renders as `enum(7)`) is declared without it, noted in the banner, and checked by layout hash only.
  - `spell_name()` / `spell_std()` (`cxxgen.cpp:389-447`): a canonical name that is not declared here — a scalar
    kind, a number or `true`/`false`, a dictionary type (its planned name), or a std type with its arguments
    spelled recursively (`std::pair<i32,ns::Box<i32>>` → `std::pair<std::int32_t, ns::Box_i32>`), adding the
    header it lives in to the includes; the headers table knows every container of the vocabulary (`<map>` for
    `map`/`multimap`, `<unordered_map>` for `unordered_map`/`unordered_multimap`, `<flat_map>`, `<set>`,
    `<unordered_set>`, `<flat_set>`, `<deque>`, `<list>`, `<forward_list>`, `<stack>`, `<utility>` for `pair`,
    `<tuple>`), so a transparent argument type `std::unordered_map<i32,std::string>` or `std::deque<i32>` is
    spelled back from its recorded vocabulary name — container, key and value — and its header included.
  - `type_ref()`, `member_type()` (`cxxgen.cpp:444-509`): the spelling of a member, element or argument: scalars,
    referenced types by hash (a hash the dictionary lacks becomes `es_serreg_missing_<hash>` and a note),
    `std::array<E, N>`, `std::string`, `std::vector<E>`, `std::optional<E>`, `std::variant<A, B, …>`, and
    `std::map<K, V>` for a `map` member and `std::set<K>` for a `set` member, recursive through `elem`. The
    dictionary records the shape, not the container, so an `std::unordered_map` member comes back as `std::map`
    — with the same schema and layout hashes (the kindspec is the shape), so the header's self-checks pass and
    the regenerated type decodes the same frames (`docs/deviations.md` "Containers").
  - `collect_deps()`, `order_types()` (`cxxgen.cpp:511-566`): a type is emitted after every type its fields
    reference (through element descriptors, and through the members of std types that are spelled rather than
    defined); among the ready types the first by canonical name goes next, so the order is total and stable.
  - `is_base()`, `empty_struct()`, `type_attrs()`, `field_attrs()` (`cxxgen.cpp:581-609`): the `<base:…>`
    pseudo-field test, the empty-class test, and the attribute lists (`name`, `since`, `deprecated`).
  - `declare_enum()` (`cxxgen.cpp:598-610`): `enum class [[…]] Name : <underlying> { a = 1, … };` (a `bool`
    underlying type gets `true`/`false` enumerators: a converted constant expression allows no narrowing).
  - `member_line`, `declare_struct()` (`cxxgen.cpp:612-713`): the struct body (below).
  - `checks()` (`cxxgen.cpp:718-746`): the `static_assert`s of one type.
  - `constant_value()` (`cxxgen.cpp:751-805`): the `V` of `es::serreg::constant<V>` for a constant argument.
  - `handler()` (`cxxgen.cpp:808-905`): one `r.on…` line for a call signature.
  - `run()` (`cxxgen.cpp:907-978`): the declarations (opening and closing one `namespace a::b` block per run of
    types), the router skeleton, then the banner, the includes, the declarations, the literal list, the checks and
    the skeleton, in that order — the declarations are generated first because they decide the includes and the
    notes of the banner.
- **`tools::cxx_header()`** (`cxxgen.cpp:996-1000`): `generator{d, o, …}.run()`.

## How it works

### Flat types: the recorded offsets reproduced

```cpp
const std::uint64_t natural = align_up(end_of(), f.align ? f.align : 1);
if (f.offset > natural)
{
    lines.push_back({"    [[=es::serreg::skip]] std::byte " + pad_name() + "[" +
                         dec(f.offset - natural) + "];\n",
                     natural, f.offset - natural, true, true});
}
```

(`cxxgen.cpp:669-675`.) For a flat type (`is_flat`) the dictionary's offsets are native, so the generated struct
must put every member where the writer's compiler did. Members are emitted in field order, and the natural
position of each one is computed from the end of the layout so far, aligned to the member's recorded alignment.
Where the recorded offset is beyond the natural position the gap is filled with a `[[=es::serreg::skip]]
std::byte _skipN[k]` member: a skipped member is left out of the descriptor and of both canonical strings, so it
changes no hash, and its bytes are scrubbed on the wire — exactly what the gap was in the original (a
`[[=skip]]`ped member there, `corpus::Skipped`; an alignment the natural layout would not give). Natural padding
(`corpus::Padded`'s 7 bytes after `tag`) is not spelled out: the compiler reproduces it. After the last member, a
recorded size above the natural `sizeof` gets a trailing skip member; a recorded alignment above every member's
gets `alignas(N)` on the struct. Two cases go the other way: an empty member whose successor's offset is *below*
the natural position was `[[no_unique_address]]` in the original (`corpus::WithEmpty`), so the attribute is put
back and the layout recomputed; anything else below the natural position is a packed layout, which the library
cannot reproduce — a `// NOTE: packed layout` line in the body, a note in the banner, and a `static_assert` that
will say so. An empty base (`<base:…>` of a class without fields) advances nothing (empty-base optimisation).
Dynamic types (`is_flat == false`) carry canonical image offsets, not native ones: their members are emitted in
field order with no padding and no `alignas`; the image layout follows from the member types alone.

### The self-check

```cpp
std::string checks(const dyn::type& t, const plan& p) const
{
    const std::string q = p.qualified;
    std::string s;
    if (t.is_flat)
        s += "static_assert(sizeof(" + q + ") == " + dec(t.size) + " && alignof(" + q +
             ") == " + dec(t.align) + ");\n";
    else
        s += "static_assert(es::serreg::descriptor_v<" + q + ">.size == " + dec(t.size) +
             " && es::serreg::descriptor_v<" + q + ">.align == " + dec(t.align) + ");\n";
```

(`cxxgen.cpp:731-740`.) After all namespaces are closed, every type — the std vocabulary included, since its
spelling must hash like the writer's — gets: `sizeof`/`alignof` for a flat type, or the image's fixed part through
`descriptor_v<T>.size`/`.align` for a dynamic one; `offsetof(T, member)` for every member of a flat type without
bases (`offsetof` on a type with bases is only conditionally supported); `canonical_name_v<T> == "…"`;
`schema_hash_v<T> == 0x…ull`; `layout_hash_v<T> == 0x…ull`. A type whose name is not reproducible keeps the
layout check only, with a comment saying what it stands for. Compiling the header is therefore the proof of the
regeneration, and a failing assert names the type and the quantity that differs.

### The router skeleton

The forms follow `router.hpp`: a literal first and only values after it is `r.on("lit"_, h)`; values only is
`r.on(h)`; a print call without constants is `r.on_print` / `r.on_println("fmt"_, h)`; anything with a constant,
or a literal that is not first, spells the whole signature — `r.on<es::serreg::lit<"fill">,
es::serreg::constant<42>, corpus::Order, es::serreg::constant<corpus::Side::sell>, es::serreg::constant<-1.5>>(h)`,
with `es::serreg::println_tag` first for a print form. The handler's parameters are the non-literal, non-constant
arguments in order: scalars by value (`std::int32_t a1`), strings as `std::string_view`, enums by value, classes by
`const T&`; each is `[[maybe_unused]]` so the empty `/* TODO */` body compiles under `-Wextra -Werror`. A constant
is spelled as the value of its own type, because the call string names the type (`const:i32=42`): `42` for `i32`
(what `42_` gives), `std::int64_t{5000000000}`, `std::uint32_t{7u}`, `true`, `'x'`, `std::byte{1}`, a floating
value as its shortest round-trip literal (`-1.5`, `1.5f`) or, when not finite, `std::bit_cast<double>(0x…ull)`, and
an enum constant as its enumerator (`corpus::Side::sell`) or a `static_cast` when no enumerator has that value. A
call that names an unknown literal or type, or a type whose name is not reproducible, becomes a comment.

### Limits

- A canonical name outside the annotation's character set (`t::W<i32,enum(7)>`: an enum template argument) cannot
  be given back; the type is declared under its sanitized identifier, noted in the banner, and only its layout
  hash is checked; calls using it are skipped in the skeleton.
- A packed layout (`#pragma pack`, `[[gnu::packed]]`) and a layout from another ABI (a 2-byte `wchar_t`, another
  `max-align`) are declared as recorded and fail their asserts; the banner says why.
- Unscoped enums come back as `enum class` (the dictionary does not record the distinction; the hashes do not
  depend on it). Private members come back public; `std::array<E, N>` stands for both `E[N]` and `std::array` (the
  same canonical string and layout).
- A member type the dictionary lacks (a source left out of `--dict`/`--elf`) is named `es_serreg_missing_<hash>`
  and noted; the header does not compile until that source is added.
