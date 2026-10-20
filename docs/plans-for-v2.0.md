# Plans for v2.0.0 — dynamic members (`std::string`, `std::vector`, `std::optional`, `std::variant`)

Status: written 2026-09-28 after v1.1.0 shipped; **implemented and shipped in v2.0.0** (`CHANGELOG.md`). Spec basis: `ser-reg-requirements-v3.md`
§3 ("dynamic type … encoded as a canonical image"), §6.5 ("dynamic (v2): u32 len | canonical image"), §0 Q6
("variant has a hash value and each of its internal types"). This document is the v2 design that v3 §6.5 defers
to; where the two differ (the 8-byte record prefix, §2) this one is normative.

Library **2.0.0** writes wire format **1.2** (a minor bump: nothing that 1.1 wrote changes; new field kinds and
one new record encoding). Library and format versions are separate numbers by design — `srdump --version`
prints both. 2.0 reads 1.0, 1.1 and 1.2 captures; 1.x readers reject 1.2 with `error::bad_version`
(the strict rule from 1.1). The compatibility corpora `tests/fixtures/golden-1.0/` and `golden-1.1/` stay frozen.

## 1. What becomes serialisable (FR-DY1)

A **dynamic type** is a class type with at least one *dynamic member* anywhere in its tree:

| member type | canonical kind | notes |
|---|---|---|
| `std::string` | `string` | bytes, no terminator |
| `std::vector<T>` | `vector<T>` | `T` flat or dynamic (or a scalar/enum) |
| `std::optional<T>` | `optional<T>` | `T` flat or dynamic |
| `std::variant<Ts...>` | `variant<T0\|T1\|…>` | each alternative flat or dynamic; `std::monostate` allowed |
| `T[N]`, `std::array<T,N>` with dynamic `T` | `array` of dynamic elements | inline fixed parts, stride = element fixed size |
| a dynamic struct as a member or base | `dyn<Name>` | inline fixed part |

Still rejected (FR-S7 diagnostics): pointers, references, unions, virtual bases/functions, bit-fields, `std::map`
and every other container, `std::string_view` **as a member** (write-only would break round-trip symmetry — use
`std::string`), `std::unique_ptr`. `as_flat<T>` remains top-level only. Top-level arguments stay: flat type,
literal, scalar, runtime string (1.1), **dynamic type** (new). A bare `std::vector<int>` as an argument is not an
argument category — wrap it in a struct.

## 2. Canonical image (FR-DY2) — the bytes

A dynamic value is one **image**: a **fixed part** followed by a **tail**. Every offset in the image is relative
to the image start. The image is placed in a record as:

```
compact record:  u32 len | u32 0 | image[len]        padded to 8      (len = fixed + tail)
full record:     u64 schema_hash | u32 payload_len(= 8 + len) | u16 layout(0) | u16 rsv | u32 len | u32 0 | image
```

The 8-byte prefix (not the 4-byte one of v3 §6.5) keeps the image 8-aligned inside the 8-aligned record, so
scalars inside the fixed part can be read in place.

**Fixed part** — members in declaration order, bases first (as `<base:Name>` pseudo-fields, §6.1b option d),
each at `offset = align_up(cursor, member_align)`; the fixed part ends at `align_up(cursor, type_align)`:

| member | image in the fixed part | size, align |
|---|---|---|
| scalar, enum, flat struct, array of flat | its native bytes (padding zeroed, bools normalised — the 1.0 scrub) | native |
| `string`, `vector`, `optional`, `variant` | a **reference slot** `{u32 off, u32 n}` | 8, 4 |
| dynamic struct | its fixed part, inline | that type's fixed size/align |
| array of dynamic `T[N]` | `N` inline fixed parts, stride = `T`'s fixed size | `N × stride`, `T`'s align |

The canonical layout is computed by that rule alone — it does **not** follow the native `offsetof` — so two
compilers/ABIs agree on the bytes (Cap'n Proto's canonicalisation property). `type_desc.size` / `align` of a
dynamic type are the fixed part's. Every padding byte is written as zero.

**Reference slot** `{u32 off, u32 n}`:

| kind | `n` | tail at `off` |
|---|---|---|
| string | byte count | the bytes |
| vector | element count | `n` element images, contiguous, stride = element image size (flat: native size; dynamic: fixed size), then (later) the elements' own tails |
| optional | 0 = nullopt, 1 = engaged | one element image when `n == 1` |
| variant | 0 = valueless, `i + 1` = alternative `i` | one image of alternative `i` when `n ≠ 0` |

`n == 0` ⇒ `off == 0` (canonical). Tail blocks are emitted depth-first in member order: after a fixed part, each
dynamic member's block is appended (aligned to its element alignment, gap bytes zero), then that block's own
references are resolved in the same way. Identical values ⇒ identical bytes.

Byte-exact example — `struct Msg { std::uint32_t id; std::string name; std::vector<std::int16_t> xs; }` with
`{7, "ab", {1, -2, 3}}`:

```
fixed part (20 bytes, align 4):  id @0 [4,4]   name slot @4 [8,4]   xs slot @12 [8,4]
  00  07 00 00 00                id = 7
  04  14 00 00 00  02 00 00 00   name: off = 20, n = 2
  0c  16 00 00 00  03 00 00 00   xs:   off = 22, n = 3   (block aligned to the i16 element)
tail:
  14  61 62                      "ab"
  16  01 00 fe ff 03 00          xs = 1, -2, 3
len = 28; compact record = u32 28 | u32 0 | image[28] | pad → 40 bytes
```

Exact offsets are produced by the encoder and frozen by the golden fixture (`tests/fixtures/golden/dynamic.*`);
the rule is what is normative, the example above is illustrative.

## 3. Dictionary (FR-DY3)

- `type_desc.is_flat = false`; `k = struct_`; `size`/`align` = fixed part. Schema string prefix `dyn<Name>`
  (a flat and a dynamic definition of one name never share a hash).
- FIELD entries (format.hpp) already carry `kind`, `n_elem`, `extent` and nested FIELDs: `string_` (size 8,
  align 4, no elem); `vector_`/`optional_` (size 8, `n_elem = 1`: the element descriptor at offset 0, size =
  element image size); `variant_` (size 8, `extent` = alternative count, `n_elem = extent`); `array_` of
  dynamic elements as today (`elem[0]` is the element, size = its fixed size). Element descriptors of struct
  kind carry the nested `type_hash`, and the nested type has its own ENTRY (registered by `touch`) — so a
  variant's alternatives are types in their own right (Q6: "both").
- Canonical member kinds in the schema/layout strings: `string`, `vector<E>`, `optional<E>`, `variant<E0|E1>`,
  where `E` renders exactly like a member kind (`i32`, `struct<Name>:0x…`, `dyn<Name>:0x…`, `array<E,N>`).
- `dyn::dictionary` parser accepts the four kinds with those arities and validates `size == 8` for reference
  slots; the decoder rejects an image whose references leave the image (`error::length_mismatch`), whose depth
  exceeds 64, or whose vector `n × stride` overflows.

## 4. API (FR-DY4)

- `serialize` / `serialize_with` / `serialize_print`: a dynamic argument is accepted like a flat one; the frame
  size comes from `frame_size(o, vs...)` (one measure pass, no allocation); `frame_size<Ts...>()` is the minimum
  (8 per dynamic record). Claiming sinks: measure → claim → encode in place. Plain sinks: ≤ 64 KiB via the stack
  buffer, larger frames streamed record by record (the image is encoded in chunks: fixed part, then tail blocks).
  Encoding allocates nothing. The flat hot path is untouched (the dynamic code is only instantiated for dynamic types).
- `deserialize` into a dynamic `T&`: validated image walk, then members are assigned (`std::string`,
  `std::vector` resized, `std::optional` engaged/reset, `std::variant` `emplace<i>`). Allocates as the target
  requires. `deserialize_raw` / `serialize_raw`: the image with its `u32 len, u32 0` prefix. **(Not implemented:
  both stay `flat_type`-only; a raw image would be new API without a call hash — `docs/deviations.md`, 2.1.7.)**
- `dispatch<Ts...>`: a dynamic `T` is delivered as `const T&` of a decoded temporary (never zero-copy);
  `view<T>` is `static_assert`ed to flat types.
- `record_view` for a dynamic record: `payload` = the image (prefix stripped), `desc`/`dyn_desc` as for flat.
- `tools::decoded_value_visitor` gains `string(name, bytes)`, `begin_vector(name, n)` / `end_vector()`,
  `optional(name, engaged)` (followed by the value's events when engaged), `begin_variant(name, index,
  alt_type_name)` / `end_variant()`. Text: `name: "…"`, `xs: [3] { … }`, `opt: null` / value, `v: <alt 1>
  value`; JSON: string, array, `null` / value, `{"alt":1,"value":…}`; TOML follows JSON's record form.
- `serialize_print` may take dynamic arguments; they render like structs (one-line text form).

## 5. Hashes and golden tests

- New golden `STATIC_REQUIRE`s for a dynamic corpus: `corpus::DynMsg { u32 id; std::string name; std::vector<i16> xs; }`,
  `corpus::DynNested { std::vector<DynMsg> msgs; std::optional<Order> maybe; std::variant<i32, Order, std::string> v; }`,
  `corpus::DynArr { std::string names[2]; }`, `corpus::DynBase : Order { std::string note; }`.
  Their schema/layout strings are printed by `[golden-print]` and frozen; blob sizes/hashes locked in `blobs.cpp`.
- New fixture `tests/fixtures/golden/dynamic.{ser,rri,text,json,toml}` (byte-exact), `orders.*` unchanged
  (moved to `golden-1.1/` as the second compatibility corpus only if any byte changes — it must not).
- Round-trip tests: every shape above, empty string/vector, nullopt, every variant alternative, monostate,
  vector of 100 000 elements (streaming path), nested vector<vector<…>> via structs, print with a dynamic arg.
- Fuzz: the `capture` target's corpus gains `dynamic.ser`; the decoder is run under ASan/UBSan on mutated images.
- Negative: `std::map` member and `std::string_view` member rejected with diagnostics naming the rule.

## 6. Out of scope for 2.0 (→ 2.1+)

`since`/`deprecated` projection (schema evolution), delta dictionary frames, INDEX footer, aarch64/ABI tag,
bit-fields, custom encoders, top-level containers as arguments, cross-endian.
