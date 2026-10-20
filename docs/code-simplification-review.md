# Code review — simplification candidates

**Scope:** the library headers (`include/es/serreg/`), the runtime (`src/`) and the registry, read for duplication,
dead code and structure that could be simpler without changing the wire format, any hash, or the public API.
Reviewed 2026-10-13 against the 2.2.3 tree. Nothing here is a bug; each item is a candidate for the next
patch release, ordered by value over risk. Items marked **done** were applied in 2.2.3.

## 1. Duplicated code

| # | What | Where | Proposal | Risk |
|---|---|---|---|---|
| S-1 | The 40-byte RRI header is written three times, field by field | `src/registry.cpp` (`export_image_since`, `export_image_scope`), `src/dictionary.cpp` (`encode_rri`) | one `internal::put_rri_header(fmt::writer&, flags, n_entries, program_hash, abi)` in `src/internal.hpp`; the three call sites pass their own program hash and ABI | none: bytes locked by `orders.rri`, the RRI fuzz target and `runtime_selftest` |
| S-2 | `base_t<T,I>` / `member_t<T,I>` aliases were spelled out as `typename[:std::meta::type_of(bases<T>()[I]):]` at many sites | `reflect.hpp`, `registry.hpp` | **done** (the parts layer: `base_t`, `part_t`, `part_name`, `get_part`, `part_annotation`); the remaining literal splices are in the gap/bool collectors (S-3) and `collect_enum_ranges` | — |
| S-3 | `make_gaps<T>()` and `make_bools<T>()` each walk the type twice (count, then fill) through their own recursive collectors | `reflect.hpp` ~1825–1855 | one collector that returns a `std::vector`, frozen once with `std::define_static_array` (as `make_fields` does), for both tables; the enum-range collector can share it | low: the tables are checked by the scrub tests and the golden blobs |
| S-4 | `dictionary_frame_bytes` and `meta_frame_bytes` build the 16-byte frame header by hand | `capture.hpp` 110–140 | call `put_frame_header` (the writer's own helper) | none: frame bytes locked by the golden fixtures |
| S-5 | `abi_long_text` (public header) and `internal::abi_text` (runtime) render the same ABI tag | `capture.hpp:143`, `src/internal.hpp:61` | keep one; the header's version is the one users can call, so `internal::abi_text` becomes a thin forward (or the header's moves to `format.hpp` as `fmt::abi_text`) | none |
| S-6 | `is_signed_kind(kind)` is defined in two tools and the same predicate exists in `projection.hpp` (`signed_int_kind`) | `src/wireshark.cpp:83`, `src/cxxgen.cpp:217`, `projection.hpp` | one `constexpr bool is_signed_kind(kind)` next to `is_scalar_kind` in `descriptor.hpp` | none |

## 2. Dead or vestigial code

| # | What | Where | Proposal |
|---|---|---|---|
| D-1 | `capture_file::start_ns_` is set and never read | `capture.hpp:311, 607` | remove (keep `last_ckpt_ns_`) |
| D-2 | `reg_node::refs` is "unused since 2.1.13, kept for layout" | `registry.hpp:84` | the node is TU-local and never on the wire: remove the field and the comment at the next ABI-breaking release (2.3.0, G-2); until then leave it |
| D-3 | `scope_hash_v<Scope>` lives in `detail` but `capture.hpp` and user code that names a scope's hash need it | `registry.hpp:287` | move to `es::serreg` (keep a `detail` alias for one release) |
| D-4 | ~210 version-tagged comments (`// 2.1.14 (review C-03): …`) in the headers and runtime | `include/`, `src/` | they are history, not explanation; replace each with the DECISIONS ID it corresponds to (`// DECISIONS P-3`) or drop it when the code is self-explanatory — the book and CHANGELOG keep the history. A mechanical pass; do it in one commit with no code change so the diff is reviewable |

## 3. Structure

| # | What | Proposal | Risk |
|---|---|---|---|
| T-1 | `reflect.hpp` is 2,700 lines: naming (canonical names, vocabulary), layout (fields, offsets, gaps, bools, enum ranges), strings/hashes, images (measure/encode/decode), descriptors | split into `detail/names.hpp`, `detail/layout.hpp`, `detail/images.hpp` included by `reflect.hpp` (the public surface does not move). The Compiler Explorer examples symlink individual headers, so each split adds symlinks in `ce-examples/*/es/serreg/detail/` | medium: no behaviour change, but every book citation into `reflect.hpp` moves; do it right after a release with the book pass in the same commit |
| T-2 | `formatters.cpp` (1,500 lines) holds text, JSON and TOML rendering with three near-identical walkers | one walker with a small renderer interface (`begin_struct`, `field`, `scalar`, …) and three renderers | medium: output locked by the golden `orders.{text,json,toml}` |
| T-3 | The three class walks in `reflect.hpp` (measure / encode / decode) "mirror each other exactly" and are kept in sync by hand | a single `for_each_part`-driven visitor with a mode parameter, as `walk.hpp` does for the dictionary side | medium: the images are locked by the golden fixtures and `blobs.cpp`; a wrong step is caught, but the refactor is large |
| T-4 | `projection.hpp` builds its plan from the reader's parts at compile time *and* the descriptor's `field_desc` at run time (two views of the same members) | when the parts layer exposes the member's canonical offset directly (`canonical_offsets_v` already exists), the `find_reader_field` lookups go away | low |

## 4. Diagnostics and names

- The deleted-overload messages and `static_assert` texts no longer carry version numbers (**done**).
- `error::hash_mismatch` is returned both for "wrong call" and "a type that does not project"; a reader of
  `decode_error` cannot tell them apart. Proposal (2.3.0, with `decode_error::projected`): `error::no_projection`
  for the second case. Not changed now because the error enum is part of the ABI (G-2).

## 5. What was checked and found fine

- No allocation or lock on the hot paths of `serialize`, `deserialize`, `dispatch`, the router, the logger and the
  rings (the projection plan cache is a lock-free list; building a plan allocates once per pair, off the hot path).
- `std::expected` everywhere a call can fail; no exceptions, no RTTI; `const` parameters; `constinit` globals.
- Every public header is self-contained (`tests/headers`, `scripts/check-includes.py`).
- The registry's static-initialisation order (guards, lazy index publication, retained entries) is as described in
  `docs/serreg-registry.md`; nothing found to simplify there without touching the loader contract.
