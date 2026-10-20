# `include/es/serreg/reflect.hpp`

The one header that uses C++26 reflection (`<meta>`, P2996; GCC 16 `-std=c++26 -freflection`). It turns a C++
type into `descriptor_v<T>` (completed in `blob.hpp`), `schema_hash_v<T>` / `layout_hash_v<T>` (XXH3-64 of the
canonical schema/layout strings, spec §6.2, FR-H1/H4) and `canonical_name_v<T>` (§6.2b); it decides which types
are `flat_type` and which are `dynamic_type`; it computes the padding-gap and bool tables (FR-S6); it
computes the canonical fixed layout of dynamic classes and implements the three mirrored image walks
`measure_image` / `encode_image` / `decode_image`; it maps the standard containers onto the wire shapes — the six
map/set containers onto `map_`/`set_` (sorted entries, `map_entry_layout`, format 1.6), `std::deque`/`list`/
`forward_list`/`stack` and the two multimaps onto `vector_`, `std::pair`/`tuple` onto positional dynamic classes —
and gives a container passed as an argument its transparent type (`wrapper_string`, `wrapper_fields_v`). Every
walk over a class goes through the **parts layer** (`base_count`, `part_count`, `part_t`, `part_name`, `get_part`),
which is what lets a tuple-like type share the code path of a reflected class. The std vocabulary is closed:
`check_supported` rejects any other standard-library class by name. This is the **compile-time reflection layer**.
Customisation points: `as_flat<T>` (FR-S8), `[[=skip]]`, `[[=since(N)]]`, `[[=deprecated(N)]]` (FR-S11). Member iteration
deliberately uses `index_sequence` over `define_static_array(...)[I]` rather than `template for` wherever
possible (the §6.1 fallback) to depend on as few companion papers as possible; `template for` is used in
`canonical_name` (template arguments) and `enumerators_fit`.

## Depends on / used by

Includes (`reflect.hpp:17-50`): `es/serreg/descriptor.hpp`, `error.hpp`, `format.hpp`, `hash.hpp`; `<meta>`,
`<algorithm>`, `<array>`, `<climits>`, `<concepts>`, `<cstddef>`, `<cstdint>`, `<cstring>`, `<functional>`,
`<memory>` (the smart-pointer rejection), `<optional>`, `<string>`, `<string_view>`, `<tuple>`, `<type_traits>`,
`<utility>`, `<variant>`, and every container it maps: `<deque>`, `<flat_map>`, `<flat_set>`, `<forward_list>`,
`<list>`, `<map>`, `<set>`, `<stack>`, `<unordered_map>`, `<unordered_set>`, `<vector>`.

Included by: `arg.hpp` (`flat_type`, `dynamic_type`, `is_scalar_v`, `scalar_kind`, `schema_hash_v`), `blob.hpp`
(`make_type_desc_base`, `mapped_t`), and via those everything above. `registry.hpp::touch` uses the parts layer
(`base_count`, `part_count`, `base_t`, `part_t`, `member_skipped`), `is_dyn_member_v`, `dyn_member`;
`serialize.hpp` uses `descriptor_v` tables, `has_as_flat`/`as_flat`, `measure_image`, `encode_image`;
`deserialize.hpp` uses `decode_image`, `is_dynamic`, `copy_flat`, `bools_canonical`; `projection.hpp` uses the
parts layer, `members<T>()`, `scalar_kind`, `is_std_array_v`, `layout_hash_v`.

## What it defines

### Public

| name | kind | meaning |
|---|---|---|
| `as_flat<T>` | primary template (undefined) | specialise with `using type = U; static U to(const T&); static T from(const U&);` |
| `has_as_flat<T>` | concept | `as_flat<T>::type` exists |
| `skip_t`, `skip` | struct / `inline constexpr` | `[[=es::serreg::skip]]` annotation |
| `since`, `deprecated` | structs `{uint16_t v}` | `[[=es::serreg::since(3)]]` annotations, on members or on the type |
| `schema_hash_v<T>`, `layout_hash_v<T>` | `inline constexpr uint64_t` | over `mapped_t<T>` |
| `canonical_name_v<T>` | `inline constexpr string_view` | `define_static_string(canonical_name<mapped_t<T>>())` |
| `schema_string_v<T>`, `layout_string_v<T>` | `inline constexpr string_view` | the exact canonical strings the two hashes are computed from (`fingerprint64(schema_string_v<T>) == schema_hash_v<T>`); `define_static_string` of `canonical_string<mapped_t<T>>(true/false)` (`reflect.hpp:2769-2774`), so one copy per type per executable, emitted only when a program reads it at run time |
| `name<N>`, `type_properties<T>` | struct / primary template | the explicit canonical name: `[[=es::serreg::name("…")]]` on a type, or `type_properties<T>::name()` for a type one cannot annotate; the std vocabulary below is a set of `type_properties` specialisations |
| `flat_type<T>` | concept | `mapped_t<T>` trivially copyable and class or enum |
| `dynamic_type<T>` | concept | class, not flat, not `string_view`, and not a dyn member type — except a container, which is a type of its own as an argument (`is_container_arg_v`: every dyn member kind but `string_`); any other non-trivially-copyable class is admitted so `check_supported` can name the rule it breaks |

### `detail` — traits and checks

| name | meaning |
|---|---|
| `is_std_array_v`, `array_traits<T>` | `E[N]` and `std::array<E,N>`: `is_array`, `elem`, `extent` |
| `scalar_kind<T>()` | `kind` of a scalar by width/signedness (`long` → `i64` on LP64); `static_assert` for `long double`, `__int128` |
| `is_scalar_v<T>` | arithmetic except `long double`, or `std::byte` |
| `mapped<T>` / `mapped_t<T>` | `as_flat<T>::type` if specialised, else `remove_cv_t<T>` |
| `dyn_member<T>` | `std::basic_string<char, std::char_traits<char>, A>` for any allocator `A` — `std::string`, `std::pmr::string` (`string_`), `std::vector<E,A>` (`vector_`, `elem`), `std::optional<E>` (`optional_`, `elem`), `std::variant<Es...>` (`variant_`, `count`); `std::map<K,V,C,A>`, `std::unordered_map<K,V,H,E,A>`, `std::flat_map<K,V,C,KC,MC>` (`map_`, `key`, `elem`, `ordered`), `std::set<K,C,A>`, `std::unordered_set<K,H,E,A>`, `std::flat_set<K,C,KC>` (`set_`, `key`, `elem` = `key`, `ordered`). `ordered` is `is_canonical_less_v<K,C>` — `std::less<K>` or `std::less<>` — for the ordered containers and `false` for the unordered ones. `std::deque<E,A>`, `std::list<E,A>`, `std::forward_list<E,A>` are `vector_` with `elem` (`reflect.hpp:301-315`); `std::multimap<K,V,C,A>` and `std::unordered_multimap<K,V,H,E,A>` are `vector_` with `elem = std::pair<K,V>`, `multi = true` and `sort_by_key` (false for a `std::less` multimap, which already iterates in key order; true otherwise) (`reflect.hpp:320-335`); `std::stack<T,C>` is `vector_` with `elem = T` and `adapter = true` (`reflect.hpp:337-342`). A user container is added as another specialisation |
| `is_canonical_less_v<K,C>` | the comparator iterates in the canonical order (ascending `operator<`) |
| `is_dyn_member_v<T>`, `is_map_or_set<T>()`, `is_container_arg<T>()` / `is_container_arg_v<T>`, `is_string_view_v<T>` | a map/set shape; a dyn member type that is an argument of its own (every kind but `string_`) |
| `is_multimap_member<T>()`, `is_container_adapter<T>()` | read the optional `multi` / `adapter` markers of a `dyn_member` specialisation (`reflect.hpp:344-357`): a multimap spells `multimap<K,V>` in the schema string and may need a stable sort; an adapter's sequence is its protected container |
| `is_tuple_like_v<T>` | `std::pair<A,B>` and `std::tuple<Ts...>` (`reflect.hpp:361-363`): positional parts named `first`/`second` or `_0`, `_1`, …, reached through `std::get`; always a dynamic class, even when every part is flat, because the parts' order in memory is the standard library's (a libstdc++ tuple stores them in reverse) |
| `is_smart_pointer_v<T>` | `std::unique_ptr<T,D>`, `std::shared_ptr<T>`, `std::weak_ptr<T>` (`reflect.hpp:364-367`): rejected by name in `check_supported` |
| `is_dynamic<T>()` | any dyn member anywhere in the tree (class, array, …) |
| `is_dynamic_class<T>()` | a class encoded as a canonical image (not itself a dyn member or array) |
| `check_supported<T>()` | FR-S7 `static_assert`s (`reflect.hpp:477-574`): `vector<bool>`; a vector/optional/variant element that occupies no bytes in the image (`check_nonempty_element<E>()`); a map's key and value / a set's key are checked the same way, and an unordered container (or one ordered by another comparator) needs `std::totally_ordered<K>` or a trivially copyable key, because the writer sorts it; a tuple-like type's parts, and `std::tuple<>` rejected ("has no parts to serialise"); then, for anything else, smart pointers by name, **any other standard-library class** ("not in the serialisable vocabulary …" — the closed vocabulary rule, `in_std` and no `type_properties<U>::name()`), recursive types, pointers, references, unions, polymorphic types, `string_view` members, and non-trivially-copyable classes that are not dynamic. The message names the rule |

### `detail` — reflection helpers

| name | meaning |
|---|---|
| `ctx()` | `std::meta::access_context::unchecked()` (FR-S9: private members are described) |
| `members<T>()`, `bases<T>()`, `enumerators<E>()` | `define_static_array` of `nonstatic_data_members_of` / `bases_of` / `enumerators_of` |
| `has_annotation<A>(info)`, `annotation_value<A>(info)` | scan `annotations_of`; value via `std::meta::extract<A>(a).v`, 0 when absent |
| `scalar_name(info)` | builtin type → kind name table (`^^int` → `"i32"`, `^^long` → `"i64"`/`"i32"` by `sizeof`) |
| `lacks_linkage(info)` | an unnamed namespace or a function between the entity and the global namespace (an enclosing class stops the walk) |
| `std_implementation_namespace(info)` | a reserved identifier (`_…`) inside `std` (`__cxx11`, `__1`, `_V2`): dropped from names |
| `scope_of<P>()` | `"scope::"` — namespaces by identifier, an enclosing class (also a template specialisation) by its own canonical name |
| `annotated_name<U>()`, `annotated_name_v<U>`, `properties_name_v<U>`, `valid_explicit_name(sv)` | the explicit names (`es::serreg::name` annotation, `type_properties<T>::name()`) as static strings, and their character check |
| `in_std(info)`, `component_types(info, out)`, `recursive_type(info)` | C-07: a reflection walk through members, bases, arrays and std containers (every template of the vocabulary, `reflect.hpp:429-435`) that finds a type containing itself; `in_std` also decides the vocabulary rule in `check_supported` |
| `render_value_arg(v)` | template-argument values: `true`/`false`, decimal, `enum(<n>)`; anything else rejected |
| `canonical_name<T>()` | explicit name → scalar kind → (no linkage-unique name: `static_assert`) → templates `scope::Name<arg,…>` with each argument rendered canonically → `scope::Name`; the std vocabulary comes in as `type_properties` specialisations |

### `detail` — parts: the positional view of a class

Everything that walks a class — fields, canonical strings, image layout, measure/encode/decode, registration,
projection — goes through these (`reflect.hpp:595-646`), so a reflected class and a tuple-like class share one
code path.

| name | meaning |
|---|---|
| `base_count<T>()`, `part_count<T>()` | `bases<T>().size()` / `members<T>().size()`; 0 and `std::tuple_size_v<T>` for a tuple-like `T` |
| `part_t<T,I>`, `base_t<T,I>` | the type of part `I` (`type_of(members<T>()[I])`, or `std::tuple_element_t<I,T>`) / of base `I` |
| `part_name<T,I>()` | `identifier_of(members<T>()[I])`; `first`/`second` for a `std::pair`, `_0`, `_1`, … for a `std::tuple` |
| `get_part<I>(v)` | `v.[:members<T>()[I]:]` (private members included) or `std::get<I>(v)`; returns a reference |
| `member_skipped<T,I>()`, `part_annotation<A,T,I>()` | `[[=skip]]` / `since`/`deprecated` of a member; `false` / 0 for tuple parts (`reflect.hpp:1263-1276`) |

### `detail` — descriptor construction

| name | meaning |
|---|---|
| `schema_hash_of<T>()`, `layout_hash_of<T>()`; `schema_hash_memo<T>`, `layout_hash_memo<T>` | the hashes; memoised per type (cc F9) |
| `element_field<E>()`, `element_field_v<E>` | array/vector/optional element descriptor (name empty); a set's key |
| `make_variant_alts<V>`, `variant_alts_v<V>` | `array<field_desc, count>` of the alternatives |
| `entry_layout {value_offset, size, align}`, `map_entry_layout<K,V>()`, `container_entry_layout<M>()` | one entry of a map block — the key at 0, the value at `align_up(image_size<K>, image_align<V>)`, the entry padded to the larger alignment (`size` is the stride); a set's entry is `{0, image_stride<K>, image_align<K>}` |
| `make_map_elems<M>()`, `map_elems_v<M>` | the two element descriptors of a `map_` FIELD — the key at entry offset 0, the value at `value_offset` |
| `wrapper_string<U>(schema)`, `make_wrapper_fields<U>()`, `wrapper_fields_v<U>` | FR-A6: the transparent type of a container argument — `dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}`, one unnamed field at 0 |
| `field_for<M>(name, offset, since, deprecated)` | builds one `field_desc` by kind; rejects nested `as_flat` |
| `member_offset<T,I>()`, `base_offset<T,I>()` | native `offset_of(...).bytes` for flat, canonical image offset for dynamic classes |
| `member_field<T,I>()`, `base_field<T,I>()` | one part (rejects bit-fields and — review B4 — `const` and `volatile` members of a reflected class: a reader would have to modify a `const` subobject; `[[=skip]]` members are never instantiated, `mutable` passes; `part_name`, `member_offset`, `part_annotation`) / one base (rejects virtual bases; name `<base:Name>`) |
| `member_skipped<T,I>()`, `kept_member_count`, `field_count<T>()` | `[[=skip]]` handling |
| `make_fields<T>()`, `fields_v<T>` | bases first, then kept members, declaration order |
| `enumerators_fit<E>()`, `make_enumerators<E>()`, `enumerators_v<E>` | values must fit `int64_t`, counts/names `uint16_t` |
| `has_fixed_underlying_type_v<E>` | decision D: `requires { E{0}; }` — list-initialisation from an `int` is well-formed only for an enum with a fixed underlying type (`enum class`, `enum E : T`) |
| `has_negative_enumerator<E>()`, `enum_underlying_kind<E>()` | decision D: the kind written into every canonical string and descriptor — `scalar_kind<std::underlying_type_t<E>>()` for a fixed type; otherwise the unsigned kind of `sizeof(E)`, or the signed kind when an enumerator is negative |
| `make_type_desc_base<T>()` | everything in `type_desc` except `blob`; `enum_fixed` for enums, `transparent` + `wrapper_fields_v` for a container argument |

### `detail` — canonical strings

`append_kindspec<M>(s, schema)`, `append_dims(s, size, align, since, deprecated, schema)`,
`append_member<T,I>`, `append_base<T,I>`, `struct_string<T>(schema, …)`, `enum_string<E>(schema)`,
`wrapper_string<U>(schema)`, `canonical_string<T>(schema)`. The public `schema_string_v<T>` / `layout_string_v<T>`
are `canonical_string<mapped_t<T>>(true / false)` as static strings.

### `detail` — gaps and bools (FR-S6)

`collect_leaves<M>(out, bools, base)`, `collect_member_leaves<U,I>`, `compute_gaps<T>()`, `gap_count`,
`make_gaps`, `gaps_v<T>`; `compute_bools<T>()`, `bool_count`, `make_bools`, `bools_v<T>`.

### `detail` — bounded enum leaves (decision D; FR-D5)

`enum_range {offset, size, is_signed, lo, hi}`, `enum_range_of<E>(offset)`, `collect_enum_ranges<M>(out, base)`,
`collect_member_enum_ranges<U,I>`, `enum_range_count<T>()`, `make_enum_ranges<T>()`, `enum_ranges_v<T>`,
`enum_leaf_in_range(range, p)`: the table of every leaf of a flat `T` whose type is an enum *without* a fixed
underlying type (members, bases, array elements, at native offsets), with the range of the minimal-width integer
that holds its enumerators. Built like `bools_v<T>` (count, then copy), and empty for every type without such a
leaf, so the check costs nothing there. Whether the readers use it is a **compile-time policy of the translation
unit**: `ES_SERREG_VALIDATE_READ` (default 0) defines `inline constexpr bool validate_read_enums`
(`reflect.hpp:1942-1945`); `copy_flat` and `bools_canonical` test it `if constexpr`, so with the default the tables
are not even instantiated and a flat value is copied as it is. `serreg_tests` is built with
`ES_SERREG_VALIDATE_READ=1`, `serreg_validate_off_tests` with the default (`files/tests.md`).

### `detail` — dynamic images

| name | meaning |
|---|---|
| `ref_slot_size = 8`, `ref_slot_align = 4`, `align_up(n, a)` | a reference slot is `{u32 off, u32 n}` |
| `fixed_layout {size, align}`, `compute_fixed_layout<T>()`, `fixed_layout_v<T>` | fixed-part geometry of a dynamic class |
| `image_size<M>()`, `image_align<M>()`, `image_stride<E>()` | size/alignment of one value's image; stride inside a block |
| `compute_canonical_offsets<T>`, `canonical_offsets_v<T>`, `base_image_offset<T,B>()`, `member_image_offset<T,I>()` | canonical offsets |
| `max_image_depth = fmt::max_nesting` (64) | decode recursion cap; the constant the dictionary walker also derives its limit from (D-10) |
| `put_flat_image(dst, v)` | memcpy + gap/bool scrub of one flat leaf (the 1.0 bytes); C arrays of classes/bools per element |
| `image_cursor {base, tail, cap, canonical_unordered}`, `alloc(n, a)`, `put_slot(dst, off, n)` | tail allocator; `canonical_unordered` (default `true`) carries `serialize_options::canonical_unordered` into the walk: unordered containers sorted by key, or written in their iteration order |
| `for_each_part<T>(fb, fm)` | bases then parts, as `integral_constant` indices (`base_count`/`part_count`) |
| `sequence_of(v)`, `sequence_size(s)`, `for_each_in_wire_order<U>(s, canonical, f)` (`reflect.hpp:2151-2225`) | the `vector_` shape over any sequence: the container itself, or a `std::stack`'s protected container (spliced: `v.[:members<U>()[0]:]`); `size()` or `std::ranges::distance` (a `std::forward_list`); the elements in wire order — as iterated, or for an `unordered_multimap` with `canonical` set stably sorted by key over an index of pointers (64 on the stack, a `std::vector` above) with `key_less`; `canonical == false` walks it as iterated |
| `measure_value`, `measure_image(v)` | total image length; every block is added with `add_block` (overflow-checked multiply and add, saturating at `measure_limit` = 2³², review B7), so an oversized or lying `size()` makes `serialize_frame` return `invalid_argument` instead of wrapping |
| `may_sort<T>()`, `may_sort_v<T>` | true when `T` or anything it contains is an unordered container, a map/set with a non-`std::less` comparator or a multimap shape sorted by key: the types whose `serialize` may allocate (the order table above 64 entries) — what `serialize_noexcept` excludes (review B1) |
| `encode_value`, `encode_image(v, image, len, canonical_unordered = true)` | write the image (never allocates with the default; an unordered container above 64 entries allocates its sort index once unless `canonical_unordered` is off) |
| `decode_ctx`, `flat_copy`, `copy_flat`, `bools_canonical`, `monostate_index`, `decode_value`, `decode_image(out, image)` | bounds-checked decode (the slot rules and byte budget of `fmt::image_budget`, `bool` repair, the valueless-variant rule; the enum range check when `validate_read_enums` — `copy_flat` returns `flat_copy {bools, invalid}`); `decode_image` returns `status` |

## How it works

### Canonical names (§6.2b)

A canonical name (decision B, spec §0.000; review C-03/C-04/C-05/C-07) is never shared by two C++ types
in one program, and is the same on every compiler and standard library. `canonical_name<T>()` tries, in order:
1. **An explicit name:** `[[=es::serreg::name("…")]]` or `es::serreg::type_properties<T>::name()`, never both.
2. **A scalar:** its kind name.
3. **A type without a linkage-unique name** (an unnamed namespace, or local to a function or lambda): a
   `static_assert` asking for an explicit name (an implicit `{anon}::X` / `{fn:f}::X` could be produced by two
   `.cpp` files at once).
4. **Otherwise:** the scope walk, plus the template's canonical arguments.

The scope walk names an enclosing class through its own canonical name, which is what tells `Outer<int>::Inner`
from `Outer<long>::Inner` (they were both `{unnamed}::Inner`), and drops std's implementation namespaces:

```cpp
template<std::meta::info P> consteval std::string scope_of()
{
    if constexpr (P == ^^::)
        return {};
    else if constexpr (std::meta::is_type(P))
        return canonical_name<typename[:P:]>() + "::";
    else
    {
        std::string s = scope_of<std::meta::parent_of(P)>();
        if constexpr (!std_implementation_namespace(P))
        {
            s += std::meta::identifier_of(P);
            s += "::";
        }
        return s;
    }
```
(`reflect.hpp:733-748`.)

**The std vocabulary** is a set of `type_properties` specialisations (`reflect.hpp:902-1081`): `std::string` for
any allocator (and `std::basic_string<C>` of another character type), `std::vector<E>`, `std::array<E,N>`,
`std::optional<E>`, `std::variant<…>`, the six map/set containers (a comparator, hasher or equality other than the
default appended by `cmp_suffix`/`hash_suffix`, the allocator never), `std::deque<E>`, `std::list<E>`,
`std::forward_list<E>`, `std::multimap<K,V>`, `std::unordered_multimap<K,V>`, `std::stack<T>` (the underlying
container is not part of the name), `std::pair<A,B>`, `std::tuple<Ts…>` (`canonical_list`), `std::monostate`. So
`Box<std::string>` is `corpus::Box<std::string>` on libstdc++ (either ABI) and libc++ alike, and
`Box<std::pmr::string>` names the same type. **The vocabulary is closed:** a standard-library class outside it
(`in_std` and no `type_properties<U>::name()` — `std::priority_queue`, `std::bitset`, `std::chrono::…`, a smart
pointer) is rejected by `check_supported` with a message that lists the vocabulary and points at `as_flat<T>` or
`type_properties<T>` (`reflect.hpp:532-547`); a user type that specialises `type_properties` for a std class takes
responsibility for its layout. Names of std types that are *not* classes (an enum) use the walk, without
`__cxx11`/`__1`/`_V2`.

**Other rules:**
- User inline namespaces stay in names (`corpus::v1::InV1`).
- Names never use a compiler spelling: scalars are kind names (`corpus::Vec<i32,3>`, not `Vec<int, 3ul>`), and
  template-argument *values* go through `render_value_arg` (`t::W<i32,true>`, `t::W<i32,-5>`, `t::W<i32,enum(7)>`,
  cc F3).
- An unnamed type is a `static_assert`, and so is a recursive type (`recursive_type`, C-07).
- `as_flat` types take the *mapped* type's name (`corpus::Stamp` → `"corpus::StampFlat"`).

**Tests:**
- All names are frozen in `tests/golden/hashes.cpp` ("6.2b canonical names").
- The negative tests `neg_anon_without_name`, `neg_local_without_name`, `neg_recursive_type`,
  `neg_bad_explicit_name` and `neg_two_explicit_names` cover the rejections.
- `tests/unit/local_in_main.cpp` checks that a named class local to `main()` serialises. GCC 16 cannot take
  `parent_of(main)`, which is why `in_std` stops at a function.

### Canonical strings and the two hashes

Schema string (`schema = true`) and layout string (`schema = false`) are built by the same code with names
elided from the second:

```
struct<corpus::Order>[32,8]{id:u64@0[8,8];px:i64@8[8,8];qty:u32@16[4,4];side:enum<corpus::Side,u8>@20[1,1];sym:array<char,8>@21[8,1];}
struct[32,8]{u64@0[8,8];i64@8[8,8];u32@16[4,4];enum<u8>@20[1,1];array<char,8>@21[8,1];}
```
(derived from the rules below for `corpus::Order`; the golden test locks the resulting hash, and
`fingerprint64("struct<t::W<i32,-5>>[4,4]{v:i32@0[4,4];}") == schema_hash_v<t::WNeg>` locks the grammar itself). Rules, from `append_kindspec` / `append_dims` /
`struct_string` (`reflect.hpp:1573-1733`):

- prefix `struct` for flat classes, **`dyn`** for dynamic classes (a flat and a dynamic definition of one name
  never collide), `enum`; the name in `<…>` only in the schema string;
- `[size,align]`, plus `;s<since>;d<deprecated>` in the schema string only when either is set (FR-S11:
  annotations change the schema hash, never the layout hash);
- one item per base (`<base:Name>:` prefix in schema) then per kept member, `name:kindspec@offset[size,align];`;
- kindspec: scalar kind name; `enum<Name,u8>` / `enum<u8>`; `array<kindspec,N>`; `struct<0x<schema hash>>` /
  `struct<L:0x<layout hash>>` (nested types by *hash*, so a change deep inside propagates); `string`,
  `vector<E>`, `optional<E>`, `variant<E0|E1|…>`, `dyn<0x…>` / `dyn<L:0x…>`; **`map<K,V>`** and **`set<K>`** —
  the wire *shape*, whatever the C++ container: a `std::map<std::string,int>` member and a
  `std::unordered_map<std::string,int>` member both spell `map<string,i32>` and give equal schema and layout hashes
  (as `string` is one kindspec for every `std::basic_string<char>`); the container's name enters only the transparent
  type of a top-level argument (below). Locked by `tests/golden/hashes.cpp` "2.2.0 golden strings and hashes of the
  map/set shapes and the transparent types (format 1.6)". `std::deque<E>`, `std::list<E>`, `std::forward_list<E>`
  and `std::stack<E>` members spell `vector<E>` — the same shape, the same hashes as a `std::vector<E>` member;
  **`multimap<K,V>`** is the schema kindspec of a `std::multimap`/`std::unordered_multimap` member, and its layout
  kindspec is `vector<dyn<L:0x…>>` — the vector of `std::pair<K,V>` it is on the wire (`reflect.hpp:1602-1618`), so
  it is layout-compatible with a `std::vector<std::pair<K,V>>` member but a different schema. A `std::pair` or
  `std::tuple` member is a nested dynamic class, `dyn<0x…>` by hash, whose own string names the parts:
  `dyn<std::pair<i32,f64>>[16,8]{first:i32@0[4,4];second:f64@8[8,8];}`. Locked by `tests/golden/hashes.cpp`
  "golden strings of std::deque/list/forward_list, std::pair/tuple, multimaps and std::stack";
- enums: `enum<Name,u8>[1,1]{buy=1;sell=2;}` (enumerators in the schema string only). The kind inside `enum<…>`
  — here and in every `enum<Name,kind>` kindspec of a member — is `enum_underlying_kind<E>()` (decision
  D): the kind of the fixed underlying type when the enum has one, otherwise the unsigned kind of `sizeof(E)`,
  or the signed kind when an enumerator is negative. That is the choice GCC and Clang make for
  `std::underlying_type_t` (`enum Plain { a, b }` → `u32`, `enum Neg { m = -1 }` → `i32`, an enumerator above
  32 bits → `u64`), so no hash moved; a compiler that picks `int` for every such enum now produces the same
  strings. The same function fills `type_desc::underlying` (`make_type_desc_base`). Locked by
  `tests/golden/hashes.cpp` "2.1.16 decision D: an enum without a fixed underlying type takes its kind from its
  enumerators" (the derived kind equals the trait's kind for the test enums and `corpus::Venue` on GCC).

`schema_hash_of` / `layout_hash_of` are `fingerprint64` of these; `schema_hash_memo<T>` / `layout_hash_memo<T>` are
hidden `inline constexpr` variables so a type referenced from many places is stringified once per TU (cc F9).
Same layout, different member names → equal layout hash, different schema hash (`Point2i`/`Pair2i`). The strings
themselves are public as `schema_string_v<T>` / `layout_string_v<T>` (`define_static_string`: one copy per type per
executable, in `.rodata` only when something reads it): `tests/golden/hashes.cpp` asserts
`fingerprint64(schema_string_v<corpus::Order>) == schema_hash_v<corpus::Order>`, and a program can print the exact
text behind a hash it meets in a dictionary.

### Field construction

`field_for<M>` (`reflect.hpp:1142`) is the one place a member type becomes a `field_desc`: it calls
`check_supported<M>()`, rejects a nested `as_flat` type (a nested member of a mapped type would be copied
unconverted — provide `as_flat` for the enclosing type), sets `size = image_size<U>()` / `align = image_align<U>()`
(native for flat members, slot/fixed-part for dynamic ones), then per kind: scalar; `enum_` + `type_hash`;
`array_` + `extent` + `elem = &element_field_v<E>`; dyn member + `elem`/`extent`; `map_` with `extent` = the
entry stride and `elem = map_elems_v<U>` (key at 0, value at its canonical entry offset), `set_` with `extent` = the key
stride and `elem = &element_field_v<K>`; else `struct_` + `type_hash`.
`member_field` adds the part's name (`part_name`: the identifier, or `first`/`second`/`_N`), the offset and the
`since`/`deprecated` values and refuses bit-fields; `base_field` names the base `<base:Name>` and refuses virtual
bases. `make_fields` lays out bases first, then kept parts (`base_count`/`part_count`), with `field_count ≤ 65535`
asserted. Private members are visible because every `members<T>()` call uses `access_context::unchecked()`.

### Gap tables (FR-S6)

```cpp
// Coverage is collected as [offset, offset+len) intervals of leaf scalars (one interval per array whose element
// type has no internal padding), merged, and complemented over [0, sizeof(T)). No per-byte work, so large
// buffer members do not hit the constexpr operation limit.
```
(`reflect.hpp:1822`). `collect_leaves` recurses through bases, members, arrays and nested structs at *native*
offsets, pushing one interval per scalar/enum leaf (and one per array of unique-representation elements), and
records every `bool` leaf's offset. `compute_gaps` sorts, merges, and complements to `sizeof(T)`; skipped members
are not covered, so their bytes are gaps (scrubbed to zero). The result is frozen into `gaps_v<T>` / `bools_v<T>`
via the same two-pass size-then-copy pattern as the blobs. `Padded { u8; u64; u16 }` yields gaps `1+7` and `18+6`;
`Nested`'s gaps are flattened to 3 (`tests/golden/hashes.cpp` "FR-S6 padding gap tables").

### Dynamic types: detection

`is_dynamic<T>()` (`reflect.hpp:1322`): scalar/enum → no; array → its element; a dyn member type or a tuple-like
type → yes; a class → any base or kept part. `is_dynamic_class<T>()` narrows to classes that are encoded *as* an image (not a
`std::vector` itself, not an array). `dynamic_type<T>` is the public concept; `flat_type` is unchanged from 1.0.

### Dynamic types: the canonical fixed layout (plan v2.0 §2)

```cpp
// Fixed-part geometry of a dynamic class: canonical offsets (declaration order, bases first, natural
// alignment of the members' image sizes), not the native offsetof.
```
(`reflect.hpp:1409`). `image_size<M>()` is 8 for a dyn member (a reference slot), `N × stride` for an array of
dynamic elements, `fixed_layout_v<U>.size` for a nested dynamic class, `sizeof(U)` otherwise; `image_align<M>()`
correspondingly 4 / element align / `fixed_layout_v<U>.align` / `alignof(U)`. `compute_canonical_offsets<T>` places
bases then members at `align_up(cur, image_align)`; `compute_fixed_layout<T>` takes the end and the max alignment,
rounded up. `member_offset<T,I>()` returns these for dynamic classes and `offset_of(...).bytes` for flat ones, so
the *same* `field_for`/`struct_string` code produces descriptors and canonical strings for both kinds — with the
consequence that a dynamic type's `offset`s in the dictionary and in `dyn<Name>[20,4]{id:u32@0…;name:string@4[8,4];…}`
are image offsets, independent of the native ABI. `descriptor_v<DynMsg>.size == 20, align == 4`; `DynBase : Order`
puts the flat `Order` image at 0 and the `note` slot at 32 (golden "2.0 dynamic types"). A `std::pair<i32,f64>` is
laid out the same way — `first` at 0, `second` at 8, `[16,8]` — whatever the standard library's object layout, which
is why a tuple-like type is always an image and never a `memcpy`.

### Dynamic types: measure / encode / decode

Three walks that mirror each other exactly (`reflect.hpp:2278-2739`). Each dispatches on the member type: flat
leaf, array, dyn member, dynamic class.

**Measure** — `measure_image(v)` starts `tail = image_size<T>()` and adds every tail block: string bytes; for a
non-empty sequence (`sequence_of(v)`, `sequence_size`: a vector, deque, list, forward_list, multimap or a stack's
container) `align_up(tail, elem align) + n × stride` then the elements' own tails (an empty one adds
nothing — it is the slot `{0,0}`, exactly as encoded; G2-2); an engaged optional's element;
the active variant alternative; a non-empty map or set `align_up(tail, entry align) + n × entry stride`, then
the keys' and values' own tails (the order of the entries does not change the size). Nothing is written or allocated; `serialize` calls this once for
`frame_size(o, vs...)`.

**Encode** — `encode_image(v, image, len, canonical_unordered)` zeroes the whole measured image `[0, len)` (G2-2),
seeds an `image_cursor` with the flag (`serialize_options::canonical_unordered`, `true` by default), then
`encode_value` writes each member at its canonical offset; a dyn member gets a slot:

```cpp
else if constexpr (k == kind::vector_)
{
    using E = typename dyn_member<U>::elem;
    const auto& seq = sequence_of(v);
    const std::size_t n = sequence_size(seq);
    if (n == 0)
        put_slot(dst, 0, 0);
    else
    {
        const std::uint32_t off = c.alloc(n * image_stride<E>(), image_align<E>());
        put_slot(dst, off, static_cast<std::uint32_t>(n));
        std::size_t i = 0;
        for_each_in_wire_order<U>(seq, c.canonical_unordered, [&](const auto& e) {
            encode_value(static_cast<const E&>(e), c.base + off + i++ * image_stride<E>(),
                         c);
        });
    }
}
```
(`reflect.hpp:2423-2440`). The sequence is walked, never indexed, so a `std::list` or `std::forward_list` costs the
same as a vector; a `std::stack` is written bottom first (its container's order); a multimap's entries are
`std::pair<K,V>` values — `static_cast<const E&>` turns the `std::pair<const K,V>` the container holds into the
element type — in key order (`for_each_in_wire_order`: as iterated for a `std::less` multimap; for an unordered
one sorted with `std::sort` over `(entry pointer, original index)` slots, the index as tie-break — stable without
`std::stable_sort`'s buffer (review B1), so equal keys keep the container's order; with `c.canonical_unordered == false` the unordered
multimap too is written as iterated). `image_cursor::alloc` bumps `tail` with alignment; blocks are emitted depth-first in member
order, and because the image was zeroed first every alignment gap and every padding byte inside an element
block is zero whatever the sink's memory held (a reused `capture_file` buffer, the 4096-byte stack buffer of a
plain sink, an `es::ipc` ring record, caller memory behind a `span_sink`); `n == 0 ⇒ off == 0` (G2-2: zeroing only the fixed part would leave alignment gaps holding whatever the buffer
held). `alloc` narrows the offset to `u32`; that is safe
because `serialize_frame` refuses any frame whose length does not fit `u32` before encoding (G2-11). Slots: string `n` = byte
count; vector `n` = element count; optional `n` = 0/1; variant `n` = 0 (valueless) or `index + 1`. Flat leaves
inside the image go through `put_flat_image` — memcpy plus the same gap/bool scrub as the 1.0 path — so identical
values give identical bytes and every padding byte is zero. A C array of classes or bools is scrubbed
element by element (`std::array` is a class and has its own tables), and a lone `bool` is normalised on the copied byte (`v ? 1 : 0` on a bool holding 2..255
is undefined, and -O2 copied the byte). `encode_value` for a base uses a C-style cast to the
base reference; for a member it splices `v.[:members<U>()[I]:]`, which is why private members work and why the
walks live in this header.

**Maps and sets (FR-W9).** The `map_`/`set_` branch allocates one block of `n × entry stride` bytes at the
entry alignment, puts the slot, and writes the entries with `encode_entry<U>` — the key at the entry's start and,
for a map, the value at `container_entry_layout<U>().value_offset` — each key or value through `encode_value`
again, so a string key's bytes and a nested value's tails follow the whole block, entry by entry:

```cpp
const std::uint32_t off = c.alloc(v.size() * std::size_t{el.size}, el.align);
put_slot(dst, off, static_cast<std::uint32_t>(v.size()));
std::size_t i = 0;
using entry_t = typename U::value_type;
if constexpr (dyn_member<U>::ordered)
    for (const entry_t& e : v)
        encode_entry<U>(e, c.base + off + i++ * std::size_t{el.size}, c);
else if (!c.canonical_unordered) // serialize_options: the container's own order, no sort
    for (const entry_t& e : v)
        encode_entry<U>(e, c.base + off + i++ * std::size_t{el.size}, c);
else
    for_each_in_canonical_order<U>(v, [&](const entry_t& e) {
        encode_entry<U>(e, c.base + off + i++ * std::size_t{el.size}, c);
    });
```
(`reflect.hpp:2461-2475`.) The **canonical order** is ascending by the key: a container whose comparator is
`std::less` (`dyn_member<U>::ordered`) already iterates that way and is written as is; every other one — the
unordered containers, or a map ordered by `std::greater` — goes through `for_each_in_canonical_order`, which
sorts an index of pointers to the entries (64 on the stack, a `std::vector` above that: the one allocation a
`serialize` of an unordered container may make) with `key_less`: `std::strong_order` for a floating-point key
(IEEE totalOrder — a NaN key, which an unordered container admits, would break the strict weak order `std::sort`
needs; review B2; the golden `unordered_map<double, i32>` in `test_containers.cpp` freezes `-inf, +0, +inf, NaN`),
`operator<` when the key has one, else `memcmp` of the key's scrubbed flat image; `key_less` is `noexcept` only when
the key's `operator<` is. So the same values are the same bytes in a `std::map`, a `std::flat_map` and a
`std::unordered_map`, and `-1 < 4 < 5` for `int` keys even though the byte order would put `ff ff ff ff` last
(the golden `dynamic.ser`, chapter 7 §7.5b). `docs/deviations.md` "Containers" records why `operator<` and not
the byte order, and that a comparator other than `std::less` changes the canonical *name* of a transparent type
but never the order on the wire.

**The opt-out.** `serialize_options{.canonical_unordered = false}` makes the middle branch take the unordered
container as it iterates: no sort, no index, no allocation above 64 entries — the same value and the same length,
possibly different bytes for the same value in another process or after a rehash. The flag lives in the
`image_cursor` so that nested containers and multimaps (`for_each_in_wire_order`) see it too; it never reaches
`measure_image`, the hashes or the dictionary, and an ordered container is written the same way whatever it says.
The readers never required the order: `decode_image` inserts with `emplace_hint(end(), …)` (an exact hint for
canonical data, merely a wrong hint otherwise) and the dictionary walker only checks the slot, so both forms decode
through every reader ("FR-A6 serialize_options::canonical_unordered: sorted by default, iteration order on
request, readers take both", `tests/unit/test_containers.cpp`).

**Decode** — `decode_image(out, image)` walks the same shape with `depth` and bounds. The slot rules
are not written here: every slot goes through `fmt::image_budget::take` (`format.hpp`), the function the
dictionary-driven walker in `src/decoder.cpp` uses too (review G2-6), so `deserialize` and `tools::verify` accept
exactly the same images. `decode_ctx` is that budget plus two results:

```cpp
struct decode_ctx : fmt::image_budget
{
    bool invalid = false;
    std::uint32_t bools = 0;
    using fmt::image_budget::image_budget;
};
```

(`reflect.hpp:2525-2530`.) `take(slot, bytes)` requires `n == 0 ⇔ off == 0`, a non-empty block at or after the root's
fixed part and inside the image, and charges `bytes` to a budget that starts at the image size (writers allocate
disjoint tails, so an honest image never runs out; slots that share one tail cannot make a small image decode
into a huge value). A string charges `n`, a vector `n × stride` through `c.take_n(s, image_stride<E>())` (D-01:
a product that wraps 64 bits is refused; checked before `resize(n)`), an optional
(`n ∈ {0,1}`) and a variant alternative their image size; `depth > max_image_depth` fails. Any of these returns `false`, which
`decode_image` maps to `error::length_mismatch`.

A **sequence block** (`vector_`) is filled through `sequence_of(out)`: a container with `resize` (vector, deque,
list, forward_list, a stack's container) is sized and filled in order; a multimap is `clear`ed and every entry
decoded into a temporary `std::pair<K,V>` and `insert`ed at the end (`reflect.hpp:2627-2653`) — duplicate keys are
data there, never `invalid_value`. A **tuple-like** output is a dynamic class: `for_each_part` decodes
`first`/`second` or `_0`… through `get_part<I>` at their canonical offsets.

A **map or set block** is charged `n × entry stride` through `take_n`; the container is `clear`ed, a hash
container `reserve`s `n`, and every entry is decoded into temporaries (the key at the entry, a map's value at
`value_offset`) and inserted with `emplace_hint(end(), …)` — exact and O(1) per entry for data in canonical order,
merely a wrong hint otherwise. An insertion that does not grow the container is a **duplicate key** on the wire
(not canonical, or not a map): `c.invalid` is set, the first value is kept, and `decode_image` ends in
`error::invalid_value` after the rest of the image, like a valueless variant. Any container of the same shape
decodes any writer's bytes: an `std::unordered_map` output reads a `std::map` member's image and vice versa.

A **valueless variant** on the wire (`n == 0`, written for a variant that was `valueless_by_exception`) decodes
to `std::monostate` when the variant has that alternative (`monostate_index<V>()`, `reflect.hpp:2578`). Otherwise
the C++ type cannot hold "no value": the member is left as it was, `c.invalid` is set, decoding continues, and
`decode_image` returns `error::invalid_value` (maintainer decision, 2026-09-29; it never silently becomes
alternative 0).

**Flat leaves** are read with `copy_flat<U>` (`reflect.hpp:2541`): a `memcpy`, then every `bool` byte of `U`
(`bools_v<U>`, the same list the writer scrubs) that is not 0 or 1 is set to 1 and counted (review
G2-12: a `bool` holding `0x02` is an invalid value representation, undefined on the first read; GCC's
`-fsanitize=bool` traps). With the range check on (decision D, FR-D5; `ES_SERREG_VALIDATE_READ=1`,
`validate_read_enums`) the same pass checks every leaf in `enum_ranges_v<U>`
with `enum_leaf_in_range`: a leaf whose integer value is outside the range of its enum (an enum *without* a
fixed underlying type holds only the values of the minimal-width integer that represents its enumerators,
[dcl.enum]; reading another value is undefined behaviour) is set to 0 — always inside the range — and counted.
`copy_flat` returns `flat_copy {bools, invalid}`; `decode_value` adds `bools` to `c.bools` and sets `c.invalid`
when `invalid` is non-zero, so `decode_image` returns `error::invalid_value` for it exactly as for a valueless
variant, after the rest of the image was decoded. The flat (non-image) paths in `deserialize.hpp` call
`copy_flat` too and report the same way (`files/include-es-serreg-deserialize.hpp.md`). `decode_image` reports the
bool count through `report_warning(warning::noncanonical_bool, …)` — a no-op unless the application installed a
handler (`error.hpp`). `bools_canonical<U>` is the check without the copy — bool bytes *and* enum ranges — used
where a zero-copy view would expose the bytes (`view<T>`, `dispatch`). Enums with a fixed underlying type are
never in the table: every value of the underlying type is a value of the enum. Values inside the range that no
enumerator names (flag-style enums) are valid.

### Canonical names of templates

```cpp
else if constexpr (std::meta::has_template_arguments(r))
{
    std::string s = scope_of<std::meta::parent_of(r)>();
    s += std::meta::identifier_of(std::meta::template_of(r));
    s += '<';
    bool first = true;
    template for (constexpr std::meta::info a :
                  std::define_static_array(std::meta::template_arguments_of(r)))
    {
        if (!first)
            s += ',';
        first = false;
        if constexpr (std::meta::is_type(a))
            s += canonical_name<typename[:a:]>();
        else if constexpr (std::meta::is_value(a) || std::meta::is_object(a))
            s += render_value_arg([:a:]);
        else
            static_assert(std::meta::is_type(a), "es::serreg: template-template arguments "
                                                 "cannot appear in a canonical name");
    }
    s += '>';
    return s;
}
```
(`reflect.hpp:866-887`; `r` is `std::meta::dealias(^^U)`, line 823.) `dealias` first, so a type alias names the underlying specialisation; each type argument is
rendered recursively (`corpus::Vec<corpus::Side,3>`), each value argument by `render_value_arg` (which is why the
argument type must be bool/integral/character/enum). No spaces, no `ul` suffixes, no default-argument expansion
beyond what `template_arguments_of` reports.

### `check_supported<T>()` — the FR-S7 wall

```cpp
static_assert(
    !is_smart_pointer_v<U>, "es::serreg: smart pointers (std::unique_ptr, std::shared_ptr, std::weak_ptr) are not serialisable: ownership is not data; store the value, or std::optional<T> for a nullable one");
if constexpr (std::is_class_v<U>)
    static_assert(!in_std(std::meta::dealias(^^U)) || has_properties_name<U>, "es::serreg: this standard-library type is not in the serialisable vocabulary (std::string, std::vector, std::deque, std::list, std::forward_list, std::array, std::optional, std::variant, std::pair, std::tuple, std::map, std::unordered_map, std::flat_map, std::multimap, std::unordered_multimap, std::set, std::unordered_set, std::flat_set, std::stack); map it with as_flat<T> or give it a canonical name with es::serreg::type_properties<T>");
if constexpr (std::is_class_v<U>)
    static_assert(!recursive_type(^^U), "es::serreg: recursive types are not supported (…)");
static_assert(!std::is_pointer_v<U> && !std::is_member_pointer_v<U>, "es::serreg: pointers are not serialisable; provide as_flat<T> to map the enclosing type");
static_assert(!std::is_reference_v<U>, "es::serreg: references are not serialisable");
static_assert(!std::is_union_v<U>, "es::serreg: unions are not serialisable; use std::variant or as_flat<T>");
static_assert(!std::is_polymorphic_v<U>, "es::serreg: polymorphic types (virtual functions/bases) are not serialisable");
static_assert(std::is_class_v<U>, "es::serreg: unsupported member type");
static_assert(!is_string_view_v<U>, "es::serreg: std::string_view is not serialisable as a member (it could not be read back); use std::string");
static_assert(std::is_trivially_copyable_v<U> || is_dynamic<U>(), "es::serreg: non-trivially-copyable types are not serialisable unless they are std::string (any allocator), std::vector, std::optional, std::variant, std::map, … or a class built from those; use as_flat<T> for anything else");
```
(`reflect.hpp:532-572`, condensed). It is called from `field_for` (every member), `canonical_string` (every type) and
`registry.hpp::touch` (first thing), so the first diagnostic a user sees names the rule, with the offending
member visible in the instantiation backtrace (P2741 message interpolation is not used — deviations doc). The
dyn-member branch recurses into element types and rejects `std::vector<bool>` by name; it recurses
into a map's key and value (a set's key) and asserts that a container the writer must sort has an ordered or
trivially copyable key; the tuple-like branch recurses into every part and rejects `std::tuple<>`. The order of
the final block is deliberate: a smart pointer and a std class outside the vocabulary are named *before* the
generic messages, so `std::priority_queue<int>` fails on "not in the serialisable vocabulary"
(`tests/negative/neg_multimap_member.cpp`), `std::unique_ptr<int>` on "smart pointers …"
(`neg_smart_pointer_member.cpp`), `std::tuple<>` on "has no parts" (`neg_empty_tuple_member.cpp`).

The dyn-member branch (D-05) also calls `check_nonempty_element<E>()` for a vector
or optional element and for each variant alternative:

```cpp
template<class E> consteval void check_nonempty_element()
{
    static_assert(
        image_stride<E>() != 0,
        "es::serreg: a vector, optional or variant element must occupy bytes in the image; "
        "std::array<T, 0> of a dynamic T occupies none");
}
```
(`reflect.hpp:401-407`.) An element with no image bytes (`std::array<T, 0>` of a dynamic `T`) cannot be told apart
by position: the dictionary parser refuses such a FIELD, and the typed decoder accepted any count of them
(2^32-1 from a 16-byte image). The negative test `negative_neg_zero_size_element`
(`tests/negative/neg_zero_size_element.cpp`) checks the diagnostic.

### Annotations

```cpp
template<class A> consteval std::uint16_t annotation_value(const std::meta::info m)
{ // since/deprecated; 0 when absent
    for (const auto a : std::meta::annotations_of(m))
        if (std::meta::remove_cv(std::meta::type_of(a)) == ^^A)
            return std::meta::extract<A>(a).v;
    return 0;
}
```
(`reflect.hpp:658`). `[[=es::serreg::since(3)]]` on a member or `struct [[=es::serreg::since(1)]] Versioned` on
a type; `[[=es::serreg::skip]]` is detected by `has_annotation<skip_t>`. Values are `uint16_t` (§11 default) and
0 means unset; they reach `field_desc`/`type_desc`, the FIELD/ENTRY bytes, and the schema string (`;s3;d0`) but
never the layout string (`Versioned` and `Unversioned` share a layout hash).

### Worked example: `DynMsg { u32 id; std::string name; std::vector<i16> xs; }`

Compile time:

- `is_dynamic<DynMsg>()` = true (two dyn members); `flat_type<DynMsg>` = false; `dynamic_type<DynMsg>` = true.
- `canonical_offsets_v<DynMsg>` = {0, 4, 12}: `id` (4, align 4) at 0; `name` slot (8, align 4) at 4; `xs` slot at
  12. `fixed_layout_v` = {20, 4}.
- schema string `dyn<corpus::DynMsg>[20,4]{id:u32@0[4,4];name:string@4[8,4];xs:vector<i16>@12[8,4];}` →
  `schema_hash_v` = `0xb487de0c417a37e8` (frozen).
- `fields_v` = three `field_desc`: `u32@0`, `string_@4 size 8`, `vector_@12 size 8 elem → element_field_v<i16>`.
- `descriptor_v.size = 20, align = 4, is_flat = false`, no gap table.

Run time for `{7, "ab", {1, -2, 3}}`:

- `measure_image`: tail starts at 20; string adds 2 → 22; vector: `align_up(22, 2) + 3 × 2` = 28. Image = 28.
- `encode_image`: zero all 28 bytes; `id` → `put_flat_image` at 0; `name` → `alloc(2, 1)` = 20, copy `"ab"`,
  slot `{20, 2}` at 4; `xs` → `alloc(6, 2)` = 22, slot `{22, 3}` at 12, three `i16` at 22.
- `decode_image` (fixed part 20, budget 28): `read_slot` at 4 → `{20, 2}`, `take` ok (20 ≥ 20, 2 bytes fit;
  budget 26), `assign`; at 12 → `{22, 3}`, `take(6)` ok (budget 20), `resize(3)`, three flat decodes.

`tests/unit/test_dynamic.cpp` "2.0 §2 byte-exact canonical image of the plan example" asserts every one of
these offsets.

### Containers as arguments: the transparent type (FR-A6)

`serialize(out, "ids"_, ids)` with a `std::vector<int>` is a container argument:
`dynamic_type<T>` admits every dyn member kind but `string_` (`is_container_arg_v`), and such a `T` gets a
descriptor like any class: `canonical_string` goes to `wrapper_string<U>` — `dyn<` + the container's vocabulary
name (`std::vector<i32>`, `std::unordered_map<i32,std::string>`; a comparator, hasher or equality other than the
defaults is appended, the allocator never) + `>[8,4]{:` + the member kindspec + `@0[8,4];}` — and
`make_type_desc_base` fills `fields = wrapper_fields_v<U>` (one `field_for<U>({}, 0, 0, 0)`: unnamed, at 0) and
`transparent = true`, which `blob_bytes.hpp` writes as `struct_flag_transparent`. Size 8, align 4, not flat. The
image of the record is therefore the slot plus its tail — exactly what the container produces inside a one-member
struct — and `measure_image`/`encode_image`/`decode_image` need no new code: `image_size<U>()` of a dyn member is
already the slot. A top-level `std::string` stays the `string` argument category (`is_container_arg` excludes it),
and `std::array<E,N>` of a flat `E` stays a flat class. Frozen: `schema_hash_v<std::vector<std::int32_t>>` =
`0x02c87ef3a015eb45` = `fingerprint64("dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}")` (`tests/golden/hashes.cpp`).
A `std::deque<std::int32_t>` argument writes the same slot and block (`layout_hash_v` equal) under its own name
`std::deque<i32>` (`schema_hash_v` different); a `std::pair` or `std::tuple` argument is not a container argument
but a dynamic class of its own (`is_tuple_like_v`, no transparent flag): `dyn<std::pair<i32,f64>>`, the record
being its 16-byte image.

### Adding a supported member kind (checklist)

The `map`/`set` shapes followed this list exactly. A new dynamic kind touches, in this header alone:
`dyn_member` specialisation; `check_supported` (element recursion); `field_for` (kind, `elem`/`extent`);
`append_kindspec` (canonical spelling); `image_size`/`image_align` if the slot differs; `measure_value`,
`encode_value`, `decode_value` (one branch each, in lockstep); the `type_properties` vocabulary name (it becomes
the transparent type's name); `component_types` (the recursion check); and `registry.hpp::touch` (element
registration). A new container of an *existing* shape is much less: `std::deque`/`list`/`forward_list`/`stack`
and the multimaps are `dyn_member` specialisations with `k = vector_` plus a `type_properties` name, and the
walks reach them through `sequence_of`/`sequence_size`/`for_each_in_wire_order` and `resize`/`insert` detection
— the one shape-level addition was the `multimap<K,V>` schema kindspec. A new tuple-like class is an
`is_tuple_like_v` specialisation if it supports `std::tuple_size`/`std::get`.
Outside it: `descriptor.hpp` (`kind` value, `to_string`), `detail/blob_bytes.hpp::put_field` (`n_elem`), `src/dictionary.cpp`
(`parse_field` arity), `src/decoder.cpp`, `src/formatters.cpp`, `src/wireshark.cpp`, `src/cxxgen.cpp`,
`tools::decoded_value_visitor` (a defaulted event), golden hashes/blobs/fixtures, and a `format_minor` bump.

### `make_type_desc_base<T>()`

Assembles name, both hashes, `size`/`align` (`image_size`/`image_align`: `sizeof` for flat, fixed part for
dynamic), type-level `since`/`deprecated` (`annotation_value` on `dealias(^^U)`), `is_flat = !is_dynamic<U>()`,
then enum (`underlying`, `enumerators_v`, `enum_fixed = has_fixed_underlying_type_v<U>`) / container argument
(`wrapper_fields_v`, `transparent = true`) / dynamic struct (`fields_v`, no scrub tables) / flat struct
(`fields_v`, `gaps_v`, `bools_v`). `blob.hpp` adds the blob span.

## Maintaining it

- **Everything canonical is golden.** `tests/golden/hashes.cpp` freezes schema/layout hashes for ~35 corpus types
  (flat, templated, annotated, skipped, `as_flat`, anonymous-namespace, five dynamic types, `std::monostate`, the
  map/set shapes and transparent types, the sequence containers, `std::pair`/`tuple` and the multimaps, and the
  `schema_string_v`/`layout_string_v` relation),
  canonical names, descriptor tables, gap tables and frame sizes; `[golden-print]` prints refreeze values. A
  change to any string rule (`append_kindspec`, `append_dims`, the `dyn`/`struct` prefixes, `scope_prefix`
  markers, `render_value_arg`) or to the canonical layout rule is a wire-format change — bump, fixtures, CHANGELOG.
  Layout changes also break `tests/unit/test_dynamic.cpp` "2.0 §2 byte-exact canonical image".
- **Spellings.** The P2996 API used here is the GCC 16 one (`CLAUDE.md` lists them: `^^T`, `[: r :]`,
  `nonstatic_data_members_of(^^T, ctx)`, `bases_of`, `parent_of`, `identifier_of`, `has_identifier`, `type_of`,
  `offset_of(m).bytes`, `is_bit_field`, `enumerators_of`, `annotations_of`, `extract<A>`, `dealias`,
  `template_of`, `template_arguments_of`, `is_type`/`is_value`/`is_object`, `define_static_string`,
  `define_static_array`, `template for`). If one fails on a new compiler, check `<meta>` before guessing, prefer
  the index-sequence fallback, and add a line to `docs/deviations.md`. Cross-check on Compiler Explorer (gcc 16 /
  clang-p2996) per `docs/ce-links.md`.
- **Constant-evaluation limits.** Costs that scale: `is_dynamic<T>()` is evaluated many times per type (it is
  not memoised; it is cheap), canonical strings once per TU per type (memoised), gap tables are interval-based
  (a 1 MiB buffer member compiles in ~3 s), `enumerators_fit` uses `template for`. Deep nesting multiplies
  `image_size`/`image_align` recomputation — memoise via `fixed_layout_v` if a probe (`tests/probe/depth4.cpp`)
  regresses. `-fsanitize=null,nonnull-attribute,returns-nonnull-attribute` breaks `enum_string` constant evaluation
  on GCC 16.2 (deviations doc).
- **Keep the three walks in lockstep.** Any change to `measure_value` must be mirrored in `encode_value` and
  `decode_value` (same alignment, same stride, same slot semantics), and to `image_size`/`image_align`, and the
  dictionary side (`src/decoder.cpp` walks the same images from `dyn::type`). The golden fixture
  `tests/fixtures/golden/dynamic.ser` and "2.0 corrupt images are rejected, never over-read" are the guards.
- **`check_supported` messages are the FR-S7 contract**; `tests/negative/neg_*.cpp` (a `std::priority_queue` member
  — the vocabulary rule, `neg_multimap_member.cpp` by its historical name —, a smart pointer, `std::tuple<>`,
  string_view member, pointer member, nested `as_flat`, unnamed struct, enum > `int64`, a zero-size element)
  compile-fail on them. New rejections
  need a negative test; the control (`neg_control_compiles.cpp`) must keep compiling. Adding a std class to the
  vocabulary means a `type_properties` specialisation *and* a `dyn_member` or `is_tuple_like_v` mapping (or
  `as_flat`); the vocabulary message lists the members, keep it current.
- **`as_flat` is top-level only** (a `static_assert` in `field_for`); do not lift it without redesigning the
  single-memcpy flat path.
- `[[=skip]]` members are never instantiated (no `member_field`, no `touch`, no leaf coverage); `Skipped`'s
  golden gap table documents that their bytes are scrubbed.
- Tests: `tests/golden/hashes.cpp` (all sections), `tests/golden/blobs.cpp`, `tests/unit/test_roundtrip.cpp`
  (FR-S6, FR-S8, FR-S10), `tests/unit/test_skip.cpp`, `test_anon_tu.cpp`, `test_empty_member.cpp`,
  `test_dynamic.cpp` (every dynamic shape, empty/engaged forms, monostate, large vectors, corrupt images),
  `tests/unit/test_containers.cpp` (every container as a member, nested containers, byte-ordered keys,
  insertion-order independence, `map` ≡ `unordered_map` bytes, duplicate key → `invalid_value`, the transparent
  type end to end; "FR-A6 std::deque, std::list, std::forward_list and std::stack are the vector shape", "FR-A6
  std::pair and std::tuple are positional parts at canonical offsets, encoded through std::get", "FR-A6
  std::multimap and std::unordered_multimap are a vector of pairs sorted by key"), `tests/unit/test_validate_off.cpp`
  (the default policy), `tests/negative/*`, `tests/probe/depth4.cpp` (compile-time depth probe).
