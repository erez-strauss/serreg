# Plans for 2.2.3 (and what moves from 2.3.0) — for the maintainer's review

*Written 2026-10-12 against library 2.2.2, wire format 1.7. Nothing here is implemented yet. Mark each item
**yes / no / later**. The marked list becomes the 2.2.3 work list, and the decisions go into `docs/DECISIONS.md`.
Sizes: **S** is under half a day, **M** about a day, **L** several days. "Blocker" means a rule in `DECISIONS.md`
that keeps an item out of a patch release unless the maintainer decides otherwise.*

Sources: `docs/review-response-2.2.2.md` (D and C items), `docs/schema-evolution-specs.md` (E1–E2),
`docs/values-validation.md` (E4–E5), and this week's questions and probes (N and S items, remove-me).

## 1. Process and documents (from the 2.2.2 review response)

- **D1 — Retarget A-16** (S, no blocker)
  Split the ledger entry into A-16a (dictionary-driven enum range check), A-16b (list/deque → S1) and A-16c
  (`canonical_unordered`); its status line stops saying "2.2.2".
- **D2 — One version string + `scripts/check-versions.py`** (S, no blocker)
  `bump-version.sh` also rewrites the version in the five flattened docs and the comments. A new checker, run by
  `run-all-tests.sh`, compares every "library X.Y.Z" in docs, book, README and SECURITY against CMake.
- **D3 — `SECURITY.md` corrections** (S, no blocker)
  State that readers accept format minors 6–7, and name the two allocation points whose failure terminates
  (`registry::push` and the first `snapshot()` publish).
- **D4 — `docs/threat-model.md` (planned; not written — the threat model stayed in `docs/SECURITY.md`)** (S, no blocker)
  One screen. One trust domain; hashes identify and CRCs detect accidents, but neither authenticates; limits bound
  the work; the code generators and `ES_SERREG_CONFLICT=abort` are outside the never-crash contract; sign the file
  to authenticate it.
- **D5 — `ipc-design.md` §5.2/§7 wording** (S, no blocker)
  The gate is published with a plain release store; only the ownership word changes by CAS.
- **D6 — `docs/deviations.md`** (S, no blocker)
  Retitle it to the current version, delete the stale sentence about the fixed-underlying-type flag, and keep the
  A-16a note until C1 lands.
- **D7 — `error.hpp` text for `invalid_value`** (S, no blocker)
  Describe what the code does in each of the three cases: valueless variant, out-of-range enum leaf, and a
  dynamic record that fails mid-image.
- **D8 — G-15 vs A-15 in DECISIONS** (S, no blocker)
  A-15 (hidden visibility) gets a line of its own for 2.3.0, and G-15's list of 3.0 items drops it.
- **D9 — Scoped dictionaries lead the logging examples** (S, no blocker)
  The book, README and story §3 open with `scoped_capture_file<Scope>` and show `log_options::floor` /
  `ES_SERREG_LOG_MIN_LEVEL`.
- **D10 — `srdump --diff` in CI as the evolution policy until level 1** (S, no blocker)
  A story with the exact CI job (`srdump --diff app-vN.rri ./app --direction both`, where the exit status is the
  verdict), plus the same in README.
- **D11 — `PROVENANCE.md`** (S, no blocker)
  Records which code and documents were written with an assistant and reviewed, by version, from 2.2.3 on. G-9
  becomes "recording"; G-7 (the license) stays open (Q9).
- **W1 — `docs/wire-format-1.7.md` (planned; not written — the wire contract is `format.hpp` and book chapter 7)** (M, no blocker)
  The byte layout ("native mode, as implemented") taken out of `format.hpp` comments, `python/serreg/format.py`
  and architecture §6 into one document. This session's frame and dictionary dumps are its examples.

## 2. Code from the review response (small, no wire change)

- **C1 — A-16a: enum range check in the dictionary readers** (M, no blocker)
  In `src/decoder.cpp` and the Python reader, a non-enumerator value of an enum without a fixed underlying type
  is `invalid_value`, and `srdump --verify` fails on it. Golden test with a crafted out-of-range frame.
- **C2 — A-16c: `serialize_options::canonical_unordered`** (S, no blocker)
  The default stays sorted, byte-identical to `std::map`. `false` writes iteration order, with no sort and no
  allocation; the writer-info frame already records the choice.
- **C3 — honest `serialize_noexcept`** (S, no blocker)
  It becomes `false` where a plain sink with CRC may take the heap branch; the rule is documented in
  `serialize.hpp` and requirements §13.
- **C4 — `char` rendering follows the writer's ABI tag** (S, no blocker)
  `srdump` and the Python reader render `char` signed or unsigned by the tag bit. Fixture: `char-unsigned` with a
  0x80 byte renders 128.
- **C5 — Python reader banner and depth 64** (S, no blocker)
  `--verify` prints "hashes not recomputed". `MAX_DEPTH` becomes 64, as in C++.
- **C6 — unknown frame flags: the walker skips, typed readers refuse** (S, docs only)
  The two behaviours are stated side by side in the requirements and the threat model.
- **C7 — the typed check rejects a non-zero full-record layout id** (S, no blocker)
  A one-line check plus documentation; current writers store 0.
- **C8 — A-13 as a documented limit** (S, docs only)
  About 80 bytes per entry per publish until exit; `snapshot_guard` stays 3.0.
- **C9 — `forget_retained()` contract written into A-14** (S, docs only)
  Specified now, built in 3.0.
- **C10 — `run-all-tests.sh` reports disabled Lua tests as SKIP with a reason** (S, no blocker)
  A log that did not run the Wireshark Lua tests then says so in its summary.
- **C11 — `'c'_` character constant** (S, no blocker)
  Added next to `k<'c'>`. The `--cxx` map choice and the RRI-only `--dict` are documented, not changed.

## 3. New since the review (this week)

- **N1 — public `schema_string_v<T>` / `layout_string_v<T>`** (S, no blocker)
  The exact strings behind `schema_hash_v` / `layout_hash_v`, as static strings, with
  `static_assert(fingerprint64(schema_string_v<T>) == schema_hash_v<T>)` in the golden tests.
- **N2 — rebuild the canonical string from a dictionary ENTRY** (M, no blocker)
  `dyn::dictionary` recomputes each type's schema and layout strings from its FIELDs and checks them against the
  stored hashes (`srdump --verify`); `srdump --schema-strings` prints them. The Python reader cannot check them
  (no XXH3 in its standard library) but can print them.
- **N3 — small corrections found this week** (S, no blocker)
  The `src/internal.hpp` comment "exactly format 1.3" (the code accepts 1.6–1.7). Slide 21's title claims
  `template for` over members; the library uses index sequences. The fix covers the content doc and both decks.
- **N4 — dictionary-delta example and fixture** (S, no blocker)
  This week's `dictdemo`: a stream with a full dictionary frame then a delta, and a file with a CHAIN-linked
  delta and checkpoints with full dictionaries. It becomes `examples/dictionary_deltas/` plus a golden fixture
  (it needs two small plugins).

## 4. Standard containers and std types — probe results and proposals

### 4.1 Finding: std types are accepted by walking library internals

A probe of about 30 std types on GCC 16 / libstdc++ (2026-10-12) shows that some std classes outside the frozen vocabulary are
accepted. They pass because reflection walks the library's private members:

| Type | What ends up in the schema string |
|---|---|
| `std::tuple<int, std::string>` | `{<base:std::_Tuple_impl<0,i32,std::string>>:dyn<…>}` |
| `std::bitset<64>` | `{<base:std::_Base_bitset<1>>:struct<…>}` |
| `std::chrono::nanoseconds` | `{__r:i64@0[8,8];}` |
| `std::flat_multimap<int,int>` | `{<base:std::_Flat_map_impl<…,true>>:dyn<…>}` |
| `std::priority_queue<int>` | `{c:vector<i32>@0[8,4];comp:struct<…>@8[1,1];}` |

These schema hashes depend on libstdc++'s internals. libc++ names and arranges those members differently, so the
same source gets a different hash on the clang lane, which breaks decision B's promise of the same hash on every
library. They may also change with a libstdc++ upgrade, and a reader built without libstdc++ (Python, or another
standard library) cannot interpret the fields.

Support is also inconsistent. `std::tuple<int, std::string>` is accepted as a "dynamic class" because a member
deep inside is a `std::string`, while `std::tuple<int, double>` and `std::pair<int,int>` are rejected as
non-trivially copyable. `std::complex<double>` is rejected with a misleading message ("descriptors exist for
class and enum types only"), because libstdc++ stores it as a built-in `__complex__ double`.

### 4.2 What is supported today

| Supported | Notes |
|---|---|
| `std::string` (any allocator), `std::vector`, `std::optional`, `std::variant`, `std::array` (also of dynamic `T`) | v2.0 |
| `std::map`, `std::unordered_map`, `std::flat_map`, `std::set`, `std::unordered_set`, `std::flat_set` | 2.2.0, format 1.6; one `map<K,V>` / `set<K>` wire shape |
| `std::monostate` (as a variant alternative), `std::byte` | |

### 4.3 Rejected today

| Type | Reason |
|---|---|
| `std::deque`, `std::list`, `std::forward_list` | not trivially copyable, not in the vocabulary (A-16b) |
| `std::multimap`, `std::multiset`, `std::unordered_multimap`, `std::unordered_multiset` | same |
| `std::pair`, `std::tuple` of flat elements | not trivially copyable |
| `std::wstring`, `std::u8string`, `std::u16string`, `std::u32string` | only `basic_string<char>` is a dyn member |
| `std::stack`, `std::queue` | not trivially copyable |
| `std::valarray`, `std::unique_ptr`, `std::expected` | not trivially copyable |
| `std::complex` | `__complex__` member; misleading message |
| `std::span`, `std::string_view` | non-owning; correctly rejected |

### 4.4 Proposals

None of the following needs a new wire kind. Each maps onto an existing shape (vector, map, set, optional,
variant, struct), so they are reader-compatible with format 1.7. They do change schema strings; S12 changes
bytes for some types (Q2).

- **S1 — `std::deque`, `std::list`, `std::forward_list` as the `vector` shape** (S, no blocker; = A-16b)
  The schema kindspec is `vector<E>`, so a list written by one program reads back as a `std::vector` in another.
  `forward_list` has no `size()`, so the writer counts it while measuring.
- **S2 — `std::multimap`/`multiset` and the unordered and flat multi-containers as the `map`/`set` shape**
  (M, needs Q3)
  Canonical order is by key, then by value bytes, so equal values give equal bytes. The kindspec is
  `multimap<K,V>` / `multiset<K>`, so the hash tells them apart; the FIELD kind stays `map_`/`set_`, so old
  readers decode them (keeping duplicates in a vector-of-entries view).
- **S3 — `std::pair` and `std::tuple` as positional structs** (M, needs Q2)
  The schema becomes `struct<std::tuple<i32,f64>>{_0:i32@0…;_1:f64@8…}` (`first`/`second` for `pair`), laid out
  canonically and copied element-wise with `std::get<I>`, never by `memcpy`: libstdc++ stores tuple elements in
  reverse order and libc++ forward. Flat element sets then work and dynamic ones keep working, with portable
  hashes.
- **S4 — `std::chrono::duration` and `std::chrono::time_point` in the vocabulary** (S, needs Q2)
  Encoded as their `rep` (`i64` for `nanoseconds`), with the period in the name
  (`std::chrono::duration<i64,std::ratio<1,1000000000>>`, already the canonical name). The member name `__r`
  leaves the schema. Very common in trading messages.
- **S5 — `std::bitset<N>` in the vocabulary** (S, needs Q2)
  Encoded as `array<u8, (N+7)/8>`, little-endian bit order, built with `test()`/`set()` so it is independent of
  the storage word.
- **S6 — `std::complex<T>` as `array<T,2>`** (S, no blocker)
  The standard guarantees the `{real, imag}` array layout ([complex.numbers]); the misleading message disappears.
- **S7 — `basic_string<char8_t | char16_t | char32_t | wchar_t>`** (S, no blocker)
  The `vector<char16>`-style shape with kindspec `u16string` and so on; `wchar` keeps its ABI-tag width.
- **S8 — `std::unique_ptr<T>` as the `optional` shape** (S, needs Q4)
  An owned, nullable value: null maps to an empty optional. `shared_ptr` stays rejected because its sharing would
  be lost.
- **S9 — `std::expected<T, E>` as a two-alternative variant shape** (S, no blocker)
  Kindspec `expected<T,E>`; index 0 is the value, index 1 the error.
- **S10 — `std::stack`, `std::queue`, `std::priority_queue` as their underlying container** (S, no blocker)
  They are read and written through the protected container member `c`, which reflection reaches. The comparator
  of `priority_queue` is not encoded (like `std::map`'s).
- **S11 — `std::valarray<T>` as the `vector` shape** (S, no blocker)
- **S12 — close the black box: reject every other `std` class with a named diagnostic** (S, needs Q2)
  "`std::X` is not in the serialisable vocabulary; use `as_flat<T>` or specialise `type_properties<T>`." This
  fixes 4.1: `std::_Tuple_impl`, `__r` and the like never reach a schema again. It changes hashes and/or bytes of
  the five accidental types above; S3–S5 and S10 give them proper replacements.

## 5. 2.3.0 candidates — can they move into 2.2.3?

- **E1 — evolution level 1, reader side (projection by member name)** (L; blocker: G-15 assigns it to 2.3.0 → Q1)
  Typed readers and `walk_values` project the writer's type into the reader's `T` by member name: widening,
  defaults, `renamed_from` on the reader's type, `projection_report`, and `srdump --diff --compat` for compiled
  types. No wire or dictionary change, and API additions only, so `SameMinorVersion` holds. The cost is size: it
  touches deserialize, dispatch, the router and the walker, plus a Python parity question (Q6).
- **E2 — `renamed_from` recorded in the dictionary** (M; blocker: a new dictionary record is a format change
  → 1.8 → P-20/G-2 make it 2.3.0)
  Needed only for offline `--diff --compat` between two dictionaries that saw a rename. 1.7 readers would skip the
  record, but a format change bumps the minor by decision.
- **E3 — A-15: hidden visibility for `libserreg` plus an exported-symbol ctest** (M; blocker: an ABI-surface
  change is a minor by G-2)
  Removes the export of every symbol not marked `ES_SERREG_API`. A consumer that linked an internal symbol would
  break inside the same SONAME, so it stays in 2.3.0.
- **E4 — validation hints (`range`, `max_size`, …) and per-side policies** (L; blockers: the
  `tag_constraints` dictionary record is a format change → 2.3.0 or later; open questions Q1–Q6 of
  `values-validation.md`)
  The annotation and policy half has no format impact. A typed-only version, with constraints not recorded in
  the dictionary, could ship in 2.2.3 if the maintainer wants it early (Q5).
- **E5 — `closed` enum hint** (S; blocker: a new enum flag bit is a format change, and whether 1.7 readers ignore
  unknown enum flag bits is unverified)
  It goes with E4.

**Summary:** E1 and the typed-only half of E4 have no technical blocker, only the recorded release plan. E2, E3,
E5 and the dictionary half of E4 are blocked by the versioning rules and remain 2.3.0, which would then carry
format 1.8: renamed-from and constraint records, the enum flag, and visibility.

## 6. Verification items for the 2.2.3 report

- **V1** — the full run on this sandbox and on the review's container profile (14 CPUs, Lua present), with the
  Lua tests listed.
- **V2** — the Compiler Explorer lanes for the 1.7 corpus (`gcc` and `clang-libcxx`) through `ce-run.py`; the
  sandbox reaches godbolt. If S3–S5/S12 are taken, the clang lane also confirms that the std types now hash
  identically.
- **V3** — `registry_measure` on the reference box: the 100k/100 ms budget, or a revised budget.

## 7. Open questions

- **Q1** Move E1 (level-1 reader projection) into 2.2.3, amending G-15 so that 2.3.0 = format 1.8 (E2, E3, E4/E5
  records)? *Recommendation: yes if the 2.2.3 date is flexible; it is the largest item by far.*
- **Q2** S3, S4, S5 and S12 change the schema hash, and for tuple/pair the bytes, of types that 2.2.2 accepted by
  accident. G-2 says patch releases never change bytes. Take them in 2.2.3 as a recorded exception ("the 2.2.2
  hashes were library-internal and not portable"), or defer them to 2.3.0? *Recommendation: 2.2.3 with the
  exception recorded; today's hashes break the clang lane anyway.*
- **Q3** Multi-containers (S2): canonical order by key and then value bytes, and kindspec `multimap<K,V>`. Should
  decoding a multimap's wire shape into a `std::map` fail (duplicates) or keep the first entry?
  *Recommendation: fail with `invalid_value`.*
- **Q4** `std::unique_ptr<T>` (S8): include it? Null is representable, but a polymorphic `T` must stay rejected.
  *Recommendation: yes, for non-polymorphic `T`.*
- **Q5** Ship the typed-only half of the validation hints (E4) in 2.2.3? *Recommendation: no; ship it together
  with the dictionary record in 2.3.0, so the tools and the typed readers agree from the start.*
- **Q6** Must the Python reader gain projection (E1) for parity, or is the reader-side projection C++ only?
- **Q7** `remove-me/`: approve the list in §8. Should `remove-me/` ship in the public tarball (`--public`) or only
  in the full one? *Recommendation: full tarball only (`#private`).*
- **Q8** Talk materials: regenerate the decks with the slide-21 fix (N3) as part of 2.2.3, or separately?
- **Q9** G-7, the license: BSL-1.0 (Boost) or Apache-2.0 WITH LLVM-exception (Beman)? This is still open, and
  D11 records it.
- **Q10** Format of the dictionary demo (N4): an example with two plugins, a test fixture, or both?

## 8. Files proposed for `remove-me/`

These files' content is superseded by `docs/DECISIONS.md`, the five flattened documents, `CHANGELOG.md` and the
test reports. Each move keeps the file's relative path under `remove-me/`, so `remove-me/docs/plans-for-v1.1.md`
is still findable. References in live files (CLAUDE.md, AGENTS.md, README, book, one source comment) are re-pointed
to the current documents in the same commit, and `check-book.py` stays at 0.

**Tier A: move in 2.2.3**

| File | Superseded by |
|---|---|
| `claude-resume.txt` | nothing; a stale session id |
| `claude-ai-training-session1.md`, `ce-work.md` | session notes (CLAUDE.md already marks them `#private`) |
| `genesis-prompt.md`, `ground-level-prompt.md` | retired in 2.2.1 (D-2); `docs/ser-reg-rebuild-prompt.md` |
| `directions-log.md`, `cc-project-update-2.1.13a.md` | `docs/DECISIONS.md` (G-14) |
| `docs/release-status-v2.1.0.md` … `v2.1.15.md` (14 files) | `CHANGELOG.md` and `docs/test-reports/` |
| `docs/release-state-2.1.16.md`, `docs/release-state-2.2.0.md` | `docs/DECISIONS.md` |
| `docs/c-review-study.md`, `docs/c-review-study-next.md`, `docs/c-review-and-roadmap-2.1.14.md` | DECISIONS and the closed review items |
| `docs/cc-project-review-v1.md`, `docs/cc-project-review-2.1.10.md`, `docs/cc-review-2.1.10-response.md`, `docs/gk-project-review-v1.md` | reviews of 1.0.x–2.1.10, all closed in DECISIONS |
| `docs/plans-for-v1.0.2.md`, `docs/plans-for-v1.1.md`, `docs/plans-for-v2.1.md`, `docs/plans-for-v2.1.16.md` | implemented; the requirements and architecture documents describe the result |

**Tier B: keep for now**

These are still cited by code comments or tests as the design reference, by section.

| File | Why it stays |
|---|---|
| `docs/ser-reg-requirements-v3.md` | the v1 spec; FR-IDs and "spec §6.x" comments throughout the headers |
| `docs/plans-for-v2.0.md` | `format.hpp`, `tests/golden/*` cite "plan v2.0 §2" |
| `docs/plans-for-containers.md` | `reflect.hpp` and `test_containers.cpp` cite it |
| `docs/repo-get-ready.md` | still the checklist for the first public push (G-11) |
| `docs/test-reports/*` | the verification record of every release |
| `review-programs-for-2.1.11/` | its own rule: turn each repro into a real test before deleting it |

Tier B could move in 2.3.0, once those comments point at architecture sections.
