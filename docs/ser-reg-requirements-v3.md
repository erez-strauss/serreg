# ser-reg — Reflection-Driven Serialization + Type Registry for C++26

**Document:** `ser-reg-requirements-v3.md` (supersedes v2)  
**Status:** v3 — review rounds 1 and 2 integrated; **baseline for M1** (no blocking open questions)  
**Target toolchain:** GCC 16.x (`-std=c++26 -freflection`), CMake ≥ 4, x86-64 Linux (v1)  
**Namespace:** `es::serreg`  
**Date:** 2026-09-27 (decisions A, G, I of 2026-10-01 in §0.00)

---

## 0. Review log

### 0.0 Decisions after v3 (maintainer, 2026-09-29; `docs/c-review-study.md`)

| # | Decision | Where integrated |
|---|---|---|
| D1 | **XXH3-64 replaces FNV-1a-64** for every identity (schema, layout, call, literal, program hash): `hash_algo` 2, wire format **1.3**, library 2.1.0. FNV is removed from the code; readers accept exactly 1.3 and refuse older files (`bad_version`) — the library was not yet in wide use, no data files had to stay readable | FR-H5, FR-H7, §6.2, `hash.hpp`, `format.hpp` |
| D2 | **ABI tag** in the FILE header (+40, the former reserved `u64`) and the RRI header (+32, the RRI header grows to 40 bytes), in the same format 1.3 | FR-W4, §6.7, `format.hpp` |
| D3 | Valueless variant (slot `n == 0`) in typed decode: a variant that has a `std::monostate` alternative decodes to it; otherwise the value is a decode error and walkers continue with the next item. **Done in 2.1.3**: `error::invalid_value`; `deserialize` still reads the later outputs, `dispatch` the later records, `walk_values` walks the value generically and reports it | FR-D5, plan v2.0 §2, `reflect.hpp` |
| D4 | A non-canonical `bool` byte on read is normalised to `true`, with an optional warning. **Done in 2.1.3**: `set_warning_handler` / `warning::noncanonical_bool` (`error.hpp`), `verify_report::warnings`; `view<T>` returns `invalid_value` instead (it cannot repair in place) | FR-S6, `error.hpp` |
| D5 | Schema evolution level 1: dictionary-driven projection by member name, with widening and `[[=renamed_from("old")]]`; no wire change | M6, `docs/c-review-study.md` §6 |

### 0.00 Decisions of 2026-10-01 (maintainer; `docs/cc-review-2.1.10-response.md` §4, library 2.1.13)

| # | Decision | Where integrated |
|---|---|---|
| A | **No abort on `dlclose`; plugins can be reloaded.** Every registration is copied into a heap entry when it is pushed (blob and name for every image, a deep copy of the descriptor for every image other than the main program). A `dlclose` drops the image's references and keeps the entries (*retained*, refs 0): lookups find them, pointers stay valid, frames still decode. A reload revives them. libserreg no longer interposes `dlopen`/`dlclose`; `ES_SERREG_ALLOW_DLCLOSE` is ignored. Replaces round-2 Q3 | FR-R10..R12, §6.3, §6.3b, §11 |
| G | **Three libraries, one package:** `es::serreg` (runtime), `es::serreg_tools` (srdump's CLI, Wireshark generator, diff) and `es::ipc` (header-only, C++23), which `es::serreg` no longer links. `find_package(serreg COMPONENTS ipc serreg tools)`; `ES_SERREG_IPC_ONLY` installs es::ipc without a reflection compiler | §5, §9, `CMakeLists.txt` |
| I | **Include rules:** package headers before system headers; every header includes what it needs to compile on its own; every header starts with `#pragma once`. Enforced by `.clang-format` (IncludeBlocks), `tests/headers` (one TU per header) and `scripts/check-includes.py` | §9, AGENTS.md §4 |

### 0.000 Decision of 2026-10-02 (maintainer; `directions-log.md` Part C, library 2.1.14)

| # | Decision | Where integrated |
|---|---|---|
| B | **Canonical names fixed now, as a hash change in 2.1.14 (wire format 1.4); no old captures to keep.** A name must never be shared by two different C++ types in one program, and must be the same on every compiler and standard library. A class nested in a class-template specialisation is named through its parent's canonical arguments (`ns::Outer<i32>::Inner`, review C-03). Standard-library types use a frozen vocabulary through `es::serreg::type_properties<T>` (`std::string`, `std::vector<E>`, `std::optional<E>`, `std::variant<…>`, `std::monostate`…; no allocator, no default argument), and std's implementation namespaces (`std::__cxx11`, `std::__1`, `std::chrono::_V2`) are dropped everywhere (C-04). **User inline namespaces stay in the name.** A type with no linkage-unique name — in an unnamed namespace or local to a function or lambda — must be named explicitly with `[[=es::serreg::name("…")]]` or `type_properties<T>` (C-03/C-05; this replaces the `{anon}` / `{fn:…}` rule). Recursive types are rejected with a clear message (C-07). Readers accept exactly 1.4 | §6.2b, `reflect.hpp`, `format.hpp`, `tests/golden/hashes.cpp`, `tests/negative/neg_*_name*.cpp`, `neg_recursive_type.cpp` |

### 0.0000 Decisions of 2026-10-03 (maintainer; `docs/c-review-and-roadmap-2.1.14.md`, library 2.1.15)

| # | Decision | Where integrated |
|---|---|---|
| C | **A typed reader reports expected versus actual, in one pass.** `deserialize`, `deserialize_print` and `deserialize_println` return `std::expected<std::size_t, decode_error>`: the error code, how many items the frame should yield (`expected` = 1 for the frame + its arguments), how many were decoded (`actual`), and the first item that failed (`failed`; 0 = the frame). No second pass and no temporaries: a frame or record that does not fit is refused before anything is written; a record that fails while it is decoded leaves the earlier arguments complete and the later ones untouched. `decode_error` converts to and compares with `error` | FR-S4, §6.4, `error.hpp` |
| O15 | **Compile-time constants are part of the call signature.** `_` builds a constant from a number as it builds a literal from a string, at any argument position (`42_`, `-7_`, `1.5_`; `k<V>` for `bool`, characters, other integer types, `float`, enums). Type and value flow into the call string and hash; the value is stored once in the call's dictionary entry and occupies no bytes in a compact frame. **Wire format 1.5** | FR-A5, §6.5, §6.6b, `literal.hpp`, `arg.hpp`, `format.hpp` |

### 0.00000 Decisions of 2026-10-06/07 (maintainer; `docs/plans-for-v2.1.16.md`, library 2.1.16)

| # | Decision | Where integrated |
|---|---|---|
| D | **Enums without a fixed underlying type are accepted; their kind is derived from the enumerators.** The underlying kind in every canonical string and descriptor is the unsigned kind of `sizeof(E)`, or the signed kind when an enumerator is negative — GCC's and Clang's own rule, so no hash moved; a compiler that picks `int` (MSVC) now agrees. Such an enum holds only the values of the minimal-width integer that represents its enumerators ([dcl.enum]); a value outside that range is undefined behaviour to read, so the typed readers can check those leaves and report `invalid_value` (the leaf reads as 0) — a compile-time policy, `ES_SERREG_VALIDATE_READ=1`, off by default (DECISIONS A-16). Enums with a fixed type are unchanged and never checked | §6.2b, FR-D5, `reflect.hpp` (`enum_underlying_kind`, `enum_ranges_v`), `deserialize.hpp` |
| E | **`srdump --diff` takes a direction.** Whether a removed or an added type, call or literal is "breaking" depends on who reads what: `--direction old-data` (default: a reader built from the new side reads old data) makes removals breaking, `new-data` makes additions breaking, `both` marks both, `none` is the 2.1.15 behaviour; layout changes, changed calls and changed literals break in every direction | FR-D3, `diff.cpp`, `cli.cpp` |
| O2 (in code) | **The never-crash contract and `decode_limits`** (answered 2026-09-30, implemented 2.1.16): FR-D5 below; `SECURITY.md` | FR-D5, `limits.hpp`, the readers, the fuzz driver |
| A1 | **`register_types<Ts...>()` / `register_call<Sig...>()`**: registration without a `serialize` or typed-reader call site | FR-R1, `registry.hpp` |
| A5 | **`srdump --cxx`**: C++ types and a `frame_router` skeleton generated from any dictionary source, self-checking through per-type hash `static_assert`s | FR-D10, `cxxgen.cpp` |
| A6 | **Captures of unlimited size**: every capture reader works on a window of `decode_limits::window_bytes` (10 MiB); no capture path reads a file whole | FR-D5, FR-D11, `reader.cpp` |

### 0.000000 Decisions of 2026-10-07 (maintainer; `docs/plans-for-containers.md`, library 2.2.0)

| # | Decision | Where integrated |
|---|---|---|
| Containers | **Maps and sets as members; any dynamic member type as a top-level argument. Wire format 1.6.** `std::map`, `std::unordered_map`, `std::flat_map` are the wire shape `map` (`kind::map_ = 53`: `{u32 off, u32 n}` → `n` entries of key then value at the pair's canonical offsets, **sorted by key**); `std::set`, `std::unordered_set`, `std::flat_set` are `set` (`kind::set_ = 54`). A member's kindspec is the shape (`map<K,V>` / `set<K>`), whatever the container, so a `std::map` member and a `std::unordered_map` member give the same schema and layout hashes and decode into each other; the container's vocabulary name (`std::unordered_map<K,V>`) appears only as the canonical name of a transparent type (a container passed as an argument). Unordered containers are sorted before encoding (the key's `operator<`, else the bytes of its flat image) so that equal values are equal bytes; a duplicate key in an image decodes as `invalid_value`. A container passed as an argument (`serialize(out, "ids"_, ids)` with a `std::vector<i32>`) is a **transparent type**: a synthetic struct ENTRY named after the container (`std::vector<i32>`), one unnamed 8-byte slot field, `struct_flag_transparent`; it travels as any dynamic-type record and renders as its value. The enum ENTRY's reserved byte now carries `enum_flag_fixed` (the enum has a fixed underlying type: no range check). `frame_flag_crc_reserved` (bit 2) is reserved for 1.7. Readers accept exactly 1.6 | FR-A6, FR-W9, §4.7, §6.5, `reflect.hpp`, `decoder.cpp`, `dictionary.cpp`, `formatters.cpp`, `wireshark.cpp`, `cxxgen.cpp` |

### 0.0000000 Decisions of 2026-10-13 (maintainer; `docs/plans-for-v2.2.3.md`)

| # | Decision | Where integrated |
|---|---|---|
| Vocabulary | **The std vocabulary is closed and complete for the common containers (DECISIONS A-17).** `std::deque`, `std::list`, `std::forward_list`, `std::stack` are the `vector<E>` shape; `std::pair`, `std::tuple` are positional dynamic classes (`first`/`second`, `_0`…`_N`), always image-encoded, `tuple<>` rejected; `std::multimap`, `std::unordered_multimap` are a `vector<pair<K,V>>` sorted by key, duplicates kept (schema kindspec `multimap<K,V>`). Every other std class, every pointer and smart pointer is rejected with a named diagnostic. `schema_string_v<T>` / `layout_string_v<T>` expose the hashed strings (A-18). No wire change | FR-A6, `reflect.hpp` (`check_supported`, `dyn_member`, the parts layer), `tests/golden/hashes.cpp`, `tests/negative/` |
| Validation | **The read-side enum range check is a compile-time policy, off by default (A-16):** `ES_SERREG_VALIDATE_READ=1` | FR-D5, decision D, `reflect.hpp` (`copy_flat`, `bools_canonical`), `docs/values-validation.md` |
| Evolution | **Schema evolution level 1 starts now, for flat types (R-9):** `renamed_from`, `tools::project<T>`, `deserialize(dict, …)`; the rest (dynamic members, router, `walk_values`, `--diff --compat`, dictionary metadata with format 1.8) follows in the next minor (G-15) | `projection.hpp`, `tests/unit/test_evolution.cpp`, `docs/schema-evolution-specs.md` §2.7 |

### 0.1 Round 2 (v2 → v3)

| Q | Decision | Where integrated |
|---|---|---|
| 1 | Literals: ID on the wire, literal text in the dictionary — **confirmed** | §4.4 (FR-L1..L5), §6.6 |
| 2 | File = header, dictionary, then one frame per `serialize()` call on that sink. **Compact is the default everywhere**; full is an opt-in archival flag. Dictionary completeness: seekable sinks back-patch at close, streams append dictionary frames on registry change | FR-W2, FR-W7, §6.5 |
| 3 | v1 **forbids `dlclose`** of any image that registered entries (other than the main executable); fail-fast with diagnostic, `ES_SERREG_ALLOW_DLCLOSE=1` opts into unlink-on-destructor. **Superseded by decision A (2.1.13, §0.00)** | FR-R10..R12, §6.3b |
| 4 | **64-bit IDs only** in v1. SBE's header is four 16-bit fields (blockLength, templateId, schemaId, version), not a wider ID; it is safe only because IDs are human-assigned. Uncoordinated hashes need width for collision safety | FR-H8, §6.10 |
| 5 | **No external dependencies** in v1; in-house minimal TOML/JSON readers. External libraries may be adopted in v2/v3 | FR-D9, §6.9 |
| 6 | `variant`: dictionary carries the variant's own hash **and** each alternative's hash + canonical name, plus index | FR-W8, §6.1, §6.7 |
| 7 | Reserve and populate `since` / `deprecated` per field and per type in v1 (data only; evolution semantics in v2) | FR-S11, §6.1, §6.7 |
| 8 | `SIGUSR2`-style dump only when `ES_SERREG_DUMP_SIGNAL=<name>` is set; handler sets an atomic flag, dump runs off-signal | FR-R9, §6.11 |

### 0.2 Round 1 (v1 → v2) — retained for traceability

| # | Review point | Where |
|---|---|---|
| 1 | String literals in the registry | §4.4, §6.6 |
| 2 | Vectors/strings/variants designed now, implemented v2 | §4.7, §6.5 |
| 3 | Raw registry representation for file/network | §6.7 RRI |
| 4 | Services in library; `srdump` = thin wrapper | §6.9, FR-D0 |
| 4b | Extract registry from executable; TOML/JSON output | §6.8, FR-D6/D7 |
| 5 | Compare with 3 libraries | §7 |
| 6 | Runtime overhead | §8 |
| 7 | Deserialization styles, visitor, buffer inspection | §6.4 |
| 8 | `dlopen` in v1 | FR-R2, §6.3 |
| 9 | Recursive registration of components | FR-R1 |
| 10 | Minimum type ID on the wire | FR-W2 compact |
| 11 | Out-of-scope list | §1.3 |
| 12 | All members serialized; `as_flat<>` to override | FR-S9 |
| 13 | Env hooks incl. FD variant | FR-R9 |
| 14 | `const` parameters by default | FR-S10, §9 |
| D1–D9 | namespace `es::serreg`; all members; padding scrub on; Catch2 v3 (§10.1); env hook in v1; Option A + ELF section copy; base classes option (d); canonical-name walk with `{anon}` allowed (superseded by §0.000 B in 2.1.14); x86-64 only | throughout |

---

## 1. Vision and scope

### 1.1 Vision

A small, fast C++26 library that serializes ordinary C++ structs/classes into a self-describing binary form for **file storage** and **inter-process communication**, using **standard reflection (P2996)** to derive the complete type dictionary at compile time. Every type — and every string literal — ever passed to `serialize()` is **registered before `main()`** as a side effect of template instantiation, and the registry **grows when shared libraries are `dlopen`ed**. Types, `serialize()` signatures and the program get **stable 64-bit hashes**. The registry can be **exported as a raw image** (file, socket, FD) and a **copy is embedded in every executable/DSO** in a dedicated ELF section, so `srdump` can decode any capture using only the dictionary — from the file, from a running process, or from the binary itself.

### 1.2 In scope (v1)

- `serialize(sink, Ts&&...)` / `deserialize(bytes, Ts&...)` for **flat** types; **literal strings** as first-class arguments.
- Compile-time descriptors via `<meta>`; registry populated at static-init and at `dlopen`; `dlclose` of registering images rejected (fail-fast) unless explicitly allowed.
- Hashes: per type (schema/layout), per call signature, per program; 64-bit everywhere.
- Wire format: file container, IPC frame stream; **compact** frames by default, **full** frames as archival option; dictionary completeness guaranteed for late-loaded types.
- Raw Registry Image (RRI): binary + JSON + TOML views; embedded ELF section copy.
- `since` / `deprecated` metadata carried per field and per type (populated from annotations; no evolution semantics yet).
- Deserialization: typed, inspect/view, compile-time `dispatch`, dynamic visitor, zero-copy view.
- Library services (`es::serreg::tools`) + thin `srdump` CLI; env-variable and signal-triggered registry dumps.
- Headers + compiled library (static and shared); tests, sanitizers, fuzzing, benchmarks; **zero external dependencies** (Catch2 is test-only, fetched by CMake).

### 1.3 Out of scope (all versions unless listed in §4.7)

Pointers, references, unions, virtual bases/functions, bit-fields, and any non-trivially-copyable type **other than** the supported dynamic types (`std::string`, `std::vector<T>`, `std::variant<Ts...>`, `std::optional<T>`, `std::array<T,N>`). All are rejected at compile time with a diagnostic naming type and member (FR-S7). Cross-endian runtime conversion, text payload formats, non-x86-64 targets, schema evolution semantics — v2+.

---

## 2. Glossary

| Term | Meaning |
|---|---|
| **Flat type** | Trivially copyable class whose members are (recursively) arithmetic, `bool`, char family, `std::byte`, enums, C arrays, `std::array`, or flat types. |
| **Dynamic type** | A class with at least one dynamic member (`std::string`, `std::vector`, `std::variant`, `std::optional`) anywhere in its tree. Encoded as a **canonical image** (§6.5), not `memcpy`. |
| **Descriptor** | `constexpr` per-type description (name, size, align, fields with offset/size/kind/nested hash/since/deprecated). |
| **Blob** | Compile-time serialized byte image of one descriptor, literal or call signature — the unit stored in the ELF section and concatenated into the RRI. |
| **RRI** | Raw Registry Image — exportable byte image of the whole registry (§6.7). |
| **Literal** | Compile-time string passed to `serialize` via `"…"_lit`; registered; transmitted as an 8-byte ID. |
| **Schema / layout / call / program hash** | identity with names / compatibility without names / ordered signature / order-independent set. All 64-bit. |
| **Frame / record** | One `serialize()` call / one argument within it. |
| **Image** | One loaded ELF object: the main executable or a DSO. |

---

## 3. Toolchain and constraints

- **Primary:** GCC 16.x, `-std=c++26 -freflection` (hard error below C++26 — CMake gates it). Refs: [GCC C++ status](https://gcc.gnu.org/projects/cxx-status.html), [GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html), [Qt blog on GCC reflection](https://www.qt.io/blog/c26-reflection-qrangemodel).
- **Cross-check:** [Bloomberg clang-p2996](https://github.com/bloomberg/clang-p2996) (`-std=c++26 -freflection-latest -stdlib=libc++`) on [Compiler Explorer](https://godbolt.org/). Upstream Clang 22/23 lack P2996.
- **Reflection used:** `^^T`, `[: r :]`, `nonstatic_data_members_of`, `bases_of`, `parent_of`, `identifier_of`, `has_identifier`, `type_of`, `offset_of(m).bytes`, `size_of`, `alignment_of`, `is_bit_field`, `enumerators_of`, `annotations_of` ([P3394](https://wg21.link/p3394)), `define_static_string/array/object` ([P3491](https://wg21.link/p3491)), `template for` ([P1306](https://wg21.link/p1306)). Docs: [cppreference `<meta>`](https://en.cppreference.com/w/cpp/meta).
- **Platform v1:** Linux x86-64 (ELF64, little-endian). Others v2.
- **Build:** CMake ≥ 4, presets; ai-box Docker images (Fedora/Ubuntu).
- **Discipline:** no RTTI dependence, no exceptions on hot paths (`std::expected`), `constinit` globals, `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.
- **Dependencies:** none for the library and tools. Catch2 v3 for tests only (CMake `FetchContent`, can be pointed at a vendored copy for offline builds).

---

## 4. Requirements

### 4.1 Serialization API

- **FR-S1** `template <sink S, class... Ts> auto serialize(S& out, const Ts&... vs) -> std::expected<std::size_t, error>;` — one frame per call, returns bytes written. `Ts` may include `lit<"…">` objects.
- **FR-S2** `sink` concept (`write(const std::byte*, std::size_t) -> bool`) and `claiming_sink` (`claim(n) -> std::byte*`, `commit(n)`) for exact-size slots in rings/disruptors. Adaptors: `span_sink`, `vector_sink`, `fd_sink`, `file_sink` (seekable; back-patches the dictionary, FR-W7).
- **FR-S3** `template <class... Ts> constexpr std::size_t frame_size();` — exact size when all `Ts` are flat/literal; `frame_size(const Ts&...)` runtime variant for dynamic members.
- **FR-S4** `template <class... Ts> auto deserialize(std::span<const std::byte>, Ts&...) -> std::expected<std::size_t, decode_error>;` — validates header, call hash and lengths **before** touching outputs, then decodes in the same pass. On failure the `decode_error` holds the `error` code and three counters (2.1.15, §0.0000 C): `expected` (1 for the frame + the number of arguments, literals and constants included), `actual` (0 if the frame was refused, else 1 + the arguments decoded into their outputs) and `failed` (the first item that failed; 0 = the frame, *i* = argument *i*). A frame or record that does not fit is reported before any output is written (`actual` ≤ 1). A record that fails while it is decoded (a dynamic image) leaves earlier arguments complete, that one valid but unspecified, later ones untouched. `invalid_value` does not stop decoding: that output keeps its value and the rest are read. `decode_error` converts to and compares with `error`.
- **FR-S5** `serialize_raw<T>` / `deserialize_raw<T>` — payload only, no header, no ID.
- **FR-S6** Padding bytes zeroed via a compile-time gap table; **on by default**; `serialize_options{.scrub_padding=false}` opt-out. No cost when `has_unique_object_representations_v<T>`.
- **FR-S7** Unsupported members → `static_assert` naming type and member and mentioning `as_flat<T>`.
- **FR-S8** Customization: `template <class T> struct as_flat;` (`using type = …; static type to(const T&); static T from(const type&);`) to map opaque types, skip private/unsupported members, or reorder. `[[=es::serreg::skip]]` on a member skips just that member.
- **FR-S9** All non-static data members are serialized, public or private (`access_context::unchecked()`). If that does not compile for a type, the user provides `as_flat<T>`. The dictionary documents exactly what was serialized.
- **FR-S10** All function parameters are `const` unless the function modifies them. Applies to API, internals and tests.
- **FR-S11 (Q7)** Annotations `[[=es::serreg::since(N)]]` and `[[=es::serreg::deprecated(N)]]` (N: `uint16_t`, 0 = unset) are read from members and from the type itself, stored in descriptors and blobs, shown by `srdump`, and **excluded from the layout hash but included in the schema hash**. v1 attaches no behaviour to them; v2 evolution rules build on them without a format bump.

### 4.2 Registry

- **FR-R1** Instantiating `serialize`/`deserialize`/`serialize_raw`/`dispatch` for `T` registers `T` and, recursively, every component type: nested structs, bases, enums, array element types, `vector<T>` element, `optional<T>` payload, every `variant` alternative.
- **FR-R2** Registration happens during static initialization of the image containing the instantiation: before `main()` for the executable and `DT_NEEDED` DSOs; at `dlopen` time for later-loaded DSOs.
- **FR-R3** One registry per process (`registry::instance()` exported from `libserreg`).
- **FR-R4** Duplicates collapse: same schema hash → one logical entry (per-image reference count). Same schema hash with a different layout hash cannot arise from two honest definitions (the schema string contains every offset, size and kind, so it determines the layout string): it is a hash collision or a corrupt image, and the registry treats it as such — `registry::conflicts()` lists both, the later definition is kept unlinked, and `ES_SERREG_CONFLICT` says whether to warn, abort or stay silent (2.1.13, decision A; wording 2.1.16, review R7).
- **FR-R5** API: `size()`, `for_each`, `find(schema_hash)`, `find_by_name`, `find_literal(id)`, `calls()`, `literals()`, `conflicts()`, `images()` (which loaded images contributed), `program_hash()`, `version()` (monotonic change counter), `snapshot()`, `export_image(sink)` (RRI), `write_json`, `write_toml`, `on_change(callback)`.
- **FR-R6** Concurrency: reads are wait-free on a snapshot; writes take an internal mutex; `version()` is atomic. Registry operations are never on the serialization hot path.
- **FR-R7** Survives `-Wl,--gc-sections`, `-flto`, static archives without `--whole-archive`.
- **FR-R8** No SIOF: registry head is `constinit`; nodes are self-linking.
- **FR-R9 (Q8)** Environment hooks in `libserreg`, evaluated at library init and after every registry change:
  - `ES_SERREG_DUMP_REGISTRY=<path|->` — write the registry to a file or stdout.
  - `ES_SERREG_DUMP_REGISTRY_FD=<n>` — write to an already-open descriptor (pipe, socket, inherited file).
  - `ES_SERREG_DUMP_FORMAT=rri|json|toml` (default `rri` for FD, `json` for path).
  - `ES_SERREG_DUMP_ON=init|change|both` (default `both`): dump at first init, on every `version()` change, or both.
  - `ES_SERREG_DUMP_EXIT=1` — exit after the first dump.
  - `ES_SERREG_DUMP_SIGNAL=<USR1|USR2|RTMIN+n>` — **only when set**, install a handler that requests a dump; nothing is installed otherwise (§6.11).
- **FR-R10 (decision A, 2.1.13; was Q3)** **`dlclose` never aborts.** `push()` copies each registration into a heap entry owned by the registry: the ENTRY blob and the name for every image, and a deep copy of the descriptor for every image other than the main program. Nothing the registry hands out points into an image that can be unloaded. A guard destructor — run by `dlclose` or at process exit, which the registry no longer needs to tell apart — drops that guard's reference and nothing else (no allocation, no free, no abort). libserreg does not interpose `dlopen`/`dlclose`. `ES_SERREG_ALLOW_DLCLOSE` is ignored (one stderr line at startup if set).
- **FR-R11 (decision A)** An entry whose references are all gone is **retained** (`refs == 0`): it stays in every later index, `find_*` still return it, its descriptor stays readable, and frames written while its image was loaded still decode. `registry::retained()` counts them; `images()` and `program_hash()` count loaded entries only; `find_type_by_name` prefers a loaded entry over a retained one. **Reload:** a registration with the key and layout of a retained entry revives it (no new memory); a changed type has a new schema hash and becomes a new entry beside the retained one; a definition with the same schema hash and another layout replaces a retained entry, and conflicts (FR-R4) only with a loaded one — when the kept definition unloads, a still-loaded conflicting definition takes its place. Every change bumps `version()`; the next published index notifies `on_change` observers once, on the publishing thread (`registry::refresh()` publishes at once; `on_change` returns an id for `remove_on_change`). Pointers and snapshots stay valid for the life of the process.
- **FR-R12** The registry records, per guard, the image it belongs to (`dladdr` at push time) so `images()`, conflict reports and the per-entry references are exact. The main program's descriptors are used in place (no copy).

### 4.3 Hashing

- **FR-H1** `schema_hash_v<T>`, `layout_hash_v<T>` constexpr 64-bit. The schema hash is the type's identity; the layout hash is a derived fingerprint of the same description with the names removed, kept because it gives the O(1) "same bytes" test that `--diff` (`names-only`) and schema evolution use (wording 2.1.16, review R7). **FR-H2** `call_hash_v<Ts...>` ordered. **FR-H3** `program_hash()` order-independent (sorted). **FR-H4** Cross-compiler stable via canonical description strings; golden values locked. **FR-H5** Algorithm ID in every header; 2 = [XXH3-64](https://github.com/Cyan4973/xxHash) since format 1.3 / library 2.1.0 (§0.0 D1; 1 = FNV-1a-64 until 2.0.8, no longer read). **FR-H6** Precedent: [Avro schema fingerprints](https://avro.apache.org/docs/1.12.0/specification/#schema-fingerprints).
- **FR-H7** Literal ID = XXH3-64 of the literal bytes (FNV-1a-64 until 2.0.8).
- **FR-H8 (Q4)** All wire IDs are 64-bit in v1. No truncated/compact IDs; rationale in §6.10. The frame header layout leaves no reserved field for a narrower ID, so adding one later is a minor format version with a new frame flag, not a redesign.

### 4.4 String literals (Q1 confirmed)

Binary-logging pattern (cf. [Binlog](https://github.com/morganstanley/binlog), [NanoLog](https://github.com/PlatformLab/NanoLog), [Quill](https://github.com/odygrd/quill)): the text lives in the dictionary, the wire carries the ID.

- **FR-L1** `"order rejected"_lit` (UDL in `es::serreg::literals`) yields `lit<fixed_string{"order rejected"}>`, a structural NTTP wrapper; `ES_SERREG_LIT("…")` macro equivalent.
- **FR-L2** Passing a `lit<S>` to `serialize` registers it (blob in the literal table + ELF section) and writes only its 8-byte ID.
- **FR-L3** `srdump`/`deserialize` resolve IDs through the registry or dictionary; unknown IDs print as `lit#<hex>`.
- **FR-L4** Literal text stored once per process (COMDAT `define_static_string`).
- **FR-L5** Runtime strings are data, never interned (dynamic-member encoding, v2).
- **FR-A5 (2.1.15, format 1.5)** Compile-time constants. `42_`, `-7_`, `0x10_`, `1.5_` (numeric literal operator `_` in `es::serreg::literals`) and `k<V>` yield `constant<V>`, where V is a scalar or an enum. An integer literal is `int`, else `long long`, else `unsigned long long`; a floating literal is `double`. A `constant<V>` argument (a) contributes `const:<scalar name>=<value>` or `const:<enum schema hash>=<underlying value>` to the call string (floating values by bit pattern), so type and value are in the call hash; (b) is recorded in the call ENTRY as an `arg_const` ARG followed by the `u64` value; (c) occupies no bytes in a compact frame; (d) in a full-records frame is the scalar or enum record it would be at run time. Readers present it as that record with the value from the dictionary; a typed reader names the same constant, and another value is `hash_mismatch`.
- **FR-A6 (2.2.0, format 1.6)** Containers as arguments. Every dynamic member type — `std::vector<E>`, `std::optional<E>`, `std::variant<Es...>`, the map and set containers of FR-W9 — may be passed to `serialize`, `deserialize`, `dispatch`, `frame_router` and the typed walkers directly, not only as a member. Its type entry is a synthetic struct (canonical name = the container's vocabulary name, one unnamed field of the member kind at offset 0, size 8, align 4, flag `transparent`); the record is the ordinary dynamic-type record (`u32 len | u32 0 | image`), whose image is the slot plus the tail — the same bytes the container produces inside a one-member struct. The call string carries the synthetic type's schema hash; no new argument category. Dictionary-driven readers render a transparent type as its value (`ids: [3, 1, 2]`, `{a: 1, b: 2}`), the Lua dissector and `--cxx` likewise. `std::string` at top level stays the `string` record; `std::array<E,N>` of a flat `E` is a flat class as before. Pointers, references, unions, bit-fields and virtual classes stay refused with a named diagnostic.

### 4.5 Wire format and dictionary

- **FR-W1** Containers: **file** (header + dictionary + frames + optional index) and **stream** (frames; dictionary out-of-band or as dictionary frames).
- **FR-W2 (Q2)** Frame modes: **compact** (one ID per frame — the call hash; record boundaries from the dictionary) is the **default for files and streams**. **Full** (per-record schema hash + length) is opt-in via `serialize_options{.full_records=true}` or `file_sink{.archival=true}` for captures that must be skippable/identifiable without their calls table.
- **FR-W3** Dictionary sufficient to decode every field: name, offset, size, align, kind, nested hash, extent, enumerators, `since`/`deprecated`, literal table, calls table.
- **FR-W4** v1: little-endian native layout; header records `endianness`, `pointer_size`, ABI tag; mismatch → error. Implemented in format 1.3 (§0.0 D2): an 8-byte ABI tag (version, endian, pointer, `long`, `wchar_t`, `char` signedness, `max_align_t`, `long double`) in the FILE and RRI headers; typed reads across ABIs already fail by hash, dictionary-driven reads use the tag for `wchar` widths, and two tags in one capture are `abi_mismatch`.
- **FR-W5** Dictionary JSON/TOML canonical (sorted) for clean diffs.
- **FR-W6** `format_version` major/minor; reader accepts equal major.
- **FR-W7 (Q2)** **Dictionary completeness.** A sink opened at time *t* writes the dictionary from a snapshot at *t*. If `registry::version()` changes afterwards (a `dlopen` added types): a seekable `file_sink` rewrites the dictionary at close and back-patches `dict_offset`/`dict_len` in the header (the initial dictionary area is left in place and ignored — or reused if the new one fits); a non-seekable sink (`fd_sink` on a pipe/socket, `claiming_sink`) emits a **dictionary frame** carrying the *delta* (new entries only) before the next data frame. Readers merge dictionary frames in order. `srdump --verify` confirms every call hash used in a capture is defined by a dictionary that precedes its first use.
- **FR-W8 (Q6)** `variant<Ts...>` in the dictionary: the variant's own schema/layout hash, `extent = sizeof...(Ts)`, and for each alternative its index, canonical name and schema hash (`elem[i]`). `srdump` prints `variant<Order,Cancel>[1]=Cancel{…}`.
- **FR-W9 (2.2.0, format 1.6)** Maps and sets. Two wire shapes, independent of the C++ container: **`map`** (`kind::map_`): the member's 8-byte slot `{u32 off, u32 n}` points at `n` *entries*, each the key's image at offset 0 and the value's image at the pair's canonical offset (`align_up(sizeof key image, align value)`), the entry's stride rounded up to the pair's alignment, tails of nested dynamic members as for any vector element; entries are **sorted by key** (the key's `operator<` where it exists, else the byte order of its flat image), so that equal values encode to equal bytes for every container, `std::unordered_map` included; **`set`** (`kind::set_`): `n` key images, likewise sorted. The FIELD carries `extent` = the entry stride and `n_elem` = 2 (key, value) or 1 (key) element descriptors with the canonical offsets. `std::map`, `std::unordered_map` and `std::flat_map` share the `map` shape and the member kindspec `map<K,V>` in both canonical strings (a member's hashes do not depend on the container, as `string` does not depend on the allocator); as top-level arguments (FR-A6) they are three transparent types with different canonical names, hence different schema hashes and one layout hash; a comparator, hasher or equality other than the defaults is part of that name; the allocator is not. Keys may be anything a member may be. Decoding: `map`/`set` insert with `emplace_hint(end(), …)`, unordered containers `reserve(n)` then `emplace`; a duplicate key is `invalid_value`; the slot rules and image budget of vectors apply. `std::multimap`/`std::multiset` are not supported (a named diagnostic). The enum ENTRY's reserved byte carries `enum_flag_fixed` (bit 0, the enum has a fixed underlying type), the struct ENTRY's flags carry `struct_flag_transparent` (bit 1, FR-A6). Readers accept exactly minor 6.

### 4.6 Services and `srdump`

- **FR-D0** All functionality lives in `es::serreg::tools`; `srdump` is `int main(int argc, char** argv) { return es::serreg::cli::run(argc, argv); }`. Flag parsing and `--config <file.toml>` loading are in `cli::run`.
- **FR-D1** `srdump <capture.ser>` — dictionary, then decoded frames. **FR-D2** `--dict-only`, `--json`, `--toml`, `--text` (default), `--verify`, `--dict <rri|json>`, `--frames A..B`, `--type <name|hash>`, `--hex`, `--stdin`, `--full`, `--flatten`; 2.1.16: `--window <size>`, `--frame-limit <size>`, `--direction`, `--cxx`, `--cxx-router`, `--namespace`.
- **FR-D3** `--diff <a> <b>` — added/removed/changed types; `identical | names-only | metadata-only (since/deprecated) | breaking | added | removed | duplicate-name`, calls and literals by id. The verdicts describe identity (did the bytes or the names move?), not decodability. 2.1.16 (decision E): `--direction old-data | new-data | both | none`, default `old-data`, decides whether removals or additions are breaking; layout changes, changed calls and changed literals break in every direction; the exit status follows.
- **FR-D4** Zero knowledge of user types at build time. **FR-D5 (the never-crash contract; §0.00000, 2.1.16)** For any sequence of input bytes — a capture, a stream, a frame, an RRI image, an ELF file, a config file, a ring header in shared memory — every public reading API returns a value or an error, and never crashes, aborts or asserts, executes undefined behaviour, or allocates or loops beyond the configured limits; errors are reported and decoding moves on to the next item where the format allows it. The limits are `decode_limits` (`limits.hpp`): frame bytes (64 MiB), dictionary bytes (256 MiB) and entries (1 000 000), image depth (64), whole-input read (1 GiB), reader window (10 MiB), problem lines kept (1000); process-wide (`set_decode_limits`) and per call where a reader takes them. Outside the contract: system out-of-memory within the limits, and policies the user asked for (`ES_SERREG_CONFLICT=abort`). Checked by the fuzz targets, which are gated on time and memory, not only on crashes (`SECURITY.md`). Since 2.1.3 the typed decoder and the dictionary-driven walker apply one set of slot rules to a dynamic image (`fmt::image_budget`: `n == 0 ⇔ off == 0`, blocks after the fixed part and inside the image, and a byte budget of the image size so shared tails cannot amplify), so `verify` and `deserialize` accept the same images.
- **FR-D6** `srdump --elf <binary-or-so>` extracts the embedded registry copy; multiple `--elf` merge. **FR-D7** `srdump --rri <image>` reads an exported RRI. **FR-D8** Formatters pluggable: text, JSON, TOML in v1.
- **FR-D10 (2.1.16)** `srdump --cxx <types.hpp|->` writes C++ declarations for every type of the dictionary sources (fixed-width scalars from kinds, `enum class E : <kind>`, `std::array`, the dynamic member types, bases, `since`/`deprecated`, padding where the dictionary has gaps, `[[=es::serreg::name("…")]]` where a plain declaration cannot spell the canonical name) followed by `static_assert`s on size, alignment, member offsets and the schema hash of every type, so compiling the header against the library proves the regeneration; `--cxx-router` adds a `frame_router` skeleton with one handler per call; `--namespace old=new` remaps.
- **FR-D11 (2.1.16)** Every reader over a capture on disk or a pipe works on a sliding window of `decode_limits::window_bytes` (`--window <size>`): a frame that straddles a window is completed from the next, a frame larger than the window grows it to that frame within `frame_bytes`; `verify` and `dump` keep state O(types). A capture's size is bounded by the file system, not by the tools.
- **FR-D9 (Q5)** No external dependencies. In-house: JSON writer/reader (full JSON grammar, small recursive-descent, no allocation-free claim — tools only), TOML writer, and a **TOML subset reader** for `--config` (tables, `key = value`, strings, integers, booleans, arrays of strings; no dates, no inline tables, no dotted keys in v1 — rejected with a line number). Full TOML via an external library is a v2/v3 option.

### 4.7 Type support matrix

| Category | v1 | v2 | Encoding |
|---|---|---|---|
| Integral, floating, `bool`, char family, `std::byte` | ✔ | | native |
| Enums | ✔ | | native; names in dictionary |
| Nested flat struct/class incl. private members and bases | ✔ | | native; bases as pseudo-fields (§6.1b) |
| C arrays, `std::array<T,N>`, multi-dim | ✔ | | native |
| `[[no_unique_address]]`, empty types | ✔ | | native |
| Literals `"…"_lit` | ✔ | | 8-byte ID |
| Opaque types via `as_flat<T>` | ✔ | | as mapped type |
| `std::string` / `std::string_view` (write-only) | design | ✔ | canonical image `{u32 off,u32 len}` + tail; since 2.1.5 any allocator (`std::pmr::string`), same wire form, allocator not hashed |
| `std::vector<T>` | design | ✔ | `{u32 off,u32 count}` + tail elements |
| `std::optional<T>` | design | ✔ | `{u8 has,pad,T-image}` inline |
| `std::variant<Ts...>` | design | ✔ | `{u16 index,pad,max-alt image}`; alternatives registered and named (FR-W8) |
| `std::map<K,V>`, `std::unordered_map<K,V>`, `std::flat_map<K,V>` | — | ✔ 2.2.0 | shape `map`: `{u32 off,u32 n}` + `n` sorted entries (key at 0, value at its canonical offset) (FR-W9) |
| `std::set<K>`, `std::unordered_set<K>`, `std::flat_set<K>` | — | ✔ 2.2.0 | shape `set`: `{u32 off,u32 n}` + `n` sorted keys (FR-W9) |
| Any dynamic member type as a top-level argument | — | ✔ 2.2.0 | transparent type: a synthetic struct around the 8-byte slot (FR-A6) |
| `std::multimap`, `std::multiset`, `std::list`, `std::deque` | ✖ | ✖ | `static_assert`; `list`/`deque` are candidates for the `vector` shape (`docs/plans-for-containers.md`) |
| Bit-fields, pointers, references, unions, virtual | ✖ | ✖ | `static_assert` |

---

## 5. Architecture overview

```
 user code                        headers <es/serreg/*.hpp>                        libserreg (.a/.so)
 ─────────                        ─────────────────────────                        ──────────────────
 serialize(sink, "msg"_lit, a) ─► reflect<T> → descriptor_v<T> (constexpr, .rodata; since/deprecated)
                              │       ├── schema/layout/call hashes (consteval, 64-bit)
                              │       └── blob_v<T> (byte image) ──────────────► [ELF section es_serreg_dict] ◄─ srdump --elf
                              ├─► registrar<T>::guard ──(odr-use ⇒ dyn-init)────► registry (constinit head, mutex writes,
                              ├─► registrar_lit<S>::guard ───────────────────────►   per-image refs, snapshots, version(),
                              ├─► registrar_call<Ts...>::guard ──────────────────►   on_change; heap copies, retained entries)
                              └─► frame writer (compact header + memcpy + gap scrub)   │
                                                                                       ├─► RRI export (file/fd/socket) ◄─ ES_SERREG_DUMP_* / signal
                                                                                       ├─► dictionary bin/json/toml; delta dictionary frames
 deserialize / inspect / dispatch / visit / view  ◄────────── dynamic decoder ◄────────┤
 srdump (1-line main) ─────────────────────────────────────► tools:: + cli::run ◄──────┘
```

---

## 6. Detailed design

### 6.1 Compile-time descriptors

```cpp
namespace es::serreg {

enum class kind : std::uint8_t {
  i8,i16,i32,i64,u8,u16,u32,u64,f32,f64,boolean,char_,char8,char16,char32,wchar,byte_,
  enum_, array_, struct_,                          // v1
  string_, vector_, optional_, variant_, literal_  // v2 encodings; kinds exist in the v1 dictionary schema
};

struct field_desc {
  std::string_view  name;        // define_static_string(identifier_of(m)) or "<base:Name>"
  std::uint32_t     offset;      // native offset (flat) or canonical-image offset (dynamic)
  std::uint32_t     size;
  std::uint16_t     align;
  kind              k;
  std::uint16_t     since;       // FR-S11: [[=since(N)]], 0 = unset
  std::uint16_t     deprecated;  // FR-S11: [[=deprecated(N)]], 0 = unset
  std::uint64_t     type_hash;   // struct_/enum_/vector_/optional_/variant_: nested schema hash
  std::uint32_t     extent;      // array_: element count; variant_: alternative count
  const field_desc* elem;        // array_/vector_/optional_: element; variant_: alternatives[extent] (name = canonical type name)
};

struct enumerator_desc { std::string_view name; std::int64_t value; };
struct gap { std::uint32_t offset, len; };

struct type_desc {
  std::string_view                 name;          // canonical (§6.2b)
  std::uint64_t                    schema_hash, layout_hash;
  std::uint32_t                    size, align;
  kind                             k;             // struct_ | enum_
  kind                             underlying;    // enum_
  bool                             is_flat;
  std::uint16_t                    since, deprecated;   // FR-S11, type level
  std::span<const field_desc>      fields;
  std::span<const enumerator_desc> enumerators;
  std::span<const gap>             padding_gaps;  // FR-S6 scrub table
  std::span<const std::byte>       blob;          // this descriptor serialized (§6.7), lives in the ELF section
};

template <class T> inline constexpr type_desc descriptor_v = make_type_desc<T>();
}
```

Generation sketch (GCC 16 spellings):

```cpp
template <class T>
consteval auto make_fields() {
  constexpr auto ctx = std::meta::access_context::unchecked();          // FR-S9
  std::vector<field_desc> out;
  template for (constexpr auto b : std::define_static_array(std::meta::bases_of(^^T, ctx)))
    out.push_back(base_field<[:std::meta::type_of(b):]>(b));            // §6.1b (c)
  template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx))) {
    static_assert(!std::meta::is_bit_field(m), "es::serreg: bit-fields unsupported (use as_flat<T>)");
    using M = typename [:std::meta::type_of(m):];
    check_supported<M>();                                                // FR-S7
    if constexpr (!has_annotation<m, skip>())
      out.push_back(field_for<M>(m, annotation_or<m, since>(0), annotation_or<m, deprecated>(0)));
  }
  return std::define_static_array(out);
}
```

Fallback if `template for` misbehaves: index-sequence expansion over `define_static_array(...)[I]`.

#### 6.1b Base classes (decided: option d)

Descriptors store one pseudo-field `"<base:Name>"` of kind `struct_` per base at `offset_of(base)`, referencing the base's own descriptor (so hashes compose and a shared header type is visibly reused). `tools::flatten_view` and `srdump --flatten` present base members inline. Virtual bases → `static_assert`.

### 6.2 Canonical description and hashes

```
schema: struct<ns::Order>[48,8;s1;d0]{<base:ns::Hdr>:struct<0x9f..>@0[16,8];id:u64@16[8,8;s0;d0];side:enum<ns::Side,u8>@24[1,1];sym:array<char,8>@25[8,1]}
layout: struct[48,8]{struct<L:0x3a..>@0[16,8];u64@16[8,8];enum<u8>@24[1,1];array<char,8>@25[8,1]}
call:   call(3){0x…,lit,0x…}          literal: lit{order rejected}
variant: variant<ns::Cmd>[24,8]{0:struct<0x..>(ns::Order);1:struct<0x..>(ns::Cancel)}
```

- `since`/`deprecated` appear only in the **schema** form (`;sN;dN`, omitted when both 0), never in **layout** (FR-S11).
- Nested types contribute their hash, not their body; `layout` uses nested layout hashes (`L:`).
- XXH3-64 (seed 0, default secret), `constexpr` for constant evaluation and run time; algorithm ID 2 (FNV-1a-64, ID 1, until 2.0.8).

#### 6.2b Canonical type names

Rules, in order (2.1.14, §0.000 B; until 2.1.13 an anonymous namespace was rendered `{anon}` and a local class `{fn:<identifier>}::`):
1. **Explicit name.** `[[=es::serreg::name("…")]]` on the type, or `es::serreg::type_properties<T>::name()`. Not both (`static_assert`). Allowed characters: letters, digits, `_ : < > , -`.
2. **Scalars:** the kind name (`i32`, `f64`, `char`…).
3. **std vocabulary**, shipped as `type_properties` specialisations: `std::string` (any allocator), `std::basic_string<C>`, `std::vector<E>`, `std::array<E,N>`, `std::optional<E>`, `std::variant<A,B,…>`, `std::pair<A,B>`, `std::tuple<…>`, `std::monostate`.
4. **No linkage-unique name → `static_assert`:** a type declared in an unnamed namespace, or local to a function or lambda, must use rule 1. Two `.cpp` files can each define their own `X` there; a header-defined one carries the same explicit name in every TU, so it stays one registry entry (FR-R4).
5. **Otherwise:** walk `parent_of` to the global namespace and join with `::`.
   - Namespaces appear by identifier, user inline namespaces included. Std's implementation namespaces (reserved identifiers inside `std`) are dropped.
   - An enclosing class, including a class-template specialisation, appears by its own canonical name (`ns::Outer<i32>::Inner`).
   - Template specialisations render their arguments canonically: types by these rules, values by kind (`Vec<i32,3>`, `W<i32,enum(7)>`).
   - Unnamed types → `static_assert`.
6. **Recursive types** (containing themselves through a member, base, array, `std::vector`, `std::optional` or `std::variant`) → `static_assert`.

Two types with the same canonical name and different layouts are a conflict (FR-R4).

### 6.3 Registry (Option A) — nodes, guards, images

```cpp
namespace es::serreg::detail {
struct node {
  const void*    payload;      // type_desc* | literal_desc* | call_desc*
  node_kind      nk;
  node*          next;
  const void*    image_base;   // FR-R12: dladdr(payload).dli_fbase, filled by push()
  std::uint32_t  refs;         // images referencing this logical entry (aliases collapse here)
};
struct guard {
  explicit guard(node* const n) noexcept { registry::instance().push(n); }
  ~guard() { registry::instance().on_guard_destroyed(n_); }     // §6.3b
  node* n_;
};
template <class T> struct registrar {
  static node        n;   // {&descriptor_v<T>, node_kind::type, nullptr, nullptr, 0}
  static const guard g;   // odr-used from serialize() ⇒ instantiated ⇒ dynamic init at image load
};
template <class T> constexpr void touch() {                       // FR-R1
  static_cast<void>(&registrar<T>::g);
  template for (constexpr auto m : members_and_bases<T>())
    touch<component_type_t<[:type_of(m):]>>();                   // arrays/vectors/optionals/variants recurse
}
}
template <sink S, class... Ts>
auto serialize(S& out, const Ts&... vs) -> std::expected<std::size_t, error> {
  static_cast<void>((detail::touch<std::remove_cvref_t<Ts>>(), ...));
  static_cast<void>(&detail::registrar_call<Ts...>::g);
  // ... frame writer
}
```

- `push()` takes the mutex, looks up the heap entry of (kind, hash), creates it (copying blob, name and, for an image other than the main program, the descriptor) or adds a reference (alias, or a retained entry revived), detects conflicts, increments `version_`, and marks the registry stale; the next reader publishes one index and runs `on_change` callbacks (2.1.11, 2.1.13).
- Snapshot = sorted views into the heap entries plus a `std::flat_map` per kind for lookup; never pointers into images (FR-R10, FR-R11).
- Standard basis: [temp.inst], [basic.start.dynamic]. Tests assert counts at `main()`, after `dlopen`, after `dlclose` (retained), and across reloads.
- 2.1.13: `node::next` became an opaque pointer to the node's heap entry (`reg_node::entry`); `refs` lives in the entry.

#### 6.3b Unload and reload (decision A, 2.1.13; replaces the v1 `dlclose` guard)

```
push(n):                                   // static initialisation of the image (before main, or inside dlopen)
  image = dladdr(n)                        // outside the mutex (loader lock, G2-5)
  lock
  e = by_key[(kind, hash)]
  none                    → e = new entry(copy of n), attach(e, n)
  same layout             → attach(e, n)            // alias, or a retained entry revived by a reload
  other layout, e retained→ by_key = new entry(copy of n), attach        // replaces it; the old copy stays alive
  other layout, e loaded  → conflict (FR-R4): rejected = new entry, attach(rejected, n, unlinked)
  version++, stale = true                  // the next reader publishes and notifies

on_guard_destroyed(n):                     // dlclose() or process exit — no difference needed
  lock; e = n.entry; e.nodes.erase(n); e.refs--             // in place: no allocation, no free
  if e.refs == 0: drop conflicts where e was rejected; if e was kept and a conflicting definition is loaded,
                  that definition becomes by_key's entry
  version++, stale = true
```

Why the v1 abort was dropped: it existed to protect pointers into an unloaded image. The heap copies protect them without killing the process, and they let frames written by an unloaded plugin stay readable. What the registry cannot protect is what the plugin owns: its code (an `on_change` callback defined in the plugin must be removed before `dlclose`), its objects, its threads; and a static libserreg inside a plugin has a registry of its own. `-fno-gnu-unique` stays: without it glibc marks a DSO `NODELETE` and a reload would not run the new code.

### 6.4 Sinks, serialize, and the five deserialization styles

Hot path (flat args, `claiming_sink`): `claim(frame_size<Ts...>())`, write the 16-byte compact header, `memcpy` each object, zero gaps from `padding_gaps`, write literal IDs, `commit`.

1. **Typed** — `deserialize(bytes, T&...)`: validates `call_hash == call_hash_v<Ts...>` and lengths, then copies.
2. **Inspect** — `inspect(bytes, const dictionary* = nullptr) -> expected<frame_view>`: `frame_view{call_hash, mode, records}`, `record_view{schema_hash, const type_desc*, payload}`. Answers "what is in this buffer" with zero copies, using the registry or a supplied dictionary.
3. **Compile-time dispatch** — `dispatch<Ts...>(bytes, overloads)` per record and `dispatch_frame(bytes, f)` per call signature; zero-copy when aligned.
4. **Dynamic visitor** — `visit(record_view, visitor)`: descriptor-driven, allocation-free; used by all formatters.
5. **Zero-copy view** — `view<T>(record_view) -> expected<const T*>` (`std::start_lifetime_as`), `error::misaligned` otherwise.

### 6.5 Wire format v3 (Q2)

```
FILE
  HEADER      magic "ESSR" | u16 major,u16 minor | u32 flags(LE, dict-present, index-present, back-patched)
              | u32 hash_algo | u64 program_hash | u64 dict_offset | u64 dict_len | u64 reserved
  DICTIONARY  RRI (§6.7) — snapshot at open; back-patched at close if version changed (FR-W7)
  FRAME*      one per serialize() call on this sink, in call order
  [INDEX]     optional footer: (u64 file_offset, u64 call_hash)* for random access

FRAME (compact, default)   u32 frame_len | u16 flags(mode=compact) | u16 n_records | u64 call_hash   (16 bytes)
                           RECORD_C* — flat: payload only (size from dictionary, 8-aligned)
                                       literal: u64 id
                                       dynamic (v2): u32 len | canonical image
FRAME (full, archival)     u32 frame_len | u16 flags(mode=full) | u16 n_records | u64 call_hash
                           RECORD_F* — u64 schema_hash | u32 payload_len | u16 layout | u16 rsv | payload
DICTIONARY FRAME (stream)  u32 frame_len | u16 flags(mode=dict) | u16 0 | u64 ESSR_DICT_SENTINEL | RRI delta
```

- A stream reader keeps a merged dictionary: initial (out-of-band or first dictionary frame) plus deltas. A file reader uses the back-patched dictionary; if the file was not closed cleanly (`back-patched` flag unset and version changed), `srdump --verify` reports which call hashes are undefined and `--dict`/`--elf` can supply them.
- Frames are always skippable by `frame_len`; records in compact frames need the calls table; a reader without it can still count and skip frames, print call hashes, and match them against an `--elf` extraction.
- Dynamic members (v2 implementation, v1 design) use the canonical image described in v2 §6.5 (offsets relative to payload start, tail data after the fixed image).

Format 1.5 (2.1.15) adds to the above: a compact RECORD_C for a **constant** argument is empty (like a literal since 1.1), and the call ENTRY's ARG for it is 24 bytes — `u8 arg_kind = 4 | u8 kind (scalar kind, 0 for an enum) | 6 rsv | u64 hash (scalar name hash or enum schema hash) | u64 value (the object's bytes, zero-extended)`; every other ARG stays 16 bytes. A full RECORD_F for a constant is the scalar or enum record of its value.

Format 1.6 (2.2.0) adds: the FIELD kinds `map` (53) and `set` (54), whose member slot `{u32 off, u32 n}` points at `n` sorted entries (FR-W9; the FIELD's `extent` is the entry stride and its element descriptors give the key's and the value's offsets inside an entry); the struct ENTRY flag `transparent` (bit 1: a container passed as an argument, FR-A6 — a dynamic-type record whose image is one slot plus its tail); the enum ENTRY's reserved byte as `flags` with bit 0 = `fixed` (the enum has a fixed underlying type). Frame flag bit 2 is reserved for a CRC32C trailer (1.7). Nothing else moved: the same program's 1.5 and 1.6 captures differ only in the minor of their headers and the `fixed` bit of their enum entries (so the dictionary's enum blobs change where enums have a fixed type; no type hash and no `program_hash` moves), and readers refuse any minor but 6 (FR-H5).

### 6.6 Literals

`fixed_string<N>` structural NTTP; `lit<S>` empty type; wire = `literal_id_v<S>` (u64). Blob `{u64 id, u32 len, bytes}` in the ELF section and RRI literal table; `registrar_lit<S>` uses the node/guard pattern. `srdump` prints the text; `--json` emits `{"lit":"…","id":"0x…"}`.

### 6.6b Constants (2.1.15)

`constant<V>` is an empty type whose value is its template argument, as `lit<S>` is for text. `operator""_` has two forms that overload on the literal's kind: the string form (`template<fixed_string S>`) and the numeric form (`template<char... Cs>`, which parses the digits at compile time). No registration of its own: the value lives in the call entry (`call_arg_desc::value`), and an enum constant registers its enum. Writers skip it in compact frames; `frame_view` and the dictionary reader hand out a record whose payload is the value stored in the entry, flagged `is_const`, so every renderer treats it as the scalar or enum it is.

### 6.7 Raw Registry Image (RRI)

```
RRI: magic "ESRG" | u16 major,u16 minor | u32 flags(delta?) | u32 hash_algo | u64 program_hash | u32 n_entries | ENTRY*
     (as built, format.hpp is normative: … u32 n_entries | u64 program_hash | u32 rsv | u64 abi_tag — 40 bytes since 1.3)
ENTRY (TLV, 8-aligned): u32 total_len | u8 tag(1 type,2 enum,3 literal,4 call,5 alias,6 image) | u8 rsv[3] | body
  type body:  u64 schema, u64 layout, u32 size, u32 align, u8 flat, u16 since, u16 deprecated, name(u16 len+bytes), u16 n_fields,
              field* { name, u32 off, u32 size, u16 align, u8 kind, u16 since, u16 deprecated, u64 type_hash, u32 extent,
                       u16 n_elem, elem* }             // variant_: n_elem = alternatives, each {name, u64 schema}
  enum body:  u64 schema, u8 underlying, u16 since, u16 deprecated, name, u16 n, {name, i64 value}*
  lit body:   u64 id, u32 len, bytes
  call body:  u64 call_hash, u16 n, {u8 kind(type|lit), u64 hash}*
  image body: u64 image_id, path(u16 len+bytes)         // export only; lists contributing images (FR-R5 images())
```

One blob format, three carriers: ELF section (ENTRY stream, no header), RRI export (header + entries, `program_hash` computed live), dictionary in files/streams (full at open, deltas as dictionary frames).

### 6.8 Embedded registry copy in the executable

```cpp
template <class T>
[[gnu::used, gnu::retain, gnu::section("es_serreg_dict")]]
inline constinit const auto blob_v = make_blob<T>();   // std::array<std::byte,N>; descriptor_v<T>.blob points here
```

`tools::extract_registry_from_elf(path)` is a self-contained ELF64 section reader (no libelf). `[[gnu::retain]]` needs binutils ≥ 2.36; CMake checks. Runtime source of truth remains the Option-A list; the section is a read-only copy for tooling.

### 6.9 Services layer, `srdump`, config (Q5)

```cpp
namespace es::serreg::tools {
  struct dictionary;                                     // from snapshot | RRI | ELF | JSON
  auto load_dictionary(std::span<const std::byte> rri) -> std::expected<dictionary, error>;
  auto load_dictionary_json(std::string_view json)     -> std::expected<dictionary, error>;   // in-house reader
  auto extract_registry_from_elf(std::string_view path)-> std::expected<dictionary, error>;
  auto verify(std::span<const std::byte> capture, const dictionary*) -> std::expected<verify_report, error>;
  auto diff(const dictionary& a, const dictionary& b)  -> diff_report;
  struct formatter { /* text | json | toml */ };
  auto dump(std::span<const std::byte> capture, const dictionary&, formatter&, const dump_options&) -> std::expected<void, error>;
}
namespace es::serreg::cli { int run(int argc, char** argv); }   // flags + --config (TOML subset, in-house)
```

In-house parsers are small, bounded, fuzzed, and used only in tools (never on the serialization path). Their scope is fixed in FR-D9; anything outside it is a clear error with a line number.

### 6.10 Why 64-bit IDs (Q4)

- **SBE's header** is 8 bytes = `blockLength:u16 | templateId:u16 | schemaId:u16 | version:u16` ([FIX SBE spec](https://www.fixtrading.org/standards/sbe/)). It is not a wider identifier; it is three coordinates (message within schema, which schema, schema version) plus the fixed-block length so old decoders can skip unknown trailing fields. 16-bit IDs are safe there only because a human assigns them in the XML — collisions are prevented by coordination.
- **Our IDs are uncoordinated hashes**, so width is what buys safety. Birthday bound (50% chance of any collision) ≈ 1.18·2^(n/2) items: **32-bit ≈ 77 000 registered entities; 64-bit ≈ 5·10⁹.** A large trading system registers thousands of types/literals/calls; 32 bits would need a startup collision check and a fallback path, 64 bits does not.
- What SBE's other fields give us we already have elsewhere: fixed-block length → dictionary sizes; schema version → `since`/`deprecated` (FR-S11) and `format_version`.
- Cost: 8 bytes per frame in compact mode — one ID per `serialize()` call, not per record (Review #10).

### 6.11 Signal-triggered dump (Q8)

Only when `ES_SERREG_DUMP_SIGNAL` is set: `libserreg` installs a `sigaction` handler for the named signal whose entire body is `dump_requested_.store(true, std::memory_order_relaxed)` (async-signal-safe). The dump itself runs from a small helper thread started at the same time (blocked on a 100 ms `sigtimedwait`/poll of the flag), so no allocation or I/O happens in signal context. Output target/format follow the same `ES_SERREG_DUMP_*` variables. Documented interaction: if the application already handles that signal, choose another (`RTMIN+n`).

---

## 7. Comparison with three serialization libraries

| Feature | **ser-reg** | **[zpp_bits](https://github.com/eyalz800/zpp_bits)** | **[SBE](https://github.com/aeron-io/simple-binary-encoding)** | **[Cap'n Proto](https://capnproto.org/)** |
|---|---|---|---|---|
| Schema source | C++ types via P2996, automatic | C++ aggregates via tricks / `members<N>` | XML → generated codecs | `.capnp` → generated code |
| Self-describing data | yes (dictionary + hashes, embedded in binary) | no | schema out-of-band; header IDs | schema out-of-band |
| Flat hot path | `memcpy` + gap scrub | field-wise, constexpr-folded to memcpy | in-place field access | in-place, pointer-based |
| Dynamic data | v2 canonical image | length-prefixed | groups + varData | segments, lists, text |
| Evolution | detect (v1); `since`/`deprecated` carried; rules v2 | none | `sinceVersion`, extension | strong (append-only) |
| Type identity | 64-bit schema/layout hash | none | 16-bit templateId + schemaId + version | 64-bit type IDs |
| Program-wide registry | **yes, pre-main, dlopen-aware** | no | no | no |
| Literal interning | **yes** | no | no | no |
| Tooling | `srdump` (file/stream/ELF/RRI, diff) | none | sbe-tool (codegen) | `capnp` CLI |
| Dependencies | none | none | Java at build time | runtime + codegen |

Adopted from each: zpp_bits — symmetric `in`/`out` sugar, constexpr sizes, `errc`-style errors; SBE — in-place `view<T>`, versioning metadata (`since`/`deprecated`), one small header per message; Cap'n Proto — canonicalization guarantee (identical values ⇒ identical bytes), offset-from-record-start dynamic image, bounds-validated reader. Also consulted: [glaze P2996](https://stephenberry.github.io/glaze/p2996-reflection/), [reflect-cpp C++26](https://rfl.getml.com/cpp26_reflection), [Binlog](https://github.com/morganstanley/binlog), [Avro](https://avro.apache.org/docs/1.12.0/specification/#schema-fingerprints).

---

## 8. Runtime overhead model

| Operation | Cost (x86-64, L1-resident) | Notes |
|---|---|---|
| `serialize` flat 64-byte struct, compact, claiming sink | 16-byte header + `memcpy(64)` + 0–2 gap stores ≈ **5–12 ns** | no registry access, no atomics, no allocation |
| + literal argument | +1 u64 store | compile-time constant |
| `serialize_raw` | `memcpy` + gaps | equals hand-written copy |
| `deserialize` typed | 2–3 compares + `memcpy` | rejects before copying |
| `inspect`/`view<T>` | header validate + pointer arithmetic, 0 copies | alignment check for `view` |
| `dispatch<Ts...>` | constexpr hash compare chain/switch | branch-predictable |
| Dynamic `visit` | ~2–5 ns per field | tools |
| Registry push (load) | mutex + hash-map lookup + `dladdr`, once per registration; a heap copy of blob and name (and, outside the main program, the descriptor) for each new entry (2.1.13) | load time only |
| Registry snapshot/export | O(entries), copies blobs | never on hot path |
| Dictionary back-patch (file close) | one snapshot + one `pwrite` | close time only |
| Binary size | one descriptor + one blob per type (≈100–450 B) | COMDAT dedup |

Zero-overhead statement: for flat types with `claiming_sink`/`serialize_raw`, the generated code equals a manual `memcpy` plus explicit zeroing of pads (verified by `-O2` assembly on Compiler Explorer and the M5 benchmark).

---

## 9. Conventions and layout

- Namespace `es::serreg`; headers `<es/serreg/…>`; Beman-style layout ([Beman](https://github.com/bemanproject/beman), [exemplar](https://github.com/bemanproject/exemplar)).
- All function parameters `const` unless modified; `constexpr`/`consteval` preferred; `constinit` mutable globals; `std::expected` errors; no exceptions; `noexcept` where true.
- Reflection-only code in `reflect.hpp`; everything else compiles without `-freflection`.
- Files: `include/es/serreg/{reflect,descriptor,blob,hash,literal,annotations,registry,serialize,deserialize,dispatch,visit,sink,format,error,platform,serreg}.hpp`; `src/{registry,dlclose_guard,rri,elf_reader,dictionary,decoder,json,toml,tools,cli,env_hook,signal_hook}.cpp`; `tools/srdump/main.cpp`; `tests/{golden,unit,negative,link,dlopen,fuzz,bench}`; `examples/`; `docs/`; `cmake/`.
- Deliverables: versioned names (`ser-reg-vNNN.tar.zst`, top-level `ser-reg-vNNN/`), never reuse; `CHANGELOG.md`.
- Headers (decision I, 2.1.13): package headers (`"..."`, `<es/...>`) come before third-party and system headers; every header includes what it needs to compile on its own and starts with `#pragma once` (`.clang-format`, `tests/headers`, `scripts/check-includes.py`).
- Libraries (decision G, 2.1.13): `es::serreg` (runtime), `es::serreg_tools` (`src/cli.cpp`, `src/wireshark.cpp`, `src/diff.cpp`), `es::ipc` (header-only, C++23, no dependency in either direction) — one CMake package `serreg` with components `serreg`, `tools`, `ipc`. The file list above is the v3 plan; the tree as built is listed in `book/package-contents.md`.

---

## 10. Testing

### 10.1 Framework: Catch2 v3 (decided)

`STATIC_REQUIRE` for golden hashes, `SECTION`s for format corpora, `BENCHMARK` for NFR-1 regression guards; fetched by CMake for tests only. (Rationale table in v2 §10.1.)

### 10.2 Test plan

1. Golden `STATIC_REQUIRE` hashes/sizes for ~20 corpus types incl. annotated (`since`/`deprecated`) and variant-bearing types; GCC 16 + clang-p2996 (CE links in `docs/ce-links.md`).
2. Negative compile tests for every rejected kind, checking diagnostic text.
3. Registry: count at `main()`; multi-TU dedup; static/shared; `dlopen` adds and `version()` increments; **`dlclose` keeps the entries (retained, descriptors and frames still valid), reloads revive them, a changed layout coexists (2.1.13; until 2.1.12: abort death test and `ES_SERREG_ALLOW_DLCLOSE=1` unlink test)**; `RTLD_NODELETE` path; `--gc-sections`; `-flto`; link-order independence of `program_hash`; anonymous-namespace conflict detection; `images()` correctness.
4. Round-trip + determinism; all five deserialization styles; literal resolution; `as_flat` and `skip`.
5. Format: golden `.ser`/`.rri`; `--verify`, `--json`, `--toml` byte-exact; `--elf` == `export_image()`; `--diff` classes incl. `metadata-only`; **dictionary completeness**: `dlopen` after opening a `file_sink` → back-patched dictionary decodes; same with a pipe `fd_sink` → delta dictionary frame decodes; unclean close detected by `--verify`.
6. Env hooks: FD dump into a pipe equals `export_image()`; `ES_SERREG_DUMP_ON=change` fires once per `dlopen`; signal dump triggered via `kill()` in a child.
7. Fuzz: reader, RRI loader, ELF extractor, JSON/TOML readers.
8. Benchmarks; assembly inspection.
9. Sanitizer presets, `-Werror`.

---

## 11. Non-blocking items (defaults apply unless changed during M1–M3)

None of these block M1. Each has a default that will be used if not revisited.

| Item | Default |
|---|---|
| Exact sentinel value for dictionary frames | `0x4553'5352'4449'4354` (`"ESSRDICT"`) |
| `since`/`deprecated` width | `uint16_t` |
| In-house TOML subset limits | as FR-D9 |
| Dictionary back-patch strategy when the new dictionary is larger | append new dictionary at end, update header offsets, leave old area (flag `back-patched`) |
| Signal helper thread poll interval | 100 ms |
| Diagnostic format for FR-R10 abort | Obsolete since 2.1.13 (decision A: no abort). Until 2.1.12: `es::serreg: refusing to unload <path>: <N> registered entries (<first 5 names>...). Use RTLD_NODELETE or ES_SERREG_ALLOW_DLCLOSE=1.` |

---

## 12. Milestones

| M | Deliverable | Exit criteria |
|---|---|---|
| **M0 ✔** | v3 agreed; `AGENTS.md`/`CLAUDE.md` in repo | this document |
| M1 | descriptors, canonical names, hashes, blobs, literals, annotations (compile-time only) | golden `STATIC_REQUIRE`s pass on GCC 16 + clang-p2996; blob round-trips through the RRI loader |
| M2 | registry (push/snapshot/version/on_change/images), dlclose guard, `serialize`/`deserialize` typed, sinks, ELF section, env + signal hooks | §10.2 #3, #6 pass |
| M3 | wire format (compact/full/dictionary frames, back-patch), RRI export, dictionary bin/json/toml, `tools::` + `cli::run`, `srdump` (dump/verify/dict/elf/rri/config) | golden files; `--elf` == `export_image()`; completeness tests |
| M4 | inspect/dispatch/visit/view, `--diff`, `--flatten`, fuzzing (incl. parsers) | 1 h fuzz clean under ASan/UBSan |
| M5 | IPC example over the SPMC disruptor, benchmarks, README, v1 release tarball | NFR-1 numbers recorded; assembly check |
| M6 (v2) | dynamic members, evolution rules using `since`/`deprecated`, aarch64, optional external parsers | `ser-reg-requirements-v4.md` |

Status (2.1.7): dynamic members shipped in 2.0.0 under `docs/plans-for-v2.0.md`, without a v4 spec. Evolution did
not ship; its plan is `docs/plans-for-v2.1.md` (level 1, decision D5), targeted at library 2.2.0. aarch64 and
external parsers are not started. `docs/deviations.md` lists every place where the code differs from this spec.

---

## 13. References

**Standards:** [P2996 Reflection](https://wg21.link/p2996) · [P1306 Expansion statements](https://wg21.link/p1306) · [P3491 define_static_*](https://wg21.link/p3491) · [P3394 Annotations](https://wg21.link/p3394) · [P3096 Parameter reflection](https://wg21.link/p3096) · [cppreference `<meta>`](https://en.cppreference.com/w/cpp/meta) · [C++26 compiler support](https://en.cppreference.com/w/cpp/compiler_support/26) · [`std::expected`](https://en.cppreference.com/w/cpp/utility/expected) · [`constinit`](https://en.cppreference.com/w/cpp/language/constinit) · [`has_unique_object_representations`](https://en.cppreference.com/w/cpp/types/has_unique_object_representations) · [`start_lifetime_as`](https://en.cppreference.com/w/cpp/memory/start_lifetime_as)

**Compilers/tooling/OS:** [GCC C++ status](https://gcc.gnu.org/projects/cxx-status.html) · [GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html) · [Qt blog: GCC reflection](https://www.qt.io/blog/c26-reflection-qrangemodel) · [clang-p2996](https://github.com/bloomberg/clang-p2996) · [Compiler Explorer](https://godbolt.org/) · [GCC variable attributes](https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html) · [GNU ld orphan sections](https://sourceware.org/binutils/docs/ld/Orphan-Sections.html) · [ELF-64 format](https://uclibc.org/docs/elf-64-gen.pdf) · [dlopen(3) — RTLD_NODELETE](https://man7.org/linux/man-pages/man3/dlopen.3.html) · [dladdr(3)](https://man7.org/linux/man-pages/man3/dladdr.3.html) · [signal-safety(7)](https://man7.org/linux/man-pages/man7/signal-safety.7.html) · [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) · [libFuzzer](https://llvm.org/docs/LibFuzzer.html)

**Serialization prior art:** [zpp_bits](https://github.com/eyalz800/zpp_bits) · [SBE](https://github.com/aeron-io/simple-binary-encoding) / [FIX SBE spec](https://www.fixtrading.org/standards/sbe/) · [Cap'n Proto encoding](https://capnproto.org/encoding.html) · [FlatBuffers](https://flatbuffers.dev/) · [Protobuf encoding](https://protobuf.dev/programming-guides/encoding/) · [Arrow IPC](https://arrow.apache.org/docs/format/Columnar.html) · [Avro fingerprints](https://avro.apache.org/docs/1.12.0/specification/#schema-fingerprints) · [Boost.PFR](https://github.com/boostorg/pfr) · [Boost.Describe](https://github.com/boostorg/describe) · [cereal](https://github.com/USCiLab/cereal) · [alpaca](https://github.com/p-ranav/alpaca) · [bitsery](https://github.com/fraillt/bitsery) · [glaze P2996](https://stephenberry.github.io/glaze/p2996-reflection/) · [reflect-cpp C++26](https://rfl.getml.com/cpp26_reflection)

**Binary logging:** [Binlog](https://github.com/morganstanley/binlog) · [NanoLog](https://github.com/PlatformLab/NanoLog) · [Quill](https://github.com/odygrd/quill)

**Hashing:** [FNV](http://www.isthe.com/chongo/tech/comp/fnv/) · [rapidhash](https://github.com/Nicoshev/rapidhash) · [xxHash](https://github.com/Cyan4973/xxHash) · [Birthday problem](https://en.wikipedia.org/wiki/Birthday_problem#Probability_table)

**Testing/format libs (future options):** [Catch2](https://github.com/catchorg/Catch2) · [toml++](https://github.com/marzer/tomlplusplus) · [simdjson](https://github.com/simdjson/simdjson) · [TOML spec](https://toml.io/en/v1.0.0)

**Practices:** [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) · [SIOF (isocpp FAQ)](https://isocpp.org/wiki/faq/ctors#static-init-order) · [Nifty counter](https://en.wikibooks.org/wiki/More_C%2B%2B_Idioms/Nifty_Counter) · [Beman](https://github.com/bemanproject/beman) · [Herb Sutter Sofia 2025 trip report](https://herbsutter.com/2025/06/21/trip-report-june-2025-iso-c-standards-meeting-sofia-bulgaria/) · [Barry Revzin's blog](https://brevzin.github.io/)

---

## Appendix A — Implementation-session prompt (v3)

> **Project:** `ser-reg` (`es::serreg`) — reflection-driven serialization + type registry for C++26. Spec: `docs/ser-reg-requirements-v3.md`; agent rules: `AGENTS.md` / `CLAUDE.md`. Implement milestone **M<N>** as specified. Decided items (§0) are not re-opened; §11 defaults apply unless the user changes them; a compiler limitation is handled by the smallest deviation, recorded in `docs/deviations.md`.
>
> **Toolchain:** GCC 16.x `-std=c++26 -freflection`; CMake ≥ 4 presets `gcc16-debug|release|asan-ubsan|clang-p2996`; cross-check on Compiler Explorer ("x86-64 gcc 16" / "x86-64 clang (experimental P2996)" with `-freflection-latest -stdlib=libc++`). Warnings as errors. No RTTI reliance, no exceptions, `std::expected`, `constinit` globals, `const` parameters by default, **no external dependencies** (Catch2 test-only).
>
> **Reflection spellings:** `^^T`, `[: r :]`, `nonstatic_data_members_of(^^T, access_context::unchecked())`, `bases_of`, `parent_of`, `identifier_of`, `has_identifier`, `type_of`, `offset_of(m).bytes`, `size_of`, `alignment_of`, `is_bit_field`, `enumerators_of`, `annotations_of`, `define_static_string/array/object`, `template for`. On failure: check GCC's `<meta>`, then fall back to index-sequence expansion; record in deviations.
>
> **Verification protocol (before any "done"):** (1) build all presets, run all tests, paste commands and final output lines; (2) M1+: golden `STATIC_REQUIRE` file compiles on GCC 16 and clang-p2996, CE links in `docs/ce-links.md`; (3) M2+: registry counts at `main()` and after `dlopen`; `dlclose` keeps entries and reload tests (2.1.13; the v1 death and unlink tests until 2.1.12); static/shared/`--gc-sections`/`-flto` builds; (4) M3+: `srdump --verify` = 0 on golden files, `--json`/`--toml` byte-exact, `--elf` on the test binary equals `export_image()`, dictionary-completeness tests (back-patch and delta frame) pass; (5) ASan/UBSan clean; (6) if the sandbox lacks GCC 16 reflection, shim only for compile-checking non-reflection code, say so, and list what is unverified.
>
> **Deliverables:** versioned tarball `ser-reg-vNNN.tar.zst` with top-level dir `ser-reg-vNNN/`, never reuse a filename; `CHANGELOG.md` entry; requirement IDs in test names and commit messages.
>
> **Reporting:** end with (a) verified & how, (b) not verified, (c) deviations, (d) next step. Short prose; code and command output are the evidence.
