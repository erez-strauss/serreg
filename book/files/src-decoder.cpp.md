# `src/decoder.cpp`

The descriptor-driven decoder: `tools::visit_record()` from `include/es/serreg/tools.hpp` (spec §6.4 style 4,
§6.1b flatten option, FR-D5). Given one record payload, its `dyn::type`, the dictionary and a
`decoded_value_visitor`, it walks the bytes exactly as the descriptor says and fires visitor events; it is the
one decoder behind `tools::verify`, `tools::dump`, `render_frame`'s struct rendering and `test_dynamic`'s
negative cases. It allocates nothing except in `alt_text()` (the variant alternative label, a small string
built once per variant value) and never reads a byte it has not first checked against the payload or, for a
v2 dynamic member, against the record's image. It never consults the registry or any compile-time
`type_desc` — only the `dyn::` descriptors — so it works on captures from other programs.

**`take_struct` and `native`.** `walk_struct` asks the visitor `take_struct(field, type,
value_location{bytes, image, image_offset, native})` before emitting `begin_struct`. If the visitor returns
true, the value is considered handled and none of its fields are walked. `native` travels down every recursion:
it is `t.is_flat` of the root record. For a flat record the bytes are the native object representation, so a
compiled type can be `memcpy`'d from them. For a dynamic record, every nested value is a position in the
canonical image, `image_offset = bytes.data() - image.data()`, and is decoded with
`serreg::detail::decode_value`. `tools::walk_values` (`walk.hpp`) is the one user of the hook; the default
implementation returns false (walk everything).

**One set of slot rules (review G2-6).** The slot checks are not written here: `walk_value` calls
`fmt::image_budget::take` (`format.hpp`), the same function the typed decoder in `reflect.hpp` calls, so
`tools::verify` and `deserialize` accept exactly the same images (two copies of the rules once disagreed on an
empty vector whose `off` pointed past the image), and slots that share one tail are limited. The walk state is a `walk_ctx`: the image, its root's fixed size, the remaining byte
budget, and `native`. `take_struct` receives a pointer to that budget in `value_location::budget`, so a value
decoded by `walk_values` charges the same budget. A `bool` byte other than 0/1 is reported to the visitor
(`noncanonical_bool`) and walked as 1 (G2-12).

**Overflow, fan-out and depth (D-01, D-02, D-10).** A vector's block is charged
through `fmt::image_budget::take_n`, which refuses an `n × stride` that wraps 64 bits (D-01: a forged element size
and alignment can make the product wrap to a small block).
`walk_ctx` carries a step budget: every call of `walk_value` and `walk_struct` spends one step, and a walk that
runs out returns `length_mismatch` (D-02). The limit is `max_depth() = 2 × min(decode_limits::image_depth,
fmt::max_nesting)` — twice the typed decoder's, because a struct level costs two units here (D-10);
`decode_limits::image_depth` (64 by default, FR-D5) narrows it per process, `fmt::max_nesting` is the ceiling.
This file also holds the process-wide warning handler
(`set_warning_handler`, `detail::report_warning`, declared in `error.hpp`).

**Maps, sets and transparent types (format 1.6; FR-W9, FR-A6).** `walk_value` has a `map_`/`set_`
case: the slot's block is `n` entries of `f.extent` bytes (charged through `take_n`), the key walked at each
entry's start and a map's value at `elem[1].offset`, under the new visitor events `begin_map`/`begin_entry`/
`end_entry`/`end_map` and `begin_set`/`end_set`. `walk_struct` has a transparent branch: a `dyn::type` with
`transparent` set — a container passed as an argument — walks its one unnamed field directly between
`begin_transparent`/`end_transparent`, so renderers show `[3, 1, 2]` and not a struct around it. All eight events
have defaults in `tools.hpp` expressed through the older events.

## Depends on / used by

Includes: `internal.hpp` (`dec`, `hex64`), `es/serreg/tools.hpp` (`decoded_value_visitor`, `visit_record`
declaration). From `dictionary.hpp`: `dyn::type`, `dyn::field`, `dyn::dictionary::find_type`. From
`descriptor.hpp`: `kind`, `is_scalar_kind`. From `error.hpp`: `result`, `error`.

Used by: `src/reader.cpp` (`tools::verify` through `counting_visitor`), `src/formatters.cpp` (`dump`'s
`text_visitor`/`json_visitor`, `struct_inline_text` for print arguments), `tests/unit/test_dynamic.cpp`
("2.0 corrupt images are rejected, never over-read"), `test_tools.cpp` (the decode-error-is-a-verify-problem
case), the golden fixture renderings, `fuzz_capture`.

## Structure

- **`max_depth()`** (`decoder.cpp:21-26`; 128 by default): recursion bound against
  self-referential (corrupt) dictionaries — `2 × min(get_decode_limits().image_depth, fmt::max_nesting)`
  (FR-D5).
- **`read_enum_value()`** (`decoder.cpp:28-74`): 1/2/4/8-byte load with sign extension for signed underlying
  kinds.
- **`emit_enum()`** (`decoder.cpp:76-90`): value → enumerator name lookup → `visitor.enumerator(...)`.
- **`walk_struct()` forward declaration** (`decoder.cpp:92-96`).
- **v2 helpers** (`decoder.cpp:98-134`): `alt_text()` (`variant<i:kind>`), `walk_ctx` (image, fixed size
  and byte budget as one `fmt::image_budget`, plus `native`; the step budget `steps` and `step()`),
  `stride_of()` (`fmt::slot_stride` of an element).
- **`walk_value()`** (`decoder.cpp:136-330`): one value of one field: scalar (with the `bool` repair), enum,
  array, struct (with flatten), string, vector, map and set (1.6), optional, variant.
- **`walk_struct()`** (`decoder.cpp:332-383`): one struct or enum type over a payload; 1.6: the transparent
  branch.
- **`tools::visit_record()`** (`decoder.cpp:387-398`): the entry point; the payload is also the image.
- **Warnings** (`decoder.cpp:401-421`): `g_warning_handler` (a `constinit std::atomic`), `set_warning_handler`,
  `detail::report_warning` (cold; a no-op without a handler).

## How it works

### `walk_struct`: the payload bound

```cpp
if (payload.size() < t.size)
    return std::unexpected(error::length_mismatch);
v.begin_struct(t.name, field_name);
for (const auto& f : t.fields)
{
    if (static_cast<std::uint64_t>(f.offset) + f.size > payload.size())
        return std::unexpected(error::length_mismatch);
    if (const auto r = walk_value(payload.subspan(f.offset, f.size), f, d, v, f.name, flatten,
                                  depth + 1, c);
        !r)
        return r;
}
v.end_struct();
```

(`decoder.cpp:349-382`.) A payload shorter than the type is refused before any event; each field's window is
re-checked against the *actual* payload (not just the descriptor's `size`, which `dictionary.cpp` already
validated) in 64-bit arithmetic. An enum type used as a record is handled here too (`t.is_enum`), accepting
only 1/2/4/8-byte payloads. A **transparent** type (1.6) takes a branch of its own after `take_struct`:

```cpp
if (t.transparent)
{ // 1.6: a container argument — the record *is* the container; renderers show the value alone
    if (t.fields.size() != 1 || t.fields[0].size > payload.size())
        return std::unexpected(error::corrupt_dictionary);
    v.begin_transparent(t.name, field_name);
    if (const auto r = walk_value(payload.first(t.fields[0].size), t.fields[0], d, v, field_name,
                                  flatten, depth + 1, c);
        !r)
        return r;
    v.end_transparent();
    return {};
}
```
(`decoder.cpp:359-370`.) The one field — the slot at 0 — is walked under the record's own field name (the
argument's name in a print frame, empty otherwise), so the vector, map or optional appears where a struct would
have; `take_struct` still runs first, so `walk_values<std::vector<int>>` can claim the whole record. A failure after `begin_struct` leaves the visitor without its `end_struct`; callers
treat any error as "this record's rendering is void" (`dump` prints `<decode error>` and moves on).

### `walk_value`: scalars, enums, arrays, structs

`walk_value()` (`decoder.cpp:136-330`) receives exactly the bytes of one value (`f.offset` already applied).
Scalars go straight to `v.scalar(name, f.k, b)` — the visitor decides how to render the bytes. An `enum_`
field must be 1/2/4/8 bytes; `emit_enum` sign-extends by the enum type's `underlying` kind and reports the
matching enumerator name (empty when unknown; the visitor prints `Type(7)`).

```cpp
case kind::array_: {
    if (f.elem.size() != 1)
        return std::unexpected(error::corrupt_dictionary);
    const dyn::field& e = f.elem[0];
    if (e.size == 0)
    {
        if (f.extent != 0)
            return std::unexpected(error::length_mismatch);
    }
    else if (f.extent > b.size() / e.size)
        return std::unexpected(error::length_mismatch);
    v.begin_array(name, f.extent);
    // v2: dynamic elements are laid out at their stride
    const std::uint64_t stride = stride_of(e);
    for (std::uint32_t i = 0; i < f.extent; ++i)
    {
        const std::uint64_t at = std::uint64_t{i} * stride;
        if (at > b.size() || e.size > b.size() - at)
            return std::unexpected(error::length_mismatch);
```

(`decoder.cpp:167-195`.) The array guard is the cc F2 / gk 1.15 bomb fix: a zero-size element with a non-zero
extent would loop `extent` times over nothing, and the element count can never exceed `b.size() / e.size`.
Elements are placed at their **stride** (v2: a dynamic element's fixed part may need padding to its alignment),
and each element window is still bounds-checked. **All stride and offset arithmetic is 64-bit**: an
earlier `stride_of` returned `u32`, so a hostile dictionary with element size `0xFFFFFFFF` and alignment 2
wrapped the stride to 0, the element check passed, and `subspan(off, 0xFFFFFFFF)` read out of bounds — found by
`serreg_fuzz capture` under `-D_GLIBCXX_ASSERTIONS` and locked by the `[fuzz]` regression test in
`tests/unit/test_dynamic.cpp`. Nested structs (`kind::struct_`) look up `f.type_hash`
(`unknown_type` if absent) and recurse through `walk_struct` with `depth + 1`.

### Flatten (§6.1b option d)

```cpp
if (flatten && name.starts_with("<base:"))
{ // §6.1b: present base members inline
    for (const auto& nf : nested->fields)
    {
        if (static_cast<std::uint64_t>(nf.offset) + nf.size > b.size())
            return std::unexpected(error::length_mismatch);
        if (const auto r = walk_value(b.subspan(nf.offset, nf.size), nf, d, v, nf.name,
                                      flatten, depth + 1, c);
            !r)
            return r;
    }
    return {};
}
```

(`decoder.cpp:200-212`.) Base classes are encoded as pseudo-fields named `<base:Name>`. With
`dump_options::flatten` (`--flatten`) their members are emitted directly into the parent struct instead of as
a nested struct — no `begin_struct` for the base. Only the name prefix triggers it; a member that is itself a
struct is never flattened.

### v2 dynamic members: image-relative reference slots

A `string_`, `vector_`, `optional_`, `variant_`, `map_` or `set_` field occupies 8 bytes in the fixed part: `{u32 off, u32 n}`,
where `off` is relative to the **image** — the whole record payload, which `visit_record` puts in the `walk_ctx`
(`decoder.cpp:387-398`: "a dynamic type's payload is its image"). Every slot goes through one check,
shared with the typed decoder:

```cpp
constexpr bool take(const slot s, const std::uint64_t bytes) noexcept
{
    if (s.n == 0)
        return s.off == 0;
    if (s.off < fixed || s.off > image.size() || bytes > image.size() - s.off || bytes > left)
        return false;
    left -= bytes;
    return true;
}
```

(`format.hpp:329-337`, `fmt::image_budget`.) `n == 0` needs `off == 0` (what every writer puts for an empty
string or vector, `nullopt` and a valueless variant); a non-empty block must start at or after the root's fixed
part, lie inside the image (`off ≤ size` first, then `bytes ≤ size - off`, so nothing overflows), and fit in the
budget. The budget starts at the image size and every block is charged: writers allocate disjoint tails, so an
honest image never runs out, while slots that share one tail (G2-6's amplification: N string slots over one
M-byte tail would make `dump` walk N·M bytes) fail with `length_mismatch` once they have referenced more bytes than
the image holds.

- **string** (`decoder.cpp:216-222`): `take(s, n)`; `v.string(name, view)`.
- **vector** (`decoder.cpp:223-246`): `take_n(s, stride)`, which multiplies in `uint64_t` and refuses a product
  that wraps (D-01); a zero-size element with `n > 0` is refused; `begin_vector` / elements via `walk_value` on `image.subspan(off + i·stride, e.size)` / `end_vector`.
  An empty vector obeys the same `off == 0` rule as the typed decoder.
- **map, set** (`decoder.cpp:247-288`, 1.6): `f.elem.size()` must be 2 (map) or 1 (set) and `f.extent` — the
  entry stride — non-zero (else `corrupt_dictionary`); `take_n(s, f.extent)` charges the whole block; then per
  entry `i` at `off + i × extent`: `begin_entry(i)`, the key walked as `"key"` from the entry's start and the
  value as `"value"` from `elem[1].offset`, `end_entry()` — for a set just the key, unnamed — between
  `begin_map(name, n)`/`end_map()` or `begin_set(name, n)`/`end_set()`. The entries are shown in wire order;
  the walker does not check that the keys are sorted or unique (the typed decoder reports a duplicate as
  `invalid_value`; here it is two entries). A key or value that is itself dynamic resolves its slots against the
  same image, like a vector element.
- **optional** (`decoder.cpp:289-302`): `n` must be 0 or 1; `take(s, e.size)`; `n == 0` → `v.null_value(name)`,
  `n == 1` → the one element at `off`.
- **variant** (`decoder.cpp:303-328`): `f.elem.size()` must equal `f.extent` (else `corrupt_dictionary`);
  `n ≤ extent`; `n == 0` → `null_value` (valueless: the image is well-formed, so the walker shows it; the typed
  decoder picks `std::monostate` or reports `invalid_value`, chapter 8); otherwise `take(s, alternative size)`,
  `begin_variant(name, n-1, alt_text(...))`, the value walked under the field name `"value"`, `end_variant`.

Every recursion passes the same `image`, so a nested dynamic struct inside a vector element still resolves its
own slots against the record's image, exactly as `docs/plans-for-v2.0.md` §2 defines (all offsets relative to the
image start). The default implementations of the dynamic visitor methods in `tools.hpp` express these events through
the flat ones (a string as an array of chars, `null` as a literal, a variant as a struct), so a visitor written
against 1.x keeps working.

### Strides

```cpp
constexpr std::uint64_t slot_stride(const std::uint64_t size, const std::uint64_t align) noexcept
{
    const std::uint64_t a = align ? align : 1;
    return (size + a - 1) / a * a;
}
```

(`format.hpp:311-315`, `fmt::slot_stride`, which `stride_of` calls; 64-bit — a `u32` version would wrap for sizes
near 2³².) The encoder lays vector elements and dynamic array elements at `align_up(size, align)`; for a flat
element `size` is already a multiple of `align`, so the stride equals the size and a flat array's layout is
unchanged.

### Depth limit, step budget and the enum rendering

`max_depth()` is checked at the top of both `walk_value` and `walk_struct`; exceeding it is
`error::unsupported`. It protects against a dictionary in which a struct (transitively) contains itself — the
dictionary parser cannot detect that because type references are by hash. A struct level costs two units (the
member in `walk_value`, then the struct in `walk_struct`), so the limit (D-10) is
`2 * fmt::max_nesting`: a type nested as deep as the typed decoder accepts (`fmt::max_nesting` levels) decodes in
`srdump` too. `decode_limits::image_depth` (FR-D5) can narrow it per process: the limit is
`2 × min(image_depth, fmt::max_nesting)`, read from `get_decode_limits()` at each check (the typed decoder's
`max_image_depth` stays the compile-time constant).

The depth limit does not bound the work: a dictionary whose struct fields overlap can make a tiny record fan out
into K^L visits. `walk_ctx` therefore holds a step budget (D-02):

```cpp
std::uint64_t steps;
bool step() noexcept
{
    if (steps == 0)
        return false;
    --steps;
    return true;
}
```
(`decoder.cpp:120-127`.) `walk_value` and `walk_struct` call `c.step()` right after the depth check and return
`error::length_mismatch` when it fails. `visit_record` starts the budget at `(2·max_depth + 2)·(payload + 1) +
65536`: every leaf of an honest record consumes at least one byte and sits at most `max_depth` units deep (one
visit per unit), and the constant leaves room for empty members, so no honest record runs out. `emit_enum` passes `enum_type` as
the type name (empty when the enum's `type_hash` is not in the dictionary; the text visitor then prints
`enum(…)`) and the value as `int64_t`, sign-extended by `underlying`.

### `visit_record`

```cpp
result<void> visit_record(const std::span<const std::byte> payload, const dyn::type& t,
                          const dyn::dictionary& d, decoded_value_visitor& v,
                          const std::string_view field_name, const bool flatten)
{
    // a dynamic type's payload is its image (its fixed part is t.size bytes; every tail follows it); a flat type's
    // payload is its native object representation and has no slots
    walk_ctx c{fmt::image_budget(payload, t.size), t.is_flat,
               static_cast<std::uint64_t>(2 * max_depth() + 2) *
                       (std::uint64_t{payload.size()} + 1) +
                   65536};
    return walk_struct(payload, t, d, v, field_name, flatten, 0, c);
}
```

(`decoder.cpp:387-398`.) `payload` is the record with any wire prefix already stripped by `reader.cpp` (the
`u32 len` of a string record, the `u32 len, u32 0` of a dynamic record), so for a dynamic type the image
*is* the payload and slot offsets are payload-relative. The third member of `walk_ctx` is the step budget
(D-02).

## Maintaining it

- **Tests:** `test_dynamic.cpp` ("2.0 corrupt images are rejected, never over-read" — slots past the image,
  `n × stride` overflow, bad optional/variant `n`; "2.0 tools: dump text/JSON of dynamic values"; the
  `[g2-6]` cases, which check that `deserialize` and `verify` reject the same images, and `[g2-12]`),
  `test_containers.cpp` (`srdump` text and JSON of map, set and transparent records, `inspect`,
  `dispatch` and `frame_router` over a container argument),
  `test_tools.cpp` ("a decode error inside a record body is a verify problem"), `runtime_selftest.cpp` with
  `tests/fixtures/negative/bomb_array_extent.ser`, the golden `orders.*`/`dynamic.*` renderings (they go
  through this walker), and `fuzz_capture` under ASan/UBSan (run it for a few minutes after any change here —
  CLAUDE.md's rule for the decoder).
- **Every new kind needs three things:** a `case` here with its own bounds check against `b` or `image`, an
  arity/size rule in `dictionary.cpp::parse_field`, and the encoder side; and it is a wire-format change
  (`format_minor`, golden blobs and fixtures, canary). The `map_`/`set_` case is the model: the block
  charged once through `take_n` with the FIELD's stride, every entry window a `subspan` of the image, the new
  visitor events defaulted in `tools.hpp`.
- **Never index `image` through a slot without `c.budget.take` first**, and never add a slot rule here only:
  change `fmt::image_budget` so the typed decoder gets it too. A block of `n` elements goes through `take_n`, never
  through `take` with a product computed here. Do arithmetic on `uint64_t` (`offset + size`). The pattern `off <= size && bytes <= size - off` is the overflow-safe one; do not
  "simplify" it to `off + bytes <= size`.
- **Stay allocation-free on the walk** (`tools.hpp` promises "zero allocations in the walker"); the only
  allocation is `alt_text` for a variant label. Do not build strings for names — pass `std::string_view`s
  into the descriptor's storage.
- **No exceptions, no RTTI**: errors are `std::unexpected`; the visitor is an abstract base with virtual
  calls only.
- **Every recursive call spends a step.** A new recursive path must call `c.step()` like `walk_value` and
  `walk_struct`, or the D-02 bound no longer holds; if it adds visits per byte, adjust the budget formula in
  `visit_record`.
- **Flatten is by name prefix `<base:`**; the reflect side and the golden schema strings fix that spelling.
- **Threading:** pure function of its arguments; the dictionary is read-only; the visitor is the caller's
  and is used on the calling thread.
