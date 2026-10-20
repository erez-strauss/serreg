# Schema evolution — the specification of the levels

**Status:** specification, written against wire format 1.7 (last revised 2026-10-13). Level 0 is what ships.
Level 1 is decided (DECISIONS R-9) and specified here in full; **its flat-type part is implemented**
(`include/es/serreg/projection.hpp`: `renamed_from`, `tools::project<T>`, `deserialize(dict, …)` — see §2.7 for
exactly what is in and what is still to come). Level 2 is specified so that level 1 does not preclude it; it is
unscheduled (R-10). Level 3 is specified only far enough to say why it is not planned (R-10). Nothing in this
document changes the wire format: levels 1 and 2 are reader-side behaviour plus two annotations that live in the
schema string, not on the wire.

The document has four parts per level: **what the reader guarantees**, **what the programmer must do when
declaring the data types** (annotations, naming, what is forbidden), **what the tools report**, and **what the
level does not do**. A table at the end puts the levels side by side.

Vocabulary: the **writer** is the program (or the version of the program) whose dictionary describes the frame;
the **reader** is the program decoding it with its own compiled types. **Backward reading** is a newer reader
reading older data (the capture-replay case); **forward reading** is an older reader reading newer data (a
deployed consumer meeting a newer producer). R-9 asks for both.

---

## 0. The two identities every level rests on

Every type has two 64-bit identities, both computed at compile time from reflection (P-1, P-3):

- the **schema hash** — XXH3-64 of the canonical schema string: the type's canonical name, every member's name,
  kind, size, offset and nested type, enumerators and their values, `since`/`deprecated` annotations. Two
  types with the same schema hash are *the same type in every respect a reader can observe*.
- the **layout hash** — XXH3-64 of the layout string: kinds, sizes, offsets, element counts — no names, no
  annotations. Two types with the same layout hash have *identical bytes for identical values*, so a `memcpy`
  decode is valid between them.

A frame names its **call** (one 64-bit hash of the whole signature: argument types by schema hash, literals,
constants, flags). A dictionary (the capture's opening block and its deltas, an `.rri`, the ELF section) maps every
hash a reader may meet to the full description. **Evolution is always dictionary-driven**: the reader looks the
writer's type up in the dictionary that travelled with the bytes and decides, per member, what to do. No level
guesses from bytes.

The consequences that hold at every level:

| Change in the writer's C++ | schema hash | layout hash | call hash |
|---|---|---|---|
| rename a member | changes | same | changes |
| add `[[=since(N)]]` / `[[=deprecated(N)]]` | changes | same | changes |
| rename the type (or `[[=name]]`) | changes | same | changes |
| add, remove, reorder, retype a member | changes | changes | changes |
| change an enumerator's value or name | changes | same (value) / changes (name only: same) | changes |
| change a string's allocator, a map's container (`std::map` → `std::unordered_map`) | same | same | same |

Note the second row: adding `[[=since]]` or `[[=deprecated]]` to a shipped member changes the schema hash (they
are part of the schema string) and therefore every call hash naming the type, while no byte changes; they are
documentation until format 1.8 (DECISIONS P-21, open, proposes taking them out of the schema string while
keeping them in the ENTRY/FIELD records and in `srdump --diff`).

---

## 1. Level 0 — what ships today

### 1.1 What the reader guarantees

- **Typed readers** (`deserialize`, `view<T>`, `dispatch`, `frame_router::on<Sig…>`) decode a frame only when the
  call hash matches a signature they were compiled with — which implies every argument's schema hash matches.
  Any difference between the writer's and the reader's definition is `hash_mismatch` (or an unmatched frame in a
  router). There is no projection, no widening, no defaulting.
- **Dictionary-driven readers** (`srdump`, `tools::walk_values` in its generic form, `inspect`, the Python reader,
  the Wireshark dissector, `to_string`) read **any** frame whose dictionary is present, whatever the reader was
  compiled with — field names, enumerators, nested values, `since`/`deprecated` markers are all shown. Evolution
  is therefore a *typed-reader* problem only; the tools never had it.
- **Two definitions in one process** (a plugin reloaded with a changed layout, a host and a plugin disagreeing):
  the registry holds both (A-3), frames written under either decode under their own dictionary entry, `--diff`
  shows the difference, a *loaded* pair with one schema hash and two layouts is a conflict (G-16).

### 1.2 What the programmer must do

Nothing beyond declaring the type. The annotations that exist today and what they mean at level 0:

| Annotation | Where | Effect today |
|---|---|---|
| `[[=es::serreg::skip]]` | a member | not serialised, not in the dictionary; reads back value-initialised |
| `[[=es::serreg::since(N)]]` | a member | recorded in the schema string and the dictionary (`since N`); **no decode behaviour** |
| `[[=es::serreg::deprecated(N)]]` | a member | recorded; no decode behaviour |
| `[[=es::serreg::name("ns::Name")]]` | a class | the canonical name (required for unnamed-namespace and local types; a way to keep a name across a move) |
| `es::serreg::as_flat<T>` | a top-level argument type | serialise `T` as another flat type |
| `es::serreg::type_properties<T>` | a std/third-party type | its vocabulary name |

The working rule, until level 1 ships, is operational rather than in the type system: keep the previous
release's `.rri` next to the binary, run `srdump --diff old.rri ./new --direction both` in CI, and treat a
`BREAKING` verdict as "a new type name (and a new literal) with two handlers in the reader" — the writer side
never has to change bytes retroactively, because the dictionary travels with them.

### 1.3 What the tools report

`srdump --diff a b --direction old-data|new-data|both|none`: per type `identical` | `names-only` |
`metadata-only` | `breaking` | `added` | `removed`; per call and literal `added` | `removed` | `changed`; exit
status 1 on any breaking entry in the chosen direction. `metadata-only` is exactly "same layout hash, different
schema hash" — the class of change level 1 will absorb.

### 1.4 What level 0 does not do

A renamed member, an added member, a widened integer, a reordered member all make a typed reader refuse the
frame. The data is never lost (the tools read it), but a typed consumer needs a second handler.

---

## 2. Level 1 — projection by member name (decided: R-9, 2.3.0)

### 2.1 What the reader guarantees

When a typed reader meets a frame whose argument type `W` (the writer's, from the dictionary) differs from the
reader's `T` but **matches it by canonical name** (or through `renamed_from`), the reader **projects** `W` into
`T` member by member, by name, under the rules of §2.3, instead of returning `hash_mismatch`. Both directions
(R-9: level 1 "in both directions"):

- **backward** — `T` is newer than `W`: members `T` added get their default; members `T` dropped are skipped;
  widened members are widened;
- **forward** — `T` is older than `W`: members `W` added are skipped (the reader never sees them); members `W`
  dropped get `T`'s default; a member `W` *narrowed* relative to `T` is widened on the way in (`W`'s u16 into
  `T`'s u32 is fine); a member `W` *widened* relative to `T` is **not** narrowed (R7: the member keeps its default
  and is reported).

The guarantees, stated as the reader's contract:

1. **Equal layout hash → the level-0 path.** When `W` and `T` have the same layout hash the decode is the `memcpy`
   / canonical-image decode of today, whatever the names and annotations say. Renames and `since` markers cost
   nothing at run time.
2. **Different layout hash → a projection**, computed once per `(W schema hash, T)` and cached (a flat table of
   `(source offset, destination offset, size, conversion)` for flat members; a plan for dynamic members), then
   applied per value. No allocation per value for flat members.
3. **Never narrowing, never reinterpretation.** A member whose writer kind cannot be represented losslessly in the
   reader's member is not written (R7); the reader's member keeps its default, the fact is counted and reported
   once per `(W, T)` pair, decoding continues. Level 1 never truncates an integer, never converts a float to an
   integer, never reinterprets bytes.
4. **Defaults are the reader's.** A member the writer did not have takes the value it has in a value-initialised
   `T{}` — default member initialisers apply (so `std::uint32_t venue = 3;` is honoured).
5. **The call still has to match in shape.** Level 1 projects *argument types*. A call whose literal, constant or
   argument *count* differs is a different call (`hash_mismatch` / unmatched), as today: the signature is the
   message's identity and literals/constants carry meaning. (Matching calls by position with projected arguments
   is level 2.)
6. **Both readers agree.** `deserialize`/`dispatch`/`frame_router` (typed) and `walk_values<Ts…>` with
   `walk_options{.project = true}` (dictionary-driven, handing out `T`s) apply the same rules and produce the same
   `T`; `srdump --diff --compat` predicts the same verdicts without running the data.

### 2.2 What the programmer must do when declaring the types

Level 1 asks for discipline in three places and offers two annotations. Everything else is inferred from the
member names.

**Names are the contract.** A member's *name* is its identity across versions. Therefore:

- Do not reuse a member name for a different meaning (the projector will copy the old value into it).
- Do not rename a member casually; when you do, say so:

  ```cpp
  struct Order {
      std::uint64_t id;
      [[=es::serreg::renamed_from("px")]] std::int64_t price;   // was `px` until v3
      std::uint32_t qty;
  };
  ```

  `renamed_from` may be repeated (`renamed_from("px")`, `renamed_from("price_ticks")`) when a member was renamed
  more than once; the oldest name is still matched. It is **not** part of the schema string, so it changes no
  hash and no bytes — it only widens what the reader accepts.

- Do not rename a type casually; when you do, keep the old name reachable:

  ```cpp
  struct [[=es::serreg::renamed_from("md::Quote")]] QuoteV2 { … };   // frames written as md::Quote project here
  ```

  (or keep the canonical name fixed with `[[=es::serreg::name("md::Quote")]]`, which is the better choice when the
  C++ name changes for a reason that has nothing to do with the data.)

**Widen, do not narrow.** When a member's range grows, grow its type in the allowed directions only:

| Writer | Reader | Level 1 |
|---|---|---|
| `iN` | `iM`, M > N | widen |
| `uN` | `uM`, M > N | widen |
| `uN` | `iM`, M > N (strictly wider) | widen (R3, kept by R-9) |
| `f32` | `f64` | widen |
| `bool` | integer | **not** projected (R7) — `bool` stays `bool` |
| `char`, `char8_t`, … | anything else | not projected |
| any | narrower, or another family | not projected (R7) |

A member that must shrink or change family gets a **new name** (and the old one `skip`-ped or kept
`deprecated`), so that old data projects to the default rather than to a wrong value.

**Order and offsets do not matter.** Members may be reordered, members may be added anywhere (not only at the
end), padding may change: the projector works by name and by the dictionary's offsets. Adding a member in the
middle changes the layout hash and therefore costs a projection on the old data — nothing else.

**Mark the lifecycle (optional, recommended).** `[[=since(N)]]` on an added member and `[[=deprecated(N)]]` on
one about to go let `--diff --compat` tell "compatible with defaults — the defaulted members are all newer than
the writer" from "incompatible — a member the writer should have had is missing". They remain informative in
level 1 (they decide *verdicts*, not decode).

**Nested types.** A nested struct member is projected recursively by *member name*; the nested *type's* name may
differ (R9; decided L3=a: "nested types project by structure, renames reported, not blocking"). So `Order{ Price px; }`
with `Price` renamed `Px` still projects as long as the member is still called `px` and `Px`'s members match by
name. The rename is reported once.

**Variants.** Alternatives are matched by the alternative type's **canonical name** (R11; decided L2=a). A renamed
alternative type therefore needs `renamed_from` on the class (R14). `std::string` matches `std::string` whatever
the allocator (the name is `string` either way). A writer alternative with no match in the reader's variant is R7
for that value (the variant keeps its default — `std::monostate` when it has one, else the first alternative's
value-initialised state — and is reported).

**Enums.** Matched as a member by name; the value is copied with the integer rules on the **underlying kind**
(decided L3=a: "enums by underlying kind"): `u8 → u16` widens, `i32 → u32` does not. Out-of-range values follow
decision D unchanged: for a reader enum **without** a fixed underlying type an out-of-range value is
`invalid_value` (the member is value-initialised, reported); for one **with** a fixed underlying type the value is
kept as it is (a flags enum keeps unknown bits). Enumerator *renames* do not matter to decode (values are copied);
`--diff` reports them.

**Arrays.** `x[N]` → `x[M]`, M ≥ N: the first N elements project, the rest default (R12, kept). M < N is R7.
`std::array`, C arrays and `std::vector<E>` are three different kinds: an array does not project into a vector
or back at level 1 (level 2 territory).

**Containers.** `vector<E1>` ↔ `vector<E2>`, `optional<E1>` ↔ `optional<E2>`, `map<K1,V1>` ↔ `map<K2,V2>`,
`set<K1>` ↔ `set<K2>`: element-wise by the same rules (R10, extended to the 1.6 shapes); the container shape must
be the same (a `vector` does not become a `set`). A key that fails to project (R7) drops that entry and reports.

**What the programmer must not do** (each is detected and reported, never silently wrong):

- give two members of one type the same `renamed_from` name (compile error);
- `renamed_from` a name that is still a member (compile error);
- use `renamed_from` to *change meaning* — the projector cannot know; a renamed member must mean what the old
  one meant;
- rely on projection across a **call-shape** change (different literal, constant or argument count): that is a
  new call and needs a new handler (§2.1 item 5).

### 2.3 The rules (normative, as decided: DECISIONS R-9)

| # | Writer member | Reader member | Action |
|---|---|---|---|
| R1 | `x`, kind K, size S | `x`, kind K, size S | copy (flat leaf) / decode (dynamic) / recurse (struct) |
| R2 | integer `x` (iN or uN) | wider integer, same signedness | widen |
| R3 | unsigned `x` (uN) | signed, strictly wider than N | widen (lossless) |
| R4 | `f32 x` | `f64 x` | widen |
| R5 | `x` | no `x` | skip (writer-only member) |
| R6 | no `x` | `x` | the reader's default (`T{}`, default member initialisers) |
| R7 | `x`, kind K | `x`, another kind or narrower | **not projected**: default kept, reported, decoding continues |
| R8 | enum `x` | enum `x` | copy by underlying kind (R2/R3); out-of-range per decision D |
| R9 | nested struct `x` | nested struct `x` | recurse by member name; nested type names may differ (reported) |
| R10 | `vector<E1>`, `optional<E1>`, `map<K1,V1>`, `set<K1>` `x` | same shape with other element types | per element, by these rules |
| R11 | `variant<A…> x` | `variant<B…> x` | the writer's alternative matched by canonical name, then R1–R10; no match → R7 for that value |
| R12 | array `x[N]` | array `x[M]`, M ≥ N | elements 0…N−1 by these rules, the rest default; M < N → R7 |
| R13 | member `old` | member `x` with `[[=renamed_from("old")]]` | treated as `x` |
| R14 | type `ns::Old` | type `ns::New` with `[[=renamed_from("ns::Old")]]` | matches for R9/R11 and for the top-level argument |
| R15 | string `x` (any allocator) | string `x` (any allocator) | copy — the allocator is not part of the name (decided L2) |

### 2.4 The API (2.3.0)

```cpp
namespace es::serreg {
// annotations (schema string: no; dictionary: yes, as metadata of the member/type)
template<std::size_t N> struct renamed_from { … };           // [[=renamed_from("old")]], repeatable

namespace tools {
struct projection_report {                                   // what one projection did
    std::uint32_t copied{}, widened{}, defaulted{}, skipped{}, conflicts{};
    std::vector<std::string> notes;                          // "Order.qty: u32 → u64 widened", "Order.flags: defaulted (since 3)", …
};
// Fill a T from a writer value described by `writer` (and `dict` for nested types); R7 is not an error.
template<class T>
result<T> project(const dyn::type& writer, const dyn::dictionary& dict, const value_location& where,
                  projection_report* report = nullptr);
struct walk_options { bool project = false; };               // walk_values<Ts…>(…, walk_options{.project = true})
}}
```

- `deserialize(frame, args…)` and `dispatch` gain a dictionary parameter overload (they already have one for
  dynamic images): with a dictionary present and a call whose arguments project, they decode through the
  projection; `decode_error` gains `projected` (how many arguments went through a projection) and the R7 count.
- `frame_router` takes a dictionary (it already can) and routes a projectable call to the handler whose
  signature matches *by projected argument types* when no exact handler exists; `router.handled()` /
  `projected()` count them.
- `frame_walk` gains `projected` and `projection_notes` — one note per distinct `(writer schema hash, T)` pair,
  not per value.
- `srdump --diff --compat old new` (or `--elf` binaries): per type present in both, one of `identical` |
  `compatible` (every reader member copied or widened) | `compatible with defaults` (defaulted members are all
  `since`-newer than the writer) | `incompatible` (an R7 case, or a defaulted member that is not newer); both
  directions reported separately; exit 1 when a type is incompatible in the requested direction.
- `srdump --project` (dump): show what a reader of the `--cxx`-generated or `--dict`-named newer dictionary
  would see — a dry run of the projection over a capture, for operators.

### 2.5 Cost

- Equal layout: zero (the same `memcpy` as today; the check is the layout-hash compare the readers already do).
- Different layout, flat `T`: one table lookup per frame (cached per `(W, T)`), then one copy per member — a
  few ns per member; no allocation after the first frame of a pair.
- Dynamic `T`: the canonical-image decode as today, with the member map consulted per member.
- First frame of a pair: building the plan is O(members), on the heap, once; the plan cache is per process,
  bounded by the number of `(W, T)` pairs met (a few dozen in practice).

### 2.6 What level 1 does not do

- No change to the call's identity: a new literal/constant/argument count is a new message.
- No narrowing, no family change, no `bool`↔integer, no array↔vector, no struct↔scalar.
- No reinterpretation of a member whose *meaning* changed under the same name (impossible to detect).
- No projection without a dictionary (a bare frame stream with no dictionary frame is level 0 — level 3's
  territory, not planned).
- No evolution of the **dictionary format** itself (that is the wire format's own versioning, P-14/P-19).

### 2.7 Implementation status

Shipped (flat `T` read from a flat `W`; `include/es/serreg/projection.hpp`, tests `tests/unit/test_evolution.cpp`):

- `[[=es::serreg::renamed_from("old")]]` on members and classes, repeatable; compile-time checks for a duplicate
  old name and for an old name that is still a member. Reader-side only: neither the schema string nor the
  dictionary carries it yet.
- `tools::projection_report` (`copied`, `widened`, `defaulted`, `skipped`, `conflicts`, `notes`),
  `tools::project<T>(writer, dict, bytes, report*)`, `projection_context{&dict, &report}`,
  `deserialize(dict | projection_context, bytes, outs…)` for compact frames (the writer's CALL from the
  dictionary) and full-records frames; exact arguments beside projected ones; literals and constants must match;
  the call shape must match (§2.1 item 5).
- Rules R1–R9 and R12–R14 (R9 for nested *flat* classes; R12 for arrays of scalars, enums and classes of equal
  layout). One plan per `(W, T)`, built on first use and kept for the life of the process; the per-frame path
  neither allocates nor locks.

Not yet (2.3.0): dynamic members (R10, R11, R15), `frame_router`/`dispatch` matching by projected argument types,
`walk_values<Ts…>` with `walk_options{.project = true}`, `srdump --diff --compat` and `--project`, `decode_error::
projected`, and `renamed_from` recorded in the dictionary (format 1.8).

---

## 3. Level 2 — structural evolution (specified, unscheduled: R-10)

Level 2 is everything that changes the *shape* of a value or a message while a programmer can still state the
mapping. Each item is specified so that level 1 leaves room for it; none is scheduled.

### 3.1 What the reader would guarantee

1. **Member moves and merges with explicit mappings** — `[[=es::serreg::from("legacy.px")]]`-style path
   annotations: a reader member filled from a writer member at another nesting (flattening a nested struct,
   or the reverse). The path is the writer's member path; the projector follows it.
2. **Programmer-supplied conversions** — `es::serreg::convert<T>` customisation point with a per-member or
   per-type function `static bool from(const W_view&, T&)` for the cases level 1 refuses (narrowing with range
   check, `bool`→`u8`, a unit change, an enum re-encoding). The conversion is compiled into the reader; the
   dictionary still drives *where* the bytes are. A conversion that returns `false` is R7 for that value.
3. **Call reshaping** — matching calls by *position and projected argument types* when the literal and constant
   parts agree: an argument added at the end of a signature is defaulted/skipped like a member; an argument
   removed likewise. The call hash still differs; the router consults a *call projection table* built from the
   dictionary (writer call → reader signature) under an explicit opt-in (`router_options{.project_calls = true}`).
4. **Array ↔ vector, optional ↔ value** — `x[N]` ↔ `vector<E> x` (elements), `optional<E> x` ↔ `E x` (an absent
   optional defaults; a present one copies). Both are safe in one direction and defaulting in the other; level 2
   defines the table.
5. **Enumerator renames with values changing** — `[[=es::serreg::enumerator_renamed_from]]`-style metadata so a
   value whose *name* survived but whose *number* changed maps by name. (Values are copied by number at level 1.)
6. **Variant alternative reshaping** — an alternative that changed type (not only name) mapped through item 2.

### 3.2 What the programmer would have to do

Everything level 1 asks, plus explicit statements for every structural change: a `from(path)` annotation per moved
member, a `convert<>` specialisation per non-widening conversion, an opt-in per router for call projection. Level
2 never infers a structural change; it executes the ones the programmer wrote down. The CI gate
(`--diff --compat`) would list every member that *needs* a statement and has none.

### 3.3 Why it is unscheduled

Levels 0 and 1 cover the capture-replay and rolling-upgrade cases the project's users have named (R-11: logs and
market data). Level 2's mappings are code the reader must carry per version pair; that is a maintenance model
(explicit migrations) rather than a type-system one, and the project has not seen the user who needs it. The
specification exists so that R-9's `renamed_from` and `projection_report` shapes stay compatible with it (the
report is where level 2's "needs a statement" lines would go).

---

## 4. Level 3 — dictionary-less evolved reads (not planned: R-10)

### 4.1 What it would mean

A typed reader decoding an **evolved** frame with **no dictionary at all** — a bare pipe carrying only frames —
would require the writer to put projection metadata (member names, kinds, offsets) on every frame or in every
stream head, i.e. a self-describing wire (tag-length-value or a per-frame schema), a format minor, and a second
decode path.

### 4.2 Why it is not planned

The library's answer to "no dictionary" is to make sure there always is one: every stream carries its dictionary
(the dictionary frame at the head of a bare stream, deltas when the registry version moves — R-7; the forward
chain in files — P-9; a capture's dictionary is complete by construction). Level 3 would pay bytes on every frame
for a case the container already covers at zero per-frame cost. A consumer that truly has nothing but frames uses
`srdump --dict`/`--elf` with the writer's binary or `.rri` — the dictionary exists somewhere, and the tools accept
it from anywhere.

---

## 5. The levels side by side

| | Level 0 (today) | Level 1 (2.3.0, R-9) | Level 2 (unscheduled) | Level 3 (not planned) |
|---|---|---|---|---|
| Typed reader on a changed type | `hash_mismatch` | projects by member name | projects by explicit mapping | would project without a dictionary |
| Rename a member | breaks typed reads | `renamed_from` | same | — |
| Add / remove a member | breaks | default / skip | same | — |
| Widen an integer or float | breaks | widened (never narrowed) | `convert<>` may narrow with a check | — |
| Reorder members | breaks | free (layout differs → projection) | same | — |
| Rename a type | breaks | `renamed_from` on the class, or `name` | same | — |
| Nested type renamed | breaks | by structure, reported | same | — |
| Variant alternative renamed | breaks | by canonical name (`renamed_from`) | retyped alternatives via `convert<>` | — |
| Enum values | copied when hashes match | copied by underlying kind; range per decision D | renamed-and-renumbered by name | — |
| Array grew | breaks | first N elements, rest default | array ↔ vector | — |
| Call shape (literal, constant, arity) | breaks | breaks (new message) | projected by position, opt-in | — |
| Needs a dictionary | tools yes, typed no | yes | yes | no (metadata per frame) |
| Wire change | — | none | none | a format minor |
| Programmer's duty | nothing | names are the contract; `renamed_from`; widen only | + explicit `from(path)` / `convert<>` per change | — |
| Tool support | `--diff --direction` | `--diff --compat`, `--project`, `projection_report` | "needs a statement" lines | — |
| Cost on the hot path | none | none when layout equal; a cached plan otherwise | user conversions | per-frame metadata |

---

## 6. Checklist for a type that must evolve well (today, for level 1 tomorrow)

1. Give every type a stable canonical name — `[[=es::serreg::name("…")]]` for anything in an unnamed namespace,
   local, or likely to move.
2. Treat member names as part of the protocol: never reuse one for a different meaning; rename with
   `renamed_from` (2.3.0) or not at all.
3. Grow integers and floats only in the widening directions of §2.2; a shrink or a family change is a new member.
4. Mark additions with `since(N)` and removals-to-be with `deprecated(N)`; keep `skip` for what must never be on
   the wire.
5. Keep the call shape stable: literals and constants are the message's identity; add data as members of a struct
   argument, not as new arguments.
6. Ship the previous release's `.rri`; run `srdump --diff old.rri ./new --direction both` in CI; read
   `metadata-only` as "level 1 will absorb this" and `breaking` as "a new type/literal or a level-1 widening".
7. For plugins: a changed layout is a new schema hash beside the retained old one — both decode; nothing to do.
