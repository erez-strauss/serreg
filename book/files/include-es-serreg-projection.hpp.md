# `include/es/serreg/projection.hpp`

Schema evolution **level 1 for flat types** (DECISIONS R-9; `docs/schema-evolution-specs.md` §2). A typed reader
that meets a frame whose argument type is not its own `T` but *matches it by canonical name* — the writer's
`evo::Order` for the reader's `evo::Order`, or an old name given by `[[=es::serreg::renamed_from("md::Quote")]]`
on `T` — projects the writer's value into `T` **member by member, by name**, from the writer's type description
in a dictionary: same kind copied, integers and `f32` widened, writer-only members skipped, reader-only members
left at their `T{}` value, anything else kept at that default and reported. The scope is a flat `T` read from a
flat writer type: a type with a dynamic member keeps the exact-hash path of `deserialize.hpp` (level 0). The plan
for a (writer type, `T`) pair is built once, on the heap, the first time the pair is met and kept for the life of
the process; the per-frame path neither allocates nor locks. The annotations are reader-side metadata: they change
neither the schema string nor the dictionary. It is included by `serreg.hpp`.

Includes: `arg.hpp` (`is_literal_v`, `is_constant_v`, `is_string_arg_v`, `arg_hash`), `descriptor.hpp`
(`type_desc`, `field_desc`, `kind`, `kind_size`, `is_scalar_kind`), `deserialize.hpp` (`read_frame_header`,
`get_record`, `record_payload_size`, `decode_result`, `decode_error`), `dictionary.hpp` (`dyn::type`,
`dyn::field`, `dyn::call`, `dyn::dictionary`), `error.hpp`, `format.hpp` (the frame and record header sizes,
`align8`), `reflect.hpp` (the parts layer, `descriptor_v`, `layout_hash_v`, `scalar_kind`, `members`,
`is_std_array_v`); `<array>`, `<atomic>`, `<cstddef>`, `<cstdint>`, `<cstring>`, `<memory>`, `<meta>`, `<span>`,
`<string>`, `<string_view>`, `<type_traits>`, `<utility>`, `<vector>`.

| name | meaning |
|---|---|
| `renamed_from<N>` (`projection.hpp:46-58`) | the annotation: `[[=es::serreg::renamed_from("px")]]` on a member (frames whose writer called it `px` fill this member), `[[=es::serreg::renamed_from("md::Quote")]]` on a class (frames written as `md::Quote` project into it). Repeatable. A string NTTP class like `name<N>`; `view()` gives the text |
| `tools::projection_report` (`projection.hpp:65-77`) | what a plan does, counted per reader member: `copied`, `widened`, `defaulted`, `skipped`, `conflicts`, and `notes` — one line per member that is not a plain copy, `"evo::Order.qty: u32 -> u64 widened"`, `"evo::Order.flags: defaulted"`, `"evo::Order.id: i64 -> u64 not projected (R7)"`, `"evo::Order.legacy: writer-only member skipped (R5)"`. `add(o)` sums the counters (not the notes) |
| `tools::project<T>(writer, dict, bytes, report = nullptr)` (`projection.hpp:566-588`) | fills a `T` from the bytes of a writer value whose type is `writer` (its `dyn::type`, from `dict`): `hash_mismatch` when the writer's type is not `T` by name, `truncated` when fewer than `writer.size` bytes are given; R7 members are not an error — they keep their default and are counted. The counters are added to `*report` on every call, the notes only by the call that built the plan |
| `projection_context {dict, report}` (`projection.hpp:592-596`) | the dictionary a projecting reader resolves writer types through, and where it counts what it did |
| `deserialize(projection_context, bytes, outs...)`, `deserialize(const dyn::dictionary&, bytes, outs...)` (`projection.hpp:792-817`) | style 1 with a dictionary: tries the exact path first — a compact frame whose call hash is the reader's, or a full-records frame — and falls back to `detail::deserialize_projecting` when the exact read fails with `hash_mismatch` (a full-records frame) or the call hash differs (a compact frame). Literals and constants must match exactly; every type argument either matches exactly or is a flat type that projects into the reader's by name |
| `detail::old_names_of<M>()`, `rename {member, old}`, `collect_renames<T>()`, `renames_v<T>`, `type_old_names_v<T>` (`projection.hpp:84-169`) | the compile-time tables of a type's `renamed_from` annotations: per member (`members<T>()[I]`'s annotations whose type is a `renamed_from<N>` specialisation) and on the class. `make_renames` `static_assert`s that two members do not claim one old name and that no old name is a current member |
| `detail::proj_step {src, dst, sk, dk, n, sstride, dstride}`, `projection_plan {writer_hash, writer_size, matched, steps, report}` (`projection.hpp:172-185`) | one plan: a table of leaf copies — the writer's byte offset, the reader's, the two kinds (`struct_`: a `memcpy` of `sstride` bytes), `n` elements and their strides. `matched` is false when the writer's type is not `T` by name: the plan is then a cached `hash_mismatch` |
| `detail::int_rank`, `signed_int_kind`, `unsigned_int_kind`, `leaf_rule(sk, dk)` (`projection.hpp:187-231`) | 0 = not projectable (R7), 1 = copy (R1), 2 = widen (R2/R3/R4): a signed integer into a wider signed one, an unsigned into a wider unsigned, an unsigned into a *strictly* wider signed one, `f32` into `f64`. `bool`, characters and `std::byte` copy only; a narrowing or a change of family is 0 |
| `detail::convert_leaf(dst, src, sk, dk)` (`projection.hpp:233-260`) | one leaf under a non-zero rule: a `memcpy` (with the `bool` repair of `copy_flat`) when the kinds are equal, `float` → `double` through the values, otherwise the integer sign- or zero-extended in a `uint64_t` and its low bytes stored (little-endian host) |
| `detail::find_writer_field(w, name, renames)`, `find_reader_field(d, name)`, `plan_note`, `kinds_text` (`projection.hpp:262-295`) | the name match: a writer field by the reader's name, else by an old name of that member |
| `detail::plan_leaf`, `plan_array<E>`, `plan_member<R>`, `plan_class<T>` (`projection.hpp:298-487`) | the plan builder, below |
| `detail::name_matches<T>(w)`, `build_plan<T>(w, dict)` (`projection.hpp:489-509`) | `w.name == descriptor_v<T>.name` or one of `type_old_names_v<T>`; a match also needs `w.is_flat`, not an enum, not a transparent type |
| `detail::plan_cache<T>` (`projection.hpp:512-536`) | the plans of `T`, one per writer schema hash, in a lock-free singly linked list (`std::atomic<node*> head`, CAS push); `get(w, dict, built)` finds or builds, `built` tells the caller its notes are new. Never freed (callers hold the pointers; one plan per writer version that was ever met) |
| `detail::apply_plan(p, out, src)` (`projection.hpp:539-551`) | `out = T{}` (the default member initialisers apply: R6), then every step: `memcpy` for `struct_`, `convert_leaf` per element otherwise. `noexcept`, no allocation |
| `detail::projectable_v<T>` (`projection.hpp:554-557`) | a reader argument that can be projected: a class, `flat_type`, no `as_flat`, not a literal, constant or string argument, default-constructible |
| `detail::deserialize_projecting(ctx, bytes, outs...)` (`projection.hpp:600-785`) | the two-pass reader: pass 1 resolves every record — exact (the writer's hash is `arg_hash<T>()`) or a plan — and checks every length before anything is written (G2-1: the outputs stay untouched when the frame does not fit); pass 2 writes them, exact records through `get_record`, projected ones through `apply_plan`. A compact frame needs the writer's CALL entry in the dictionary (`unknown_call`; the argument count must agree, `length_mismatch`), a full-records frame carries the hashes and lengths itself (a projected record's length must be the writer's `size`). A writer type the dictionary lacks is `unknown_type`; a frame with the `meta` flag is `hash_mismatch` |

## How the plan is built

`plan_class<T>` walks the reader's descriptor in parts order — bases first, as `<base:Name>` pseudo-fields matched
by that name, then every kept part by `part_name` — and for each one looks the writer's field up by name
(`find_writer_field`: the current name, then the member's `renamed_from` names). `plan_member<R>` dispatches on
the reader member's C++ type:

| rule | what `plan_member` does |
|---|---|
| R1 same kind | `plan_leaf` with `leaf_rule == 1`: a `proj_step` copy |
| R2 / R3 / R4 widen | `leaf_rule == 2`: `iN → wider iN`, `uN → wider uN`, `uN → strictly wider iN`, `f32 → f64`; counted in `widened`, noted |
| R5 writer-only member | after the walk, every writer field that is neither a reader name nor an old name: `skipped`, noted |
| R6 reader-only member | no writer field: `defaulted`, noted; the value is what `T{}` gives it (`apply_plan` starts from `T{}`) |
| R7 anything else | narrowing, a change of family, `bool` ↔ integer, an enum against a non-enum, a writer type missing from the dictionary, a nested writer type that is not flat, an array that shrank, a multi-dimensional array (a `static_assert` on the reader side): no step, `conflicts`, a note ending in `(R7)` |
| R8 enums | by underlying kind: the writer's `dyn::type::underlying` against `scalar_kind<underlying_type_t<E>>()`, through `leaf_rule` — so an enum that grew its underlying type widens, and the values are not checked against the enumerators |
| R9 nested flat classes | a nested writer type with the reader member's layout hash is one `memcpy` step (R1 for the whole object); otherwise, when it is flat, `plan_class<U>` recurses by member name (noted when the nested type's name differs) |
| R12 arrays | `plan_array<E>`: the writer's `N` elements into the reader's `M ≥ N` (one step with `n = N` and the two strides, `leaf_rule` on the element kind; an array of classes needs an equal layout); `M > N` is noted ("the new elements are defaulted") |
| R13 `renamed_from` on a member | the second lookup of `find_writer_field`; `renames_v<T>` is checked at compile time for duplicates and for a name the type still has |
| R14 `renamed_from` on a class | `name_matches<T>` accepts the old names in `type_old_names_v<T>` |

R10 (containers element by element), R11 (variants by alternative name) and R15 (strings) concern dynamic
members and are level 2; a member that merely moved, or whose neighbours' padding changed, needs no rule: every
step carries both offsets, so it is found by name wherever it is.

**Costs.** One plan per (writer schema hash, `T`), built with the dictionary the first time such a frame is met
(a few allocations, a CAS); from then on a frame costs a list walk to find the plan plus one `memcpy` or
`convert_leaf` per step. `deserialize(dict, …)` of a frame whose call hash is the reader's own is the plain
`deserialize` and costs nothing more than before.

**Out of scope at level 1** (`docs/schema-evolution-specs.md` §2.6, §3): dynamic members (a `T` with a string, vector,
optional, variant, map or set is level 0: exact hash only — `projectable_v` requires `flat_type`); `frame_router`
and `dispatch` (they match by hash and have no dictionary argument); a typed `walk_values<Ts...>` match by name
(it matches by schema hash and walks a changed type generically); `srdump --diff --compat` (a verdict that applies
these rules to two dictionaries); and `renamed_from` in the dictionary (the annotation stays on the reader side,
so a dictionary-only reader cannot see it).

Tests: `tests/unit/test_evolution.cpp` — "R-9 level 1: an old Order projects into the new one by member name
(widen, default, rename, R12)" (the `evo_old::Order` → `evo::Order` case in compact and full-records frames: widened
`qty` and `weight`, renamed `px`, defaulted `flags` and `venue` with its initialiser, the array that grew, the
skipped `legacy`; level 0 still refuses the frame; the notes arrive with the first frame only), "R-9 level 1:
tools::project<T> and the R7 rule" (`i64 → u64` is not projected and keeps the default, the note text, a type of
another name is `hash_mismatch`, too few bytes `truncated`) and "R-9 level 1: renamed_from on a class, exact
arguments beside a projected one, call-shape rules" (`evo::NewName` from `evo::OldName` next to a literal and a
`u32`; another literal or argument count is refused; the exact path through the dictionary overload). The writer
types live in `evo_old` under `[[=es::serreg::name("evo::Order")]]`, so both versions exist in one process as an
old and a new build of one program would.
