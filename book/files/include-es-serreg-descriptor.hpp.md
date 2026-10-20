# `include/es/serreg/descriptor.hpp`

The compile-time type description data model (spec §6.1): the `kind` enumeration, `field_desc`, `type_desc`,
`enumerator_desc`, `gap`, and the helpers `kind_size`, `to_string(kind)`, `is_scalar_kind`. It is the contract
between the **compile-time reflection layer** (`reflect.hpp`/`blob.hpp` fill these structures) and the
**compiled runtime** (`libserreg` reads them through `registry`), so it has **no reflection dependency**: every
type here is a literal, structural aggregate that can be constant-initialised into `.rodata`. It also owns the
`ES_SERREG_HIDDEN` visibility macro that every per-image entity in the library is declared with.

## Depends on / used by

Includes: `<cstddef>`, `<cstdint>`, `<span>`, `<string_view>`.

Included by: `reflect.hpp` (builds `field_desc`/`type_desc`), `detail/blob_bytes.hpp` (serialises them into ENTRY blobs), `blob.hpp`,
`dictionary.hpp` (`dyn::field`/`dyn::type` mirror them with owned strings and reuse `kind`), `literal.hpp`
(only for `ES_SERREG_HIDDEN`), and through those by `registry.hpp`, `serialize.hpp` (`type_desc::padding_gaps`,
`bool_offsets` in `scrub_range`), `deserialize.hpp` (`record_view::desc`), `tools.hpp` (`kind` in the visitor).

## What it defines

### Macro

| name | meaning |
|---|---|
| `ES_SERREG_HIDDEN` | `[[gnu::visibility("hidden")]]`; applied to `descriptor_v`, `call_desc_v`, the blob arrays, the memo variables and the `registrar*` templates so that each image (executable or DSO) carries its own copy and none of them gets `STB_GNU_UNIQUE` binding (which would mark a DSO `DF_1_NODELETE` and make `dlclose` a silent no-op, so a reload would not run the new code) |

### Kinds

```cpp
enum class kind : std::uint8_t
{
    i8 = 1, i16, i32, i64, u8, u16, u32, u64, f32, f64,
    boolean, char_, char8, char16, char32, wchar, byte_,   // scalars: values 1…17
    enum_ = 32, array_, struct_,                            // v1 composites
    string_ = 48, vector_, optional_, variant_, literal_,   // v2 (dynamic) kinds
    map_,     // 1.6 (2.2.0): sorted (key, value) entries — std::map, std::unordered_map, std::flat_map, …
    set_      // 1.6 (2.2.0): sorted keys — std::set, std::unordered_set, std::flat_set, …
};
```
(`descriptor.hpp:18-47`, condensed). The value ranges are deliberate: `is_scalar_kind(k)` is `k < 32`; the gaps
18…31, 35…47 and above 54 are rejected by the dictionary parser as unknown. `literal_` (52) is reserved (no FIELD
of that kind is ever emitted; literals are top-level arguments, not members); `map_` is 53 and `set_` 54 (format
1.6).

| function | meaning |
|---|---|
| `kind_size(kind)` | native byte size of a scalar kind (1, 2, 4 or 8), 0 for anything non-scalar; used by `frame_view::compact_size` for scalar records and by the decoder |
| `to_string(kind)` | the canonical kind name: `i8 … u64 f32 f64 bool char char8 char16 char32 wchar byte enum array struct string vector optional variant literal map set`, `"?"` otherwise. These names are what appear in canonical strings and in the call string for scalar arguments (`i32`), so they are hash-relevant |
| `is_scalar_kind(kind)` | `static_cast<uint8_t>(k) < 32` |

### Structures

| name | fields | meaning |
|---|---|---|
| `gap` | `uint32_t offset, len` | one padding range inside a native flat object (FR-S6 scrub table) |
| `field_desc` | `string_view name; uint32_t offset, size; uint16_t align; kind k; uint16_t since, deprecated; uint64_t type_hash; uint32_t extent; const field_desc* elem` | one member, base pseudo-field (`"<base:Name>"`) or element descriptor (a map's key and value, a set's key) |
| `enumerator_desc` | `string_view name; int64_t value` | one enumerator |
| `type_desc` | `string_view name; uint64_t schema_hash, layout_hash; uint32_t size, align; kind k, underlying; bool is_flat, transparent, enum_fixed; uint16_t since, deprecated; span<const field_desc> fields; span<const enumerator_desc> enumerators; span<const gap> padding_gaps; span<const uint32_t> bool_offsets; span<const byte> blob` | one struct or enum type; `transparent` marks the synthetic type of a container argument (`struct_flag_transparent`), `enum_fixed` an enum with a fixed underlying type (`enum_flag_fixed`) |

## How it works

### `field_desc` is overloaded by kind

The same 48-byte record describes every shape; which fields are meaningful depends on `k`:

| `k` | `offset` | `size` / `align` | `type_hash` | `extent` | `elem` |
|---|---|---|---|---|---|
| scalar | native offset (flat) or canonical image offset (dynamic class) | `sizeof`/`alignof` | 0 | 0 | null |
| `enum_` | as above | `sizeof`/`alignof` of the enum | the enum's schema hash | 0 | null |
| `struct_` | as above | native size (flat) or fixed-part size (dynamic) | nested schema hash | 0 | null |
| `array_` | as above | whole array (`N × stride`) | 0 | element count `N` | one element descriptor (name empty) |
| `string_` | canonical offset | 8 / 4 (reference slot) | 0 | 0 | null |
| `vector_`, `optional_` | canonical offset | 8 / 4 | 0 | 0 | one element descriptor |
| `variant_` | canonical offset | 8 / 4 | 0 | alternative count | `alternatives[extent]` |
| `map_` | canonical offset | 8 / 4 | 0 | the entry stride (`detail::map_entry_layout<K,V>().size`) | `elem[0]` the key (offset 0), `elem[1]` the value at its canonical entry offset |
| `set_` | canonical offset | 8 / 4 | 0 | the key stride | one key descriptor (offset 0) |

`elem` points into `.rodata`: for arrays, vector/optional and a set's key it is `&detail::element_field_v<E>`, for variants it
is `detail::variant_alts_v<V>.data()`, for a map `detail::map_elems_v<M>.data()` (all in `reflect.hpp`). A `map_`/`set_`
element descriptor's `offset` is the offset *inside one entry* — the only place a FIELD's `offset` is not an offset
inside the owner's image. Element descriptors are recursive (`int a[2][3]` is
`array_ → array_ → i32`), which is why `detail/blob_bytes.hpp::put_field` and `src/dictionary.cpp::parse_field` are recursive
too.

### `type_desc`

`k` is `struct_` or `enum_` only; `underlying` and `enum_fixed` are meaningful for enums; `is_flat` distinguishes a memcpy-able
native image from a dynamic type whose `size`/`align` describe the *fixed part* of its canonical image; `transparent` marks the synthetic `struct_` of a container passed as an argument — one unnamed
field at 0, `size` 8, `align` 4, `is_flat` false — which renderers show as its value, not as a struct. The three scrub spans are empty for dynamic types (the image encoder writes every byte itself) and
for enums. `blob` is the descriptor's own ENTRY bytes — the span is set by `blob.hpp::make_type_desc`, the base
fields by `reflect.hpp::make_type_desc_base`:

```cpp
template<class T> consteval type_desc make_type_desc()
{
    embed_type_blob<T>(); // this TU's section copy (embed_elf.hpp), or nothing
    type_desc d = make_type_desc_base<T>();
    d.blob = type_blob_bytes_v<T>;
    return d;
}
```
(`blob.hpp:29`). Every `string_view` in these structures points at a `std::define_static_string` result or a
string literal, every span at an `inline constexpr` variable template — nothing is owned, nothing needs a
destructor, and `descriptor_v<T>` is therefore a `constexpr` object in `.rodata` with hidden visibility.

### Worked example: `descriptor_v<corpus::Order>`

`struct Order { std::uint64_t id; std::int64_t px; std::uint32_t qty; Side side; char sym[8]; }` (32 bytes,
align 8; `tests/golden/corpus.hpp:162`; `Side` is `enum class : uint8_t`, `corpus.hpp:65`). The values below are what the golden
tests assert or what follows from the layout:

| `type_desc` member | value |
|---|---|
| `name` | `"corpus::Order"` |
| `schema_hash` / `layout_hash` | `0x367f8cd8c6441dd2` / `0x0661bb3e388c01fe` |
| `size` / `align` | 32 / 8 |
| `k` / `underlying` / `is_flat` | `struct_` / unused / true |
| `fields` | 5 entries, see below |
| `padding_gaps` | `{29, 3}` (tail padding after `sym`, which ends at 29) |
| `bool_offsets` | empty |
| `blob` | 264 bytes, hash `0x8bb3bcf5a04fdc0c` |

| field | `k` | `offset` | `size` | `align` | `type_hash` | `extent` | `elem` |
|---|---|---|---|---|---|---|---|
| `id` | `u64` | 0 | 8 | 8 | 0 | 0 | — |
| `px` | `i64` | 8 | 8 | 8 | 0 | 0 | — |
| `qty` | `u32` | 16 | 4 | 4 | 0 | 0 | — |
| `side` | `enum_` | 20 | 1 | 1 | `schema_hash_v<Side>` | 0 | — |
| `sym` | `array_` | 21 | 8 | 1 | 0 | 8 | → `{k = char_, size 1}` |

And the dynamic counterpart `descriptor_v<corpus::DynMsg>`: `size 20, align 4, is_flat false`, fields `id:u32@0`,
`name:string_@4 [8,4]`, `xs:vector_@12 [8,4] elem → {k = i16}`, no scrub tables — offsets are canonical image
offsets, not `offsetof`.

### `kind_size` and record walking

`kind_size` is the reader's only source of a scalar record's length in a compact frame
(`frame_view::compact_size` → `kind_size(res_.arg_scalar_kind(i))`) and of a scalar leaf's length in the
descriptor-driven decoder; `is_scalar_kind` gates the leaf case in `src/decoder.cpp`. Both are `constexpr` and
branch on the enumerator value, so a new scalar kind that is not added to `kind_size` decodes as length 0 and
fails verification — a loud failure, by design.

### Why hidden visibility lives here

The macro sits in the lowest header so that `literal.hpp` (which does not include `blob.hpp`) can apply it to
`literal_blob_bytes_v<S>`. The comment above `ES_SERREG_HIDDEN` states the two reasons (`descriptor.hpp:10-13`): no `STB_GNU_UNIQUE` (glibc
would then refuse to unload the DSO, and a reloaded plugin would not run its new code), and one copy per image,
which is the unit of the registry's per-image reference counting (one reference per image on the heap entry,
`registry_entry::refs`, and `image_base`).

## Maintaining it

- **This header defines wire semantics indirectly.** `kind` values and `to_string(kind)` names appear in every
  FIELD/ENTRY blob (`detail/blob_bytes.hpp`), every canonical string (`reflect.hpp`) and therefore every schema/layout hash. Any
  change to a value, a name, or the scalar/non-scalar boundary at 32 is a format change: `tests/golden/hashes.cpp`,
  `tests/golden/blobs.cpp`, the fixtures under `tests/fixtures/golden*/` and `tests/canary/rri_canary.cpp` all fail,
  and `format_minor` (or major) must be bumped with a `CHANGELOG.md` entry.
- **Adding a kind** (the way `string_`…`variant_` and later `map_`/`set_` were added): pick the next value in the 48+ range, extend
  `to_string`, `detail/blob_bytes.hpp::put_field` (`n_elem` rule), `src/dictionary.cpp::parse_field` (arity/size validation —
  unknown values are rejected, so an old reader refuses the new dictionary with `corrupt_dictionary`, which is the
  intended behaviour), `src/decoder.cpp` and the text/JSON formatters, and the `kind_size` switch if it is a scalar.
- **Field order of `field_desc`/`type_desc` is not wire order** (the blob layout is `format.hpp`'s), so reordering
  members for packing is allowed but pointless; the structs are constant-initialised once per type.
- `since`/`deprecated` are `uint16_t` by spec §11 default; do not widen them without touching FIELD/ENTRY.
- **Aggregate initialisation.** `reflect.hpp` creates `field_desc f{}` and assigns members by name, `registry.hpp`
  initialises `reg_node` positionally — adding a member to `type_desc` is safe, adding one to `field_desc` is safe,
  but keep the defaults zero so the value-initialised `{}` remains meaningful.
- Tests: `tests/golden/hashes.cpp` "6.1 descriptor field tables" (field counts, offsets, kinds, `elem` chains,
  `extent`, `since`/`deprecated`, private members, `[[no_unique_address]]`), "FR-S6 padding gap tables"
  (`padding_gaps`), "2.0 dynamic types: canonical strings, hashes, fixed layouts" (`is_flat`, slot sizes,
  `variant_` extent), "2.2.0 golden strings and hashes of the map/set shapes and the transparent types (format
  1.6)" (`map_entry_layout`, `transparent`); `tests/golden/blobs.cpp` for the byte encoding; `tests/unit/test_tools.cpp` and the `fuzz_rri`
  target for the parser side.
