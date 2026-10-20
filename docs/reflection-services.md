# Reflection services — what `es::serreg` uses from C++26 reflection, and what it does not

> **Companion:** `serreg-reflection.md` is the per-function reference of `reflect.hpp` (one paragraph each, with
> file:line and consteval context). This document is the conceptual map; the two are kept separate on purpose
> — this one is read once, that one is looked up.

This document walks through C++26 static reflection (P2996 and its companion papers, as shipped by GCC 16 with
`-std=c++26 -freflection`) in the order a reader needs it: each topic uses only what the topics before it
established. Part A is the vocabulary; Part B is every `std::meta::*` construct the library uses, with the
place and the reason; Part C is the rest of the facility — what the library does not use, and the use cases
each service exists for. Part D collects the practical facts (what runs when, what the compiler does and does
not allow) that decided the library's shape.

Code lives in `include/es/serreg/reflect.hpp` (naming, layout, canonical strings, images) and
`include/es/serreg/projection.hpp` (the `renamed_from` annotation). Everything reflective is header-only: the
runtime (`src/`) never includes `<meta>`.

---

## Part A — The vocabulary

### A.1 `std::meta::info` — a handle to a program entity

Reflection gives the program a *value* that stands for an entity of the program: a type, a data member, a base
class, a namespace, a function, an enumerator, a template, a template argument, a value, an annotation. That
value has the type `std::meta::info`. It is a scalar (copyable, comparable with `==`), it exists only during
constant evaluation, and nothing can be done with it at run time except hold it.

Two `info` values compare equal when they reflect the same entity: `^^int == ^^int`, and `dealias(^^std::size_t)
== ^^unsigned long` on LP64. The library relies on this equality everywhere it classifies a type ("is this
`std::vector`?", "is the enclosing namespace `std`?").

### A.2 The reflection operator `^^`

`^^X` produces the `info` for `X`: `^^T` for a type (also a dependent one inside a template), `^^ns` for a
namespace, `^^::` for the global namespace, `^^std::vector` for a class template (not a specialisation), `^^f`
for a function. It is the only way to *enter* the reflective world from ordinary code; everything else derives
from an `info` already in hand.

### A.3 Splicing `[: r :]` — back to the program

A splice turns a constant `info` back into the entity it reflects: `typename [: r :]` is the type, `[: m :]`
inside an expression is the member (so `obj.[: m :]` reads it), `template [: t :]` the template. The argument
must be a constant expression; that is what forces the "member loop" to be either a `template for` or an index
sequence over a constant array (A.6).

### A.4 Metafunctions run at compile time: `consteval`

Every `std::meta::*` function is `consteval`. It can be called only from a constant-evaluation context: a
`consteval` function, the initialiser of a `constexpr` variable, a template argument, a `static_assert`, the
condition of an `if constexpr`. The whole of `reflect.hpp`'s analysis is therefore a tree of `consteval`
functions whose *results* — strings, hashes, byte arrays, offset tables — are materialised into `constexpr`
variables that the run-time code reads. Nothing reflective survives into the binary except those results.

### A.5 `std::vector` and `std::string` during constant evaluation

C++20 made `std::vector`, `std::string` and the algorithms usable in constant evaluation, with one rule: memory
allocated during the evaluation must be freed before it ends. A `consteval` function may build a
`std::vector<field_desc>` or a `std::string` and return it, and a caller that is itself `consteval` may consume
it — but such an object cannot be the value of a `constexpr` variable. The bridge from "a vector computed at
compile time" to "a constant array the program can read" is the next topic.

### A.6 Promoting compile-time results to static storage: `std::define_static_array`, `std::define_static_string`

`std::define_static_array(range)` (P3491) takes a range of a *structural* type — scalars, `info`, aggregates of
those — evaluated during constant evaluation, and returns a `std::span<const T>` over an array with static
storage duration whose contents are those elements. `std::define_static_string(sv)` does the same for text and
returns a `const char*` / `string_view` to a null-terminated static string. Two calls with equal contents give
the same address (the arrays are deduplicated by value), so a `define_static_string` of a type's canonical name
costs one copy per executable, not one per use.

The library's idiom, repeated for members, bases, enumerators, template arguments and annotations:

```cpp
template<class T> consteval auto members()
{   // a span<const std::meta::info> over a static array — indexable by a constant I
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx()));
}
```

Because `members<T>()[I]` is a constant expression when `I` is a template parameter, it can be spliced: `typename
[: std::meta::type_of(members<T>()[I]) :]` is the type of the I-th member. This is the index-sequence pattern
that drives every walk in the library (`part_t<T, I>`, `get_part<I>(v)`, the fold expressions in
`for_each_part`). A `std::string_view` is *not* structural, so a table of names is built from
`define_static_string` results, and a `std::vector<field_desc>` becomes a `constexpr std::array<field_desc, N>`
through a count-then-fill pair of `consteval` functions (`field_count<T>()`, `make_fields<T>()`), the same way
the ENTRY blobs become `std::array<std::byte, N>`.

### A.7 Expansion statements: `template for`

`template for (constexpr std::meta::info x : static_range) { … }` (P1306) unrolls its body once per element,
with `x` a constant in each copy — so the body may splice it. The range must itself be usable in constant
evaluation and its elements must be constants: a `define_static_array` result qualifies, a `std::vector`
returned by a `consteval` call does not (hence the `define_static_array(...)` wrapped around every
`template for` in the library). The library uses `template for` in exactly four places (B.9) and index
sequences everywhere else, on purpose: an expansion statement needs the compiler to support P1306, an index
sequence needs only `define_static_array` and splices, and the error messages of a fold expression name the
member index.

### A.8 Access: `std::meta::access_context`

Member queries take an *access context*: `access_context::current()` sees what the calling code could name,
`access_context::unchecked()` sees everything, private members included. Serialization needs the whole object,
so the library passes `unchecked()` everywhere (`detail::ctx()`); the `[[=skip]]` annotation, not access, is how
a member is left out.

### A.9 Annotations: `[[= expr ]]`

P3394 lets a declaration carry constant values: `struct S { [[=es::serreg::since(3)]] int venue; };`. The values
are not attributes in the old sense — they are objects of ordinary types, reachable through
`std::meta::annotations_of(m)` as `info`s, each with a `type_of` and extractable with `std::meta::extract<T>`.
This is how `skip`, `since`, `deprecated`, `name("…")` and `renamed_from("…")` reach the library without any
macro, base class or trait specialisation.

---

## Part B — Every `std::meta` construct the library uses

Grouped by the task it serves. "Where" names the function in `reflect.hpp` unless stated; the companion
documents `serreg-reflection.md` and `serreg-registry.md` give the line numbers and one paragraph per function.

### B.1 Reaching the members of a class

| Construct | Where | What for |
|---|---|---|
| `nonstatic_data_members_of(^^T, ctx)` | `members<T>()`, `component_types` | the data members in declaration order, private ones included (A.8). The library never enumerates static members, member functions or nested types: only the object's bytes matter |
| `bases_of(^^T, ctx)` | `bases<T>()`, `component_types` | direct base classes, in order; a base is a positional *part* that precedes the members and appears in the schema as a `<base:Name>` pseudo-field |
| `access_context::unchecked()` | `ctx()` | see A.8 |
| `type_of(m)` | `part_type`, `base_t`, `component_types`, `has_annotation` | the declared type of a member, base or annotation; always wrapped in `dealias`/`remove_cv` before it is classified |
| `offset_of(m).bytes` | `member_offset`, `base_offset`, the gap/bool/enum-range collectors | the native byte offset of a member or base subobject — the number the flat image, the padding-gap table and the `FIELD` records carry. `.bytes` is the byte part of the `member_offset` result; the bit part is unused because bit-fields are rejected |
| `is_bit_field(m)` | `member_field` (a `static_assert`) | a bit-field has no byte address and no `offset_of` in bytes; the library refuses it with a named diagnostic instead of mis-describing it |
| `is_virtual(b)` | `base_field` (a `static_assert`) | a virtual base has no fixed offset inside the most-derived object; refused |
| `identifier_of(m)` | `part_name`, `enumerators_fit`, the enum string | the member's spelling, which becomes the `FIELD` name and part of the schema string |

### B.2 Classifying a type

| Construct | Where | What for |
|---|---|---|
| `dealias(t)` | everywhere a type is inspected | strips `typedef`/`using` names so `std::uint32_t` and `unsigned int` are the same `info`; the canonical string spells the kind (`u32`), never the alias |
| `remove_cv(t)` | with `dealias` | a `const` member serialises like a non-const one |
| `remove_all_extents(t)` | `component_types`, `recursive_type` | reaches the element type of `T[N][M]` for the containment check |
| `is_class_type(t)` | `component_types` | only classes have members to recurse into |
| `is_type(r)`, `is_value(r)`, `is_object(r)` | `canonical_name` (template arguments), `lacks_linkage`, `scope_of` | tells a type argument (`std::array<char, 8>`'s `char`) from a value argument (`8`) and a scope that is a class from one that is a namespace |
| `is_function(p)`, `is_namespace(p)` | `in_std`, `lacks_linkage` | a parent that is a function means a local class (no linkage name); an unnamed namespace means the same |
| `has_identifier(r)` | `lacks_linkage`, `std_implementation_namespace`, `canonical_name` | anonymous namespaces and unnamed classes have none; such a type must carry `[[=name("…")]]` or it is rejected ("has no unique canonical name") |
| `has_template_arguments(t)`, `template_of(t)`, `template_arguments_of(t)` | `component_types`, `canonical_name`, `old_names_of`, `annotated_name` | recognises `std::vector<E>`, `std::pair<K,V>`, … by comparing `template_of(t)` with `^^std::vector`, etc. — a comparison of templates, immune to how the standard library spells its specialisations; spells a user template's name and arguments canonically (`t::W<i32,-5>`); and finds `name<N>`/`renamed_from<N>` annotations, which are specialisations of a template and so cannot be matched by `type_of(a) == ^^A` |
| `parent_of(r)` | `in_std`, `lacks_linkage`, `std_implementation_namespace`, `scope_of`, `canonical_name` | walks outward from an entity to the global namespace: this is how a *qualified, library-independent* name is built (`ns::Order`, `std::vector<i32>`), skipping libstdc++'s `std::__cxx11` and libc++'s `std::__1` (`std_implementation_namespace`) |

### B.3 Enumerations

| Construct | Where | What for |
|---|---|---|
| `enumerators_of(^^E)` | `enumerators<E>()` | the enumerators in declaration order; their names and values go into the enum ENTRY and the schema string (`enum<corpus::Side,u8>[1,1]{buy=1;sell=2;}`) |
| `[: e :]` on an enumerator, `identifier_of(e)` | `enumerators_fit`, the enum string, `enum_range_of` | the value (through `std::to_underlying([:e:])`) and the name; `enum_range_of` derives from the values the minimal-width range an enum *without* a fixed underlying type may hold (decision D) |

The underlying type comes from `std::underlying_type_t`, not from reflection, and whether the enum has a *fixed*
underlying type from a core-language probe (`has_fixed_underlying_type_v<E> = requires { E{0}; }` — list-
initialising an enum from an integer is well-formed only when the underlying type is fixed), because P2996's
`has_fixed_underlying_type` was not relied on across both compilers.

### B.4 Annotations

| Construct | Where | What for |
|---|---|---|
| `annotations_of(r)` | `has_annotation`, `annotation_value`, `annotated_name`, `old_names_of` | the `[[= … ]]` values on a member or a class |
| `type_of(a)` + `remove_cv` | the same | which annotation it is: `skip_t`, `since`, `deprecated` by `== ^^A`; `name<N>` and `renamed_from<N>` by `template_of` |
| `extract<A>(a)`, `extract<typename [: ty :]>(a)` | `annotation_value`, `annotated_name`, `old_names_of` | the annotation's value as an object of its type: `since{3}.v`, `name<N>::view()` |

### B.5 Static storage for compile-time results

| Construct | Where | What for |
|---|---|---|
| `std::define_static_array` | `members`, `bases`, `enumerators`, `canonical_name` (template arguments), `annotated_name`, `old_names_of` | A.6: constant, indexable spans of `info` the index-sequence walks splice |
| `std::define_static_string` | `canonical_name_v`, `annotated_name_v`, `properties_name_v`, `part_name` in `make_fields`, `scope_name_v`, the rename tables | the names the descriptors, the `FIELD` records, the dictionary and the diagnostics point at — one copy each |

### B.6 Splices in the library

- `typename [: type_of(members<T>()[I]) :]` → `part_t<T, I>`; `typename [: type_of(bases<T>()[I]) :]` →
  `base_t<T, I>`: the types the walks recurse into.
- `v.[: members<T>()[I] :]` → `get_part<I>(v)` for a class (a `std::get<I>` for a tuple-like): the member
  reference the encoders and decoders read and write — private members included, which is why measure/encode/
  decode live in `reflect.hpp` and not in the runtime.
- `[: e :]` for an enumerator's value; `typename [: ty :]` for the type of an annotation; `typename [: P :]` for a
  scope that is a class.

### B.7 What is computed from these, and where it ends up

| Compile-time result | Built by | Lives in |
|---|---|---|
| canonical name (`ns::Order`, `std::map<i32,std::string>`) | `canonical_name<T>()` from `parent_of`/`identifier_of`/`template_arguments_of`, or `[[=name]]`, or `type_properties<T>::name()` | `canonical_name_v<T>` (static string), the struct/enum ENTRY, the dictionary |
| schema string / layout string | `canonical_string<T>(schema)` over the parts: names, kinds, offsets, sizes, alignments, `since`/`deprecated`, enumerators | `schema_string_v<T>` / `layout_string_v<T>`; hashed by XXH3-64 into `schema_hash_v` / `layout_hash_v` |
| `field_desc` table | `make_fields<T>()` from `offset_of`, `type_of`, `identifier_of`, annotations | `descriptor_v<T>.fields` (`.rodata`), the `FIELD` records of the ENTRY |
| padding gaps, bool offsets, enum ranges | the leaf collectors over `offset_of` | `gaps_v<T>`, `bools_v<T>`, `enum_ranges_v<T>` — the scrub tables the flat writer and the readers use |
| canonical offsets of a dynamic class | `compute_canonical_offsets<T>()` from sizes and alignments of the parts | the image layout (`{u32 off, u32 n}` slots) |
| the ENTRY blob | `type_blob_bytes<T>()` from the descriptor | `type_blob_bytes_v<T>` (`std::array<std::byte, N>`), copied into the `es_serreg_dict` section (see `registry-services.md`) |
| rename tables | `collect_renames<T>()` from `annotations_of` on each part | `renames_v<T>`, `type_old_names_v<T>` (`projection.hpp`) |

### B.8 Checks that only reflection can make

`check_supported<T>()` is a `consteval` audit run once per type before any of the above is built: it rejects
pointers, references, unions, polymorphic classes, bit-fields, virtual bases, `std::string_view` members, smart
pointers, `std::tuple<>`, every `std` class outside the vocabulary (`in_std` + `has_properties_name`), types that
contain themselves (`recursive_type`, a reflective graph walk over `component_types` that never instantiates a
template, so a recursive type gets a sentence instead of a 400-line instantiation backtrace), and types without a
linkage-unique name (`lacks_linkage`). Each rejection is a `static_assert` whose message names the rule and the
alternative (`as_flat<T>`, `type_properties<T>`, `std::optional<T>`, `[[=name("…")]]`).

### B.9 The four `template for`s

1. `annotated_name<U>()` — over `annotations_of(^^U)`: finds the `name<N>` annotation (the splice
   `extract<typename [: ty :]>` needs `ty` constant, which only an expansion statement or a template gives).
2. `old_names_of<M>()` (`projection.hpp`) — the same for `renamed_from<N>`, repeatable.
3. `canonical_name<T>()` — over `template_arguments_of(r)` of a user class template: each argument is spliced
   as a type (`canonical_name<[:a:]>()`) or rendered as a value.
4. `enumerators_fit<E>()` — over `enumerators<E>()`: reads each value through `[: e :]`.

Everything else — fields, strings, offsets, images, registration, projection plans — is an index sequence over
a `define_static_array` result (A.6), the form the file's header comment calls the "fallback" and which is in
practice the primary.

---

## Part C — What the library does not use, and what each service is for

The facility is larger than a serializer needs. Services below are grouped; for each: what it does and the kind
of program that wants it. None of them is wrong for this library — each was left out because the serializer
has no question it would answer.

### C.1 Members beyond data: functions, static members, nested entities

`members_of(^^T, ctx)` returns *every* member — functions, static data members, member templates, nested types,
enumerations — and `static_data_members_of`, and the predicates `is_function`, `is_static_member`,
`is_constructor`, `is_destructor`, `is_special_member_function`, `is_virtual`, `is_pure_virtual`,
`is_override`, `is_final`, `is_deleted`, `is_defaulted`, `is_explicit`, `is_noexcept`, `is_const`/`is_volatile`
(on member functions), `return_type_of`, `parameters_of`, `has_default_argument`.

Use cases: generating bindings (Python, Lua, a REST surface) from a class's public interface; a mock/stub
generator; a command-line parser that maps sub-commands to member functions by name (`identifier_of`); interface
conformance checks ("every `virtual` in `Base` is overridden in `Derived`") as `static_assert`s; a
dependency-injection container that reads constructor parameters. The serializer only ever looks at the
object's bytes, so `nonstatic_data_members_of` is the whole story.

### C.2 Access and visibility

`is_public`, `is_protected`, `is_private`, `is_accessible(m, ctx)`, `access_context::current()` /
`::via(^^Friend)`.

Use cases: a serializer that *respects* encapsulation (serialise public members only, or members accessible
from a designated friend); a documentation generator that documents the public surface; a style checker
(`static_assert` that a value type has no protected data). The library's design decision is the opposite —
the full object image, with `[[=skip]]` as the opt-out — so it always passes `unchecked()`.

### C.3 Type properties the standard traits already answer

`is_const_type`, `is_volatile_type`, `is_lvalue_reference_type`, `is_pointer_type`, `is_enum_type`,
`is_scalar_type`, `is_trivially_copyable_type`, `is_default_constructible_type`, `size_of`, `alignment_of`,
`bit_size_of`, `is_complete_type`, `is_enumerable_type`, `has_fixed_underlying_type`, `underlying_type_of`
(and `type_` prefixed variants in older drafts).

These are the `<type_traits>` and `sizeof`/`alignof` facts phrased on `info`. They matter when the *only* thing
in hand is an `info` — a type reached through `template_arguments_of` that one does not want to splice, or a
member type that may be incomplete. The library splices first (`part_t<T, I>`) and then uses the ordinary
traits (`std::is_trivially_copyable_v`, `sizeof`, `std::underlying_type_t`), which read better in diagnostics
and are what the non-reflective parts of the code already use. `size_of`/`alignment_of` on `info` would let the
canonical-offset computation avoid splicing; it was not needed.

### C.4 Constructing reflections of values and arrays

`reflect_constant(v)` (a reflection of a constant value), `reflect_object(obj)`, `reflect_function(f)`,
`reflect_constant_string`, `reflect_constant_array`, `constant_of`, `value_of`, `object_of`, `extract<T>(r)` for
non-annotation reflections.

Use cases: building template argument lists at compile time (`substitute`, C.6) from computed values;
turning a `consteval`-computed table into a *template argument* (which the library instead stores in a
`constexpr` variable); a compile-time interpreter or expression evaluator that passes values around as `info`.
`define_static_array` is implemented over `reflect_constant_array` + `extract`; the library uses the
higher-level form only.

### C.5 Templates: `substitute`, `can_substitute`, `is_template`, `is_class_template`, `is_specialization`

`substitute(^^tmpl, {args…})` instantiates a template with reflected arguments and returns the specialisation
as an `info`; `can_substitute` asks whether that would be well-formed without hard errors.

Use cases: type-list algorithms without a metaprogramming library (map a pack of `info`s through a template,
build `std::variant<Ts…>` from a computed set of types); a serializer that *constructs* a reader type for a
dictionary entry at compile time; "does `Hash<T>` exist for this `T`?" probes that replace `requires`-expression
gymnastics. The library's type set is fixed by the program's calls, so it never has to build a type from
reflected pieces.

### C.6 Namespaces and the global scope

`members_of(^^ns)` on a namespace (all entities declared in it), `is_namespace`, `is_inline_namespace`,
`is_namespace_member`.

Use cases: **registration by scope** — "every class in `namespace msgs` is a message": iterate the namespace,
filter with `is_class_type` and an annotation, instantiate the registrar for each — no call site needed. This is
the one unused service with an obvious home in this library: `register_types<Ts…>()` today asks the programmer
to name the types; a `register_namespace<^^msgs>()` built on `members_of` would find them. It was not done
because `members_of` on a namespace sees only the declarations visible *at that point of the TU*, so the result
depends on include order — acceptable for a dedicated registration TU, surprising elsewhere. Also: generating a
test that round-trips every type of a namespace; an API inventory tool.

### C.7 Source locations and spellings

`source_location_of(r)`, `display_string_of(r)`, `u8identifier_of`, `u8display_string_of`, `qualified_name_of`
(in some drafts), `has_identifier` on everything.

`display_string_of` is the compiler's rendering (`std::vector<int, std::allocator<int>>`) — exactly what the
canonical names must *not* depend on, which is why the library builds names from `parent_of` + `identifier_of`
by hand (B.2). `source_location_of` is for diagnostics and tooling: a serializer could record the file:line
where a message type is declared into the dictionary (a `tag_source` ENTRY) so that `srdump` can say "declared at
md/quote.hpp:41"; a documentation generator; an "unused message type" linter.

### C.8 Data-member and class synthesis: `define_aggregate`, `data_member_spec`

`define_aggregate(^^Incomplete, {data_member_spec(^^int, {.name = "x"}), …})` completes a forward-declared
class with the given members, at compile time.

Use cases: **struct-of-arrays transforms** (from `struct P {float x, y;}` synthesise `struct SoA { float* x;
float* y; }`), tuple-to-named-struct conversion, generating a *reader* type for a dictionary ENTRY whose layout
is known only from data (a compile-time dictionary → a C++ type, which is what `srdump --cxx` does as text
today), builder/options structs with one member per annotated field. For evolution level 2 this is the service
that would synthesise the writer's old layout as a type so the projection could be a typed copy instead of an
offset table. GCC 16 implements it; the library does not need to invent types.

### C.9 Enumerations beyond the list

`is_enumerator`, `is_scoped_enum`/`is_unscoped_enum` (on `info`), `enumerators_of` with ranges in
`template for` producing a `switch`, `constant_of` on an enumerator.

Use cases: `enum_to_string`/`string_to_enum` without macros (the library does the first half inside the dump
tools from the *dictionary*, not from reflection, so a reader without the type still works); exhaustive
`switch` generation; flag-set printers. `is_scoped_enum` would let the library spell `enum class` in the
`--cxx` output; today it prints `enum class` for every enum, which is a documented approximation.

### C.10 Parameters, lambdas, and expression-level reflection

`parameters_of(^^f)`, `identifier_of(param)`, `has_identifier(param)`, `is_explicit_object_parameter`,
`variable_of`, `has_consistent_identifier`.

Use cases: logging the *names* of a function's parameters together with their values (`log(f, args…)` prints
`price=…, qty=…`), a command-line binder, Python-style keyword arguments, contract-like diagnostics. The
serializer's call signature is positional and nameless by design (the literal gives the message its meaning),
so parameter names never enter the wire format.

### C.11 `std::meta::exception` and the diagnostics of reflection

Metafunctions throw `std::meta::exception` (a constant-evaluation-time exception, P3560) when a precondition
fails — `type_of(^^int)`, `offset_of` on a function. Catching it in a `consteval` function turns "is this
well-formed?" into a boolean without a hard error.

Use case: probing — "does this entity have a parent?" (GCC 16 cannot take `parent_of(^^main)`; the library's
`in_std` stops at a function *before* asking). A library written against the final standard would wrap such
probes in `try`/`catch` of `std::meta::exception`; `reflect.hpp` avoids the situations instead because the
compilers it must build on differ in what throws.

### C.12 Expansion-statement forms the library does not use

`template for` over a *tuple-like* (`std::tuple`, `std::array`) or over an initializer list of `info`s, and
`template for` with structured bindings.

Use cases: iterating a `std::tuple` of handlers, a `std::array<info, N>` of chosen members (a "serialise these
three fields only" projection selected at compile time). The library reaches tuple elements through `std::get<I>`
in an index sequence for the same reason it does for members (A.7).

---

## Part D — Practical facts that shaped the code

1. **Everything is `consteval`, so everything is checked at compile time and costs nothing at run time.** A
   serialized type adds to the binary: its canonical name, its descriptor and `FIELD` table, its scrub tables,
   its ENTRY blob (twice: the `.rodata` copy the descriptor points at and the `es_serreg_dict` copy), and the
   instantiated encoders. It adds nothing to the per-message path beyond the `memcpy` or the image walk.

2. **Splices need constants; constants need `define_static_array`.** A `std::vector<info>` returned from a
   `consteval` function cannot be spliced from or iterated by `template for`; wrapping it in
   `define_static_array` is what makes `members<T>()[I]` usable. Non-structural element types (`string_view`,
   a struct holding one) cannot be promoted this way — hence the count-then-fill `std::array` pattern for the
   `field_desc` and rename tables.

3. **Two compilers, one dialect.** GCC 16 (`-freflection`) and clang-p2996 (`-freflection-latest`) agree on the
   set in Part B. Known differences the library works around: GCC cannot take `parent_of(^^main)` (so walks stop
   at the first function parent); clang-p2996 rejects a `std::string` temporary in an `if constexpr` condition
   (so names are compared through `define_static_string` results); EDG rejects `template for`. The Compiler
   Explorer lanes (`docs/ce-links.md`) keep both compilers honest on the canonical names.

4. **Names are built, never asked for.** No `display_string_of` anywhere: a name that depends on the standard
   library's spelling (`std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>`) would
   move the schema hash between libstdc++ and libc++. The canonical-name rules (DECISIONS P-3) are implemented
   with `parent_of`, `identifier_of`, `template_of` and `template_arguments_of` only.

5. **Reflection reads private members; the ODR does not care.** Serialising private members through
   `unchecked()` and splices is well-formed and does not require friendship; it is also why the walks must be in
   the header (a runtime library cannot splice a type it has not seen).

6. **What would change with the services in Part C.** `members_of(^^ns)` would give registration by namespace
   (C.6); `define_aggregate` would give synthesised reader types for old layouts (C.8); `source_location_of` would
   give "declared at" in the dictionary (C.7); `parameters_of` would give named arguments for the logger (C.10).
   Each is a feature decision, not a gap in the serializer.

---

## Part E — The library's own reflection services: `es::serreg::*` built directly on `std::meta`

Parts A–D describe the language. This part lists what the library *adds on top*: the public names a program
uses, each of which is a `consteval` computation over the constructs of Part B materialised into a constant.
"Customise" says how a user type influences the result without touching the library.

### E.1 Annotations the library defines (A.9)

| Annotation | On | Effect | Read through |
|---|---|---|---|
| `[[=es::serreg::skip]]` | a member | left out of the fields, the strings, the image and the registry walk; its bytes become a padding gap the flat writer scrubs | `has_annotation<skip_t>` → `member_skipped<T, I>` |
| `[[=es::serreg::since(N)]]`, `[[=es::serreg::deprecated(N)]]` | a member or a class | recorded in the `FIELD`/ENTRY (`u16`) and in the schema string, so they are part of the schema hash; shown by the dump tools; the input of evolution reports | `annotation_value<A>` → `part_annotation<A, T, I>` |
| `[[=es::serreg::name("ns::Name")]]` | a class or enum | replaces the computed canonical name (for a type in an unnamed namespace, local to a function, or one that must keep a stable name after moving); validated character set; rejected together with `type_properties` | `annotated_name<U>()` (a `template for`, B.9) |
| `[[=es::serreg::renamed_from("old")]]` | a member or a class, repeatable | evolution level 1: a writer's member/type with the old name fills this one; not part of the strings or the dictionary | `old_names_of<M>()` → `renames_v<T>`, `type_old_names_v<T>` (`projection.hpp`) |

### E.2 Customisation points (ordinary templates, consulted by the reflective code)

| Point | Shape | What it changes |
|---|---|---|
| `es::serreg::type_properties<T>` | `static consteval std::string name();` | the canonical name of a type that cannot be annotated — the mechanism the library itself uses for the whole std vocabulary (`std::string`, `std::vector<E>`, `std::map<K,V>`, `std::deque<E>`, …), so names never depend on a standard library's spelling; the presence of a specialisation is also what admits a `std` class into the vocabulary (`has_properties_name`) |
| `es::serreg::as_flat<T>` | `using type = U; static U to(const T&); static T from(const U&);` | serialise `T` *as* another flat type `U`: `T` is never reflected — `U` is; the way to carry a type with pointers, unions or non-trivial members, or to pin a wire layout independent of `T` |
| `es::serreg::formattable<T>` | opt-in | `std::formatter` for any serialisable value through `to_string` (uses the same walks) |

### E.3 Public constants per type — the reflection results a program can read

All are `inline constexpr`, computed once per type per executable, zero-cost unless read at run time.

| Name | Type | Built from | Typical use |
|---|---|---|---|
| `canonical_name_v<T>` | `std::string_view` | `parent_of`/`identifier_of`/`template_*` or `name`/`type_properties` | diagnostics, dictionary lookups by name, the schema string's head |
| `schema_string_v<T>`, `layout_string_v<T>` | `std::string_view` | `canonical_string<T>(true/false)` over the parts | seeing exactly what is hashed; golden tests (`fingerprint64(schema_string_v<T>) == schema_hash_v<T>`); documentation generators |
| `schema_hash_v<T>`, `layout_hash_v<T>` | `std::uint64_t` | XXH3-64 of the two strings | the identity of a type on the wire and in the registry; `static_assert`s that freeze a format |
| `descriptor_v<T>` | `const type_desc&` (`.rodata`) | `make_fields`, gaps, bools, enumerators, the ENTRY blob | everything run-time: the registry node's payload, the dump tools, the projection planner, user code that wants fields and offsets without reflection of its own |
| `call_hash_v<Ts…>`, `call_desc_v<Ts…>` | hash / descriptor of a call signature | the call string over `arg_name<T>` of each argument | matching a frame to a handler; `frame_router` |
| `lit<"text">::id`, `lit<…>::text()` | literal id and text (the text is read from the literal's own ENTRY blob) | `fingerprint64` | message identity in print/log frames |

### E.4 Concepts and categories

| Concept | Meaning |
|---|---|
| `flat_type<T>` | trivially copyable class or enum: its native bytes are the record (after padding scrub) |
| `dynamic_type<T>` | a class that needs a canonical image (strings, containers, optionals, variants, tuple-likes inside) |
| `detail::serializable_arg<T>` | the five argument categories: a type (flat or dynamic), a literal, a constant (`42_`, `k<V>`), a scalar, a runtime string |
| `has_as_flat<T>`, `has_properties_name<T>` | the two customisation points are present |

The split flat / dynamic is decided by reflection-free traits, but which *members* make a class dynamic
(`member_dynamic<T, I>`), and whether a type is supported at all (`check_supported<T>()`, B.8), are reflective.

### E.5 The parts layer — the walk every service shares

`detail::base_count<T>()`, `part_count<T>()`, `part_t<T, I>`, `base_t<T, B>`, `part_name<T, I>()`,
`get_part<I>(v)`, `part_annotation<A, T, I>()`: one positional view of a class (bases first, then parts) over
`members<T>()`/`bases<T>()` for ordinary classes and over `std::tuple_size`/`std::get` for `std::pair` and
`std::tuple`. Fields, canonical strings, canonical offsets, measure/encode/decode, registration (`touch`) and
the projection planner are all index-sequence folds over this layer, so a new kind of class (a tuple-like, say)
is added in one place. `for_each_part(v, f)` is its run-time form.

### E.6 Services that are reflective at compile time and empty at run time

| Service | What happens at compile time | At run time |
|---|---|---|
| `serialize(sink, args…)` | `touch_arg<T>()` for each argument instantiates the registrars (the odr-use), `check_supported`, the call string and hash, the measure/encode walk | header + `memcpy` or image walk; the registration already happened at static init |
| `deserialize`, `dispatch`, `frame_router`, `view<T>` | the same registrars, the record layout from `record_payload_size<T>()`/`is_dynamic<T>()`, the decode walk | hash compare, bounds checks, copy or image decode |
| `register_types<Ts…>()`, `register_call<Sig…>()` | the registrars only | nothing (an empty inline function) |
| `tools::project<T>`, `deserialize(dict, …)` | the rename tables, `descriptor_v<T>`, the part types for the planner | a plan built once per writer type, then offset copies |
| `type_blob_bytes_v<T>` and the `es_serreg_dict` copy | the ENTRY bytes from the descriptor | bytes in `.rodata` and in the section; nothing executes |

### E.7 Services that are deliberately *not* reflective

`to_string`/`to_json`/`render` of a value *with* the type use the walks above; `srdump`, `walk_values` without
compiled types, the Wireshark and C++ generators, and the Python reader work from the **dictionary** (the ENTRYs
of §1 in `registry-services.md`) and never instantiate a template per type — so a reader needs none of the
writer's headers, and a type's description is computed exactly once, by the compiler that saw it.
