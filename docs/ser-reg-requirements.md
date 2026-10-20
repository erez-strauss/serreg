# ser-reg — Requirements (`es::serreg`)

**Library:** 2.2.6 · **Wire format:** 1.7 (readers accept 1.6 and 1.7) · **Hash algorithm:** XXH3-64 (`hash_algo` 2) ·
**Date:** 2026-10-07 · **Status:** the single, current requirements document (decision D-1).

---

## 0. Scope and status

This document states what the library, its tools and its package **must do as they stand at 2.2.6**, plus the
items planned for **2.3** and **3.0**, each marked **planned**. It is written from the current state: no history, no
milestones, no review narrative. A one-word "since X" appears only where a reader of an old capture needs it.

| What | Where |
|---|---|
| Decisions and their rationale (IDs `R-*`, `P-*`, `A-*`, `C-*`, `G-*`, `T-*`, `D-*`) | `docs/DECISIONS.md` — every requirement that stems from a decision cites its ID |
| How the library is built (byte tables, data structures, the registry's internals) | `docs/ser-reg-architecture.md` (companion) |
| How it is verified | `docs/ser-reg-testing.md` (companion), `docs/test-and-log-protocol.md`, `scripts/run-all-tests.sh` |
| How it is used | `docs/ser-reg-stories.md` (companion), `book/` |
| The wire contract, constant by constant | `include/es/serreg/format.hpp` — where this document and the header disagree, the header is right |
| Rules for agents and contributors | `AGENTS.md`, `CLAUDE.md` |
| The old specification, plans, reviews, deviations | `docs/ser-reg-requirements-v3.md`, `docs/plans-for-*.md`, `docs/deviations.md` — history only |

Requirement IDs (`FR-S1`, `FR-R11`, `FR-W7`, `NFR-1`, …) are referenced by tests and commit messages and are kept.
IDs introduced in this document (not in the v3 spec): `FR-W10`–`FR-W14`, `FR-G1`–`FR-G8`, `FR-T1`–`FR-T6`,
`FR-R13`–`FR-R16`, `FR-V1`–`FR-V6`, `FR-A7`–`FR-A11`. Appendix A indexes every ID with the tests that cite it.

Normative language: **MUST** / **never** are requirements; "is", "does" describe the required behaviour of the
current release; **planned** marks a requirement that is not in the code yet and names the release it is bound to.

---

## 1. Goals and non-goals

### 1.1 Goals

1. **Serialize ordinary C++ types without macros, registration calls or code generation.** The complete description
   of every type ever passed to `serialize()` is derived at compile time by C++26 reflection (P2996); flat types
   travel at `memcpy` speed, dynamic types as a canonical image (identical values ⇒ identical bytes) (G-3).
2. **A process-wide type registry filled by instantiation.** Every type, literal, constant-bearing call signature and
   scope is registered before `main()` and at `dlopen`, as a side effect of template instantiation; a copy of every
   entry is embedded in the binary, so a tool can read a program's schema offline (R-5).
3. **Self-describing data.** Every capture carries its dictionary; `srdump`, the Python reader and the Wireshark
   dissector decode any capture with the dictionary alone, knowing nothing about the user's types.
4. **Stable 64-bit identities** per type (schema, layout), per call signature, per literal and per program,
   independent of compiler and standard library (P-1, P-3).
5. **Binary logging as a primary use** (R-11): a logger whose envelope costs zero bytes beyond a timestamp, scoped
   dictionaries per log file, and log views in the tools.
6. **Captures of unlimited size, bounded memory, never a crash on wrong data** (R-2, R-12): windows, limits, CRC,
   checkpoints and resynchronisation.
7. **A core library for the public**, aimed at Boost, Beman or the standard (R-1); every design choice is judged
   against that.

### 1.2 Non-goals

- Pointers, references, unions, bit-fields, virtual bases and virtual functions, `std::multimap`/`std::multiset`,
  `std::list`/`std::deque` (candidates, A-16), recursive types, zero-size dynamic elements: rejected at compile time
  with a named diagnostic (FR-S7). Nothing is silently reinterpreted.
- Cross-endian or cross-ABI conversion at run time: the ABI tag detects a mismatch (FR-W4); a portable wire mode is
  3.0 (P-18, **planned**).
- Text payload formats on the wire; interning of run-time strings (FR-L5).
- Schema evolution level 3 (dictionary-less evolved reads): not planned (R-10). Level 2 is unscheduled.
- Lazy registration (function-local statics), `dlopen`/`dlclose` interposers, an abort on `dlclose`, pointers from
  the registry into an unloadable image, IDs narrower than 64 bits on the wire, external dependencies.
- Collision resistance against an adversary: hashes identify, they do not authenticate (one trust domain, R-2).

---

## 2. Users, platforms, toolchain lanes

### 2.1 Users and trust model

- **Users are the public** (R-1). The API, the diagnostics and the documentation address a team that has never seen
  the project.
- **One trust domain; the code never crashes on wrong data** (R-2). Input is corrupt or buggy, not hostile, but:
  every known bug is fixed; reasonable size limits apply by default and the caller can override them
  (`decode_limits`, §7.4); for any sequence of input bytes every reading API returns a value or an error and never
  crashes, aborts, asserts, executes undefined behaviour, or allocates or loops beyond the limits (FR-D5). Captures
  may be several TB (a trading week); frames are limited to 64 MiB; files and pipes are read in windows of
  `decode_limits::window_bytes` (10 MiB), grown for one frame up to the frame limit when a frame is larger.

### 2.2 Platform and toolchain (G-1)

| Lane | Compiler and flags | Role |
|---|---|---|
| `gcc` | GCC 16.x, `-std=c++26 -freflection` (hard error below C++26; CMake gates it) | reference; builds the whole tree |
| `clang-libcxx` | Bloomberg clang-p2996, `-std=c++26 -freflection-latest -stdlib=libc++` | cross-check of canonical strings, hashes and the Compiler Explorer examples |
| `clang-libstdcxx` | clang-p2996 + GCC 16 libstdc++ | unavailable (libstdc++'s `<meta>` is GCC-intrinsic-only) |
| EDG | `edg-trunk-reflection` | rejects `template for`; not a lane |

- Platform: Linux x86-64, ELF64, little-endian. binutils ≥ 2.36 (`[[gnu::retain]]`). Windows, macOS and aarch64
  follow after Linux is complete, clang-p2996 with the MSVC ABI first (G-6, **planned**); the executable-format seam
  (`src/image.hpp`, `detail/embed_*.hpp`; C-9) is where a PE backend goes.
- Build: CMake ≥ 4 with presets (`gcc16-debug`, `gcc16-release`, `gcc16-static`, `gcc16-static-none`,
  `gcc16-ubsan-trap`, `gcc16-asan-ubsan`, `gcc16-tsan`, `clang-p2996`, `opt-gcc16-debug`); options
  `ES_SERREG_BUILD_SHARED`, `ES_SERREG_BUILD_TOOLS`, `ES_SERREG_BUILD_TESTS`, `ES_SERREG_BUILD_EXAMPLES`,
  `ES_SERREG_WERROR`, `ES_SERREG_CATCH2_DIR`, `ES_SERREG_IMAGE_FORMAT=elf|none`, `ES_SERREG_IPC_ONLY`.
- **No dependencies** for the library and the tools (C-6). Catch2 v3 is test-only. In-house parsers only: JSON
  writer/reader, TOML writer, a TOML subset reader for `--config` (FR-D9).
- Warnings (C-4): `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
  -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation`
  (+ `-Wduplicated-cond -Wduplicated-branches -Wlogical-op` on GCC), `-Werror` on by default for every target.

### 2.3 Package (A-4)

Three libraries, one CMake package `serreg` (`find_package(serreg CONFIG REQUIRED [COMPONENTS ipc serreg tools])`;
pkg-config `serreg`, `serreg-tools`, `es-ipc`; a vcpkg overlay port; `FetchContent`):

| Target | Contents | Rule |
|---|---|---|
| `es::serreg` (`libserreg`, static and shared) | reflection headers, registry, sinks, capture files, logger, readers, decoder, formatters, limits | nothing on the hot path allocates or locks; no exceptions, no RTTI |
| `es::serreg_tools` (`libserreg_tools`) | `cli` (the `srdump` command line), `--diff`, Wireshark generator, C++ generator, `render_value`, `process_dictionary`, `tools::info` | depends on `es::serreg`; never the reverse; tool code never moves into `es::serreg` |
| `es::ipc` (header-only, C++23, no reflection) | variable-size shared-memory rings | no dependency in either direction; `-DES_SERREG_IPC_ONLY=ON` installs it alone |
| `srdump` | `int main(int argc, char** argv) { return es::serreg::cli::run(argc, argv); }` | one line (FR-D0) |

Code rules that are requirements of every contribution (A-1, C-1, C-2, C-3, C-5): `const` parameters by default;
`std::expected<T, error>` errors, `noexcept` where true; `constinit` namespace-scope mutables, no static-init-order
dependencies; `std::print`/`std::println` only (library diagnostics through `detail::report_line`); every `main()`
only parses the command line and runs an application class; package headers before system headers, every header
self-contained with `#pragma once`; clang-format 22 clean (tree clean under 18–23).

---

## 3. Serialization (FR-S*)

### 3.1 API

- **FR-S1** `template <sink S, class... Ts> auto serialize(S& out, const Ts&... vs) -> std::expected<std::size_t, error>;`
  — one frame per call; returns bytes written. `serialize_with(serialize_options, out, vs...)` takes options;
  `serialize<"literal">(out, vs...)` spells a leading literal as a template argument.
- **FR-S2** Sinks. `sink` concept: `write(const std::byte*, std::size_t) -> bool`. `claiming_sink`: `claim(n) ->
  std::byte*`, `commit(n)` (`commit` may return `bool`; a refused commit is a failed `serialize`). Adaptors:
  `span_sink`, `vector_sink`, `fd_sink`, `capture_file` (§5.2), `scoped_capture_file<Scope>` (§8.4), the
  `es::ipc` producer. A sink whose `frame_crc()` returns true receives every frame with a CRC32C trailer (FR-W10).
- **FR-S3** `frame_size<Ts...>()` is `constexpr` and exact when every argument is flat, a literal or a constant, and a
  minimum otherwise; `frame_size(options, vs...)` is exact for every argument, including the CRC trailer.
- **FR-S4** `deserialize(std::span<const std::byte>, Ts&...) -> std::expected<std::size_t, decode_error>` (§7.1).
- **FR-S5** `serialize_raw<T>` / `deserialize_raw<T>` — payload only, no header, no id; flat types only.
- **FR-S6** Determinism (R-4). Padding bytes are zeroed from a compile-time gap table and `bool` is normalised, **on
  by default**; `serialize_options{.scrub_padding = false}` opts out. No cost when
  `has_unique_object_representations_v<T>`. Unordered containers are sorted before encoding (FR-W9).
- **FR-S7** Every unsupported member is a `static_assert` naming the member (through the instantiation backtrace),
  listing the supported containers and mentioning `as_flat<T>`. Covers pointers (other than a top-level `const char*`
  string argument), references, unions, bit-fields, virtual classes, `std::string_view` members, multimaps,
  multisets, lists, deques, recursive types, zero-size dynamic elements.
- **FR-S8** Customisation. `template <class T> struct as_flat;` (`using type = …; static type to(const T&); static T
  from(const type&);`) maps an opaque type — top-level arguments only (a nested member whose type has `as_flat` is
  rejected). `[[=es::serreg::skip]]` drops one member: zero on the wire, never registered.
- **FR-S9** Every non-static data member is serialized, private members included (`access_context::unchecked()`);
  the dictionary documents exactly what was serialized (R-3).
- **FR-S10** All function parameters are `const` unless the function modifies them (API, internals, tests).
- **FR-S11** `[[=es::serreg::since(N)]]` and `[[=es::serreg::deprecated(N)]]` (`uint16_t`, 0 = unset) on members and
  types are stored in descriptors, blobs and dictionaries, shown by the tools, **in the schema hash and never in the
  layout hash** (P-6). No behaviour is attached to them until evolution level 1 (R-9, **2.3 — planned**).

### 3.2 Argument categories

| Argument (`remove_cvref_t`) | Call string | ARG kind | Compact record | Full-record payload |
|---|---|---|---|---|
| flat type `T` | `<hex schema hash>` | `type` (0) | `sizeof(T)` bytes, scrubbed, pad 8 | the bytes |
| dynamic type (a class with a dynamic member; a container as an argument, FR-A6) | `<hex schema hash>` | `type` (0) | `u32 len \| u32 0 \| canonical image` | the same |
| `lit<S>` (FR-L1) | `lit:<hex id>` | `literal` (1) | **nothing** (FR-L6) | `u64 id` |
| scalar — **FR-A1**: `bool`, char family, `signed char`…`unsigned long long`, `float`, `double`, `std::byte`, enums | the kind name (`i32`, `f64`, `bool`, `wchar`…) or the enum's schema hash | `scalar` (2) / `type` (0) | `sizeof` bytes, pad 8 | the bytes |
| run-time string — **FR-A2**: `const char*`, `const char (&)[N]`, `std::string_view`, `std::basic_string<char, …, A>` | `string` | `string` (3) | `u32 len, bytes`, pad 8 | the same |
| `constant<V>` (`42_`, `k<V>`; FR-A5) | `const:<kind>=<value>` / `const:<enum hash>=<value>` | `const` (4) + `u64 value` in the ENTRY | **nothing** | the scalar or enum record |

- **FR-A3** `frame_size<Ts...>()` is exact when no argument is a run-time string or a dynamic type; the run-time
  form is exact always and makes one `size()`/`strlen` pass, no allocation.
- **FR-A4** Typed outputs: a string record decodes into `std::string&` (copy) or `std::string_view&` (a view into the
  frame, valid while the buffer is); a scalar into a non-const lvalue of the same scalar type; `dispatch` and
  `frame_router` match `std::string_view` and scalars like any other type; `record_view` carries `arg_kind` and the
  scalar `kind`.
- **FR-A6** Any dynamic member type — `std::vector<E>`, `std::optional<E>`, `std::variant<Es...>`, the map and set
  containers — may be passed directly to `serialize`, `deserialize`, `dispatch`, `frame_router` and the typed
  walkers. Its type entry is a **transparent type**: a synthetic struct named after the container's vocabulary name,
  one unnamed field of the member kind at offset 0 (size 8, align 4), `struct_flag_transparent`; its record is the
  ordinary dynamic-type record. Dictionary-driven readers render it as its value (`ids: [3, 1, 2]`). A comparator,
  hasher or equality other than the defaults is part of the name; the allocator is not. `std::string` at top level
  stays the `string` record; `std::array<E,N>` of a flat `E` stays a flat class.

### 3.3 Type support matrix

| Category | Status | Encoding |
|---|---|---|
| Integral, floating, `bool`, char family, `std::byte` | ✔ | native |
| Enums, fixed underlying type or not | ✔ | native; enumerators in the dictionary; kind of an unfixed enum derived from its enumerators (P-8); `enum_flag_fixed` in the ENTRY |
| Nested flat class incl. private members and bases | ✔ | native; a base is the pseudo-field `<base:Name>` of kind `struct_` at its offset (P-4); virtual bases rejected |
| C arrays, `std::array<T,N>`, multi-dimensional | ✔ | native |
| `[[no_unique_address]]`, empty members, empty types | ✔ | native |
| `lit<S>` literals, `constant<V>` constants | ✔ | 0 bytes in a compact frame |
| Opaque types via `as_flat<T>` | ✔ | as the mapped type |
| `std::basic_string<char, traits, A>` — any allocator (`std::pmr::string`) | ✔ | slot `{u32 off, u32 len}` + bytes; allocator not hashed; other character types rejected |
| `std::vector<T>` | ✔ | slot `{u32 off, u32 n}` + `n` element images (stride = element size rounded to its alignment) |
| `std::optional<T>` | ✔ | slot + 0 or 1 image |
| `std::variant<Ts...>` | ✔ | slot `{u32 off, u32 index + 1}` + the alternative's image; alternatives named and hashed in the dictionary (FR-W8); `std::monostate` allowed |
| `std::map`, `std::unordered_map`, `std::flat_map` | ✔ | shape `map` (`kind::map_` 53): slot + `n` entries sorted by key (FR-W9) |
| `std::set`, `std::unordered_set`, `std::flat_set` | ✔ | shape `set` (`kind::set_` 54): slot + `n` sorted keys (FR-W9) |
| Any of the above containers as a top-level argument | ✔ | transparent type (FR-A6) |
| `std::multimap`, `std::multiset` | ✖ | `static_assert` |
| `std::list`, `std::deque` | ✖ | `static_assert`; the `vector` shape for them is A-16 (**2.2.x — planned**) |
| Bit-fields, pointers, references, unions, virtual functions, recursive types, `std::array<T,0>` of a dynamic `T` as an element | ✖ | `static_assert` |

---

## 4. Registry (FR-R*)

- **FR-R1** Instantiating `serialize`, `deserialize`, `serialize_raw`, `dispatch`, a `frame_router` handler, `to_string`
  (FR-A11) or `register_types<Ts...>()` / `register_call<Sig...>()` for `T` registers `T` and, recursively, every
  component type: nested classes, bases, enums, array elements, vector elements, optional payloads, variant
  alternatives, map keys and values, set keys. A call signature registers its call entry; a literal its literal
  entry. Registration needs no call site: `register_types` / `register_call` register without one (R-5), and
  `ES_SERREG_REGISTER_NAMESPACE(ns[, selector])` registers every serialisable type of a namespace at once (A-22:
  `std::meta::members_of` at the point of instantiation; non-serialisable and `[[=skip]]` types skipped).
- **FR-R2** Registration happens during the static initialisation of the image that holds the instantiation: before
  `main()` for the executable and `DT_NEEDED` DSOs, inside `dlopen` for later-loaded DSOs. Never lazily.
- **FR-R3** One registry per process, `registry::instance()`, exported from the shared `libserreg`. A static
  `libserreg` inside a `dlopen`ed DSO is a second registry; the second copy reports itself with one diagnostic line.
- **FR-R4** Identity and conflicts (G-16). Entries are keyed by (kind, hash); duplicates across images collapse into
  one entry with per-image references. Same schema hash with a different layout hash is a collision or a corrupt
  image: both definitions are listed by `registry::conflicts()`, the first loaded definition stays, the later one is
  kept unlinked, `ES_SERREG_CONFLICT=warn|abort|silent` (default `warn`, one stderr line) says what else happens, and
  a loaded definition takes over when the kept one unloads. Two types with one canonical name and different layouts
  are a conflict. `serialize()` never consults the registry and never returns `schema_conflict`.
- **FR-R5** API: `size()`, `snapshot()` (every entry: types, enums, literals, calls — filter on `registry_entry::nk`),
  `find_type(schema_hash)`, `find_type_by_name`, `find_literal(id)`, `conflicts()`, `has_conflict()`, `images()`,
  `program_hash()`, `version()` (monotonic change counter), `generations()` (indexes published), `retained()`,
  `export_image()` / `export_image_since(version)` / `export_image_scope(scope_hash, since)` (RRI), `scope_calls()`,
  `to_json`, `to_toml`, `on_change(cb) -> id`, `remove_on_change(id)`, `refresh()`, `unload_guard_active(addr)`.
- **FR-R6** Concurrency: reads are lock-free over a published, immutable index (one `std::flat_map` per kind); writes
  take an internal mutex; `version()` is atomic. Publication is deferred: a registration marks the index stale, the
  next reader publishes one index for every change since the last one and runs the `on_change` callbacks once, on its
  own thread; `refresh()` publishes at once. Registry operations are never on the serialization hot path.
- **FR-R7** Registration survives `-Wl,--gc-sections`, `-flto`, static archives without `--whole-archive`.
- **FR-R8** No static-initialisation-order dependency: the registry head is `constinit`; nodes self-link.
- **FR-R9** Environment hooks in `libserreg`, evaluated at library init and after registry changes:
  `ES_SERREG_DUMP_REGISTRY=<path|->`, `ES_SERREG_DUMP_REGISTRY_FD=<n>`, `ES_SERREG_DUMP_FORMAT=rri|json|toml`
  (default `rri` for an FD, `json` for a path), `ES_SERREG_DUMP_ON=exit|change|both` (default `exit`),
  `ES_SERREG_DUMP_EXIT=1`, `ES_SERREG_DUMP_SIGNAL=USR1|USR2|RTMIN+n` — only when set is a handler installed; its whole
  body stores an atomic flag; the dump runs on a helper thread that `sigwait`s the signal (blocked at library load,
  so load `libserreg` before creating threads). `ES_SERREG_ALLOW_DLCLOSE` is ignored (one stderr line if set).
- **FR-R10** `dlclose` never aborts (A-3). `push()` records each registration as a heap entry owned by the registry;
  for images other than the main program it copies the blob, the name and a deep copy of the descriptor (FR-R14 for
  the main program). Nothing the registry hands out points into an image that can be unloaded. A guard destructor —
  run by `dlclose` or at process exit, which the registry need not tell apart — drops that guard's reference and
  nothing else (no allocation, no free, no abort). `libserreg` does not interpose `dlopen`/`dlclose`.
- **FR-R11** An entry without references is **retained** (`refs == 0`): it stays in every later index, `find_*` return
  it, its descriptor stays readable, frames written while its image was loaded still decode. `images()` and
  `program_hash()` count loaded entries only; `find_type_by_name` prefers a loaded entry. **Reload:** a registration
  with the key and layout of a retained entry revives it (no new memory); a changed type is a new schema hash and a
  new entry beside the retained one; the same schema hash with another layout replaces a retained entry and conflicts
  (FR-R4) only with a loaded one. Every change bumps `version()`. Pointers, snapshots and indexes stay valid for the
  life of the process; nothing is freed before exit.
- **FR-R12** Per guard, the registry records the image it belongs to (`dladdr` at push time, outside the mutex), so
  `images()`, conflict reports and per-entry references are exact. `images()` lists one image per entry with loaded
  registrations.
- **FR-R13** Budgets (G-5). Push + publish of 10 000 entries / 10 MB of blobs **MUST** take ≤ 10 ms and 100 000
  entries / 100 MB ≤ 100 ms in a release build on the reference box; `tests/bench/registry_measure --assert` enforces
  both (label `bench`), `--plugin` measures the deep-copy path of a `dlopen`ed image, `--elf` the reader side.
- **FR-R14** Main-program registrations are viewed, not copied (C-11): the executable is never unmapped before exit,
  so its ENTRY blobs, names and descriptors are referenced in place; only other images' entries are copied.
- **FR-R15** Scopes (P-16, A-9). Serialising into a `scoped_sink` (a sink type with `using dictionary_scope = Scope;`)
  odr-uses a `registrar_scoped<Scope, Sig...>` whose node (`node_kind::scope`: scope hash, call hash, scope name) is
  pushed like any registration and has no ENTRY in the full image. `export_image_scope(scope_hash, since)` is the
  closure of the scope's calls over their argument types (recursively) and literals; `export_image_since(version)`
  is a delta (`rri_flag_delta`); `registry_entry::added` is the version at which a definition appeared. No per-frame
  cost, no run-time bitmap.
- **FR-R16** Retained entries stay in `export_image()` so old frames remain readable; a scoped export (FR-R15)
  holds only what the scope reaches. `registry::forget_retained()` is A-14 (**3.0 — planned**).

---

## 5. Wire format (FR-W*)

`include/es/serreg/format.hpp` is normative for every constant; the architecture document carries the byte tables.
All integers are little-endian, fixed width; every frame, record and ENTRY is a multiple of 8 bytes; all ids are
64-bit (FR-H8). Frame headers are 16 bytes; a field is never added without a format-minor bump.

### 5.1 Containers and frames

- **FR-W1** Two containers: a **file** (`"ESSR"` header, opening dictionary block, frames) and a **stream** (frames
  only; the dictionary out of band or as dictionary frames).
- **FR-W2** Frame modes (R-7). **Compact** is the default everywhere: the 16-byte header `u32 frame_len | u16 flags |
  u16 n_records | u64 call_hash` and one record per argument, boundaries from the dictionary. **Full records** are
  opt-in per call (`serialize_options{.full_records = true}`): each record is `u64 hash | u32 payload_len | u16
  layout | u16 rsv | payload`, self-describing without the calls table.
- **FR-W3** The dictionary (an RRI, §6.3) is sufficient to decode every field: name, offset, size, align, kind,
  nested hash, extent, element descriptors, enumerators, `since`/`deprecated`, flags, the literal table, the calls
  table with its argument kinds and constant values.
- **FR-W4** Native little-endian layout; the FILE and RRI headers carry an 8-byte **ABI tag** (version, endianness,
  pointer, `long`, `wchar_t`, `char` signedness, `max_align_t`, `long double`) (P-2). Typed reads across ABIs fail by
  hash; dictionary-driven readers decode by the file's sizes and use the tag for `wchar` widths; two tags in one
  capture are `abi_mismatch`.
- **FR-W5** JSON and TOML renderings of a dictionary are canonical (sorted) so that diffs are clean.
- **FR-W6** `format_version` major.minor in every header; the reader's acceptance window is FR-V3.
- **FR-W7** Dictionary completeness (R-7). A sink opened at registry version *t* writes the dictionary of *t*. When
  `registry::version()` moves afterwards (a `dlopen`), the next data frame that needs it is preceded by a
  **dictionary frame** holding the delta (FR-W12) — in files and streams alike (`capture_file` checks one relaxed
  load per frame; `capture_policy{.dictionary_updates = false}` removes the check; on a bare stream
  `dictionary_tracker::update(sink)` from the application's idle path). Readers merge dictionary frames in order.
  `srdump --verify` confirms that every call hash used is defined by a dictionary that precedes its first use.
- **FR-W8** A `variant<Ts...>` entry carries the variant's own hashes, `extent = sizeof...(Ts)` and, per alternative,
  its index, canonical name and schema hash. `srdump` prints `variant<Order,Cancel>[1]=Cancel{…}`.
- **FR-W9** Maps and sets are wire shapes, not container types (P-7). `map`: the slot `{u32 off, u32 n}` points at
  `n` entries, each the key image at 0 and the value image at `align_up(sizeof key image, align value)`, stride
  rounded to the pair's alignment; **sorted by key** (the key's `operator<`, else the bytes of its flat image).
  `set`: `n` sorted key images. The FIELD's `extent` is the entry stride; `n_elem` = 2 (key, value) or 1 (key). The
  member kindspec is the shape (`map<K,V>`, `set<K>`) whatever the container, so a `std::map` and a
  `std::unordered_map` member have equal hashes and decode into each other. Decoding: `emplace_hint(end(), …)`, hash
  containers `reserve(n)`; a duplicate key is `invalid_value` (the first value kept); the vector slot rules and image
  budget apply. Sorting an unordered container above 64 entries allocates (the documented exception), and so
  does a CRC frame larger than the stack buffers on a plain (non-claiming) sink: the trailer covers the whole
  frame, so it cannot be streamed and is assembled on the heap (a claiming sink never pays this).
- **FR-W10** CRC32C trailer (P-10). Frame flag bit 2: the last 8 bytes of the frame are `u32 crc32c(frame[0, len-8))`,
  `u32 0`. Enabled by `capture_options::crc` (then the FILE flag `crc_all` is set), `serialize_options::crc`, or a
  sink's `frame_crc()`. Every reader checks it: a frame whose CRC does not match is reported ("crc mismatch (frame
  skipped)") and skipped — never decoded, never delivered; typed readers return `crc_mismatch`. Default off: zero
  bytes, zero work. `crc32c()` (`hash.hpp`) is constexpr with an SSE4.2 run-time path.
- **FR-W11** Checkpoint frames (P-11, P-17). Frame flag bit 3, `call_hash = "ESSRCKPT"`, `n_records = 0`; a 64-byte
  fixed payload `u16 version(1) | u16 flags(bit0 full RRI follows) | u32 rri_len | u64 prev | u64 next (patched) |
  u64 data_frames_before | u64 data_bytes_before | u64 wall_clock_ns | u64 latest_dictionary_offset | u64
  registry_version`, optionally followed by the full dictionary (`checkpoint_dictionary`). Written every
  `checkpoint_bytes` and/or every `checkpoint_every` (checked every 64 frames), on `checkpoint()`, and once at
  close; linked both ways; the opening CHAIN's `first_checkpoint` points at the first. The footer index of the old
  design is replaced by checkpoints (a footer does not survive a crash). Default off.
- **FR-W12** The dictionary chain; nothing is ever rewritten (P-9). The opening block of a file is `RRI | pad to 8 |
  CHAIN(u64 next_dictionary, u64 first_checkpoint)`; every later dictionary frame holds a **delta** (`rri_flag_delta`:
  the entries whose registry version is newer than the last written) and ends with `u64 next`. Every link is written
  as zero and patched **exactly once** with one aligned 8-byte `pwrite` when its target exists. A file truncated
  anywhere is readable up to the cut. The writer never rewrites the opening dictionary at close; FILE flag bit 2
  (back-patched) is read from 1.6 files and never written. `tools::info` follows the chain by seeks; sequential readers
  merge deltas as they meet them.
- **FR-W13** Writer-info frame (P-12). Frame flag bit 4, `call_hash = "ESSRINFO"`, payload `u16 version(1) | u16 n |
  ITEM × n` with `ITEM = u16 key | u16 len | bytes | pad 2`. Keys (`fmt::info_key`): library version, format,
  program, hostname, pid, start time, `crc`, `checkpoint_bytes`, `checkpoint_ns`, dictionary (`full` |
  `scoped:<name>`), sync policy, clock, thread, ABI (the long form of the tag); the keys `scrub_padding`,
  `full_records` and `canonical_unordered` are defined for writers that fix those per-call options per file —
  `capture_file` does not write them. Unknown keys are skipped. **On by default** (`capture_options::writer_info`; `info_host = false` for reproducible
  files; `info_thread = true` records the opening thread's id for `thread_policy::once`). `srdump --info` prints it
  without walking frames.
- **FR-W14** Meta frames are consumed, never delivered (P-13). Dictionary, checkpoint and writer-info frames count in
  `frame_ref::index` and are invisible to callbacks, `verify` counts (`frames`, `bytes`), `frame_walk::frames`, dumps
  and routers.

### 5.2 Capture files and options (A-11)

`capture_file` (`basic_capture_file<Scope = void, capture_policy P = {}>`): `open(path, capture_options)`,
`write`/`claim`/`commit`, `checkpoint()`, `close()` (final checkpoint, flush, `fdatasync`, no rewrite), `frame_crc()`.
One writer; not thread-safe. **Every 1.7 feature is opt-in; the defaults write what 1.6 wrote** except the header
minor, the 16-byte CHAIN and the writer-info frame:

| `capture_options` field | Default | Meaning |
|---|---|---|
| `crc` | `false` | CRC32C trailer on every data frame (FR-W10) |
| `checkpoint_bytes` / `checkpoint_every` | `0` / `0 ns` | checkpoint frames by size and/or time (FR-W11) |
| `checkpoint_dictionary` | `false` | each checkpoint carries the full dictionary |
| `writer_info` | `true` | the writer-info frame (FR-W13) |
| `sync` | `sync_policy::dictionary` | `none` / `dictionary` (a delta is durable before the frames that need it) / `checkpoint` / `frame` |
| `program`, `info_host`, `info_thread`, `log_clock`, `clock` | `{}`, `true`, `false`, `{}`, `nullptr` | writer-info content and the wall clock for checkpoints |

`capture_policy{.dictionary_updates = true}` is an NTTP: `false` removes the per-frame registry version compare for a
program that never `dlopen`s.

---

## 6. Hashes and dictionary (FR-H*, FR-D*)

### 6.1 Hashes

- **FR-H1** `schema_hash_v<T>` (the type's identity: names, annotations, nested names) and `layout_hash_v<T>` (the same
  description with names removed: the O(1) "same bytes" test used by `--diff` and evolution), both `constexpr`
  64-bit. **FR-H2** `call_hash_v<Ts...>` over the ordered call string (`call(n){…}`, `print(n){…}`, `println(n){…}`,
  `log(n){…}`). **FR-H3** `program_hash()` order-independent over the loaded types' schema hashes.
- **FR-H4** Cross-compiler stability through canonical description strings that contain no compiler spelling; golden
  values locked by `STATIC_REQUIRE` and reproduced on the `gcc` and `clang-libcxx` lanes. A string change is a format
  minor bump with a CHANGELOG note (C-7). `since`/`deprecated` are in the schema form only.
- **FR-H5** The algorithm id is in every header: 2 = XXH3-64 (seed 0, default secret), constexpr and run time. Nothing
  else is computed; files with another `hash_algo` are `bad_version` (P-1).
- **FR-H7** A literal's id is XXH3-64 of its bytes. **FR-H8** All wire ids are 64-bit; no truncated or compact ids.
- Canonical names (P-3), in order: (1) an explicit `[[=es::serreg::name("…")]]` or `type_properties<T>::name()` (not
  both; letters, digits, `_ : < > , -`); (2) scalars by kind name; (3) the frozen std vocabulary (`std::string` for
  any allocator, `std::vector<E>`, `std::array<E,N>`, `std::optional<E>`, `std::variant<…>`, `std::pair`,
  `std::tuple`, `std::monostate`, the map and set containers) without inline namespaces, allocators or default
  arguments; (4) a type without a linkage-unique name (unnamed namespace, local to a function or lambda) **MUST** be
  named explicitly — `static_assert` otherwise; (5) otherwise the walk of `parent_of` to the global namespace joined
  with `::`, user inline namespaces kept, std's implementation namespaces dropped, an enclosing class-template
  specialisation named through its canonical arguments (`ns::Outer<i32>::Inner`), template arguments rendered
  canonically (`Vec<i32,3>`, `W<i32,enum(7)>`); (6) recursive types are rejected. Within one program a name never
  denotes two layouts (FR-R4).

### 6.2 Dictionary content and carriers

- One blob format, three carriers: the `es_serreg_dict` ELF section (ENTRY stream, no header; one TU-local copy per
  instantiating translation unit, deduplicated by hash by every reader), the RRI export (40-byte header + entries),
  and the dictionary inside files and streams (opening block + deltas). The embedded copy is read by tools, never by
  the program itself; `tools::extract_embedded_dictionary` is a self-contained ELF64 reader (no libelf).
  `ES_SERREG_IMAGE_FORMAT=none` builds without the section (offline tools then need an `.rri`).
- ENTRY tags: struct (1), enum (2), literal (3), call (4); `u8 version = 1`. The struct flags byte carries `flat`
  (bit 0) and `transparent` (bit 1); the enum flags byte carries `fixed` (bit 0). A call ENTRY carries `u16 flags`:
  `print` (bit 0), `newline` (bit 1), `log` (bit 2).
- **FR-D6** `srdump --elf <binary-or-so>` extracts the embedded copy (several `--elf` merge); **FR-D7** `--rri
  <image>` reads an exported RRI; `--dict <rri>` side-loads a dictionary for a bare stream. **FR-D8** Renderers: text,
  JSON, TOML. **FR-D9** In-house parsers only: a JSON writer and reader, a TOML writer, and a **TOML subset reader**
  for `--config` (tables, `key = value`, strings, integers, booleans, arrays of strings; no dates, inline tables or
  dotted keys — rejected with a line number); all bounded, fuzzed, never on the serialization path.
- Conflicts inside a dictionary — two entries for one hash, a literal or call id collision — are recorded
  (`dyn::dictionary::id_conflicts`) and reported by `--verify`.

---

## 7. Reading, decoding, limits and the never-crash contract

### 7.1 The reading styles

1. **Typed** — `deserialize(bytes, Ts&...)` validates the header, the call hash (`call_hash_v<Ts...>`) and every
   length **before** writing any output, then decodes in the same pass (R-8). On failure the `decode_error` holds the
   `error` code and three counters: `expected` (1 for the frame + the number of arguments, literals and constants
   included), `actual` (0 if the frame was refused, else 1 + the arguments decoded) and `failed` (the first item that
   failed; 0 = the frame). A flat record or a frame that does not fit is refused before any output is written; a
   dynamic image that fails while it is decoded leaves earlier arguments complete, that one valid but unspecified,
   later ones untouched; `invalid_value` does not stop decoding. `decode_error` converts to and compares with `error`.
   `deserialize_print` / `deserialize_println` render a print frame to a string with the same return type.
2. **Inspect** — `inspect(bytes, const dictionary* = nullptr) -> expected<frame_view>`: lazy `record_view`s
   (`schema_hash`, descriptor, `arg_kind`, `kind`, `is_const`, payload), zero copies, from the registry or a
   supplied dictionary.
3. **Dispatch** — `dispatch<Ts...>(bytes, overloads)` per record and `dispatch_frame` per signature; zero-copy when
   aligned.
4. **Router** — `frame_router`: `on(handler)`, `on(lit, handler)`, `on_print`/`on_println(fmt, handler)`,
   `on_log(handler(level, line))` (FR-G7); the frame's call hash picks the handler, the handler's parameters are the
   decoded values; `tools::for_each_frame(path | span, router.callback())` over memory, a mapped file or windows.
5. **Dictionary-driven** — `tools::visit_record`, `walk_values`, `render_frame`, `render_value`: descriptor-driven,
   needs no user types; what every formatter, `srdump`, `to_string` and the Wireshark generator use.
6. **Zero-copy view** — `view<T>(record_view) -> expected<const T*>` (`std::start_lifetime_as`); `misaligned` otherwise;
   a non-canonical `bool` byte is `invalid_value` here (it cannot repair in place).

Every reader works from the dictionary of the capture (opening block, deltas, checkpoints) merged with `--dict`,
`--elf` and `--rri` sources, never from the reader's registry alone; unknown call hashes are reported (`lit#<hex>`,
`unknown_type`), not guessed.

### 7.2 Values the C++ type cannot hold (R-4, P-5, P-8)

- A `bool` byte other than 0/1 reads as `true` with an optional warning (`set_warning_handler`,
  `warning::noncanonical_bool`, `verify_report::warnings`).
- A valueless variant (`n == 0`) decodes to `std::monostate` when the variant has one, otherwise `invalid_value`;
  decoding continues with the next item.
- An enum without a fixed underlying type holds only the values of the minimal-width integer that represents its
  enumerators: the typed readers check such leaves and report `invalid_value` (the leaf reads as 0); enums with a
  fixed type are never checked. The dictionary-driven readers (`verify`, `walk_values`, `srdump`) have the `fixed`
  flag and **MUST** apply the same check — A-16 (**2.2.x — planned**; today they print the number).
- A duplicate map or set key is `invalid_value`; the first value is kept.
- One set of slot rules for every image reader (`fmt::image_budget`): `n == 0 ⇔ off == 0`, blocks after the fixed
  part and inside the image, a byte budget of the image size so shared tails cannot amplify; `verify` and
  `deserialize` accept the same images.

### 7.3 The never-crash contract — FR-D5 (R-2)

For any sequence of input bytes — a capture, a stream, a frame, an RRI, an ELF file, a config file, a ring header in
shared memory — every public reading API returns a value or an error and **never** crashes, aborts or asserts,
executes undefined behaviour, or allocates or loops beyond the configured limits. Errors are reported and decoding
moves on to the next item where the format allows it (a frame with a bad `frame_len` stops a walk because it cannot
resynchronise — unless a checkpoint chain exists and `--resync` is given, FR-T2). Outside the contract: system
out-of-memory within the limits, and policies the user asked for (`ES_SERREG_CONFLICT=abort`). The fuzz targets
(T-4) check the contract and are gated on time and memory, not only on crashes (`docs/SECURITY.md`).

### 7.4 Limits — `decode_limits` (A-7)

Process-wide (`get_decode_limits` / `set_decode_limits`, thread-safe) and per call where a reader takes them
(`tools::file_options`, `block_options`, `dump`/`verify` over a path, `srdump --window`, `--frame-limit`); read once
per walk, not per frame. Defaults are chosen so that no honest data reaches them:

| Field | Default | Bounds |
|---|---|---|
| `frame_bytes` | 64 MiB | the largest frame any reader accepts (and the writer limit of R-2) |
| `dictionary_bytes` | 256 MiB | an RRI, an embedded dictionary, a dictionary frame |
| `dictionary_entries` | 1 000 000 | `n_entries` of one dictionary; more is `corrupt_dictionary` |
| `image_depth` | 64 | nesting of struct levels (= `fmt::max_nesting`, which bounds the typed decoder at compile time) |
| `input_bytes` | 1 GiB | the largest input read whole (an RRI, an ELF, a config file, a span-based capture read, a pipe dump) |
| `window_bytes` | 10 MiB | the window of every capture reader over a path or pipe |
| `problems` | 1000 | problem lines kept per walk; afterwards only a count |

- **FR-D11** Every reader over a capture on disk or a pipe works on a sliding window of `window_bytes` (`pread`/
  `read`): a frame that straddles a window is completed from the next; a frame larger than the window grows it to
  that frame within `frame_bytes`; `verify` and `dump` keep state O(types) (R-12). A capture's size is bounded by the
  file system, not by the tools. A text dump of a pipe reads it whole within `input_bytes` (the dictionary must
  precede the frames and a pipe cannot be read twice); a file or `--dict` avoids it. `es::ipc` validates its headers by
  its own rules, outside `decode_limits`.

---

## 8. Print, literals, constants, logging (FR-L*, FR-P*, FR-G*)

### 8.1 Literals (R-6)

- **FR-L1** `"order rejected"_lit` and `"order rejected"_` (UDLs in `es::serreg::literals`), `serialize<"…">(out, …)`
  and `constexpr lit<"…"> k;` all yield `lit<fixed_string{…}>`, a structural NTTP wrapper; four spellings, one literal.
- **FR-L2** Passing a `lit<S>` to `serialize` registers it (blob in the literal table and the ELF section).
- **FR-L3** Readers resolve ids through the registry or the dictionary; an unknown id prints as `lit#<hex>`.
- **FR-L4** The literal's text is stored once per image, in the ENTRY blob; `lit<S>::text()` and the registry view it
  there. **FR-L5** Run-time strings are data (bytes on the wire), never interned.
- **FR-L6** In a compact frame a literal record occupies **no bytes**: the call hash names the exact signature and
  the call ENTRY lists `lit:<id>` at that position; `n_records` still counts every argument. Full-records mode keeps
  `u64 id`. `serialize(out, "heartbeat"_)` is a bare 16-byte header.

### 8.2 Compile-time constants — FR-A5 (A-6)

`42_`, `-7_`, `0x10_`, `1.5_` (numeric `operator""_`) and `k<V>` yield `constant<V>` (V a scalar or an enum) at any
argument position. An integer literal is `int`, else `long long`, else `unsigned long long` (also for hexadecimal,
octal and binary; `k<0xFFFFFFFFu>` for another type); a floating literal is `double` (at most 15 significant digits,
exponent within ±22; otherwise a compile error naming `k<value>`); a character constant is `k<'c'>`; no class-type
constants. A constant (a) contributes `const:<kind>=<value>` (floating values by bit pattern) or `const:<enum schema
hash>=<underlying value>` to the call string, so type and value are in the call hash; (b) is an `arg_const` ARG
followed by `u64 value` in the call ENTRY (24 bytes); (c) occupies no bytes in a compact frame; (d) in a full-records
frame is the scalar or enum record it would be at run time. Readers present it as that record (`is_const`); a typed
reader names the same constant, another value is `hash_mismatch`. `constant<V>{}` is the long form of `k<V>`.

### 8.3 Print frames — FR-P*

- **FR-P1** `print(out, "fmt {} {}"_, args...)` / `println(...)` (and `_with(options, …)`,
  `print<"…">`): the format string is a literal, the frame is exactly `serialize(out, lit, args...)`'s with
  the call string `print(n){…}` / `println(n){…}`; nothing is formatted by the writer.
- **FR-P2** The format string is checked at compile time by the standard's own checker (`std::format_string<render_t<Ts>...>`):
  placeholder count, positional indices, escaped braces and every `{:spec}` against the argument's rendered type.
  Scalars render as themselves; `std::byte`, `char8_t`, `char16_t`, `char32_t`, `wchar_t` are rejected; literals,
  strings, enums (enumerator name, else the number) and classes (the one-line text form) render as `string_view`.
  Nested replacement fields (`{:>{}}`) are a compile error. A user's `std::formatter<T>` is never consulted by readers.
- **FR-P3** The call ENTRY's flags carry `print` (bit 0) and `newline` (bit 1); `println` costs zero bytes per frame.
- **FR-P4** Rendering is a reader service: `tools::render_frame(frame, dictionary) -> result<std::string>`, per
  placeholder, with the argument's C++ type chosen at run time from the dictionary kind; nothing throws; a corrupt
  dictionary is `corrupt_dictionary`.
- **FR-P5** `srdump` text mode prints the rendered message for print and log frames (`--raw` for records); JSON and
  TOML keep the records and add `"message"`; `deserialize_print(bytes, dict, std::string&)` for programs.
- **FR-P6** `print` is hot-path clean: header + `memcpy` of scalars and string bytes, no formatting, no
  allocation, `noexcept` for `noexcept` sinks, `frame_size` constexpr when no run-time string.

### 8.4 Logging — FR-G* (R-11, A-8, A-9, P-15, P-16)

`log.hpp`: `enum class level { trace, debug, info, warn, error, fatal, off }`; `logger<log_options Opts, sink S>`
with `logger(S&, level min = trace)`, `enabled(level)`, `set_level`, `min_level`, `set_clock(fn)`,
`write<L, File, Line>(lit<F>, args...)`, the level-named members `trace(lit<F>, args...)` … `fatal(…)` (file `"-"`,
line 0), `last_error()`, `failed()`. The macro `ES_SERREG_LOG(lg, lvl, "fmt {}"_, args...)` passes `__FILE__` and
`__LINE__`.

- **FR-G1 Gate.** `enabled(level)` is one relaxed atomic load of the run-time minimum level; `ES_SERREG_LOG` evaluates
  **no argument** when the level is off. A logger's call site never allocates, locks or formats; the frame is built
  as a print frame (FR-P6).
- **FR-G2 Floor.** `log_options::floor` and the translation-unit macro `ES_SERREG_LOG_MIN_LEVEL` are compile-time
  floors: a call below the floor compiles to nothing (no registration, no code); `enabled()` is `false` at compile
  time there.
- **FR-G3 Envelope.** A log frame is a print frame with the call flag `log` (bit 2) whose signature is `log_tag,
  constant<u8 level>, lit<File>, constant<u32 Line>, constant<u8 mask>, u64 time, [u32 thread], [u64 sequence],
  lit<Format>, args…`. Level, file, line and the options mask are zero-byte constants of the call; the timestamp,
  and by policy the thread id and sequence, are the only bytes beyond `println`'s. `log_layout` gives
  readers the fixed positions. Rendering: `<time> <LEVEL> <file>:<line> [tid] #seq message`, the time as
  `YYYY-MM-DD HH:MM:SS.nnnnnnnnn` for the system clock and as raw ticks otherwise.
- **FR-G4 Policies.** `log_options` is a structural NTTP, so every policy is `if constexpr` and two loggers with
  different policies are two call signatures: `thread_policy { none, once, per_message }` (`once`: the writer
  thread's id goes into the writer-info frame, `capture_options::info_thread`; `per_message`: a `u32` argument),
  `sequence_policy { none, per_sink }` (a `u64` counted by the logger), `clock_policy { system, steady, tsc, user }`
  (`user`: `set_clock`), and the mask constant records thread, sequence and clock so a reader renders the timestamp
  without the writer's source. A write returns `bool`; failures are counted in `failed()` and kept in `last_error()`.
- **FR-G5 Scoped dictionaries.** `dictionary_policy { scoped, full }`: a logger over a plain sink **MUST** use `full`
  (a `requires` clause enforces it); over a `scoped_sink` it uses the sink type's `dictionary_scope`. A
  `scoped_capture_file<Scope>`'s opening dictionary and deltas hold only the calls serialised into sinks of that
  scope, closed over their argument types and literals (FR-R15) — computed at compile time, written per file, no
  per-frame cost. The writer-info frame records `dictionary = scoped:<name>` or `full`.
- **FR-G6 Tool views.** `srdump --level <lvl>` prints bare rendered lines (no dictionary) of the frames at or above
  the level; `--json` gives `{"frame":N,"log":{level,time,file,line,[thread],[sequence],message}}` per line;
  `--since <t>` / `--until <t>` filter by timestamp (nanoseconds or an ISO date, UTC). The dictionary labels log calls
  `log(...)` in text and `"form":"log"` in JSON/TOML; the Wireshark dissector renders `LEVEL file:line [tid] #seq`.
- **FR-G7 Router.** `frame_router::on_log(handler(level, std::string_view line))` receives the rendered line of every
  log frame no typed handler took (A-12).
- **FR-G8 Cost.** With defaults (`thread = once`, `sequence = none`, `clock = system`) a log frame costs the 16-byte
  header + 8 bytes of timestamp + the arguments' records; with `trace` floors in a translation unit nothing is
  registered for the compiled-out calls.

---

## 9. Rendering values (FR-A7..A11)

`to_string.hpp` (A-10), for any value `serialize` accepts except a bare literal or constant:

- **FR-A7** `to_string(v) -> std::string`: the one-line text form print and `srdump` show — `Type{field: value, …}`,
  enums by enumerator name, strings quoted, scalars as numbers, containers as `[…]` / `{k: v, …}`.
- **FR-A8** `to_json(v) -> std::string`: `srdump`'s JSON value — enums `{"name":…,"value":…}`, 64-bit integers beyond
  ±2^53 as strings, maps as `[{"key":…,"value":…}]`.
- **FR-A9** `render(v, tools::format) -> result<std::string>` (text | json | toml) is the primitive; it fails with
  `unknown_type` only when `T`'s entry is not in the process dictionary, or with a decoder error for a corrupt image.
- **FR-A10** `template <class T> struct formattable : std::false_type {};` — specialise as `true_type` to get
  `std::formatter<T>` (`std::format`, `std::print`); the spec `{:j}` renders the JSON form.
- **FR-A11** Rendering is dictionary-driven: `detail::record_of(v)` (the compact record bytes) + `tools::render_value`
  over `tools::process_dictionary()` (the registry's image parsed once per registry version under a shared lock), so
  the text equals what readers show. Instantiating it registers `T` like a serialize call (FR-R1). It allocates, takes
  a shared lock and links `es::serreg_tools`: **not for the hot path** — serialize the value and let a reader render.

---

## 10. Tools: srdump, Wireshark, C++ generator, Python reader (FR-T*, FR-D*)

- **FR-D0** All functionality is in `es::serreg::tools` and `es::serreg::cli`; `srdump`'s `main` is one line; the CLI
  is embeddable (`cli::parse`, `cli::execute`, `cli::app{name, version, extra_usage, on_extra}`, `cli::run(argc, argv,
  app)`). `srdump` never does anything the library cannot do (A-2).
- **FR-D1** `srdump <capture.ser>` prints the dictionary, then every data frame decoded. Exit codes 0 / 1 / 2.
- **FR-D2** Options:

| Group | Flags |
|---|---|
| sources | `<capture.ser>`, `--stdin`, `--elf <bin>` (repeatable), `--rri <image>`, `--dict <rri>`, `--rri-out <file>` |
| output | `--text` (default), `--json`, `--toml`, `--dict-only`, `--verbose`, `--raw`, `--hex`, `--full` (no elision), `--flatten`, `--program-hash`, `--version` |
| selection | `--frames A..B`, `--type <name\|hash>`, `--level <lvl>`, `--since <t>`, `--until <t>` |
| verification and recovery | `--verify`, `--info`, `--resync`, `--window <size>`, `--frame-limit <size>` |
| comparison | `--diff <a> <b>`, `--direction old-data\|new-data\|both\|none` |
| generation | `--wireshark <out.lua>`, `--cxx <types.hpp\|->`, `--cxx-router`, `--namespace old=new` |
| configuration | `--config <file.toml>` (the FR-D9 subset) |

- **FR-D3** `--diff <a> <b>` classifies every type, call and literal: `identical | names-only | metadata-only |
  breaking | added | removed | duplicate-name`. The verdicts describe identity, not decodability. `--direction`
  (default `old-data`: a reader built from the new side reads old data) decides whether removals (`old-data`) or
  additions (`new-data`) are breaking, `both` marks both, `none` neither; layout changes, changed calls and changed
  literals break in every direction; the exit status follows (A-5). `--compat` (projection report) is R-9
  (**2.3 — planned**).
- **FR-D4** Zero knowledge of user types at build time: every tool works from dictionaries.
- **FR-D10** `--cxx` writes C++ declarations for every type of the dictionary sources (fixed-width scalars, `enum
  class E : <kind>`, `std::array`, the dynamic member types — every `map` shape as `std::map`, every `set` as
  `std::set` — bases, `since`/`deprecated`, padding where the dictionary has gaps, `[[=es::serreg::name("…")]]` where a
  plain declaration cannot spell the name) followed by `static_assert`s on size, alignment, member offsets and the
  schema hash of every type; `--cxx-router` adds a `frame_router` skeleton with one handler per call; `--namespace
  old=new` remaps. The generated header is compiled and run in the test suite (T-7).
- **FR-T1** `--info`: header, dictionary blocks (the chain), checkpoints and writer-info items of a 1.7 file by seeks —
  O(links), never O(frames) — through `tools::info(path) -> result<capture_info>` (`to_text`, `to_json`); a 1.6 file
  yields its dictionary and no chains; a pipe is `unsupported` (T-8).
- **FR-T2** `--resync`: after an unparsable frame the windowed reader scans forward 8 bytes at a time for the next
  checkpoint frame and continues there; the skipped range is reported.
- **FR-T3** `--level`, `--since`, `--until`: the log views of FR-G6.
- **FR-T4** The pure-Python reader (`python/serreg`, stdlib only, Python ≥ 3.10; T-6): `python3 -m serreg <file.ser>
  [--json|--text] [--verify] [--info] [--level LVL] [--dict x.rri] [--rri] [--dict-only] [--hex]` parses RRI/ENTRY
  streams, FILE 1.6 (back-patched) and 1.7 (CHAIN, deltas, CRC, checkpoints, writer info), bare streams, compact and
  full records, flat structs, enums, arrays, the whole canonical image (strings, vectors, optionals, variants, maps,
  sets, nested), transparent types, print and log rendering with a `std::format` subset; it never raises on corrupt
  input. **Semantic parity with `srdump --json`** on every golden and corpus fixture and a generated log capture is a
  ctest (`python_reader_parity`). Not covered: XXH3 recomputation, `--elf`, `--diff`, `--resync`, pipes.
- **FR-T5** The Compiler Explorer examples (`ce-examples/ce-example0..7`) are part of every build and test run:
  each compiled as one translation unit against its own header symlinks with `-O1 -W -Wall -Wextra -Wshadow -Werror`
  and run (ctest label `ce`); the godbolt lanes (`ce-run.py`, `docs/ce-links.md`) remain the cross-compiler gate (T-3).
- **FR-T6** `tests/bench/registry_measure` synthesises N struct entries of B bytes, pushes them as guards do, and
  reports push, publish, lookup, export, parse and RSS; `--assert` enforces FR-R13; the architecture document lists the
  options a program with many types has (fewer, larger message types; scoped capture files;
  `capture_policy{.dictionary_updates = false}`; `checkpoint_dictionary = false`).
- Wireshark (G-13, T-5): `--wireshark <out.lua>` generates a dissector for the dictionary's types and calls (Wireshark
  4.2+, Lua 5.2 and 5.4); the heuristic recognises frames by call hash and the 1.7 meta sentinels; the dissector and
  `srdump --json` **MUST** agree on every record of the fixtures (parity under tshark and plain Lua).
- `share/serreg.magic` teaches `file(1)` captures and registry images.

---

## 11. Shared libraries, loader, versioning and compatibility (FR-V*)

### 11.1 Loader

- The per-image entities (`descriptor_v`, `call_desc_v`, `registrar*`, blobs) have hidden visibility; the `es::serreg`
  target adds `-fno-gnu-unique` (ELF builds; `STB_GNU_UNIQUE` symbols would make glibc mark a DSO `NODELETE` and
  defeat reloading). `ES_SERREG_BUILD_SHARED` (default ON) is required when `dlopen`ed libraries serialise (FR-R3).
- Unload and reload follow FR-R10–FR-R12. What the registry cannot protect: a plugin's own code (an `on_change`
  callback defined in a plugin **MUST** be removed before `dlclose`), objects and threads.
- Installed targets carry their final `$ORIGIN`-relative RPATH; `cmake --install` never relinks.
- Hidden visibility for the runtime library with an exported-symbol list checked by a ctest is A-15 (**2.3 — planned**).

### 11.2 Versioning (G-2, P-14, P-19, P-20)

- **FR-V1** SONAME is `major.minor`: `libserreg.so.2.2`, `libserreg_tools.so.2.2` (ctest `soname_major_minor`).
- **FR-V2** The CMake package declares `COMPATIBILITY SameMinorVersion`: `find_package(serreg 2.2)` accepts 2.2.x only.
  Until 3.0 the minor is the compatibility unit: a wire-format or ABI change bumps the library minor (2.3 would carry
  format 1.8); patch releases never change bytes.
- **FR-V3** Format acceptance window: readers accept the current format minor and the previous one — `format_minor =
  7`, `format_minor_oldest = 6`. Older minors are `bad_version`; a newer minor is `bad_version`, never guessed at.
  Writers stamp the current minor. Readers reject unknown frame flags.
- **FR-V4** The recorded exception: format 1.7 shipped in a patch release of 2.2 because the library was not yet
  outside its development tree. No further exception is planned; the rule of FR-V2 applies to every later release.
- **FR-V5** After 3.0 (**planned**): semantic versioning with an additive-only wire format — new frame flags, ENTRY
  tags, ARG kinds and info keys may appear within a major and a reader skips what it does not know; removing or
  re-meaning a field is a new major. 2.x design never precludes it.
- **FR-V6** Versions change only through `scripts/bump-version.sh`; deliverables are versioned
  (`ser-reg-project-vX.Y.Z.tar.gz` with top-level `ser-reg-project-vX.Y.Z/`, built by `scripts/make-release.sh`, never
  reused); one `CHANGELOG.md` entry per version; every new file classified in `public-files-list.txt` (C-8).
- Byte-level format changes refreeze `tests/golden/blobs.cpp`, the golden fixtures and the canary together with the
  `format_minor` bump and a CHANGELOG line — never silently (C-7).

---

## 12. IPC (`es::ipc`)

`es::ipc` (G-4) is a header-only C++23 library of variable-size shared-memory rings — SPSC queue and SPMC broadcast,
ring layout 2 (`detail::ring_version = 2`): every record starts with its size, sequence and type, positions count
units of 16/32/64 bytes in 32-bit words, gates separate an ownership word (changed only by CAS) from a position word
(plain stores). Regions are files on tmpfs or hugetlbfs or anonymous mappings, with normal, 2 MiB or 1 GiB pages and
optional `mlock`. The producer is a `claiming_sink`, so `serialize()` builds the frame in place; consumers
`deserialize`/`dispatch` in place. It validates its headers by its own never-crash rules and has no dependency on
`es::serreg` in either direction. A layout change bumps `ring_version`. The fixed-slot rings that preceded it
(`es::ipc_old`) are out of the build (G-17). Design, guarantees, tests and the latency picture: `docs/ipc/ipc-design.md` (§4–§5 are
normative for record headers and gates), `docs/ipc/testing-and-benchmarks.md`, `docs/ipc/latency-analysis.md`,
`docs/ipc/ipc-optimization-directions.md`.

---

## 13. Performance budgets

- **NFR-1 Hot-path ceilings** (`tests/bench/bench.cpp`, mean over 100 000 iterations, release `libserreg`):
  `serialize()` of a 32-byte flat struct into a `span_sink` **< 50 ns**; `inspect()` of a one-record frame
  **< 500 ns**. Under a Debug library both ceilings are relaxed 4× (`ES_SERREG_BENCH_DEBUG_LIB`). Benchmarks gate
  as functional smoke tests; numbers are recorded only from the reference box (T-9).
- **Zero-overhead statement:** for flat types with a `claiming_sink` or `serialize_raw`, the generated code equals a
  manual `memcpy` plus explicit zeroing of the pads; `serialize()` leaves no call at `-O2` (`[[gnu::always_inline]]`
  fill lambdas; ctest `serialize_inlines_at_O2`, C-5).
- **Nothing on the serialization hot path allocates or locks**; no registry access, no atomics beyond the optional
  per-frame relaxed load of the registry version in a `capture_file` (removable, §5.2) and the logger gate. Documented
  exceptions: a dynamic image above 64 KiB on a plain sink is streamed in 16 KiB chunks; sorting an unordered container
  above 64 entries allocates (FR-W9); `to_string` allocates by design (FR-A11).
- **Registry (FR-R13):** 10 000 entries / 10 MB → push + publish ≤ 10 ms; 100 000 entries / 100 MB → ≤ 100 ms
  (release, reference box); lookup O(log n) through the `flat_map`; load-time cost per registration = one mutex, one
  hash-map lookup, one `dladdr`, a heap copy for images other than the main program.
- **Per-frame overhead of 1.7 features:** CRC 8 bytes + one CRC32C per frame when on; checkpoints 64 bytes per
  checkpoint (plus the dictionary when asked); the dictionary version compare one relaxed load; all zero when off.
- **Compile time:** descriptor generation is linear in the number of members; a 1 MiB buffer member compiles in about
  3 s; `tests/probe` bounds a depth-4 nesting at 60 s. **Binary size:** one descriptor and one blob per type per
  translation unit (≈100–450 B).

---

## 14. Non-blocking defaults (current values)

| Item | Value |
|---|---|
| Dictionary frame sentinel | `0x4553'5352'4449'4354` (`"ESSRDICT"`) |
| Checkpoint frame sentinel | `0x4553'5352'434b'5054` (`"ESSRCKPT"`) |
| Writer-info frame sentinel | `0x4553'5352'494e'464f` (`"ESSRINFO"`) |
| `since` / `deprecated` width | `uint16_t`, 0 = unset |
| TOML subset for `--config` | as FR-D9 |
| Dictionary update strategy in files | inline delta frames linked by a forward chain, each link patched once; never a rewrite at close (FR-W12) |
| Signal-triggered dump | a `sigwait` helper thread started at library load (no polling) |
| Abort diagnostic on `dlclose` | none — `dlclose` never aborts (FR-R10) |
| `ES_SERREG_CONFLICT` | `warn` |
| `ES_SERREG_DUMP_ON` | `exit` |
| Checkpoint timer granularity | checked every 64 frames |
| `sync_policy` | `dictionary` |
| Writer-info frame | on; host, pid and start time on; writer thread id off |
| Hot-path stack frame limit / stream chunk | 64 KiB / 16 KiB |
| Unordered-container sort index on the stack | up to 64 entries |
| Pipe read buffer | the 10 MiB window (`window_bytes`), grown for one frame up to `frame_bytes` (R-2) |

---

## 15. Planned: 2.3 evolution level 1, 3.0 contract

### 15.1 2.2.x patch releases (format 1.7, no byte changes)

- **A-16 (2.2.x — planned):** the dictionary-driven enum range check in `verify`, `walk_values` and `srdump` (§7.2);
  `std::list` / `std::deque` as the `vector` shape; `serialize_options::canonical_unordered = false` to skip the
  sort for writers that do not need byte identity (the writer-info key already exists).

### 15.2 2.3.0 — schema evolution level 1 (R-9, G-15; format unchanged)

A reader built from a newer **or** an older definition reads the other side's frames through the dictionary:
- members matched by name; **widening allowed, narrowing never** (`i16 → i32`, `f32 → f64`); R3 (unsigned → strictly
  wider signed) and R12 (an array that grew) kept;
- `[[=es::serreg::renamed_from("old")]]` on members and classes;
- variant alternatives matched by the alternative type's canonical name (`std::string` regardless of allocator; a
  renamed alternative needs `renamed_from` on the class);
- nested types project by structure when the member name matches — a renamed nested type is reported
  (`projection_report`, `srdump --diff --compat`), not blocking;
- enums match by underlying kind; `since`/`deprecated` gain their meaning;
- no wire change; the typed readers, `dispatch` and `frame_router` take a projection; the layout hash remains the O(1)
  "same bytes" fast path.
- Also 2.3.0: hidden visibility for the runtime library with an exported-symbol list and a ctest (A-15).
- Levels 2 (structural changes beyond level 1) unscheduled; level 3 not planned (R-10).

### 15.3 3.0 — the frozen public contract

- **P-18 Portable wire mode** beside the native one: fixed-width little-endian scalars, declared widths for `long` and
  `wchar_t`, canonical offsets for flat types; the `memcpy` fast path stays when the ABI tags match.
- **P-19 / FR-V5 Additive-only wire format** after 3.0; semantic versioning.
- **D-5 A written wire specification** (not `format.hpp` comments); the Python reader's `format.py` and the architecture
  document's byte tables are its draft.
- **A-13 Index reclamation:** a `snapshot_guard` with epoch reclamation beside `snapshot()` (today every published
  index lives until exit, about 80 bytes per entry per publish).
- **A-14 `registry::forget_retained()`.**
- Open (G-6..G-11): Windows / macOS / aarch64 (clang-p2996 with the MSVC ABI first); the license (MIT today; BSL-1.0
  for Boost, Apache-2.0 WITH LLVM-exception for Beman); layering under neutral names (`fingerprint`, `serialize`,
  `registry`, `tools`, `ipc`) and renaming at submission; provenance of AI-assisted code recorded from now on; an SG7
  experience paper on canonical type identity with P2996 and load-time registration (2027 mailing); the public
  repository (`public-files-list.txt` set, CI lanes `gcc` and `clang-libcxx`, OSS-Fuzz later).

---

## Appendix A — Requirement ID index

One line per ID: ID — title — tests that cite it (`grep -rl "<ID>" tests/`; paths relative to `tests/`). "—" means no
test cites the ID yet; the test that covers the behaviour is named in parentheses where one exists.

### Serialization

| ID | Title | Where tested |
|---|---|---|
| FR-S1 | `serialize` — one frame per call | unit/test_roundtrip.cpp |
| FR-S2 | sinks and claiming sinks | unit/test_roundtrip.cpp |
| FR-S3 | `frame_size` constexpr / run-time | golden/hashes.cpp, unit/test_roundtrip.cpp |
| FR-S4 | typed `deserialize`, `decode_error` | unit/test_dynamic.cpp, unit/test_readers.cpp, unit/test_roundtrip.cpp |
| FR-S5 | `serialize_raw` / `deserialize_raw` | unit/test_roundtrip.cpp |
| FR-S6 | padding scrub, `bool` normalisation | golden/hashes.cpp, unit/test_print.cpp, unit/test_roundtrip.cpp |
| FR-S7 | unsupported members rejected by name | negative/CMakeLists.txt, negative/neg_multimap_member.cpp, negative/neg_pointer_member.cpp, negative/neg_string_view_member.cpp |
| FR-S8 | `as_flat<T>`, `[[=skip]]` | golden/corpus.hpp, negative/neg_nested_as_flat.cpp, unit/test_roundtrip.cpp |
| FR-S9 | all members, private included | golden/corpus.hpp, golden/hashes.cpp, unit/test_roundtrip.cpp |
| FR-S10 | `const` parameters | unit/test_roundtrip.cpp, unit/test_skip.cpp |
| FR-S11 | `since` / `deprecated` | golden/corpus.hpp, golden/hashes.cpp |
| FR-A1 | scalar arguments | golden/hashes.cpp, unit/test_print.cpp |
| FR-A2 | run-time string arguments | unit/test_print.cpp |
| FR-A3 | `frame_size` with strings | unit/test_print.cpp |
| FR-A4 | string / scalar outputs | unit/test_print.cpp |
| FR-A5 | compile-time constants | golden/hashes.cpp, unit/test_roundtrip.cpp |
| FR-A6 | containers as arguments (transparent types) | golden/hashes.cpp, unit/test_containers.cpp |

### Registry

| ID | Title | Where tested |
|---|---|---|
| FR-R1 | registration by instantiation, recursive | unit/CMakeLists.txt, unit/premain_check.cpp, unit/test_registry.cpp |
| FR-R2 | registration at image load | link/CMakeLists.txt, link/link_types.hpp, link/main.cpp, unit/premain_check.cpp, unit/test_registry.cpp |
| FR-R3 | one registry per process | dlopen/host.cpp, link/CMakeLists.txt, link/main.cpp, unit/test_registry.cpp |
| FR-R4 | identity, duplicates, conflicts | dlopen/plugin_conf_a.cpp, dlopen/plugin_conf_b.cpp, golden/corpus.hpp, unit/CMakeLists.txt, unit/test_anon_tu.cpp, unit/test_conflict.cpp |
| FR-R5 | registry API | unit/test_registry.cpp |
| FR-R6 | concurrency, deferred publication | dlopen/CMakeLists.txt, dlopen/host.cpp, dlopen/plugin.cpp |
| FR-R7 | `--gc-sections`, `-flto`, archives | dlopen/host.cpp |
| FR-R8 | no SIOF | — (unit/premain_check.cpp) |
| FR-R9 | environment and signal hooks | unit/CMakeLists.txt, unit/test_capture.cpp, unit/test_registry.cpp |
| FR-R10 | `dlclose` never aborts | unit/test_registry.cpp |
| FR-R11 | retained entries, reload | dlopen/host.cpp |
| FR-R12 | per-image accounting | dlopen/host.cpp, link/main.cpp, unit/test_registry.cpp |
| FR-R13 | registry budgets 10k / 100k | — (bench/registry_measure.cpp; ctests `registry_measure_10k`, `registry_measure_100k`) |
| FR-R14 | main-program entries viewed in place | — (bench/registry_measure.cpp, unit/test_registry.cpp) |
| FR-R15 | scope nodes, scoped and delta exports | — (unit/test_log.cpp "the scoped dictionary holds the scope's calls…") |
| FR-R16 | retained entries in exports | — (dlopen/host.cpp) |

### Wire format

| ID | Title | Where tested |
|---|---|---|
| FR-W1 | file and stream containers | unit/test_capture.cpp |
| FR-W2 | compact default, full records opt-in | unit/runtime_selftest.cpp, unit/test_capture.cpp, unit/test_roundtrip.cpp |
| FR-W3 | dictionary sufficiency | unit/runtime_selftest.cpp |
| FR-W4 | native layout, ABI tag | unit/test_tools.cpp |
| FR-W5 | canonical JSON / TOML | unit/runtime_selftest.cpp |
| FR-W6 | format version in headers | — (unit/test_format17.cpp "the readers still read 1.6 files, and refuse 1.5 and 1.8") |
| FR-W7 | dictionary completeness | unit/CMakeLists.txt, unit/test_capture.cpp, unit/test_sinks.cpp |
| FR-W8 | variants in the dictionary | — (golden/hashes.cpp, unit/test_dynamic.cpp) |
| FR-W9 | map and set shapes | golden/hashes.cpp |
| FR-W10 | CRC32C trailer | — (unit/test_format17.cpp "CRC32C trailers…", "serialize_options{.crc} on a plain sink…") |
| FR-W11 | checkpoint frames | — (unit/test_format17.cpp "checkpoint frames…"; golden `features.ser`) |
| FR-W12 | dictionary chain, deltas, no back-patching | — (unit/test_format17.cpp "a registry change between frames writes a dictionary delta inline…") |
| FR-W13 | writer-info frame | — (unit/test_format17.cpp "a capture file is written as 1.7…"; ctest `golden_srdump_info_features`) |
| FR-W14 | meta frames consumed | — (unit/test_format17.cpp; python/parity.py) |

### Hashes and dictionary

| ID | Title | Where tested |
|---|---|---|
| FR-H1 | schema and layout hashes | golden/CMakeLists.txt, golden/hashes.cpp |
| FR-H2 | call hash | — (golden/hashes.cpp) |
| FR-H3 | program hash, order-independent | golden/CMakeLists.txt, link/CMakeLists.txt, link/main.cpp, link/tu_extra_first.cpp, unit/runtime_selftest.cpp, unit/test_hash.cpp, unit/test_registry.cpp |
| FR-H4 | cross-compiler stability | golden/CMakeLists.txt, golden/hashes.cpp, probe/depth4.cpp |
| FR-H5 | algorithm id, XXH3-64 | unit/CMakeLists.txt, unit/test_hash.cpp, unit/test_print.cpp |
| FR-H7 | literal id | golden/hashes.cpp |
| FR-H8 | 64-bit ids only | — (canary/, golden/blobs.cpp) |
| FR-D6 | `--elf` extraction | support/test_support.hpp, unit/CMakeLists.txt, unit/runtime_selftest.cpp, unit/test_capture.cpp, unit/test_registry.cpp, unit/test_skip.cpp |
| FR-D7 | `--rri` | — (unit/test_tools.cpp) |
| FR-D8 | text / JSON / TOML renderers | — (golden/compare_srdump.cmake) |
| FR-D9 | in-house parsers, TOML subset | — (fuzz/fuzz_targets.cpp, fixtures/corpus/config) |

### Reading and limits

| ID | Title | Where tested |
|---|---|---|
| FR-D5 | the never-crash contract, `decode_limits` | fuzz/CMakeLists.txt, fuzz/fuzz_driver.cpp, fuzz/fuzz_targets.cpp, fuzz/fuzz_targets.hpp, unit/CMakeLists.txt, unit/runtime_selftest.cpp, unit/test_limits.cpp, unit/test_roundtrip.cpp |
| FR-D11 | windowed readers | — (unit/test_limits.cpp) |

### Print, literals, constants, logging

| ID | Title | Where tested |
|---|---|---|
| FR-L1 | literal spellings | unit/test_roundtrip.cpp |
| FR-L2 | literal registration | unit/test_roundtrip.cpp |
| FR-L3 | id resolution, `lit#<hex>` | golden/blobs.cpp, unit/runtime_selftest.cpp |
| FR-L4 | text stored once per image | unit/test_roundtrip.cpp |
| FR-L5 | run-time strings never interned | — (unit/test_print.cpp) |
| FR-L6 | literals cost 0 bytes | golden/hashes.cpp, unit/CMakeLists.txt, unit/runtime_selftest.cpp, unit/test_print.cpp, unit/test_readers.cpp, unit/test_roundtrip.cpp, unit/test_tools.cpp |
| FR-P1 | print frames | golden/hashes.cpp, unit/test_print.cpp |
| FR-P2 | compile-time format check | negative/CMakeLists.txt, negative/neg_print_arg_count.cpp, negative/neg_print_bad_spec.cpp |
| FR-P3 | call flags print / newline | — (golden/blobs.cpp) |
| FR-P4 | `render_frame` | unit/test_print.cpp |
| FR-P5 | srdump print view | unit/test_print.cpp |
| FR-P6 | print hot-path clean | — (bench/hot_inline.cpp) |
| FR-G1 | logger gate | — (unit/test_log.cpp "the gate evaluates nothing below the level…") |
| FR-G2 | compile-time floor | — (unit/test_log.cpp, same case) |
| FR-G3 | log envelope | — (unit/test_log.cpp "the envelope by policy…") |
| FR-G4 | thread / sequence / clock policies | — (unit/test_log.cpp "the envelope by policy…") |
| FR-G5 | scoped dictionaries, `dictionary_policy` | — (unit/test_log.cpp "the scoped dictionary…", "a logger over a plain sink needs dictionary_policy::full…") |
| FR-G6 | `--level`, `--since`, `--until` | — (unit/test_log.cpp; python/parity.py) |
| FR-G7 | `frame_router::on_log` | — (unit/test_log.cpp) |
| FR-G8 | log frame cost | — (unit/test_log.cpp "the envelope by policy…") |

### Rendering values

| ID | Title | Where tested |
|---|---|---|
| FR-A7 | `to_string` | — (unit/test_to_string.cpp "to_string registers the type and renders…") |
| FR-A8 | `to_json` | — (unit/test_to_string.cpp, same case) |
| FR-A9 | `render(v, format)` | — (unit/test_to_string.cpp "dynamic members, containers as values…") |
| FR-A10 | `formattable<T>` | — (unit/test_to_string.cpp "formattable<T>: std::format and std::print…") |
| FR-A11 | dictionary-driven, registers, not hot path | — (unit/test_to_string.cpp) |

### Tools

| ID | Title | Where tested |
|---|---|---|
| FR-D0 | services in the library, one-line `main` | — (examples/embedded_cli.cpp; golden/compare_srdump.cmake) |
| FR-D1 | `srdump <capture>` | unit/runtime_selftest.cpp, unit/test_capture.cpp |
| FR-D2 | options | unit/test_registry.cpp |
| FR-D3 | `--diff`, `--direction` | unit/runtime_selftest.cpp, unit/test_registry.cpp |
| FR-D4 | no user types at build time | — (golden/compare_srdump.cmake) |
| FR-D10 | `--cxx`, `--cxx-router` | — (cxxgen/) |
| FR-T1 | `--info`, `tools::info` | — (unit/test_format17.cpp "checkpoint frames…"; ctest `golden_srdump_info_features`) |
| FR-T2 | `--resync` | — (unit/test_format17.cpp "--resync: after a corrupt frame_len…") |
| FR-T3 | log views | — (unit/test_log.cpp) |
| FR-T4 | Python reader parity | — (python/parity.py; ctest `python_reader_parity`) |
| FR-T5 | CE examples as tests | — (ce/CMakeLists.txt; ctests `ce_exampleN_builds_and_runs`) |
| FR-T6 | `registry_measure` | — (bench/registry_measure.cpp) |

### Versioning and performance

| ID | Title | Where tested |
|---|---|---|
| FR-V1 | SONAME `major.minor` | — (ctest `soname_major_minor`, tests/CMakeLists.txt) |
| FR-V2 | `SameMinorVersion` | — (the install + `find_package` consumer stage of `run-all-tests.sh`) |
| FR-V3 | acceptance window 1.6 + 1.7 | — (unit/test_format17.cpp "…refuse 1.5 and 1.8"; corpus `*-1.6.*`, `*-1.7.*`) |
| FR-V4 | the format-1.7 patch-release exception | — (policy) |
| FR-V5 | additive-only after 3.0 | — (planned) |
| FR-V6 | bump script, versioned deliverables | — (scripts/make-release.sh checks) |
| NFR-1 | hot-path ceilings | bench/bench.cpp, unit/test_registry.cpp |
