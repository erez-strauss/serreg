# es::serreg — the reflection layer, entry by entry

> **Companion:** `reflection-services.md` explains the same material by topic (concepts first, with what the library does not use and why); this document is the per-function reference.

*Library 2.2.5. Line numbers refer to the tree as of 2026-10-19. All reflection lives in one header,
`include/es/serreg/reflect.hpp`, which is the only header that includes `<meta>` apart from
`include/es/serreg/projection.hpp` (the `renamed_from` tables, last section). Everything here is in namespace
`es::serreg::detail` unless noted. The companion document for the registry is
[`serreg-registry.md`](serreg-registry.md).*

## How to read the "Context" line

| Context | Meaning |
|---|---|
| **consteval** | An immediate function. Every call is evaluated by the compiler, even when the call is written inside an ordinary run-time function (for example `touch<T>()` calling `check_supported<T>()`). Nothing of it remains in the binary except the constants it produced. |
| **compile-time constant** | A `constexpr` variable template, a class template (trait) or a concept. There is no call: the value is computed when the template is instantiated, and it is used in `if constexpr`, `static_assert`, template arguments or to initialise `constexpr` data. |
| **run time** | An ordinary (inline, templated) function compiled to machine code. Reflection still shapes it at compile time: index sequences built from `base_count<U>()`/`part_count<U>()`, splices such as `v.[:members<U>()[I]:]` behind `get_part<I>(v)`, and `constexpr` tables, but the loop over the *data* runs when the program runs. |

## Summary

| Entry | Where | Context |
|---|---|---|
| `component_types(t, todo)` | `reflect.hpp:414` (calls: 448, 460) | consteval |
| `is_scalar_v<U>` | `reflect.hpp:191` | compile-time constant |
| `check_supported<T>()` | `reflect.hpp:471` | consteval |
| `is_dyn_member_v<U>` | `reflect.hpp:337` | compile-time constant |
| `dyn_member<U>` | `reflect.hpp:211–336` | compile-time constant (trait) |
| `dyn_member<U>::count` | `reflect.hpp:235` | compile-time constant |
| `check_nonempty_element<…>()` | `reflect.hpp:395` | consteval |
| `kind` | `descriptor.hpp:18` | both: compile-time and run time |
| `recursive_type(root)` | `reflect.hpp:444` | consteval |
| `has_annotation<A>(m)` | `reflect.hpp:645` | consteval |
| `canonical_name<T>()` | `reflect.hpp:661` (decl), `816` (def) | consteval |
| `lacks_linkage(r)` | `reflect.hpp:698` | consteval |
| `std_implementation_namespace(ns)` | `reflect.hpp:715` | consteval |
| `concept dynamic_type<T>` | `reflect.hpp:2662` (namespace `es::serreg`) | compile-time constant (concept) |
| `for_each_part<U>(…)` calls | `reflect.hpp:2255, 2370, 2604` | run time |
| `measure_value` | `reflect.hpp:2050` (decl), `2185` (def) | run time |
| `for_each_in_canonical_order(v, f)` | `reflect.hpp:2146` | run time |
| `encode_value` | `reflect.hpp:2051` (decl), `2275` (def) | run time |
| `decode_value` | `reflect.hpp:2053` (decl), `2466` (def) | run time |
| `template<class T, class FB, class FM> void for_each_part(fb, fm)` | `reflect.hpp:2057` | run time |
| `collect_member_enum_ranges<U, I>(out, base)` | `reflect.hpp:1914` (decl), `1949` (def) | consteval |
| `in_std(t)` | `reflect.hpp:406` | consteval |

---

## component_types(t, todo)

**Where:** `consteval void component_types(const std::meta::info type, std::vector<std::meta::info>& out)` at
`reflect.hpp:414`; the call `component_types(t, todo)` is inside `recursive_type()` at lines 448 and 460.
**Context:** consteval.

Given the reflection of a type, `component_types` appends to `out` the reflections of the types directly inside
it. That means the type of every non-static data member and of every base (`nonstatic_data_members_of`,
`bases_of` with `access_context::unchecked()`, so private members count too). It strips cv-qualifiers, aliases and
array extents first, and returns at once for a type that is not a class. A standard-library class is handled
separately, through `in_std()`. For the containers the library can encode (`std::vector`, `optional`, `array`,
`variant`, `map`, `unordered_map`, `flat_map`, `set`, `unordered_set`, `flat_set`, `deque`, `list`,
`forward_list`, `multimap`, `unordered_multimap`, `stack`) and the tuple-likes (`pair`, `tuple`), it appends their
type template arguments, which are the element, key and mapped types. Every other `std` class, `std::basic_string`
included, contributes nothing, because no user type can hide inside it. The function works purely on
`std::meta::info` values: it instantiates no templates. It is the step function of the graph walk in
`recursive_type()`, which keeps `todo` as a work list.

## is_scalar_v\<U\>

**Where:** `reflect.hpp:191–193`. Used in `check_supported` (474), `canonical_name` (819), the field and kindspec
builders (1153, 1500), `is_dynamic` (1312), the gap collector (1761, 1770), `collect_enum_ranges` (1925), and in
`registry.hpp:332` (`touch`) and `arg.hpp:48, 64`.
**Context:** compile-time constant.

`is_scalar_v<U>` is `true` for the arithmetic types except `long double`, plus `std::byte`: `bool`, the `char`
family, the signed and unsigned integers, `float` and `double`. These are the leaves of every schema; each maps to
one `kind` with a fixed native size. Every recursive walk in the library starts with `if constexpr
(is_scalar_v<U>)` to stop there. `long double` is excluded because its size and representation differ between
ABIs and the wire format has no kind for it. At the call level, `arg.hpp` builds `is_scalar_arg_v` on top of it, so
a top-level `int` or `double` argument becomes the "scalar" argument category rather than a type entry.

## consteval void check_supported()

**Where:** `template<class T> consteval void check_supported()` at `reflect.hpp:471–568`. Called from `touch<T>()`
(`registry.hpp:330`), `field_for<M>()` (`reflect.hpp:1140`) and `canonical_string<T>()` (`reflect.hpp:1723, 1730`),
and recursively from itself.
**Context:** consteval. The call inside the run-time `touch<T>()` is still an immediate invocation, evaluated
during compilation.

This is the gatekeeper that implements FR-S7: a type that cannot be serialised is rejected at the user's call site
with a sentence that names the rule, instead of a page of template errors. It works on `mapped_t<T>` (after any
`as_flat<T>` mapping) and recurses by shape. Scalars and enums pass. Arrays check their element. Dynamic members
check their element, key and mapped types, call `check_nonempty_element`, refuse `std::vector<bool>`, and require
an order on the key of an unordered container. A tuple-like (`std::pair`, `std::tuple`) must not be empty and
checks every element through `std::tuple_element_t`. Every other type is first refused if it is a smart pointer
(`is_smart_pointer_v`: ownership is not data) and, when it is a class, if it is a standard-library type outside
the serialisable vocabulary, detected as `in_std` without a `type_properties<T>::name()` (the `has_properties_name`
concept at 466), so that an unknown `std` class gets one message naming the vocabulary rather than a layout
error. What remains must not be recursive (`recursive_type`), a pointer or member pointer, a reference, a union,
polymorphic or a `std::string_view` member, and must be trivially copyable unless it is a dynamic class. Each
refusal is a `static_assert` whose message begins with `es::serreg:`. It runs before anything else in
`touch<T>()`, so the intended diagnostic fires first.

## is_dyn_member_v\<U\>

**Where:** `template<class T> constexpr bool is_dyn_member_v = dyn_member<std::remove_cv_t<T>>::value;` at
`reflect.hpp:337`. Used at 364, 374, 387, 481, 1168, 1316, 1347, 1361, 1521, 1936, 2196, 2286, 2490, and in
`registry.hpp:343`.
**Context:** compile-time constant.

`is_dyn_member_v<U>` asks whether `U` is one of the standard types the library encodes as a slot in a canonical
image instead of as raw bytes: any `std::basic_string<char, …>`, `std::vector`, `std::optional`, `std::variant`,
(format 1.6) one of the six maps and sets, or one of the containers that share the `vector` wire shape:
`std::deque`, `std::list`, `std::forward_list`, `std::multimap`, `std::unordered_multimap` and `std::stack`. It is
the shorthand for `dyn_member<U>::value` with cv-qualifiers removed. It separates the "dyn member" branch from
the "class" branch in every walk: the schema-string builder, the image layout, `check_supported`,
`measure_value`/`encode_value`/`decode_value`, and the registration walk in `touch<T>()`, which registers a
container's element types but gives the container no entry of its own.

## dyn_member\<U\>

**Where:** primary template at `reflect.hpp:211` (`std::false_type`); specialisations at 218 (`basic_string`),
222 (`vector`), 227 (`optional`), 232 (`variant`), 248 (`map`), 256 (`unordered_map`), 264 (`flat_map`), 271
(`set`), 279 (`unordered_set`), 286 (`flat_set`), 295 (`deque`), 300 (`list`), 305 (`forward_list`), 315
(`multimap`), 323 (`unordered_multimap`), 331 (`stack`).
**Context:** compile-time constant (a trait class).

`dyn_member<U>` is the trait that describes a dynamic member. Each specialisation derives from `std::true_type`
and carries `static constexpr kind k` (`string_`, `vector_`, `optional_`, `variant_`, `map_` or `set_`) plus the
types the encoder needs: `elem` for vectors, optionals, maps and sets, `key` for maps and sets, `count` for
variants. Maps and sets also carry `ordered`, which is `true` when the container already iterates in canonical
order (`std::less<K>` or `std::less<>`); an unordered container is sorted by the writer. `std::deque`, `std::list`
and `std::forward_list` are the `vector_` shape with an `elem`, differing from `std::vector` only in how the
writer walks them and how the reader fills them. A `std::multimap` or `std::unordered_multimap` is a `vector_` of
`elem = std::pair<K, V>` with two more flags: `multi` (the schema kindspec is `multimap<K,V>`, the layout string the
vector's) and `sort_by_key` (`true` unless the container already orders by `std::less`), so duplicate keys are
data and the order among equal keys is the container's. `std::stack` is the `vector_` shape of its element with
`adapter = true`: the walks reach its protected container through `sequence_of`. Allocators, comparators and
hashers are deliberately absent from the trait. They are properties of the reader's object, not of the data, so
`std::map`, `std::unordered_map` and `std::flat_map` of the same `K, V` share one wire shape.

## dyn_member\<U\>::count

**Where:** `static constexpr std::size_t count = sizeof...(Es);` in `dyn_member<std::variant<Es...>>`,
`reflect.hpp:235`. Used to build `std::make_index_sequence<dyn_member<U>::count>{}` in `check_supported` (496),
`touch<T>()` (`registry.hpp`) and the variant branches of the image walks.
**Context:** compile-time constant.

`count` exists only on the `std::variant` specialisation and is the number of alternatives. Code that has to visit
every alternative type, to check it, register it or write its schema, expands an index sequence of this length
and uses `std::variant_alternative_t<I, U>` for each index. It is also the `extent` of a variant's FIELD in the
dictionary entry, so a dictionary-driven reader knows how many alternative descriptors follow.

## check_nonempty_element\<typename dyn_member\<U\>::elem\>()

**Where:** `template<class E> consteval void check_nonempty_element()` at `reflect.hpp:395–401`. Called from
`check_supported` at 490 (vector and optional element), 495 (each variant alternative), 502 (map or set key) and
506 (map value).
**Context:** consteval.

(Review D-05.) It is a single `static_assert` that `image_stride<E>()` is not zero: every element
of a vector, optional, variant, map or set must occupy at least one byte in the canonical image. An element such
as `std::array<T, 0>` of a dynamic `T` occupies none, so its positions cannot be told apart. The dictionary
refused such an element, and the typed decoder would have accepted any count of them, up to 2³²−1 from a 16-byte
image. Rejecting it where it is declared closes that hole at compile time.

## kind — the enum of item kinds

**Where:** `enum class kind : std::uint8_t` at `include/es/serreg/descriptor.hpp:18`, namespace `es::serreg`, with
`kind_size()`, `to_string(kind)` and `is_scalar_kind()` next to it.
**Context:** both. It is used at compile time (`dyn_member<U>::k`, `scalar_kind<T>()`, the kindspec text in schema
strings) and at run time (`field_desc::k`, the `u8 kind` of every FIELD in an ENTRY, the decoders' switches).

`kind` names the wire kind of a value. The scalars are `i8 … u64`, `f32`, `f64`, `boolean`, `char_`, `char8`,
`char16`, `char32`, `wchar` and `byte_`, numbered 1–17 and all below 32 (`is_scalar_kind`). The composite kinds are
`enum_` (32), `array_` and `struct_`. The dynamic kinds start at 48: `string_`, `vector_`, `optional_`, `variant_`,
`literal_` (reserved), and since format 1.6 `map_` and `set_`. The numbers are part of the wire format, because the
dictionary stores them in every FIELD, so they never change. `descriptor.hpp` has no reflection dependency, so the
same enum is shared by `reflect.hpp`, the compiled `libserreg`, the decoders and `srdump`.

## recursive_type()

**Where:** `consteval bool recursive_type(const std::meta::info root)` at `reflect.hpp:444–463`. Called once, from
the class branch of `check_supported`, at 544: `static_assert(!recursive_type(^^U), …)`.
**Context:** consteval.

(Review C-07.) It answers whether a class contains itself through members, bases, arrays,
`std::array`, `std::vector`, `std::optional`, `std::variant`, the maps and sets, the other sequence containers or
the tuple-likes. Such a type has no finite canonical description, and without this check the compiler stops with
an opaque template-recursion error. The function is a depth-first walk over reflections with two
`std::vector<std::meta::info>` lists, `todo` and `seen`. It visits each type once, uses `component_types()` to
expand each, and returns `true` when it meets the starting type again. Because it walks `std::meta::info` values,
it instantiates nothing on the way, which is what makes the check possible before the recursive instantiation
would happen.

## has_annotation(const std::meta::info m)

**Where:** `template<class A> consteval bool has_annotation(const std::meta::info m)` at `reflect.hpp:645`.
Called from `member_skipped<T, I>()` at 1255. Its sibling `annotation_value<A>(m)` at 652 reads `since` and
`deprecated`.
**Context:** consteval.

`has_annotation<A>(m)` returns `true` when the declaration reflected by `m` carries a C++26 annotation (P3394,
`[[=value]]`) whose type, cv-removed, is `A`. It loops over `std::meta::annotations_of(m)` and compares
`remove_cv(type_of(a))` with `^^A`. The library uses it for `[[=es::serreg::skip]]` (type `skip_t`): a skipped
member is left out of the schema string, the FIELD list and the registration walk, and its bytes are zeroed on the
wire. `annotation_value<A>` is the same loop with `std::meta::extract<A>(a).v`, which yields the numbers of
`[[=since(N)]]` and `[[=deprecated(N)]]`.

## canonical_name\<T\>()

**Where:** declared at `reflect.hpp:661`, defined at 816–886. Called from `scope_of` (732), from itself for
template arguments (873), `canonical_list` (890), the std vocabulary (911–1052), the base pseudo-field name (1244),
the schema-string builders (1507, 1627, 1646, 1665, 1696), and through
`canonical_name_v<T> = std::define_static_string(…)` at 2625.
**Context:** consteval. The string reaches run time only through `canonical_name_v`, which `define_static_string`
turns into a `string_view` over static storage.

`canonical_name<T>()` builds the qualified type name that appears in schema strings, schema hashes, ENTRY blobs and
`srdump` output, such as `corpus::Order`. The rules apply in order. A scalar is named by its kind (`u32`). A name
given explicitly by `type_properties<T>::name()` or by the `[[=es::serreg::name("…")]]` annotation wins, and
giving both is an error. An unnamed type, or one without linkage (`lacks_linkage`), is rejected with a message
that says how to name it. A template specialisation becomes `scope::template<args>`, with type arguments named
recursively and value arguments rendered by the library (`3`, `true`, `enum(7)`). Anything else is
`scope_of<parent_of(r)>()` plus `identifier_of(r)`. It never uses `display_string_of` or `__PRETTY_FUNCTION__`, and
it drops the standard library's implementation namespaces, so the same source gives the same name, and the same
hash, on GCC with libstdc++ and on clang-p2996 with libc++.

## consteval bool lacks_linkage(const std::meta::info r)

**Where:** `reflect.hpp:698–712`. Called from `canonical_name` at 851: `else if constexpr (lacks_linkage(r))`.
**Context:** consteval.

(Review C-03/C-05.) It walks `parent_of` from `r` towards the global namespace and returns `true`
when it meets a function, meaning the class is local to a function, or a namespace without an identifier, meaning
an unnamed namespace, before it meets a class. Such a type has no linkage-unique name: two `.cpp` files may each
define their own `namespace { struct Quote; }` with different layouts, and an identical canonical name would make
them collide in the registry. `canonical_name` therefore rejects the type unless it is named explicitly. An
enclosing class stops the walk with `false`, because the class carries its own rule.

## bool std_implementation_namespace(const std::meta::info ns)

**Where:** `consteval bool std_implementation_namespace(const std::meta::info ns)` at `reflect.hpp:715–724`. Used
by `scope_of<P>()` at 736.
**Context:** consteval.

(Review C-04.) It returns `true` for a namespace whose identifier begins with `_` and which sits
somewhere inside `std`: libstdc++'s `std::__cxx11` and `std::chrono::_V2`, libc++'s `std::__1`. Those names are
reserved to the implementation and differ between standard libraries. `scope_of` leaves them out of the scope
prefix, so a standard-library type gets the same canonical name whichever library compiled it. Before this rule,
`std::monostate` was `std::__1::monostate` under libc++, and the hash of every variant holding it differed between
compilers.

## concept dynamic_type\<T\>

**Where:** `reflect.hpp:2662–2665`, namespace `es::serreg` (public). Used in `arg.hpp:166` (`serializable_arg`),
`serialize.hpp:127` (the `noexcept` computation) and `deserialize.hpp:520, 947, 999`.
**Context:** compile-time constant (a concept).

`dynamic_type<T>` is satisfied by a class type, after `as_flat` mapping, that is encoded as a canonical image
rather than copied as raw bytes. It must not be a `flat_type`, must not be `std::string_view`, and must be either
a class that is not itself a dyn member type or, since format 1.6, a container passed as an argument (the
transparent wrapper type). The comment at 2655 explains a deliberate choice: any non-trivially-copyable class is
admitted by the concept, so that overload resolution succeeds and `check_supported()` can then name the rule the
type breaks, instead of the user seeing "no matching overload". The reader uses the concept to select the image
decoder (`decode_image`) instead of a `memcpy`.

## for_each_part\<U\>

**Where:** the three call sites `for_each_part<U>(…)` at `reflect.hpp:2255` (in `measure_value`), 2370 (in
`encode_value`) and 2604 (in `decode_value`).
**Context:** run time. The call is an ordinary inlined call. The two lambdas passed to it receive
`std::integral_constant<std::size_t, I>` values, so inside them `I` is a constant expression and `base_t<U, B>`
and `get_part<I>(v)` (the splice `v.[:members<U>()[I]:]` or `std::get<I>(v)`) are resolved at compile time.

At each call site the walk has reached the dynamic-class case: the value is a class with a dyn member somewhere in
its tree, or a tuple-like. The first lambda handles each base, by casting `v` to the base type `base_t<U, B>` and
recursing. The second lambda handles each part, by recursing into `get_part<I>(v)` unless `member_skipped<U, I>()`.
The three call sites are written the same way on purpose, so that measuring, encoding and decoding visit the parts
of a class in the same order: bases first, then parts in declaration order. That order is also the order of the
fields in the schema string.

## measure_value

**Where:** declared at `reflect.hpp:2050`, defined at 2185–2266, wrapped by `measure_image<T>(v)` at 2268, which
`serialize.hpp:87` calls to compute a frame's size.
**Context:** run time.

`measure_value(v, tail)` is the first of the three mirrored image walks. It adds to `tail` the number of bytes the
canonical image of `v` needs beyond its fixed part: string bytes, vector and map blocks with their alignment, the
optional's payload, the chosen variant alternative, and recursively the tails of any dynamic elements and members.
A value that is not dynamic adds nothing; its bytes are already counted in its parent's fixed part. The `vector`
branch is written once for every container of that shape: `sequence_of(v)` yields the sequence (the container, or
a `std::stack`'s underlying one), `sequence_size` counts it (`std::ranges::distance` for `std::forward_list`), and
an empty sequence adds nothing, because it is encoded as the slot `{0, 0}`. `measure_image` starts from
`image_size<T>()`, the fixed part, so `serialize()` knows the exact frame length before writing a byte. That lets
it claim an exact-size slot from a claiming sink and never reallocate.

## for_each_in_canonical_order(const V& v, F&& f)

**Where:** `template<class U, class V, class F> void for_each_in_canonical_order(const V& v, F&& f)` at
`reflect.hpp:2146–2170`. Called from the map and set branch of `encode_value` at 2350, when
`dyn_member<U>::ordered` is `false`.
**Context:** run time.

Format 1.6 requires the entries of a map or set to be written in ascending key order, so that equal values give
equal bytes whatever the C++ container. An `std::unordered_map` iterates in hash order, and a map with a custom
comparator in its own order. This function calls `f` on the entries of `v` sorted by `key_less`: the key's
`operator<` when it has one, otherwise a `memcmp` of the keys' scrubbed flat images. It sorts pointers to the
entries, never the entries: up to 64 pointers live in an array on the stack, and above that a `std::vector` is
used. That vector is the one allocation a `serialize()` of an unordered container may make. Ordered containers
skip the function and iterate directly, and so does an unordered one when the caller asked for it:
`serialize_options::canonical_unordered = false` travels in `image_cursor::canonical_unordered` and makes the
map/set branch write the container's own order (no sort, no allocation). Its sibling for multimaps is
`for_each_in_wire_order`, which keeps equal keys in the container's order with a stable sort and takes the same
flag.

## encode_value

**Where:** declared at `reflect.hpp:2051`, defined at 2275–2382, wrapped by `encode_image<T>(v, image, len,
canonical_unordered = true)` at 2388, which `serialize.hpp:204` calls with `o.canonical_unordered`.
**Context:** run time.

`encode_value(v, dst, c)` writes the canonical image of `v`. A flat value is copied with `put_flat_image`:
`memcpy`, then the padding gaps zeroed and `bool` bytes set to 0 or 1. Arrays encode element by element. A string,
vector, optional, map or set allocates its block in the tail through the `image_cursor` (`c.alloc(size, align)`)
and writes an 8-byte `{u32 offset, u32 count}` slot at `dst`. The `vector` branch walks `sequence_of(v)` with
`for_each_in_wire_order<U>(seq, c.canonical_unordered, f)`, which iterates a vector, deque, list, forward_list,
stack or ordered multimap as it is and stably sorts an unordered multimap's entries by key, so every container of
the shape is written the same way. The map/set branch has three arms: an ordered container as iterated, an
unordered one as iterated when `c.canonical_unordered` is `false` (the opt-out of `serialize_options`), else
through `for_each_in_canonical_order`. `image_cursor` carries `{base, tail, cap, canonical_unordered}`; the flag
defaults to `true` and never affects sizes, hashes or the readers. A variant writes its index and its alternative. A dynamic class recurses into its bases and parts with
`for_each_part`, each at its canonical offset (`base_image_offset`, `member_image_offset`). `encode_image` first
zeroes the whole measured image, so every gap byte is 0 whatever the sink's memory held before. The function never
allocates; the measure pass already sized the buffer.

## decode_value

**Where:** declared at `reflect.hpp:2053`, defined at 2466–2618, wrapped by `decode_image<T>(out, image)` at
2621, which `deserialize.hpp:720` and `1004` call.
**Context:** run time.

`decode_value(out, c, at, depth)` is the typed reader of a canonical image and the mirror of `encode_value`. It
follows the same shapes in the same order and fills a C++ object: it sizes a sequence (`resize` on a vector,
deque, list, forward_list or a stack's container, then fills it in order) or, for a multimap, decodes each entry
into a temporary and inserts it; it engages or resets optionals, emplaces the variant's alternative, and inserts
map and set entries. Every slot is checked against the shared rules in `fmt::image_budget` before it is followed:
offset inside the image, alignment, count times stride within the byte budget, and nesting depth at most
`max_image_depth` (64). This makes hostile or truncated input return `false` instead of crashing (FR-D5). Flat
leaves go through `copy_flat<U>`, which repairs `bool` bytes and, when `ES_SERREG_VALIDATE_READ` is on, zeroes
enum leaves outside their range. `decode_image` turns the result into `length_mismatch`, `invalid_value` or a
`noncanonical_bool` warning.

## template\<class T, class FB, class FM\> inline void for_each_part(FB&& fb, FM&& fm)

**Where:** `reflect.hpp:2057–2063`.
**Context:** run time. It is an ordinary inline function template. Its body expands two parameter packs built from
`std::make_index_sequence<base_count<T>()>` and `std::make_index_sequence<part_count<T>()>`, which are
compile-time constants obtained by reflection (or from `std::tuple_size` for a tuple-like).

This is the helper behind the `for_each_part<U>(…)` call sites above. Inside a generic lambda it receives two index
packs, `B...` and `I...`, and calls `fb(std::integral_constant<std::size_t, B>{})` for every base, then
`fm(std::integral_constant<std::size_t, I>{})` for every part, as fold expressions. Passing the index as an
`integral_constant` instead of a plain `size_t` is the point of the design: it keeps the index a constant
expression inside the callback, so `members<T>()[I]` can be spliced (through `get_part<I>`). It is the library's
substitute for `template for` over members. The header comment at lines 13–15 states the choice: member iteration
uses index sequences over `define_static_array(...)[I]`, so as to depend on as few P2996 companion papers as
possible. The function itself does no work; `fb` and `fm` do.

## template\<class U, std::size_t I\> consteval void collect_member_enum_ranges(std::vector\<enum_range\>& out, const std::size_t base)

**Where:** declared at `reflect.hpp:1914`, defined at 1949–1954. Called from `collect_enum_ranges<M>()` at 1943.
The result becomes `enum_ranges_v<T>` (1970), which the run-time functions `copy_flat` (2429) and
`bools_canonical` (2447) read when `validate_read_enums` is `true`.
**Context:** consteval.

(Decision D, FR-D5.) An enum without a fixed underlying type (`enum E { a, b, c };`) may only hold
the values of the smallest bit-field that represents all its enumerators. Reading any other value is undefined
behaviour, so readers must check such leaves before using them. `collect_member_enum_ranges<U, I>` handles member
`I` of class `U`. Unless the member is skipped, it calls `collect_enum_ranges` on the member's type at offset
`base + offset_of(members<U>()[I]).bytes`. That function recurses through bases, members and arrays and appends one
`enum_range { offset, size, is_signed, lo, hi }` for every bounded enum leaf, computed by `enum_range_of<E>()`. The
vector exists only during constant evaluation. `make_enum_ranges<T>()` copies it into a `std::array`, the constant
table `enum_ranges_v<T>`. The check is a compile-time policy of the translation unit (`ES_SERREG_VALIDATE_READ`,
default off): when it is off the readers never name the table, so it is not even instantiated; when it is on, a
type without such enums gets an empty table and the run-time check compiles to nothing.

## in_std()

**Where:** `consteval bool in_std(std::meta::info t)` at `reflect.hpp:406–413`. Called from `component_types` at
420 and 436, and from `check_supported` at 533.
**Context:** consteval.

`in_std(t)` returns `true` when the entity reflected by `t` is declared, directly or through nested namespaces,
inside namespace `std`. It walks `parent_of` until the global namespace `^^::` and compares each parent with
`^^std`. It stops early at a function, for two reasons: a class local to a function is never `std`'s, and GCC 16
cannot take `parent_of` of `main` (review C-05). `component_types` uses it to treat the standard library as a black
box during the recursion check. It looks inside the known containers' template arguments and nowhere else in
`std`, so the walk never descends into library internals. `check_supported` uses it to close the vocabulary: a
`std` class that is not a dyn member, a tuple-like or named through `type_properties` is rejected with a message
listing the supported types.

---

# Additional entries (not in the original list)

These are the other reflection functions, traits and tables in `reflect.hpp` that the entries above depend on or
feed. Each one is marked *(additional)*.

| Entry | Where | Context |
|---|---|---|
| `ctx()` | `reflect.hpp:571` | consteval |
| `members<T>()`, `bases<T>()`, `enumerators<E>()` | `reflect.hpp:576, 580, 584` | consteval |
| `base_count<T>()`, `part_count<T>()` | `reflect.hpp:594, 601` | consteval |
| `part_type<T, I>`, `part_t<T, I>`, `base_t<T, I>` | `reflect.hpp:608–617` | compile-time constant (alias templates) |
| `part_name<T, I>()` | `reflect.hpp:618` | consteval |
| `get_part<I>(v)` | `reflect.hpp:634` | run time (`constexpr`) |
| `annotation_value<A>(m)` | `reflect.hpp:652` | consteval |
| `member_skipped<T, I>()`, `part_annotation<A, T, I>()` | `reflect.hpp:1250, 1257` (decl. 642, 643) | consteval |
| `array_traits<T>` | `reflect.hpp:110–125` | compile-time constant (trait) |
| `scalar_kind<T>()` | `reflect.hpp:127` | consteval |
| `mapped<T>`, `mapped_t<T>` | `reflect.hpp:196–206` | compile-time constant (trait) |
| `is_map_or_set<T>()`, `is_container_arg<T>()` | `reflect.hpp:362, 372` | consteval |
| `is_multimap_member<T>()`, `is_container_adapter<T>()` | `reflect.hpp:338, 345` | consteval |
| `is_tuple_like_v<T>`, `is_smart_pointer_v<T>` | `reflect.hpp:355, 358` | compile-time constant |
| `is_dynamic<T>()`, `member_dynamic<T, I>()`, `is_dynamic_class<T>()` | `reflect.hpp:1309, 1302, 384` | consteval |
| `scope_of<P>()` | `reflect.hpp:727` | consteval |
| `annotated_name<U>()`, `properties_name<U>()`, `valid_explicit_name(n)` | `reflect.hpp:747, 765, 775` | consteval |
| `render_value_arg(v)` | `reflect.hpp:789` | consteval |
| `type_properties<T>` (std vocabulary) | `reflect.hpp:900–1075` | consteval (`name()`) |
| `image_size<M>()`, `image_align<M>()`, `image_stride<E>()` | `reflect.hpp:1344, 1358, 1371` | consteval |
| `compute_canonical_offsets`, `member_image_offset`, `base_image_offset`, `compute_fixed_layout` | `reflect.hpp:1378, 1402, 1398, 1406` | consteval |
| `map_entry_layout<K, V>()`, `container_entry_layout<M>()` | `reflect.hpp:1116, 1123` | consteval |
| `field_for<M>(…)`, `member_field`, `base_field`, `make_fields`, `fields_v` | `reflect.hpp:1136, 1228, 1238, 1294, 1299` | consteval / constant table |
| `make_enumerators<E>()`, `enumerators_v`, `enumerators_fit<E>()` | `reflect.hpp:1452, 1456, 1428` | consteval / constant table |
| `has_fixed_underlying_type_v<E>`, `enum_underlying_kind<E>()` | `reflect.hpp:1466, 1474` | compile-time constant / consteval |
| `append_kindspec`, `append_dims`, `append_member`, `append_base` | `reflect.hpp:1497, 1584, 1602, 1620` | consteval |
| `struct_string`, `enum_string`, `wrapper_string`, `canonical_string<T>(schema)` | `reflect.hpp:1638, 1659, 1690, 1716` | consteval |
| `schema_hash_of<T>()`, `layout_hash_of<T>()`, `schema_hash_v`, `layout_hash_v` | `reflect.hpp:1736, 1740, 2637, 2639` | consteval / constant |
| `canonical_name_v<T>` | `reflect.hpp:2641` | compile-time constant |
| `schema_string_v<T>`, `layout_string_v<T>` | `reflect.hpp:2646, 2649` | compile-time constant |
| `collect_leaves`, `compute_gaps`, `gaps_v`, `compute_bools`, `bools_v` | `reflect.hpp:1757–1855` | consteval / constant table |
| `ES_SERREG_VALIDATE_READ`, `validate_read_enums` | `reflect.hpp:1866, 1869` | compile-time constant |
| `enum_range_of<E>()`, `collect_enum_ranges<M>()`, `enum_ranges_v<T>` | `reflect.hpp:1877, 1917, 1970` | consteval / constant table |
| `enum_leaf_in_range(r, p)` | `reflect.hpp:1972` | run time |
| `put_flat_image<U>(dst, v)` | `reflect.hpp:1998` | run time |
| `sequence_of(v)`, `sequence_size(s)`, `for_each_in_wire_order<U>(s, canonical, f)` | `reflect.hpp:2067, 2078, 2087` | run time |
| `key_less(a, b)`, `encode_entry<U>(e, at, c)` | `reflect.hpp:2134, 2174` | run time |
| `measure_image`, `encode_image`, `decode_image` | `reflect.hpp:2268, 2388, 2621` | run time |
| `copy_flat<U>(dst, src)`, `bools_canonical<U>(src)` | `reflect.hpp:2417, 2440` | run time |
| `monostate_index<V>()` | `reflect.hpp:2454` | consteval |
| `concept flat_type<T>` | `reflect.hpp:2653` | compile-time constant (concept) |
| `make_type_desc_base<T>()` | `reflect.hpp:2670` | consteval |

## ctx() *(additional)*

**Where:** `consteval std::meta::access_context ctx()` at `reflect.hpp:571`. **Context:** consteval.

It returns `std::meta::access_context::unchecked()`, and every member and base query in the library passes it.
The unchecked context lets reflection see private and protected members and bases, so a class with private data is
serialised whole without friend declarations (FR-S9). Keeping it in one function means the access policy is a
single line to audit.

## members\<T\>(), bases\<T\>(), enumerators\<E\>() *(additional)*

**Where:** `reflect.hpp:576, 580, 584`. **Context:** consteval.

These three wrap `nonstatic_data_members_of(^^T, ctx())`, `bases_of(^^T, ctx())` and `enumerators_of(^^E)` in
`std::define_static_array`. That turns the `std::vector<std::meta::info>` the metafunctions return, which cannot
outlive constant evaluation, into an array in static storage. The array can then be indexed with a constant index,
`members<T>()[I]`, inside index-sequence expansions, and its `size()` is a constant for
`std::make_index_sequence`. Every walk over a class or an enum in the library goes through them, directly or
through the parts layer below.

## base_count\<T\>(), part_count\<T\>() *(additional)*

**Where:** `reflect.hpp:594, 601`. **Context:** consteval.

The parts layer is the positional view of a class that every walk uses, so that a reflected class and a tuple-like
(`std::pair`, `std::tuple`) share one code path. A class has bases and parts: its non-static data members, or for a
tuple-like its elements. `base_count` is `bases<T>().size()`, or 0 for a tuple-like; `part_count` is
`members<T>().size()`, or `std::tuple_size_v<T>`. Everything that walks a class (`field_count`, `make_fields`,
`is_dynamic`, the canonical offsets, `canonical_string`, `for_each_part`, `touch_components`, `collect_renames`,
`plan_class`) builds its two index sequences from these two numbers.

## part_type\<T, I\>, part_t\<T, I\>, base_t\<T, I\> *(additional)*

**Where:** `reflect.hpp:608–617`. **Context:** compile-time constants (a trait and two alias templates).

`part_t<T, I>` is the type of part `I`: the splice `typename [:type_of(members<T>()[I]):]` for a reflected class,
`std::tuple_element_t<I, T>` for a tuple-like (the `part_type` trait selects by `is_tuple_like_v<T>`). `base_t<T, I>`
is the type of base `I`, `typename [:type_of(bases<T>()[I]):]`. They replace the splices that used to be written at
every walk, so a reader of `member_field`, `member_dynamic`, `compute_canonical_offsets`, `append_member`,
`touch_member` or the image walks sees one spelling.

## part_name\<T, I\>() *(additional)*

**Where:** `template<class T, std::size_t I> consteval std::string part_name()` at `reflect.hpp:618`.
**Context:** consteval.

The name a part has in the schema string and the FIELD list: `identifier_of(members<T>()[I])` for a reflected
member; `first`/`second` for a `std::pair`; `_0`, `_1`, … for a `std::tuple`. `member_field` turns it into a static
string with `define_static_string`, and `append_member` writes it before the `:` of a schema item. Tuple-like
parts therefore have stable names on the wire even though the standard gives them none.

## get_part\<I\>(v) *(additional)*

**Where:** `template<std::size_t I, class T> constexpr decltype(auto) get_part(T& v) noexcept` at
`reflect.hpp:634`. **Context:** run time (`constexpr`; the choice of access is made at compile time).

It returns a reference to part `I` of the object `v`: `std::get<I>(v)` for a tuple-like, otherwise the splice
`v.[:members<T>()[I]:]`. The three image walks (`measure_value`, `encode_value`, `decode_value`) reach every part
through it, which is why a `std::pair` or `std::tuple` is always encoded element-wise at canonical offsets:
libstdc++ stores a tuple's elements in reverse order, so its native layout is never copied.

## annotation_value\<A\>(m) *(additional)*

**Where:** `template<class A> consteval std::uint16_t annotation_value(const std::meta::info m)` at
`reflect.hpp:652`. **Context:** consteval.

It is the value-reading sibling of `has_annotation`. It returns the `.v` field of the first annotation of type `A`
on `m`, read with `std::meta::extract<A>(a)`, or 0 when there is none. It reads `[[=since(N)]]` and
`[[=deprecated(N)]]` on members (through `part_annotation`), and on the type itself in `struct_string`,
`enum_string` and `make_type_desc_base`. The numbers end up in the schema string (`;sN;dN`), the FIELD and the
ENTRY, so they are schema metadata that travels with the dictionary.

## member_skipped\<T, I\>(), part_annotation\<A, T, I\>() *(additional)*

**Where:** `reflect.hpp:1250–1256` and `1257–1263`; both are forward-declared at 642–643, before the parts layer
needs them. **Context:** consteval.

`member_skipped<T, I>()` is `has_annotation<skip_t>(members<T>()[I])`: does member `I` of `T` carry
`[[=es::serreg::skip]]`? For a tuple-like it is `false`, because its elements carry no annotations. Every per-member
walk checks it with `if constexpr`. A skipped member is left out of `field_count`, the FIELD list, the schema and
layout strings, the canonical offsets, the gap and bool tables (its bytes are therefore zeroed as padding), the
enum ranges, the image walks and `touch_member`. Its type is never instantiated, so it may be anything, even a type
the library rejects. `part_annotation<A, T, I>()` is `annotation_value<A>` of member `I`, and 0 for a tuple-like
part; `member_field` and `append_member` read `since` and `deprecated` through it.

## array_traits\<T\> *(additional)*

**Where:** `reflect.hpp:110–125`. **Context:** compile-time constant (a trait).

`array_traits<T>` treats a C array `E[N]` and `std::array<E, N>` alike: `is_array`, `elem` and `extent`. The
schema string writes both as `array<kindspec,N>`, and every walk recurses into `elem` through this trait, so the
two spellings give the same hash. A dynamic array (one whose element is dynamic) has its image size computed
element by element in `image_size`.

## scalar_kind\<T\>() *(additional)*

**Where:** `template<class T> consteval kind scalar_kind()` at `reflect.hpp:127`. **Context:** consteval.

It maps each scalar C++ type to its `kind`. The mapping is exact by type for `bool`, `char`, `char8_t`, `char16_t`,
`char32_t`, `wchar_t` and `std::byte`, and by signedness and size for the integers and floating types, so `long` is
`i64` on LP64 and `i32` on LLP64. That is why the canonical strings spell widths and never `long`. Anything else,
such as `long double` or `__int128`, is rejected with a `static_assert`. It feeds the kindspec text, `field_for`
and `enum_underlying_kind`.

## mapped\<T\>, mapped_t\<T\> *(additional)*

**Where:** `reflect.hpp:196–206`. **Context:** compile-time constant (a trait).

`mapped_t<T>` is the type that is actually described and serialised for `T`. It is `T` without cv-qualifiers,
unless the user specialised `as_flat<T>` (FR-S8), in which case it is `as_flat<T>::type`. Every reflection entry
point starts with `using U = mapped_t<T>;`, so a user can serialise a third-party type by mapping it to a flat
struct of their own without touching the third-party code.

## is_map_or_set\<T\>(), is_container_arg\<T\>() *(additional)*

**Where:** `reflect.hpp:362, 372` (and `is_container_arg_v` at 379). **Context:** consteval.

`is_map_or_set` asks whether a dyn member type has kind `map_` or `set_`. `is_container_arg` asks whether a type
passed as a top-level argument is a container other than a string. Since format 1.6 such an argument becomes a
type of its own: a transparent wrapper with one unnamed field, named after the container (`std::vector<i32>`). It
gets a registry entry and is decoded as a value, while a top-level `std::string` stays the "string" argument
category.

## is_multimap_member\<T\>(), is_container_adapter\<T\>() *(additional)*

**Where:** `reflect.hpp:338, 345`. **Context:** consteval.

Two probes on the `dyn_member` trait that read its optional flags through a `requires` expression and return
`false` when the flag is absent. `is_multimap_member` is `dyn_member<T>::multi`: the `vector_`-shaped member is a
`std::multimap` or `std::unordered_multimap`, whose schema kindspec is `multimap<K,V>` and whose decoder inserts
instead of resizing. `is_container_adapter` is `dyn_member<T>::adapter`: the member is a `std::stack`, and
`sequence_of` must reach its protected container. Both keep the `dyn_member` specialisations free of dummy flags.

## is_tuple_like_v\<T\>, is_smart_pointer_v\<T\> *(additional)*

**Where:** `reflect.hpp:355–357` and `358–361`. **Context:** compile-time constants.

`is_tuple_like_v` is `true` for `std::pair<A, B>` and `std::tuple<Ts...>`. Such a type is a dynamic class whose
parts are its elements, reached positionally (`part_count`, `part_t`, `part_name`, `get_part`): it is encoded as a
canonical image even when every element is flat, because its native layout belongs to the standard library.
`is_smart_pointer_v` is `true` for `std::unique_ptr`, `std::shared_ptr` and `std::weak_ptr`; `check_supported`
rejects them with a message that says to store the value, or `std::optional<T>` for a nullable one, since
ownership is not data.

## is_dynamic\<T\>(), member_dynamic\<T, I\>(), is_dynamic_class\<T\>() *(additional)*

**Where:** `reflect.hpp:1309, 1302, 384`. **Context:** consteval.

`is_dynamic<T>()` answers whether `T` has a dyn member anywhere in its tree: in its parts, bases, array elements
or nested classes. A tuple-like is dynamic by definition. Skipped members don't count (`member_dynamic`, which
asks `is_dynamic<part_t<T, I>>()`). A dynamic type is encoded as a canonical image; anything else is copied as raw
bytes. `is_dynamic_class` narrows it to a class that is not itself a dyn member or an array. The answer selects
the `struct` or `dyn` prefix of the schema string, the record encoding (`u32 len, u32 0, image`), the image layout
rules and the decoder.

## scope_of\<P\>() *(additional)*

**Where:** `template<std::meta::info P> consteval std::string scope_of()` at `reflect.hpp:727`.
**Context:** consteval.

It builds the `scope::` prefix of a canonical name from the reflection `P` of the enclosing entity. Namespaces
contribute their identifiers, outermost first. An enclosing class, including a class-template specialisation,
contributes its own canonical name, so a nested class is named `Outer<i32>::Inner`. Namespaces of the standard
library's implementation are dropped (`std_implementation_namespace`). The reflection is a template parameter, so
`if constexpr` can branch on it.

## annotated_name\<U\>(), properties_name\<U\>(), valid_explicit_name(n) *(additional)*

**Where:** `reflect.hpp:747, 765, 775`, plus `annotated_name_v` and `properties_name_v` at 764 and 773.
**Context:** consteval; the `_v` constants are static strings made with `define_static_string`.

These implement explicit canonical names. `annotated_name` finds a `[[=es::serreg::name("…")]]`
annotation on the type, matching by `template_of(type) == ^^es::serreg::name` inside a `template for` over its
annotations. `properties_name` calls `type_properties<U>::name()` when the user specialised it.
`valid_explicit_name` restricts the characters to letters, digits, `_`, `:`, `<`, `>`, `,` and `-`.
`canonical_name` prefers these names to the reflected one, which is how a type in an unnamed namespace, or a
third-party type, gets a stable name.

## render_value_arg(v) *(additional)*

**Where:** `template<class V> consteval std::string render_value_arg(const V v)` at `reflect.hpp:789`.
**Context:** consteval.

It renders a template argument value inside a canonical name: `true`/`false`, decimal integers, and `enum(N)`
with the underlying value. It never uses a compiler's spelling, so `Buffer<120>` is the same text on every
compiler. Any other kind of value argument, such as a floating-point or class-type value, is rejected, because its
text would not be portable.

## type_properties\<T\> — the std vocabulary *(additional)*

**Where:** primary template in namespace `es::serreg` near the top of `reflect.hpp`; library specialisations at
`reflect.hpp:900–1075`: `basic_string`, `vector`, `array`, `optional`, `variant`, the six maps and sets, `deque`,
`list`, `forward_list`, `multimap`, `unordered_multimap`, `stack`, `pair`, `tuple`, `monostate`.
**Context:** consteval (`static consteval std::string name()`).

The library specialises its own customisation point for the standard types that can appear in a schema, giving
each a frozen name such as `std::vector<E>`, `std::map<K,V>`, `std::multimap<K,V>`, `std::stack<T>` or
`std::monostate`. Allocators and a stack's underlying container are omitted, and a comparator or hasher is spelled
only when it is not the default (`cmp_suffix`, `hash_suffix`). Names are therefore independent of the standard
library's internal spelling (decision B). The same list closes the vocabulary in `check_supported`: a `std`
class without a `name()` here is rejected. User code specialises the same template to name third-party types.

## image_size\<M\>(), image_align\<M\>(), image_stride\<E\>() *(additional)*

**Where:** `reflect.hpp:1344, 1358, 1371`. **Context:** consteval.

These give the size and alignment of a value inside a canonical image:
- a flat value: `sizeof` and `alignof`, the same as in memory
- a dyn member: the 8-byte `{u32 off, u32 n}` slot with alignment 4
- a dynamic class (a tuple-like included): its fixed part (`fixed_layout_v`)
- a dynamic array: element by element

`image_stride` is the size rounded up to the alignment: the distance between elements of a vector block. These
numbers are the `[size,align]` in schema strings, the `size` in FIELDs, and the arithmetic the encoder and decoder
use.

## compute_canonical_offsets, member_image_offset, base_image_offset, compute_fixed_layout *(additional)*

**Where:** `reflect.hpp:1378, 1402, 1398, 1406`; tables `canonical_offsets_v` (1396) and `fixed_layout_v` (1341).
**Context:** consteval.

A dynamic class's fixed part is not laid out with the compiler's `offsetof`, because its members are slots, not
the C++ objects. These functions place bases (`base_t<T, B>`) first and then the kept parts (`part_t<T, M>`) in
declaration order, each at the next offset its image alignment allows, and compute the total size and alignment.
The result is a canonical layout, the same for every compiler, which the encoder writes and a dictionary-driven
reader can recompute from the FIELDs.

## map_entry_layout\<K, V\>(), container_entry_layout\<M\>() *(additional)*

**Where:** `reflect.hpp:1116, 1123`. **Context:** consteval.

Format 1.6 lays out one entry of a map block like a two-member dynamic class: the key at 0, the value at the first
offset its alignment allows, the entry padded to the larger alignment. A set's entry is its key. The value offset
and the stride go into the map FIELD (`elem[1].offset`, `extent`), so readers can walk the block without
recomputing it.

## field_for\<M\>(…), member_field, base_field, make_fields, fields_v *(additional)*

**Where:** `reflect.hpp:1136, 1228, 1238, 1294, 1299`. **Context:** consteval; `fields_v<T>` is a constant table.

`field_for` builds one `field_desc` for a member, base or element: name, offset, image size and alignment, kind,
nested type hash, extent, element descriptors, `since` and `deprecated`. It calls `check_supported` first.
`member_field` describes part `I` through the parts layer (`part_t`, `part_name`, `part_annotation`) and supplies
the native offset (`offset_of(m).bytes`), or for a dynamic class the canonical image offset; `base_field` does the
same for a base, named `<base:Name>`. `make_fields` assembles the array, bases first, over
`base_count`/`part_count` index sequences; it becomes `type_desc::fields` and, serialised, the FIELD list of the
type's ENTRY.

## make_enumerators\<E\>(), enumerators_v, enumerators_fit\<E\>() *(additional)*

**Where:** `reflect.hpp:1452, 1456, 1428`. **Context:** consteval; `enumerators_v<E>` is a constant table.

`make_enumerators` turns each enumerator into an `enumerator_desc { name, int64 value }`. The name goes through
`define_static_string(identifier_of(e))` and the value through `std::to_underlying([:e:])`. `enumerators_fit`
checks, in a `template for` over the enumerators, that values fit `int64_t` and that names and counts fit `u16`,
as the dictionary encoding requires. The table feeds the enum ENTRY, the schema string, `srdump`'s
`corpus::Side::buy` rendering, and the enum range check.

## has_fixed_underlying_type_v\<E\>, enum_underlying_kind\<E\>() *(additional)*

**Where:** `reflect.hpp:1466, 1474`. **Context:** a compile-time constant (detected by `requires { E{0}; }`) and a
consteval function.

An enum with a fixed underlying type (`enum class`, `enum E : T`) takes that type's kind. For an unscoped enum
without one, the library derives the kind from the source instead of the compiler's choice (decision D):
the unsigned kind of `sizeof(E)`, or the signed kind if an enumerator is negative. GCC and MSVC therefore agree on
the schema. The fixed flag also goes into the enum ENTRY, and it decides whether the range check applies.

## append_kindspec, append_dims, append_member, append_base *(additional)*

**Where:** `reflect.hpp:1497, 1584, 1602, 1620`. **Context:** consteval.

These are the pieces of the canonical string. `append_kindspec` writes a member's kind: `u32`, `enum<Name,u8>`,
`array<…,N>`, `struct<0x…>`, `dyn<0x…>`, `string`, `vector<…>`, `map<K,V>` and so on. A multimap member
(`is_multimap_member`) is the one place where the schema and layout texts differ in shape: the schema string
writes `multimap<K,V>`, so a multimap and a `std::vector<std::pair<K,V>>` have different schema hashes, while the
layout string writes the `vector<…>` of pairs it is on the wire, so the two share a layout hash. `append_dims`
writes `[size,align;sN;dN]`. `append_member` and `append_base` write one `name:kindspec@offset[…];` item, the
member's name coming from `part_name` and its type from `part_t`. Each takes a `schema` flag: with it the names are
kept, and without it they are dropped to give the layout string.

## struct_string, enum_string, wrapper_string, canonical_string\<T\>(schema) *(additional)*

**Where:** `reflect.hpp:1638, 1659, 1690, 1716`. **Context:** consteval.

`canonical_string<T>(schema)` dispatches to `enum_string` for enums, `wrapper_string` for a container argument,
and `struct_string` for classes, after `check_supported`. `struct_string` folds `append_base` and `append_member`
over index sequences of `base_count<U>()` bases and `part_count<U>()` parts. The result is the complete schema or
layout text of the type, such as `struct<corpus::Order>[32,8]{id:u64@0[8,8];…}`, which is then hashed, and which
`schema_string_v`/`layout_string_v` expose.

## schema_hash_of\<T\>(), layout_hash_of\<T\>(), schema_hash_v, layout_hash_v *(additional)*

**Where:** `reflect.hpp:1736, 1740`; public constants at 2637 and 2623. **Context:** consteval; the `_v` constants
are compile-time constants.

They hash `canonical_string<T>(true)` and `canonical_string<T>(false)` with `fingerprint64` (XXH3-64, `constexpr`
in `hash.hpp`). The schema hash identifies a type on the wire and in the registry. The layout hash tells
bit-compatible types apart from incompatible ones, and is what the registry compares to detect a conflict.

## canonical_name_v\<T\> *(additional)*

**Where:** `reflect.hpp:2641`, namespace `es::serreg`. **Context:** compile-time constant.

It is `std::define_static_string(canonical_name<mapped_t<T>>())`: the canonical name as a `std::string_view` over
static storage, the form run-time code can hold. It is `type_desc::name`, the `name` of every registry node, and
the scope name of a scoped sink.

## schema_string_v\<T\>, layout_string_v\<T\> *(additional)*

**Where:** `reflect.hpp:2646, 2649`, namespace `es::serreg` (public). **Context:** compile-time constants.

The exact texts the two hashes are computed from, as `std::string_view`s over static storage:
`define_static_string(canonical_string<mapped_t<T>>(true))` and the same with `false`, so that
`fingerprint64(schema_string_v<T>) == schema_hash_v<T>`. They exist for tests, tools and diagnostics that want to
see why two hashes differ; nothing is emitted into the binary unless a program reads them at run time.

## collect_leaves, compute_gaps, gaps_v, compute_bools, bools_v *(additional)*

**Where:** `reflect.hpp:1757–1855`. **Context:** consteval; `gaps_v<T>` and `bools_v<T>` are constant tables in
`.rodata`.

`collect_leaves` walks a flat type's leaves at their native offsets (`offset_of`) and records the byte interval
each scalar covers, plus the offset of every `bool`. An array of a type with unique object representations counts
as one interval, so a 1 MiB buffer costs one entry, not a million. `compute_gaps` sorts and merges the intervals
and takes the complement over `[0, sizeof(T))`: the padding. `serialize` zeroes those gaps and normalises the bools
from these tables, so equal values give equal bytes (FR-S6).

## ES_SERREG_VALIDATE_READ, validate_read_enums *(additional)*

**Where:** `reflect.hpp:1866–1869`. **Context:** compile-time constant (a macro and the `inline constexpr bool` it
sets).

The read-side policy for enums without a fixed underlying type. `ES_SERREG_VALIDATE_READ` defaults to 0, and
`validate_read_enums` is `ES_SERREG_VALIDATE_READ != 0`. With 0 the readers copy the bytes as they are and the
`enum_ranges_v` tables are not instantiated; with 1 `copy_flat` and `bools_canonical` check every bounded enum
leaf, set a leaf outside its range to 0 and report the read as `error::invalid_value` while the rest of the frame
is still decoded. Like `ES_SERREG_LOG_MIN_LEVEL`, it is a property of the translation unit that instantiates the
reader, not of the wire format.

## enum_range_of\<E\>(), collect_enum_ranges\<M\>(), enum_ranges_v\<T\> *(additional)*

**Where:** `reflect.hpp:1877, 1917, 1970`. **Context:** consteval; `enum_ranges_v<T>` is a constant table.

`enum_range_of` computes the valid range of an enum without a fixed underlying type: the smallest bit-field that
holds every enumerator, signed when an enumerator is negative. `collect_enum_ranges` is the walk that
`collect_member_enum_ranges` serves; it records one range per bounded enum leaf. The table is only named by the
readers when `validate_read_enums` is `true`; it is empty for most types, and the readers' checks then compile
away.

## enum_leaf_in_range(r, p) *(additional)*

**Where:** `inline bool enum_leaf_in_range(const enum_range& r, const std::byte* const p) noexcept` at
`reflect.hpp:1972`. **Context:** run time.

It reads the leaf at `p` as an integer of `r.size` bytes, sign-extending when the range is signed, and reports
whether it lies in `[lo, hi]`. `copy_flat` and `bools_canonical` call it for every entry of `enum_ranges_v<U>` when
the validation policy is on.

## put_flat_image\<U\>(dst, v) *(additional)*

**Where:** `reflect.hpp:1998`. **Context:** run time.

It writes a flat value into an image: `memcpy`, then zero the padding gaps and normalise the bools from the
constant tables. A C array of classes or bools is processed element by element. `encode_value` uses it for every
flat leaf, and `key_less` uses it to compare keys by their bytes.

## sequence_of(v), sequence_size(s), for_each_in_wire_order\<U\>(s, canonical, f) *(additional)*

**Where:** `reflect.hpp:2067, 2078, 2087`; `key_less` is forward-declared at 2065 for the sort. **Context:** run
time (`sequence_of` is `constexpr`; its branch is chosen at compile time).

The three helpers that let one `vector` branch serve every container of that shape. `sequence_of(v)` returns the
sequence behind a member: the container itself, or for a `std::stack` (`is_container_adapter`) its protected
underlying container, reached by splicing the adapter's single data member. `sequence_size(s)` is `s.size()` when
the sequence has one and `std::ranges::distance(s)` for `std::forward_list`. `for_each_in_wire_order<U>(s,
canonical, f)` calls `f` on the entries in the order they are written: the container's own order for everything
but an unordered multimap (or one ordered by a comparator other than `std::less`), whose entries — when
`canonical` is `true`, the value of `image_cursor::canonical_unordered` — are stably sorted by `key_less` on the
key over an array of pointers (64 on the stack, a `std::vector` above that), so that equal keys keep their
relative order and equal values give equal bytes; with `canonical == false` the multimap too is walked as
iterated (the sort decision `constexpr bool sort` is still compile-time; the flag only selects between the two
run-time loops).

## key_less(a, b), encode_entry\<U\>(e, at, c) *(additional)*

**Where:** `reflect.hpp:2134, 2174`. **Context:** run time.

`key_less` orders map and set keys for the canonical order, and multimap keys for `for_each_in_wire_order`. It
uses the key's `operator<` when it has one, and otherwise compares the keys' scrubbed images with `memcmp`.
`encode_entry` writes one map or set entry: the key at the entry's start and, for a map, the value at
`container_entry_layout<U>().value_offset`.

## measure_image, encode_image, decode_image *(additional)*

**Where:** `reflect.hpp:2268, 2388, 2621`. **Context:** run time.

These are the entry points of the image walks for a root value. `measure_image` returns the fixed part plus the
measured tail. `encode_image(v, image, len, canonical_unordered = true)` zeroes the buffer, seeds the
`image_cursor` with the flag and encodes. `decode_image` decodes and maps failures to
`length_mismatch` or `invalid_value`, and emits the `noncanonical_bool` warning. `serialize.hpp` and
`deserialize.hpp` call only these three.

## copy_flat\<U\>(dst, src), bools_canonical\<U\>(src) *(additional)*

**Where:** `reflect.hpp:2417, 2440`. **Context:** run time.

`copy_flat` is how a typed reader takes a flat value out of a frame: `memcpy`, then any bool byte other than 0 or 1
is repaired to 1, and, only when `validate_read_enums` is `true`, any bounded enum leaf outside its range is set
to 0. Both counts are returned in a `flat_copy`, so the caller can warn or report `invalid_value`.
`bools_canonical` performs the same checks without copying; the zero-copy `view<T>()` uses it to decide whether a
pointer into the frame is safe to hand out. With the policy off (the default), both functions are the bool repair
alone and the enum tables are never instantiated.

## monostate_index\<V\>() *(additional)*

**Where:** `reflect.hpp:2454`. **Context:** consteval.

It returns the index of `std::monostate` among a variant's alternatives, or `variant_npos`. `decode_value` uses
it when an image holds a valueless variant: if the C++ type has a `monostate` alternative it is emplaced,
otherwise the decode reports `invalid_value`.

## concept flat_type\<T\> *(additional)*

**Where:** `reflect.hpp:2653`, namespace `es::serreg`. **Context:** compile-time constant (a concept).

A class or enum (after `as_flat` mapping) that is trivially copyable. Its record is its native bytes, written with
`memcpy` plus the scrub, and read back with `copy_flat`. It is the counterpart of `dynamic_type`, and the
`serializable_arg` concept accepts either.

## make_type_desc_base\<T\>() *(additional)*

**Where:** `reflect.hpp:2670`. **Context:** consteval.

It assembles the `type_desc` of a type from everything above: canonical name, schema and layout hashes, image
size and alignment, `since` and `deprecated`, flat or not, the kind, enumerators or fields, and the gap and bool
tables. `blob.hpp`'s `make_type_desc<T>()` adds the ENTRY blob bytes, and the result is `descriptor_v<T>`, the
constant the registry nodes point at. It is the hand-off point from reflection to the registry described in
[`serreg-registry.md`](serreg-registry.md).

---

# projection.hpp — schema evolution level 1

`include/es/serreg/projection.hpp` is the second header that uses `<meta>`: it reads the `renamed_from`
annotations. It lets a typed reader accept a frame whose writer type matches the reader's by canonical name (or
through `renamed_from`) but not by hash, projecting the writer's flat image into the reader's object member by
member (rules R1–R14 in the header comment). Only flat types project; a type with a dynamic member keeps the
exact-hash path. Each entry is marked *(additional)*; namespaces are given where they are not `detail`.

| Entry | Where | Context |
|---|---|---|
| `renamed_from<N>` | `projection.hpp:46` (namespace `es::serreg`) | compile-time constant (annotation type) |
| `tools::projection_report` | `projection.hpp:65` | run time |
| `old_names_of<M>()` | `projection.hpp:84` | consteval |
| `collect_renames<T>()`, `make_renames<T>()`, `renames_v<T>` | `projection.hpp:102, 139, 151` | consteval / constant table |
| `collect_type_old_names<T>()`, `type_old_names_v<T>` | `projection.hpp:152, 169` | consteval / constant table |
| `proj_step`, `projection_plan` | `projection.hpp:172, 178` | run time (heap object) |
| `leaf_rule(sk, dk)` | `projection.hpp:216` | `constexpr`, called at run time |
| `convert_leaf(dst, src, sk, dk)` | `projection.hpp:233` | run time |
| `plan_leaf`, `plan_array<E>`, `plan_member<R>`, `plan_class<T>` | `projection.hpp:298, 321, 378, 449` | run time (once per writer type) |
| `name_matches<T>(w)`, `build_plan<T>(w, dict)` | `projection.hpp:489, 500` | run time |
| `plan_cache<T>` | `projection.hpp:512` | run time (lock-free list) |
| `apply_plan<T>(p, out, src)` | `projection.hpp:540` | run time |
| `projectable_v<T>` | `projection.hpp:555` | compile-time constant |
| `tools::project<T>(writer, dict, bytes, report)` | `projection.hpp:568` | run time |
| `projection_context` | `projection.hpp:592` (namespace `es::serreg`) | run time |
| `deserialize_projecting(ctx, bytes, outs...)` | `projection.hpp:601` | run time |
| `deserialize(ctx, bytes, outs...)`, `deserialize(dict, bytes, outs...)` | `projection.hpp:794, 813` (namespace `es::serreg`) | run time |

## renamed_from\<N\> *(additional)*

**Where:** `template<std::size_t N> struct renamed_from` at `projection.hpp:46`, namespace `es::serreg`.
**Context:** compile-time constant (an annotation type, like `name<N>`).

`[[=es::serreg::renamed_from("px")]]` on a member says that frames whose writer called the member `px` fill it;
on a class, `[[=es::serreg::renamed_from("md::Quote")]]` says that frames written as `md::Quote` project into it.
It is repeatable and reader-side only: it changes neither the schema string nor the dictionary, so a type's hashes
do not move when a rename annotation is added.

## tools::projection_report *(additional)*

**Where:** `projection.hpp:65`, namespace `es::serreg::tools`. **Context:** run time.

The count of what a plan does per reader member (`copied`, `widened`, `defaulted`, `skipped`, `conflicts`) and
`notes`, one line per member that is not a plain copy, such as `evo::Order.qty: u32 -> u64 widened`. `add()`
accumulates another report's counts. `project` and the projecting `deserialize` fill it on request; the notes are
appended only by the call that built the plan, so they are not repeated per frame.

## old_names_of\<M\>() *(additional)*

**Where:** `template<std::meta::info M> consteval std::vector<std::string> old_names_of()` at `projection.hpp:84`.
**Context:** consteval.

The one reflection query of the header. For the member or type reflection `M`, it iterates `annotations_of(M)` with
`template for` and collects the text of every annotation whose type is a specialisation of
`es::serreg::renamed_from` (`template_of(ty) == ^^es::serreg::renamed_from`, then `extract`). The vector lives
only during constant evaluation; the callers below freeze its strings with `define_static_string`.

## collect_renames\<T\>(), make_renames\<T\>(), renames_v\<T\> *(additional)*

**Where:** `projection.hpp:102, 139, 151`; the checks `renames_distinct` and `renames_not_current` at 118 and 127.
**Context:** consteval; `renames_v<T>` is a constant table.

`collect_renames` walks the kept parts of `T` (`part_count`, `member_skipped`, `part_name`) and records one
`rename { member, old }` per `renamed_from` on each. `make_renames` turns it into a `std::array` after two
`static_assert`s: no two members may claim the same old name, and an old name may not be a member the type still
has. `renames_v<T>` is what `find_writer_field` consults when a reader member is missing from the writer's FIELD
list.

## collect_type_old_names\<T\>(), type_old_names_v\<T\> *(additional)*

**Where:** `projection.hpp:152, 169`. **Context:** consteval; `type_old_names_v<T>` is a constant table.

The same for the type itself: the `renamed_from` annotations on the class, as `string_view`s. `name_matches<T>`
accepts a writer type whose canonical name is `descriptor_v<T>.name` or any of these.

## proj_step, projection_plan *(additional)*

**Where:** `projection.hpp:172, 178`. **Context:** run time (a heap object built once per `(writer, T)` pair).

A `proj_step` is one copy or conversion: source and destination byte offsets, source and destination leaf kinds
(`struct_` for a `memcpy` of a nested object with an equal layout), element count and strides. A
`projection_plan` is the writer's schema hash and size, `matched` (the writer is `T` by name; `false` makes the
read a `hash_mismatch`), the steps, and the `projection_report` produced while planning.

## leaf_rule(sk, dk) *(additional)*

**Where:** `constexpr int leaf_rule(const kind sk, const kind dk) noexcept` at `projection.hpp:216`, with
`int_rank`, `signed_int_kind` and `unsigned_int_kind` at 187–214. **Context:** `constexpr`; called at run time
while planning.

The scalar compatibility rule as one function: 1 for the same kind (R1), 2 for a widening (a signed integer to a
wider signed one, an unsigned to a wider unsigned or to a strictly wider signed one, `f32` to `f64`: R2–R4), 0 for
anything else (narrowing, another family, `bool` to or from an integer: R7). Only scalar kinds take part; enums
are compared by their underlying kind (R8).

## convert_leaf(dst, src, sk, dk) *(additional)*

**Where:** `projection.hpp:233`. **Context:** run time (the per-frame path).

Executes one leaf of a plan whose `leaf_rule` is not 0: a `memcpy` of `kind_size(sk)` bytes for the same kind (a
`bool` byte above 1 is repaired to 1), `float` to `double` through a conversion, and an integer widening by
loading the source into a `uint64_t`, sign-extending when the source kind is signed, and storing the destination's
low bytes (the format is little-endian).

## plan_leaf, plan_array\<E\>, plan_member\<R\>, plan_class\<T\> *(additional)*

**Where:** `projection.hpp:298, 321, 378, 449` (`plan_class` is declared at 373 so `plan_member` can recurse);
helpers `find_writer_field`, `find_reader_field`, `plan_note`, `kinds_text` at 262–295. **Context:** run time, once
per `(writer, T)` pair; this is where allocation happens.

`plan_class<T>` walks the reader's bases and kept parts over `base_count`/`part_count` index sequences, finds each
one's writer FIELD by name (a base by its `<base:Name>` pseudo-field name, a part by `part_name`, or through
`renames_v<T>`) and calls `plan_member<part_t<T, I>>` or `plan_member<base_t<T, B>>`; writer-only FIELDs are
counted as skipped (R5). `plan_member<R>` dispatches on the reader member's C++ type: a missing writer field means
the member keeps its `T{}` value (R6); an enum is planned as its underlying kind against the writer's enum ENTRY
(R8); an array goes to `plan_array` (R12: the writer's `N` elements into the reader's `M ≥ N`, elements of equal
layout copied, scalar and enum elements through `plan_leaf`); a nested class with an equal layout hash is one
`memcpy` step, otherwise a flat one recurses into `plan_class<U>` by member name (R9); a scalar goes to
`plan_leaf`, which applies `leaf_rule` and records a step, a widening note or an R7 conflict.

## name_matches\<T\>(w), build_plan\<T\>(w, dict) *(additional)*

**Where:** `projection.hpp:489, 500`. **Context:** run time.

`name_matches` compares the writer type's canonical name with `descriptor_v<T>.name` and `type_old_names_v<T>`.
`build_plan` allocates the `projection_plan`, records the writer's hash and size, sets `matched` when the name
matches and the writer type is flat, not an enum and not a transparent wrapper, and then runs `plan_class<T>` from
offset 0 with the reader's name as the path prefix of every note.

## plan_cache\<T\> *(additional)*

**Where:** `template<class T> struct plan_cache` at `projection.hpp:512`. **Context:** run time.

The plans of `T`, one per writer schema hash, kept for the life of the process in a lock-free singly linked list
(`static inline std::atomic<node*> head`). `get(w, dict, built)` scans the list under an acquire load, and when
the writer's hash is new builds a plan and pushes a node with a CAS loop; `built` tells the caller that this call
created the plan, so the plan's notes are reported once. Nothing on the per-frame path allocates or locks once the
plan exists; two threads meeting the same new writer at once may each build a plan, and both stay valid.

## apply_plan\<T\>(p, out, src) *(additional)*

**Where:** `projection.hpp:540`. **Context:** run time (the per-frame path, `noexcept`).

`out = T{}` first, so reader-only members take their default member initialisers (R6), then every `proj_step`:
a `memcpy` of `sstride` bytes per element for `struct_` steps, `convert_leaf` per element otherwise. `src` holds
at least `plan.writer_size` bytes, which the callers have checked.

## projectable_v\<T\> *(additional)*

**Where:** `projection.hpp:555`. **Context:** compile-time constant.

A reader argument that may be projected: a class that is a `flat_type`, has no `as_flat` mapping, is not a
literal, constant or string argument, and is default-constructible (for `out = T{}`). `project` requires it;
`deserialize_projecting` falls back to an exact-hash match for every other argument category.

## tools::project\<T\>(writer, dict, bytes, report) *(additional)*

**Where:** `projection.hpp:568`, namespace `es::serreg::tools`. **Context:** run time.

Fills a `T` from the bytes of a value of writer type `writer` (a `dyn::type` from `dict`). It registers `T`
(`touch<T>()`), fetches or builds the plan, returns `hash_mismatch` when the writer is not `T` by name and
`truncated` when fewer than `writer.size` bytes are given, applies the plan and, when a report is passed, adds the
plan's counts (and its notes if this call built it). R7 members are not an error: they keep their default and are
counted.

## projection_context *(additional)*

**Where:** `struct projection_context` at `projection.hpp:592`, namespace `es::serreg`. **Context:** run time.

The two pointers a projecting reader needs: the `dyn::dictionary` it resolves writer calls and types through, and
an optional `projection_report` to count into. Passing a bare `dyn::dictionary&` to `deserialize` builds one with
no report.

## deserialize_projecting(ctx, bytes, outs...) *(additional)*

**Where:** `template<class... Ts> decode_result<std::size_t> deserialize_projecting(…)` at `projection.hpp:601`.
**Context:** run time.

The two-pass reader behind the projecting `deserialize`. Pass 1 resolves every record without writing anything
(G2-1): the writer's argument hash comes from the full-record header or from the writer's CALL entry in the
dictionary; a hash equal to the reader's `arg_hash<T>()` means an exact record, whose size is checked as
`validate_frame` would; otherwise, for a `projectable_v` argument, the writer type is looked up, `plan_cache<T>`
gives the plan, and `matched`, the record length and the frame bounds are checked. Any failure is reported with
the index of the failing argument. Pass 2 writes the outputs: `get_record` for exact records, `apply_plan` for
projected ones, adding to the report; an `invalid_value` from an exact record is kept as a soft error so the
remaining arguments are still filled.

## deserialize(ctx, bytes, outs...), deserialize(dict, bytes, outs...) *(additional)*

**Where:** `projection.hpp:794` and `813`, namespace `es::serreg` (public). **Context:** run time.

Style 1 reading with a dictionary. The overload first tries the exact path, `deserialize(bytes, outs...)`, when
the frame is a full-records frame or its call hash is the reader's `call_hash_v<Ts...>`; only a `hash_mismatch`
on a full-records frame, or a compact frame of another call, goes on to `deserialize_projecting`. So a frame whose
hashes all match costs nothing more than before. Compact frames need the writer's CALL entry in the dictionary;
literals and constants must match exactly. The `dyn::dictionary&` overload wraps it in a `projection_context`
without a report.
