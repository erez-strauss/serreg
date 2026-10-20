# Plan: containers as arguments, and maps and sets as members

Status: **implemented and released as 2.2.0, wire format 1.6 (2026-10-07).** Raised by the maintainer 2026-10-07,
designed by the lead the same day, scheduled as 2.2.0 by the maintainer. What shipped follows this plan with these
differences (details in `docs/deviations.md` "Containers" and `remove-me/docs/release-state-2.2.0.md`):

- the member kindspec is the shape (`map<K,V>`, `set<K>`) for every container — the container's name is **not**
  in a member's hash (as `string` is for every `std::basic_string<char>`), so `map ↔ unordered_map ↔ flat_map`
  members are interchangeable without evolution rules; the container's vocabulary name enters only the
  **transparent type** of a top-level argument (`std::unordered_map<i32,std::string>`), where it is the C++ type
  the reader must name; there is no `container_traits<T>` customization point — the six containers are
  `dyn_member` specialisations, and a user container is added the same way;
- the canonical order is ascending by the key's `operator<` (its bytes when it has none), not the byte order for
  every key (§3 option 1); `std::map`/`std::set`/`flat_*` with `std::less` write their iteration order, every other
  container (unordered, or another comparator) is sorted; no `canonical_unordered = false` option;
- `std::list`/`std::deque` and `multimap`/`multiset` did not ship (named diagnostics);
- the enum ENTRY's `fixed` flag and the `frame_flag_crc_reserved` bit rode along; checkpoint/index frames, CRC32C
  and `renamed_from` did not (still format 1.7 candidates).

The text below is the design as proposed, kept as the record.

## 1. Containers as top-level arguments: a transparent wrapper type

Today an argument is a class (flat or dynamic), a literal, a constant, a scalar or a runtime string; a dynamic
member type (`std::vector<E>`, `std::optional<E>`, `std::variant<…>`) has no type entry of its own and is refused
as an argument (`dynamic_type` excludes `is_dyn_member_v`).

- Every dynamic-member type used as an argument gets a **synthetic struct ENTRY**: canonical name = the member
  vocabulary name (`std::vector<i32>`, `std::optional<corpus::Order>`, `std::map<std::string,f64>`), one unnamed
  FIELD at offset 0 (the 8-byte slot), size 8, not flat, and a new flag `struct_flag_transparent`.
- The record is the ordinary dynamic-type record (`u32 len, u32 0, image`); the image is the slot plus the tail:
  the same bytes the container produces today inside a one-member struct. The call-string item is the synthetic
  type's schema hash. No new argument category, no ARG change.
- `serialize`, `deserialize`, `dispatch`, `frame_router`, `walk_values`, `inspect`, `verify`, `srdump`, Wireshark
  all go through the existing type-argument path; renderers that see `transparent` print the field's value
  directly.
- Scope: vector, optional, variant, the new map/set shapes, the sequence containers below. `std::string` at top
  level stays the `string` record. `std::array<E,N>` of a flat `E` is already a flat class.
- Until then, the top-level case gets a `static_assert` naming the rule ("wrap it in a struct") instead of an
  overload-resolution failure (2.1.16 candidate, B10 in `remove-me/docs/plans-for-v2.1.16.md`).

## 2. Maps and sets as members: two new wire shapes

Wire shape (the bytes) and C++ type (the name) are separate. Shapes today: `string_`, `vector_`, `optional_`,
`variant_`. New:

| Kind | Image | FIELD |
|---|---|---|
| `map_` | slot `{u32 off, u32 n}` → `n` images of `std::pair<K,V>` (K then V at canonical offsets, tails as for any nested dynamic member), **sorted by key** | `n_elem = 2`: key FIELD, value FIELD |
| `set_` | slot → `n` key images, sorted | `n_elem = 1` |

The bytes of a `map_` equal those of a `std::vector<std::pair<K,V>>` sorted by key (`std::pair` is already a
vocabulary class). The new kind exists so the dictionary and the renderers can tell a map from a vector of pairs
(JSON `{"k": v}`, text `{k: v}`) and so the schema strings differ (injectivity, decision B).

`container_traits<T>` (a customization point like `type_properties<T>`) maps C++ types onto shapes:

| C++ type | Shape | Name | Note |
|---|---|---|---|
| `std::map`, `std::flat_map` | `map_` | `std::map<K,V>`, `std::flat_map<K,V>` | key order: canonical for free |
| `std::unordered_map` | `map_` | `std::unordered_map<K,V>` | sorted before encoding (§3) |
| `std::set`, `std::flat_set`, `std::unordered_set` | `set_` | likewise | |
| `std::list`, `std::deque` | `vector_` | `std::list<E>`, `std::deque<E>` | same bytes as a vector |
| user containers | by specialisation | user-given | no library change needed |

Same shape ⇒ same layout hash; different name ⇒ different schema hash. Evolution level 1's R10 extends to the two
shapes, so `unordered_map ↔ map` with equal K, V projects trivially. Custom comparators are part of the C++ type
and enter the canonical name (`std::map<K,V,my_less>`).

## 3. The canonical-order rule (the one design decision)

Identical values ⇒ identical bytes holds for `std::map`/`std::set` by iteration order and fails for
`std::unordered_*` (bucket count, insertion history). Options:

1. **Sort by the key's encoded bytes** (memcmp order of the key image): deterministic for every key type, needs
   no `operator<`, the reader never re-sorts. Cost: an index array of `n` pointers sorted at write time — a stack
   scratch up to a bound, heap above it (the same documented exception as the >16 KiB image on plain sinks).
2. Write iteration order and flag the FIELD non-canonical: fast, gives up the guarantee for exactly the containers
   people put in messages.

**Recommendation: 1 by default**, with `serialize_options{.canonical_unordered = false}` for writers who measure
and do not care. Duplicate keys in an image decode as `invalid_value` for `map`/`set`; `multimap`/`multiset`
would be the same shapes with a `multi` flag, later if ever.

Decoding: `map`/`set` use `emplace_hint(end(), …)` in order (O(n) for sorted input); `unordered_*` `reserve(n)`
then `emplace`; the slot rules and byte budget (`fmt::image_budget`) apply as for vectors. Keys may be anything a
member may be.

## 4. Cost and placement

- Format: two kind values, one struct flag → a format minor. Bundle with the other pending format items
  (checkpoint/index frames, CRC32C, `renamed_from` in the dictionary, retained flag) in one "exactly 1.6" step.
- Code: `reflect.hpp` (traits, pair-image element path, the sort, the wrapper descriptor), `decoder.cpp`,
  `formatters.cpp` (text/JSON/TOML), `dictionary.cpp` (kinds, `n_elem` checks), `wireshark.cpp` (Lua),
  `diff.cpp` (shape-aware verdicts), golden corpus (`DynMap`, `DynSet`, a top-level vector frame), fixtures,
  canary, fuzz corpora, book chapter 7.
- Release: not 2.1.16 (hardening, no format change). 2.2.0 as "containers", with evolution level 1 (no format
  change) as 2.3.0 — or both in 2.2 as one larger step. Maintainer's call when scheduling.

## 5. Tests (when scheduled)

Golden strings and hashes for `std::map<i32,f64>`, `std::unordered_map<std::string,corpus::Order>`,
`std::set<u64>`, a top-level `std::vector<i32>` (empty and non-empty; the empty one is the 16-byte header +
`u32 8, u32 0` + an all-zero slot); byte identity of a map and the equal vector of pairs; an unordered map
filled in two insertion orders producing one image; duplicate keys → `invalid_value`; corrupt slot bombs through
the fuzz corpora; `srdump` text/JSON/TOML and the Lua dissector parity on the new fixture frames; `--diff`
`map ↔ unordered_map` = same layout, different schema.
