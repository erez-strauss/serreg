# 8. Dynamic members: the canonical image

A class may hold `std::string` (with any allocator: `std::pmr::string`, an arena string —
`std::basic_string<char, std::char_traits<char>, Alloc>`), `std::vector<T>` (any allocator, so `std::pmr::vector`
too), `std::optional<T>` and `std::variant<Ts...>`, and (format 1.6) `std::map`, `std::unordered_map`,
`std::flat_map`, `std::set`, `std::unordered_set` and `std::flat_set` — at
any depth, in bases, as array elements, as elements of vectors, as keys and values of maps. Such a class is a
**dynamic type**; it is written
not by `memcpy` but as a **canonical image** whose bytes depend only on the values and on the rule below, never on
the compiler's native layout. The same holds for `std::deque`, `std::list`, `std::forward_list` and `std::stack`
(the `vector` shape), `std::multimap` and `std::unordered_multimap` (a `vector` of `std::pair<K,V>` sorted by key),
and for `std::pair` and `std::tuple`, which are dynamic classes of their own (§8.1c). Design records:
`docs/plans-for-v2.0.md`, `docs/plans-for-containers.md`.

## 8.1 The rule

An image = **fixed part** + **tail**. Offsets inside the image are relative to its first byte.

**Fixed part.** Members in declaration order, bases first (as `<base:Name>` pseudo-fields), each placed at
`offset = align_up(cursor, member_align)`; the fixed part ends at `align_up(cursor, type_align)`. The
member's *image* is:

| member | image | size, align |
|---|---|---|
| scalar, enum, flat class, array of flat | native bytes (padding zeroed, bools 0/1) | native |
| `string`, `vector`, `optional`, `variant`, `map`, `set` (and the containers that share these shapes, §8.1c) | reference slot `{u32 off, u32 n}` | 8, 4 |
| nested dynamic class (a user class, a `std::pair`, a `std::tuple`) | its fixed part, inline | that class's fixed size/align |
| `T[N]` / `std::array<T,N>` with dynamic `T` | `N` inline element images at the element stride | `N × stride`, `T`'s align |

`type_desc.size/align` of a dynamic type are the fixed part's; `is_flat` is false; the schema string starts with
`dyn<Name>` so a flat and a dynamic definition of one name can never share a hash.

**Reference slot** `{u32 off, u32 n}`:

| kind | `n` | tail block at `off` |
|---|---|---|
| string | byte count | the bytes (no terminator) |
| vector | element count | `n` element images, contiguous, stride = `align_up(elem size, elem align)`; then the elements' own tails |
| optional | 0 nullopt / 1 engaged | one element image when 1 |
| variant | 0 valueless / `i+1` = alternative `i` | one image of alternative `i` |
| map | entry count | `n` **entries** in ascending key order, contiguous, stride = `align_up(value_offset + value size, max(key align, value align))`; an entry is the key's image at 0 and the value's image at `value_offset = align_up(key size, value align)`; then the keys' and values' own tails |
| set | key count | `n` key images in ascending order, stride = `align_up(key size, key align)`; then their tails |

`n == 0 ⇒ off == 0`. Tail blocks are appended depth-first in member order — a member's block, then the
references *inside* that block, then the next member. Each block is aligned to its element alignment with zero
gap bytes. The encoder zeroes the whole measured image before writing it, so this holds on every sink —
including a reused `capture_file` buffer, a plain sink's stack buffer, an `es::ipc` ring record or caller memory behind a
`span_sink`: identical values always give identical bytes, and every padding byte is zero. An empty vector,
string, a disengaged optional and a valueless variant allocate nothing (slot `{0,0}`). Readers do not depend on
the gap bytes (review G2-2): they only follow slots.

**What readers accept (review G2-6).** Both decoders — `deserialize` (`reflect.hpp`) and the
dictionary-driven walker behind `srdump`, `verify` and `walk_values` (`src/decoder.cpp`) — check every slot with
the same function, `fmt::image_budget::take` (`format.hpp`):

- `n == 0` requires `off == 0`;
- a non-empty block starts at or after the root's fixed part and lies inside the image;
- all blocks together reference at most as many bytes as the image holds. Writers allocate disjoint tails, so an
  honest image always passes; slots that share one tail cannot turn a small image into a huge value (without
  the budget, 64 string slots over one 3 000-byte tail would decode into 192 000 bytes).

A slot that breaks a rule is `length_mismatch` in both, so the two decoders never disagree about an image.

## 8.1b Maps and sets: the shape, the entry, the canonical order (format 1.6)

The wire knows two **shapes**, `map` (`kind::map_` = 53) and `set` (`kind::set_` = 54), and six C++ containers map
onto them: `std::map`, `std::unordered_map` and `std::flat_map` are `map`; `std::set`, `std::unordered_set` and
`std::flat_set` are `set`. The shape, not the container, is what the member's kindspec names — `map<string,i32>`,
`set<u64>`, `map<u8,struct<corpus::DynMsg>>` — so a `std::map<std::string,int>` member and a
`std::unordered_map<std::string,int>` member give the **same schema and layout hashes**, the same FIELD and the
same bytes, exactly as `string` is one kindspec for every `std::basic_string<char>`; a reader may decode either into
either (the container is a property of the reader's object, like the allocator). The container's own name enters
only the transparent type of a top-level argument (chapter 3 §3.3c).

**The entry.** A map's block is `n` entries laid out like a two-member dynamic class: the key's image at 0, the
value's image at `value_offset = align_up(key size, value align)`, the entry padded to the larger of the two
alignments (`detail::map_entry_layout<K,V>()` in `reflect.hpp`: `{value_offset, size, align}`, `size` being the
stride). A set's entry is the key at its own stride. Key and value may be anything a member may be: a string key
is a slot in the entry whose bytes follow the block, a `DynMsg` value is its 20-byte fixed part inline with its
tails after the block. The FIELD records it all: `extent` = the entry stride, and two element descriptors (key at
0, value at `value_offset`) for a map, one for a set, so a dictionary-driven reader lays the block out from the
FIELD alone (chapter 7 §7.4).

| entry | key | value | `value_offset` | stride | align |
|---|---|---|---|---|---|
| `map<string,i32>` | slot, 8/4 | 4/4 | 8 | 12 | 4 |
| `map<i32,string>` | 4/4 | slot, 8/4 | 4 | 12 | 4 |
| `map<u8,DynMsg>` | 1/1 | fixed part, 20/4 | 4 | 24 | 4 |
| `map<u16,u8>` | 2/2 | 1/1 | 2 | 4 | 2 |
| `map<string,Order>` | slot, 8/4 | 32/8 | 8 | 40 | 8 |
| `set<u16>` | 2/2 | — | — | 2 | 2 |

**The canonical order.** Entries are written **ascending by the key** — by the key's `operator<` when it has one,
else by the bytes of its scrubbed flat image (`detail::key_less`). A container that already iterates that way
(`std::map`, `std::set`, `flat_*` with `std::less`) is written as is; every other one — the unordered containers,
or an ordered one with another comparator — is sorted first through an index of pointers to its entries (64 on the
stack, a heap vector above that: the one allocation a `serialize` of an unordered container may make). So the
determinism guarantee of §8.1 holds for maps and sets too: the same values are the same bytes in every container
and after any insertion history. The golden `dynamic.ser` shows it: an `std::unordered_map<int32_t,std::string>`
`{{5,"five"},{4,"four"},{-1,"minus one"}}` is on the wire as `-1, 4, 5` — `operator<` on `int`, not the byte order,
which would have put `ff ff ff ff` last (chapter 7 §7.5b). Why `operator<` and not bytes, and why a comparator
changes a transparent type's *name* but never the order: `docs/deviations.md` "Containers".

**The opt-out.** The sort is a *writer's* choice: `serialize_with(serialize_options{.canonical_unordered =
false}, out, v)` writes an unordered container (and an unordered multimap) in its iteration order — no index, no
sort, no allocation — for a writer that does not need byte-identical frames and would rather not pay for the sort
on a large table. The value, the length, the hashes and the dictionary are the same; only the entry order on the
wire may differ between two processes or after a rehash, so `--diff` and content hashes stop being meaningful for
those frames. Readers never required the order: the typed reader inserts with `emplace_hint(end(), …)` (exact for
canonical data, a wrong hint otherwise) and the dictionary walker only checks the slot, so both forms decode
through every reader. An ordered container is unaffected by the option. ("FR-A6
serialize_options::canonical_unordered: sorted by default, iteration order on request, readers take both",
`tests/unit/test_containers.cpp`.)

**Duplicates and what is refused.** A map or set on the wire has unique keys by construction. An image that
carries two equal keys (not canonical, or forged) decodes with `error::invalid_value`: the first entry is kept,
the rest of the image is still read (§8.4).

## 8.1c The rest of the std vocabulary: which container is which shape

| C++ type | wire shape | notes |
|---|---|---|
| `std::deque<E>`, `std::list<E>`, `std::forward_list<E>` | `vector<E>` | a block of `n` element images at the element stride, walked (never indexed) in the container's order; the reader `resize`s and fills. Identical bytes and hashes to a `std::vector<E>` member |
| `std::stack<E, C>` | `vector<E>` | its underlying container `C`, bottom first, so the reader's `top()` is the writer's; the name `std::stack<E>` omits `C` |
| `std::multimap<K,V>`, `std::unordered_multimap<K,V>` | `vector<std::pair<K,V>>` — schema kindspec `multimap<K,V>` | entries sorted by key: a `std::less` multimap as iterated, anything else stably sorted (`for_each_in_wire_order`; as iterated with `canonical_unordered = false`), so equal keys keep the container's order; duplicate keys are data, the reader `insert`s every entry; layout-compatible with a `std::vector<std::pair<K,V>>` member, a different schema |
| `std::pair<A,B>`, `std::tuple<Ts…>` | a dynamic class `dyn<std::pair<…>>` | parts `first`/`second` or `_0`, `_1`, … at canonical offsets (§8.1), inline in the enclosing fixed part, written and read element-wise through `std::get` — never `memcpy`, because the standard library's object layout is not the canonical one (a libstdc++ tuple stores its elements in reverse). `std::pair<i32,f64>` is `[16,8]{first:i32@0;second:f64@8}` |
| anything else in `std` | — | refused at compile time: smart pointers ("ownership is not data"), `std::tuple<>` ("no parts"), and every class outside the vocabulary ("not in the serialisable vocabulary …", listing it) — `tests/negative/neg_multimap_member.cpp` (a `std::priority_queue`), `neg_smart_pointer_member.cpp`, `neg_empty_tuple_member.cpp` |

Nothing here added a shape: the writer and reader walks reach every row through the `vector` branch
(`sequence_of`, `sequence_size`, `for_each_in_wire_order` in `reflect.hpp`) or the dynamic-class branch (the parts
layer: `part_count`, `part_name`, `get_part`), which is why the dictionary, `srdump`, the Wireshark dissector and
the Python reader needed no change to show a `std::list` (`[7, 8]`) or a pair (`first: -1`, `second: "b1"`).
Tests: `tests/unit/test_containers.cpp` "FR-A6 std::deque, std::list, std::forward_list and std::stack are the
vector shape", "FR-A6 std::pair and std::tuple are positional parts at canonical offsets, encoded through std::get",
"FR-A6 std::multimap and std::unordered_multimap are a vector of pairs sorted by key"; golden strings in
`tests/golden/hashes.cpp` "golden strings of std::deque/list/forward_list, std::pair/tuple, multimaps and std::stack".

## 8.2 Worked example

`struct DynMsg { std::uint32_t id; std::string name; std::vector<std::int16_t> xs; }` with `{7, "ab", {1,-2,3}}`:

```
fixed part (20 bytes, align 4):  id@0[4,4]   name slot@4[8,4]   xs slot@12[8,4]
00  07 00 00 00                 id = 7
04  14 00 00 00  02 00 00 00    name: off 20, n 2
0c  16 00 00 00  03 00 00 00    xs:   off 22, n 3    (block aligned to alignof(i16) = 2)
tail
14  61 62                       "ab"
16  01 00 fe ff 03 00           1, -2, 3
len = 28 → record = u32 28 | u32 0 | image | pad → 40 bytes; frame = 56 bytes
```

Nested: `std::vector<DynMsg>` with two elements puts two 20-byte fixed parts back to back at the block offset
(stride 20), and *then* the first element's `"ab"` and `xs` bytes, then the second's (empty: slots `{0,0}`).
Chapter 7.5 shows the real dump of `DynNested`.

A map: `std::map<std::string, std::int32_t>{{"a", 1}, {"b", 2}}` as a top-level argument — a transparent
type whose image is the slot plus its tail:

```
fixed part (8 bytes, align 4):  the map slot @0[8,4]
00  08 00 00 00  02 00 00 00    slot: off 8, n 2            (block of 2 × 12 bytes, aligned to 4)
tail
08  20 00 00 00  01 00 00 00    entry 0: key slot {32, 1}   ("a" lives after the block)
10  01 00 00 00                 entry 0: value 1 @ value_offset 8
14  21 00 00 00  01 00 00 00    entry 1: key slot {33, 1}
1c  02 00 00 00                 entry 1: value 2
20  61 62                       "a" @32, "b" @33            (the keys' tails, entry by entry, after the block)
len = 34 → record = u32 34 | u32 0 | image | pad → 48 bytes; frame = 16 + 48 = 64 bytes
```

The block is allocated first (2 × stride 12 at alignment 4, so `off` 8), then each entry is encoded in canonical
order and allocates its own tails — which is why both strings land after the whole block. These bytes follow from
`encode_value`'s `map_`/`set_` branch and `image_cursor::alloc` (`reflect.hpp`); chapter 7 §7.5b annotates the
same rule on the checked-in `corpus::DynMap` frame.

## 8.3 Why this design

- **Compiler-independent bytes.** Cap'n Proto's canonicalisation property: no `offsetof` for the dynamic parts, so
  a capture from GCC can be read by a program built with another compiler once it has the same reflection
  facilities. It is not ABI-free: nested *flat* members are native bytes (`sizeof`, `memcpy`), and `long` /
  `wchar_t` follow the host — which is why format 1.3 records the writer's ABI tag (chapter 7 §7.3).
- **Random access without parsing.** Offsets from the image start let a reader jump to any member from the
  dictionary alone; the decoder validates every jump (`off ≤ len`, `n × stride ≤ len − off`, depth ≤ 64) and never
  reads outside the image.
- **One rule for all six kinds.** A single 8-byte slot shape keeps the fixed part small and the descriptor
  simple (kind + element descriptor(s)); a map's block is a vector of two-member entries, so maps and sets need no
  new mechanism, only two kinds and a sort.
- **8-byte prefix.** `u32 len, u32 0` (not the 4-byte prefix sketched in the v3 spec) keeps the image 8-aligned
  inside the 8-aligned record, so scalars in the fixed part can be read in place.

## 8.4 Writing and reading

- `serialize(out, msg)` — `measure_image(v)` (one pass over the value: string sizes, vector counts) gives the
  exact length, the sink is asked for it, `encode_image` writes the fixed part then the tail blocks, patching
  slots as it allocates. No allocation on a claiming sink; a plain sink allocates only for one record beyond the
  16 KiB chunk.
- `deserialize(bytes, msg)` — `decode_image` walks the same structure, assigning members through reflection
  splices (private members included): `std::string::assign`, `std::vector::resize` + per element,
  `optional::emplace/reset`, `variant::emplace<i>`; a map or set is `clear`ed, `reserve`d when it is a
  hash container, and filled with `emplace_hint(end(), key, value)` entry by entry — O(1) per entry for data in
  canonical order, merely a wrong hint for any other — into **any container of the shape**: an
  `std::unordered_map` output reads a `std::map` writer's bytes and vice versa. An entry whose key is already
  present (a duplicate on the wire) is dropped and the read reports `error::invalid_value` after the rest of the
  image, like a valueless variant. A valueless variant on the wire (`n = 0`) decodes to
  `std::monostate` if the variant has it; otherwise that member is left as it was and `deserialize` returns
  `error::invalid_value` after reading the rest of the frame. `srdump`
  shows it as `null` either way. A `bool` byte other than 0/1 reads as `true` (optional warning, `error.hpp`).
- `dispatch<DynMsg>` delivers a decoded temporary; `view<T>` is flat-only; `record_view.payload` is the image.
- `srdump`/`tools::visit_record`: `string(name, bytes)`, `begin_vector/end_vector`, `null_value`,
  `begin_variant(name, index, "variant<i:kind>")/end_variant`, `begin_map(name, n)` /
  `begin_entry(i)` / the key as `"key"` and the value as `"value"` / `end_entry` / `end_map`, `begin_set(name, n)`
  / `end_set`, `begin_transparent(type, field)` / `end_transparent` — all with defaults that express them through
  the older events (a map is a vector of `entry` structs to a visitor that does not override them). Text: `name: "ab"`, `xs: [1, -2, 3]`,
  `side: null`, `v: variant<2:string> { value: "var" }`, `by_name: {"a": 1, "b": 2, "c": 3}`, `ids: {10, 20, 30}`;
  JSON: `"name":"ab"`, `"xs":[1,-2,3]`, `"side":null`, `"v":{"alt":2,"type":"string","value":"var"}`,
  `"by_name":[{"key":"a","value":1},…]` (a key need not be a string, so a map is an array of entries),
  `"ids":[10,20,30]`.

## 8.5 Limits and rejections

- `std::string_view` **members** are rejected (could not be read back); `std::vector<bool>` is rejected (a
  bitset — use `std::vector<std::uint8_t>`); smart pointers, `std::any` and the standard containers without a wire
  shape are rejected. The diagnostics name the rule.
- A key of an unordered container (or of one ordered by a comparator other than `std::less`) must have `operator<`
  or be trivially copyable, because the writer sorts it (a `static_assert` names the rule).
- A top-level `std::string` is a runtime-string argument, not a transparent type; every other container is an
  argument of its own (chapter 3 §3.3c) — no need to wrap it in a struct.
- **Allocators.** The allocator of a string or vector is a property of the reader's object, not of the
  data: it is neither written nor hashed. A struct with `std::pmr::string` members writes the same image as the
  same struct with `std::string` members, and a top-level `std::pmr::string` argument writes the very same frame
  as a `std::string` one (call hash included). Decoding assigns into the output object, so a `std::pmr::string`
  or `std::pmr::vector` member keeps the memory resource it was constructed with; elements that a decode creates
  (the strings inside a `std::vector<std::pmr::string>`) use their default resource. Other character types
  (`std::wstring`, `std::u8string`) are not strings on the wire and stay rejected.
- `n` and `off` are 32-bit: one image is at most 4 GiB and a vector, map or set at most 2³²−1 elements.
- Schema evolution (`since`/`deprecated` projection) is not implemented for dynamic types; the hashes change when
  the member list changes, exactly as for flat types.

## 8.6 What is frozen

`tests/golden/hashes.cpp` locks the schema/layout hashes and fixed layouts of `corpus::DynMsg`, `DynNested`,
`DynArr`, `DynBase`, `DynMap` and `std::monostate`, the `map<K,V>`/`set<K>` kindspecs and the transparent
types' names and hashes; `blobs.cpp` their ENTRY blobs; `tests/fixtures/golden/dynamic.*` the
bytes and renderings of a nine-frame capture (four frames of dynamic classes, then a `DynMap`, a top-level vector
compact, empty and with full records, and a `println` with a vector and a map); `tests/unit/test_dynamic.cpp` the
byte-exact example above, every shape's round trip, the corrupt-image rejections and the tools output;
`tests/unit/test_containers.cpp` the six containers, nested containers, the canonical order, duplicate keys and
the transparent type.
