# ser-reg — Architecture (`es::serreg`)

**Library:** 2.2.6 · **Wire format:** 1.7 (readers accept 1.6 and 1.7) · **Hash algorithm:** XXH3-64 (`hash_algo` 2) ·
**Date:** 2026-10-07 · **Status:** the single, current description of how the library is built (decision D-1).

This document says how the code is structured, what is computed when, how data flows through it and what it
costs. What the library must do is in `docs/ser-reg-requirements.md` (FR-IDs cited here), why it is built this way
is in `docs/DECISIONS.md` (decision IDs cited here). `include/es/serreg/format.hpp` is normative for every wire
constant; where this document and the header disagree, the header is right. Symbols are cited by file and name,
never by line. No history: a bare "(since 2.x)" appears only where a reader of an old capture needs it.

---

## 1. Overview

### 1.1 Three libraries, one package (A-4)

| Target | What is in it | Depends on |
|---|---|---|
| `es::serreg` (`libserreg`, static and shared) | the headers under `include/es/serreg/` — reflection, descriptors, hashes, blobs, literals and constants, the registry API, sinks, `serialize`, `capture_file`, the logger, the typed readers, `frame_router`, `walk_values`, `dyn::dictionary`, `decode_limits`; and `src/{registry,image_elf,image_none,elf_reader,file_io,dictionary,reader,decoder,formatters}.cpp` | nothing (libc, libstdc++, `-ldl` for the ELF backend) |
| `es::serreg_tools` (`libserreg_tools`) | `src/{cli,diff,wireshark,cxxgen}.cpp` — the `srdump` command line (`es::serreg::cli`), `tools::diff`, the Lua dissector generator, the C++ generator; `tools::render_value`, `tools::process_dictionary` and `to_string.hpp` link it | `es::serreg` |
| `es::ipc` (header-only, C++23) | `include/es/ipc/` — variable-size shared-memory rings (SPSC queue, SPMC broadcast), regions, producer and consumer, a `claiming_sink` adaptor | nothing; no reflection flag; `-DES_SERREG_IPC_ONLY=ON` installs it alone (G-4) |
| `srdump` | `tools/srdump/main.cpp`: `return es::serreg::cli::run(argc, argv);` | `es::serreg_tools` |

The dependency arrows point one way: tools depend on the runtime, nothing depends on `es::ipc`, and `es::ipc`
depends on nothing. Tool code never moves into `es::serreg` (A-2, FR-D0). The fixed-slot rings that preceded
`es::ipc` are out of the build (G-17; `remove-me/include/es/ipc_old/` until 2.3.0).

### 1.2 The idea in one paragraph

A call `serialize(sink, "fill"_, order)` instantiates templates; nothing else happens at the call site. The
instantiation (a) builds, at compile time, a complete description of every type in the call — a `type_desc` in
`.rodata`, two canonical strings, two 64-bit hashes, and an ENTRY blob that is the description in wire form;
(b) odr-uses one static guard per type, literal, call signature and scope, so the image that contains the call
pushes those entries into a process-wide registry during its static initialisation (before `main()`, or inside
`dlopen`) — **registration by instantiation** (R-5, FR-R1/R2); (c) places a copy of every ENTRY in the ELF
section `es_serreg_dict`, so a tool can read the program's schema from the binary without running it — the
**embedded dictionary**; and (d) at run time writes a 16-byte frame header followed by the raw bytes of the
arguments. Every reader — the typed `deserialize`, `srdump`, the Wireshark dissector, the Python reader — decodes
frames from a **dictionary** (the ENTRY stream a capture carries, the registry exports, or the section holds), never
from a reader's own compiled types alone: **dictionary-driven readers** (FR-D4). The three carriers hold byte-identical
ENTRYs, which is why `srdump --elf ./app` and `srdump capture.ser` agree.

### 1.3 The three phases

```
compile time   reflect.hpp → descriptor_v<T>, schema/layout strings → fingerprint64 → blob (ENTRY bytes) → es_serreg_dict copy
load time      registrar<T>::g (odr-used by serialize) → registry::push() → heap entry → published index (flat_maps)
run time       serialize(): header + records → sink (span, vector, fd, capture_file, ipc ring)
               readers: deserialize / inspect / dispatch / frame_router / walk_values / srdump ← dictionary (RRI)
```

---

## 2. Compile time

### 2.1 Files and boundaries

| Header | Role | Reflection |
|---|---|---|
| `descriptor.hpp` | the data model: `kind`, `field_desc`, `enumerator_desc`, `gap`, `type_desc`; `ES_SERREG_HIDDEN` | none (shared with `libserreg`) |
| `hash.hpp` | `fingerprint64` (XXH3-64, `if consteval` byte assembly, `memcpy` at run time), `fingerprint64_u64s`, `crc32c` (constexpr table + SSE4.2 at run time), string helpers | none |
| `format.hpp` | every wire constant, `abi_tag`, `cursor`, `writer`, `slot`, `image_budget`, `detail::cbuf`, `to_blob_array` | none |
| `literal.hpp` | `fixed_string`, `lit<S>`, `constant<V>`, the UDLs, `literal_blob_bytes_v<S>` | none |
| `reflect.hpp` | **the only reflection user**: `descriptor_v<T>`'s pieces, canonical names and strings, `schema_hash_v`, `layout_hash_v`, `canonical_name_v`, `flat_type`/`dynamic_type`, gap and bool tables, enum kinds, the dynamic-image measure/encode/decode walks | `<meta>`, P2996 |
| `arg.hpp` | the five argument categories, `call_string<Ts...>`, `call_args_v`, `call_flags` | includes `reflect.hpp` |
| `detail/blob_bytes.hpp` | `call_arg_desc`, `call_desc`, the form tags (`print_tag`, `println_tag`, `log_tag`), the consteval ENTRY builders `type_blob_bytes_v`, `call_blob_bytes_v` | includes `reflect.hpp` |
| `detail/embed_elf.hpp` / `detail/embed_none.hpp` | the TU-local section copies (`type_blob_copy_v`, …) or nothing (C-9) | none |
| `blob.hpp` | completes `descriptor_v<T>` (adds `.blob`), `call_desc_v<Ts...>`, `call_hash_v`, `call_name_v`, `literal_blob_v` | includes `reflect.hpp` |

Two known exceptions to "reflection only in `reflect.hpp`": `registry.hpp` (`detail::touch_components` splices member
types to register them) and `blob.hpp` (includes `reflect.hpp`), both recorded in `docs/deviations.md`. Member
iteration uses `std::index_sequence` over `std::define_static_array(nonstatic_data_members_of(^^T,
access_context::unchecked()))[I]`; `template for` appears only over a template's arguments (`canonical_name`) and an
enum's enumerators (`enumerators_fit`), so the headers depend on as few P2996 companion papers as possible.

### 2.2 What is computed, and when

Everything below is a constant expression. The consteval boundary is the `inline constexpr` variable templates: a
value is computed once per type per translation unit at the point of instantiation, COMDAT-folded per image, and
hidden (`ES_SERREG_HIDDEN`, so no `STB_GNU_UNIQUE` binding and one copy per image — the registry's per-image
reference model).

1. **Admission** — `detail::check_supported<T>()` (`reflect.hpp`): every member type is one of the supported kinds
   or a `static_assert` names the type and member and mentions `as_flat<T>` (FR-S7). A recursive type is detected
   by a reflection walk before any template recursion (`contains_self`). `detail::mapped_t<T>` is `as_flat<T>::type`
   when specialised, else `T` (FR-S8).
2. **Members** — `members<U>()`, `bases<U>()`, `member_skipped<U, I>()` (`[[=es::serreg::skip]]`),
   `annotation_value<since>` / `<deprecated>` (FR-S11). Private members are included
   (`access_context::unchecked()`, FR-S9). Each base is one pseudo-field `<base:Name>` of kind `struct_` at the
   base's offset (P-4); virtual bases are rejected.
3. **Field descriptors** — `fields_v<U>`: name (`std::define_static_string(identifier_of(m))`), offset (native
   `offset_of(m).bytes` for flat classes, the canonical image offset for dynamic ones), size, align, kind, nested
   schema hash, `extent`, element descriptors (`elem_v`), `since`/`deprecated`. Memoised per type: nested
   references reuse the nested type's values.
4. **Canonical name** — `detail::canonical_name<T>()`, rules of P-3 in order: an explicit name
   (`[[=es::serreg::name("…")]]` or `type_properties<T>::name()`, never both); scalars by kind name (`i32`, `f64`,
   `char`, `wchar`, `byte`); the frozen std vocabulary (`type_properties` specialisations: `std::string` for any
   allocator, `std::vector<E>`, `std::array<E,N>`, `std::optional<E>`, `std::variant<…>`, `std::pair`,
   `std::tuple`, `std::monostate`, the six map/set containers — comparator or hasher other than the defaults kept,
   allocator dropped); a type without a linkage-unique name (unnamed namespace, local class) is a `static_assert`
   unless rule 1 applies; otherwise the walk of `parent_of` to the global namespace joined with `::`, user inline
   namespaces kept, std's reserved implementation namespaces dropped, an enclosing class-template specialisation
   named through its canonical arguments (`ns::Outer<i32>::Inner`), template arguments rendered canonically
   (`Vec<i32,3>`, `W<i32,enum(7)>`, `true`); recursive types rejected.
5. **Canonical strings** — `schema_string<T>()` and `layout_string<T>()` (`append_kindspec`): the schema form carries
   names, nested names and `;sN;dN` annotations; the layout form carries sizes, offsets and nested *layout* hashes
   (`L:`) only. A dynamic class's schema string starts with `dyn<Name>` so a flat and a dynamic definition of one
   name never share a hash. Kindspecs: a scalar's kind name, `enum<Name,u8>` / `enum<u8>`, `array<spec,N>`,
   `struct<0x…>` / `struct<L:0x…>`, `string`, `vector<spec>`, `optional<spec>`, `variant<a|b>`, `map<K,V>`, `set<K>`
   (the shape, whatever the container, P-7). The underlying kind of an enum without a fixed underlying type is the
   unsigned kind of `sizeof(E)`, or the signed kind when an enumerator is negative (P-8).
6. **Hashes** — `schema_hash_v<T> = fingerprint64(schema string)`, `layout_hash_v<T>`, and for calls
   `call_hash_v<Ts...> = fingerprint64(call_string<Ts...>())` where the call string is `call(n){…}`, `print(n){…}`,
   `println(n){…}` or `log(n){…}` over the argument categories in order (FR-H1/H2). A literal's id is
   `fingerprint64` of its bytes (FR-H7). Golden `STATIC_REQUIRE`s (`tests/golden/hashes.cpp`) freeze every string
   and hash (T-2).
7. **Scrub tables** — `gaps_v<U>` (padding intervals: leaf coverage merged and complemented over `[0, sizeof)`, no
   per-byte work) and `bools_v<U>` (offsets of every `bool` leaf) for flat types (FR-S6). A dynamic class has no
   tables: the encoder writes every byte of the image.
8. **Descriptor** — `detail::make_type_desc_base<T>()` assembles the `type_desc`; `blob.hpp`'s
   `detail::make_type_desc<T>()` adds `.blob` and calls `embed_type_blob<T>()`, which takes the address of this TU's
   section copy. `descriptor_v<T>` is the finished constant.
9. **ENTRY blobs** — `detail/blob_bytes.hpp` builds the bytes with `detail::cbuf` (a constexpr byte builder:
   `std::bit_cast` into byte arrays, `begin_entry`/`end_entry` back-patch `total_len` and pad to 8) and freezes them
   with the two-pass `detail::to_blob_array<P>()` (size first, then contents) into a `std::array<std::byte, N>`.
   `type_blob_bytes_v<T>`, `literal_blob_bytes_v<S>` (`literal.hpp`), `call_blob_bytes_v<Ts...>`.

### 2.3 `fixed_string`, literals and constants

`fixed_string<N>` is a structural NTTP; `lit<S>` is an empty type with `str`, `size`, `id` and `text()` (a view into
the literal's own ENTRY blob at `fmt::literal_text_offset`: the text exists exactly once per image, FR-L4). The four
spellings — `"x"_lit`, `"x"_`, `serialize<"x">(…)`, `constexpr lit<"x"> k` — yield one type (FR-L1).
`constant<V>` is to a number what `lit<S>` is to a string: `42_`, `-7_`, `1.5_` (the numeric `operator""_`, parsing the
digits at compile time: `int`, else `long long`, else `unsigned long long`; a floating literal is `double`) and
`k<V>` for every other scalar or enum (A-6, FR-A5). `detail::const_bits` gives the 8 value bytes (zero-extended;
floating values by bit pattern) for the call string (`const:i32=42`, `const:<enum hash>=2`) and the ARG.

### 2.4 Argument categories (`arg.hpp`)

| Category | `remove_cvref_t` of the argument | Call-string item | ARG | Compact record |
|---|---|---|---|---|
| type | a `flat_type` or `dynamic_type` (a class, an enum, a container as argument) | `<hex schema hash>` | `arg_type` | `sizeof` bytes / `u32 len, u32 0, image` |
| literal | `lit<S>` | `lit:<hex id>` | `arg_literal` | nothing |
| scalar | `bool`, char family, integers, `float`, `double`, `std::byte` | the kind name | `arg_scalar` | the bytes |
| string | `const char*`, `char[N]`, `std::string_view`, `std::basic_string<char, …, A>` | `string` | `arg_string` | `u32 len, bytes` |
| constant | `constant<V>` | `const:<kind>=<value>` | `arg_const` + `u64 value` | nothing |

Names are category names, not C++ type names: `serialize(out, "abc", 1)` and `deserialize(bytes, std::string&, int&)`
build the same call string. A container passed as an argument (`std::vector<int>`, a map, …) is a **transparent
type** of its own: a synthetic dynamic class named after the container's vocabulary name with one unnamed field at
offset 0 (size 8, align 4) and `struct_flag_transparent` (FR-A6). A top-level `std::string` stays the `string`
category.

### 2.5 Type support (what reflection admits)

Flat: arithmetic, `bool`, the char family, `std::byte`, enums (fixed underlying type or not), nested flat classes
with private members and bases, C arrays and `std::array` at any depth, `[[no_unique_address]]` and empty members.
Dynamic members (`detail::dyn_member<U>`): `std::basic_string<char, traits, A>` (`string_`), `std::vector<E>`
(`vector_`), `std::optional<E>` (`optional_`), `std::variant<Es...>` (`variant_`), `std::map` / `std::unordered_map` /
`std::flat_map` (`map_`), `std::set` / `std::unordered_set` / `std::flat_set` (`set_`); a class with one anywhere in
its tree is a dynamic class (`is_dynamic<U>()`). Rejected by `static_assert`: pointers, references, unions,
bit-fields, virtual classes, `std::string_view` members, multimaps and multisets, `std::list` and `std::deque`
(the `vector` shape for them is A-16, planned 2.2.x), recursive types, zero-size dynamic elements
(`std::array<T, 0>` of a dynamic `T`).

---

## 3. Load time — the registry

### 3.1 Nodes and guards (`registry.hpp`)

For every type, literal, call signature and scope there is one `reg_node` and one `detail::guard` per image:
`registrar<T>::node` / `::g`, `registrar_lit<S>`, `registrar_call<Ts...>`, `registrar_scoped<Scope, Ts...>` — static
data members of class templates, COMDAT, hidden. A node is constant-initialised in `.data`:
`{payload (type_desc* | call_desc* | scope_desc* | nullptr), blob, hash, name, nk, entry, image_base, refs, linked}`.
The guard's constructor calls `registry::instance().push(&node)`; its destructor calls `on_guard_destroyed`.

Odr-using `registrar<T>::g` instantiates its definition ([temp.inst]), and a static data member of a class template
is dynamically initialised with the image that instantiated it — before `main()` for the executable and its
`DT_NEEDED` DSOs, inside `dlopen` for a plugin (FR-R2). `detail::serialize_frame` performs the odr-uses:
`touch_arg<Sig>()` for every signature element, `&registrar_call<Sig...>::g`, and, for a `scoped_sink`,
`&registrar_scoped<typename S::dictionary_scope, Sig...>::g`. `detail::touch<T>()` recurses through components —
bases, members (skipped members excepted), array elements, vector/optional elements, variant alternatives, map
keys and values, set keys — so every reachable type registers (FR-R1). `register_types<Ts...>()` and
`register_call<Sig...>()` run the same odr-uses without a call site (R-5); `register_namespace<^^ns, Select>`
(`register_namespace.hpp`, A-22) runs `touch<T>()` for every serialisable type `std::meta::members_of(^^ns)` yields
that the selector accepts. Nothing happens at run time: the calls are empty and fold away.

Survival under `-Wl,--gc-sections`, `-flto` and static archives (FR-R7) follows from the guards being dynamic
initialisers of odr-used objects. `-fno-gnu-unique` is a PUBLIC compile option of `es::serreg` on GCC: without it a
vague-linkage variable becomes `STB_GNU_UNIQUE`, glibc marks the DSO `DF_1_NODELETE`, and a rebuilt plugin never
reloads.

### 3.2 `push()` — heap entries, views and copies (`src/registry.cpp`)

The registry state is a leaked function-local static `G()` (no static-initialisation order, outlives `atexit`):
`mu`, `all` (every `entry` ever made), `by_key` (`(nk, hash)` → current entry), `conflicts`, `callbacks`, the
published `index`, `retired` generations, `stale`, `generations`, the conflict policy and the dump helper state.

`registry::push(n)`:

1. For a literal, set `n->name` to the text inside the blob (no second copy).
2. Outside the mutex (the loader lock may be taken, never under `mu`): `n->image_base = image_of(payload)`
   (`dladdr`) and `main_image()` (`dl_iterate_phdr` to the executable's lowest `PT_LOAD`), both through the image
   backend (`src/image.hpp`).
3. Under `mu`, with `slot = by_key[(nk, hash)]` and `appears = version() + 1`:
   - no entry → `make_entry(n, main_base)`, attach, `added = appears`;
   - same definition (not a type, or equal layout hash) → `attach`: an alias from another TU/image, or a **retained
     entry revived** by a reload (its `image_base` is updated);
   - another layout and the entry is retained (`refs == 0`) → a new entry replaces it in `by_key`; the old one
     stays alive;
   - another layout and the entry is loaded → **conflict** (FR-R4, G-16): a new, *unlinked* entry holds the rejected
     definition, `(kept, rejected)` is recorded, and after the lock `report_conflict` prints one line
     (`ES_SERREG_CONFLICT=warn|abort|silent`).
   - `mark_stale_locked()`: bump the atomic version, set `stale`.

`make_entry` decides what is owned. **The main program's registrations are viewed, not copied (C-11, FR-R14):**
the executable is never unmapped before exit, so `entry.blob`, `entry.name` and `entry.payload` point into its
image. Every other image's registration gets `blob_copy`, `name_copy` (a literal's text is viewed inside the blob
copy) and `desc_copy`, an `owned_desc` deep copy of the descriptor — names in a `std::deque<std::string>` (stable
addresses), every field array and nested `elem` array, enumerators, gap and bool tables, call arguments — so
nothing the registry hands out points into an image that can be unloaded (FR-R10). `push` is `noexcept`: it runs
in static initialisation, where an allocation failure has no caller to report to.

### 3.3 Unload, retention, reload (A-3)

`registry::on_guard_destroyed(n)` runs inside `dlclose` and at process exit — the registry need not tell them
apart. Under `mu`, in place, without allocating or freeing: erase the node from `entry.nodes`, decrement `refs`,
move `image_base` to another loaded node if the leaving image was the recorded one. When `refs` reaches 0 the
entry is **retained**: it stays in every later index, `find_*` return it, its descriptor stays readable, frames
written while its image was loaded still decode (FR-R11). A rejected definition that unloads leaves no conflict
behind; when a kept definition unloads and a loaded conflicting one exists, that one becomes `by_key`'s entry and
its nodes are marked `linked` (G-16). Nothing is ever freed before exit (`free_at_exit` releases only retired
indexes at `exit`); `registry::forget_retained()` is A-14 (3.0). There is no `dlopen`/`dlclose` interposer and no
abort.

What the registry cannot protect: a plugin's own code (an `on_change` callback defined in a plugin must be removed
before `dlclose`), its objects and threads; and a static `libserreg.a` linked into a plugin is a second registry
(`serreg_init` detects it through `es_serreg_registry_marker_v3()` and prints one line, FR-R3).

### 3.4 The published index (FR-R6)

A change only marks the registry stale. The next reader — `snapshot()`, and through it `size()`, `find_type`,
`find_call`, `find_literal`, `export_image*`, `program_hash`, `images()` — runs `publish_if_stale()`: under `mu`,
`publish_locked()` builds one `registry_index` for the whole burst: `entries` (views `registry_entry{nk, hash, name,
blob, payload, image_base, refs, added}` of every heap entry, sorted by `(nk, hash)`), `conflicts`, `version`,
`retained`, and `lookup` — one `std::flat_map<u64, u32>` per node kind (type, literal, call, scope), adopted from
pre-sorted vectors with `std::sorted_unique`. The index is published with an `exchange` (acq_rel); the old
generation goes to `retired` (never freed before exit, A-13); `stale` is cleared with release. Readers acquire-load
the pointer and binary-search the keys: lock-free, O(log n), no pointer into an image. `generations()` counts
indexes published (a program that registers 10 000 types before `main()` publishes one). After the publish, and
outside `mu`, the `on_change` callbacks run once on the publishing thread (never under the loader lock unless that
reader is itself inside a constructor); `refresh()` publishes and notifies on the calling thread; callbacks do
not run once `exiting` is set.

`version()` is one acquire load of an atomic that moves on every change (aliases included) — what
`capture_file::before_frame` and `dictionary_tracker::update` compare against.

### 3.5 Scope nodes (P-16, A-9, FR-R15)

A `scoped_sink` declares `using dictionary_scope = Tag;`. Serialising into it odr-uses
`registrar_scoped<Tag, Sig...>::g`, whose node has `node_kind::scope`, payload `scope_desc{scope_hash, call_hash,
scope_name}` (`scope_hash_v<Tag> = fingerprint64(canonical_name_v<Tag>)`), key `fingerprint64_u64s({scope_hash,
call_hash})` and no blob. Scope nodes are in the index (fourth `flat_map`) and in `snapshot()` but never in a full
export. Everything is compile-time; nothing per frame.

### 3.6 Exports, deltas and scoped exports

- `export_image()` = `export_image_since(0)`: a 40-byte RRI header (`"ESRG"`, 1.7, flags, `hash_algo` 2,
  `n_entries`, the program hash over the loaded **and retained** type hashes of this generation, `host_abi_tag`)
  followed by every non-scope entry's blob in index order. Retained entries are included (A-14, FR-R16).
- `export_image_since(v)`: the entries whose `added > v`, header flag `rri_flag_delta` — what a dictionary frame
  carries after a `dlopen`. `added` is the version at which the *definition* appeared; a revived entry keeps it.
- `export_image_scope(scope_hash, since)`: the closure of the scope's calls — their literal arguments, their type
  arguments (and enum constants), and recursively every type hash a FIELD or element descriptor of those types
  names — computed on `dyn::dictionary` of the full image (`add_rri(export_image_since(0))`), then the selected
  entries' blobs in index order, full or delta. `scope_calls(scope_hash)` counts the `(scope, call)` pairs.
- `export_image(fd | path)`, `to_json()`, `to_toml()` (through `to_dictionary` → `dyn::dictionary`).

### 3.7 Environment hooks and the init constructor (FR-R9)

`serreg_init` (`[[gnu::constructor(101)]]` in `registry.cpp`) runs when `libserreg` is loaded: it warns about a
second copy, reads `ES_SERREG_DUMP_REGISTRY`, `_FD`, `_FORMAT`, `_ON`, `_EXIT`, `_SIGNAL`, `ES_SERREG_CONFLICT` once,
blocks the dump signal in the loading thread (later threads inherit the mask), starts the `sigwait` helper thread
when a signal is configured (the handler body is one atomic store; all I/O happens on the helper), and registers
the exit hook (`exit_hook`: the exit dump when configured, `exiting = true`, `free_at_exit`). `ES_SERREG_ALLOW_DLCLOSE`
is ignored with one line.

### 3.8 The executable-format seam (C-9)

`src/image.hpp` declares what the registry asks of the program image — `image_of(addr)`, `main_image()`,
`image_name(base)`, `can_unload(addr)` (`registry::unload_guard_active`), `first_registry_marker()` — with one
backend per build (CMake `ES_SERREG_IMAGE_FORMAT`): `image_elf.cpp` (`dladdr`, `dl_iterate_phdr`, the `DT_FLAGS_1`
`NODELETE` probe, `dlsym` of the marker) or `image_none.cpp` (one image, nothing unloads; refused together with
`ES_SERREG_BUILD_SHARED=ON`). The header side of the seam is `detail/embed_elf.hpp` / `embed_none.hpp`
(`ES_SERREG_EMBED_NONE`): the section copies, or none. Free functions, no vtable; nothing here is on the frame path.

### 3.9 The `es_serreg_dict` section and `--elf`

Each translation unit that instantiates a descriptor, literal or call gets a TU-local `static constexpr` copy of the
blob in section `es_serreg_dict`, 8-aligned, `[[gnu::used, gnu::retain]]` (binutils ≥ 2.36). TU-local on purpose:
GCC places every COMDAT variable that names a custom section into one section carrying the group of whichever came
first, which breaks multi-TU links; duplicates across TUs are harmless because readers key by hash. The section is
read offline by `tools::extract_embedded_dictionary` (`src/elf_reader.cpp`: a self-contained ELF64 reader — magic,
section headers, `.shstrtab`, bounds- and overflow-checked, capped at `decode_limits::input_bytes`; `src/file_io.cpp`
does the I/O) and is what `srdump --elf` decodes. The program itself never reads it: the registry is the run-time
source of truth. Several `--elf` sources merge.

---

## 4. Run time — writing

### 4.1 Sinks (`sink.hpp`, FR-S2)

| Concept / type | Shape | Notes |
|---|---|---|
| `sink` | `write(const std::byte*, size_t) -> bool` | `false` ⇒ `error::sink_full`, or `state().error()` when the sink has `status state() const` (`detail::sink_with_state`) |
| `claiming_sink` | `claim(n) -> std::byte*` (nullptr ⇒ full), `commit(n)` (`void` or `bool`; a refused commit fails the `serialize`) | the frame is assembled in place: rings, `span_sink`, `vector_sink`, `capture_file` |
| `scoped_sink` | `sink` with `typename S::dictionary_scope` | registers `(scope, call)` nodes (§3.5) |
| `span_sink` | caller memory, `used`, no wrap | claiming |
| `vector_sink` | appends to a `std::vector<std::byte>` | claiming; allocates, so `serialize` into it is not `noexcept` |
| `fd_sink` | blocking descriptor, sticky `io_error` state, `reset()` | not seekable: dictionary updates are dictionary frames |
| `basic_capture_file<Scope, P>` | the FILE container (§4.3) | claiming, `frame_crc()`, `state()` |
| `es::ipc::sink` | a ring producer | claiming |

A sink whose `frame_crc()` returns `true` (`detail::sink_wants_crc`) receives every frame with a CRC32C trailer whatever
the caller's options. `detail::report_line(fmt, args...)` (`sink.hpp`) is the library's one diagnostic path: `std::format_to_n`
into a 1 KiB stack buffer and `write_fd(2, …)`, no allocation, no exception — usable from destructors, the init
constructor and inside `dlclose` (C-1).

### 4.2 Frame assembly (`serialize.hpp`, FR-S1, FR-W2)

`serialize(out, vs...)` → `serialize_with({}, out, vs...)` → `detail::serialize_frame<Sig...>(o, out, vs...)`, where
`Sig...` is the call signature (an optional form tag, then the argument categories; `print` passes
`print_tag, lit<F>, Ts...`, the logger `log_tag, …`). `serialize<"lit">(out, vs...)` is exactly
`serialize(out, lit<"lit">{}, vs...)`. `serialize_options{scrub_padding = true, full_records = false, crc = false}`.

`serialize_frame` in order:

1. The odr-uses of §3.1 (registration by instantiation).
2. `static_assert`s: at most 65535 records (`u16 n_records`); the largest form of this signature fits `u32
   frame_len`; a run-time check of the same for strings and images (`error::invalid_argument` above 4 GiB).
3. `o.crc |= sink_wants_crc(out)`; `total = frame_size<Ts...>(o)` (constexpr: exact when no argument is a string or
   a dynamic type, FR-S3) or `frame_size(o, vs...)` (one `size()`/`strlen`/`measure_image` pass, no allocation,
   FR-A3); `flags = full | crc`.
4. `fill`, a `[[gnu::always_inline]]` lambda (C-5; at `-O2` GCC would otherwise keep it out of line behind a
   closure): `put_frame_header` (`u32 frame_len | u16 flags | u16 n_records | u64 call_hash_v<Sig...>`), then
   `put_record` per argument — a constant writes nothing in a compact frame; `put_value_record` writes an object
   (`put_object`: `memcpy` of `mapped_t<T>`, then `scrub_range` zeroes the gaps and normalises the bools when
   `scrub_padding`; nothing when `has_unique_object_representations_v`), a literal's `u64 id` only in full-records
   mode, a scalar's bytes, `u32 len + bytes` for a string, `u32 len, u32 0, image` for a dynamic type
   (`encode_image`), each padded to 8; a full record is prefixed `u64 hash | u32 payload_len | u16 layout(0) | u16
   rsv`. Then `put_crc_trailer` when `o.crc`: `u32 crc32c(frame[0, total-8))`, `u32 0`.
5. Where the bytes go:
   - `claiming_sink`: `claim(total)`, `fill(p)`, `commit_claimed` — no intermediate copy.
   - plain sink, no run-time-sized argument and the largest form ≤ `stack_frame_limit` (64 KiB): an exactly sized
     `alignas(16)` stack buffer, one `write`.
   - plain sink, run-time size ≤ 4096: a 4 KiB stack buffer (the common short-message case).
   - plain sink with CRC above that: a heap buffer (the trailer covers the whole frame, so it cannot be streamed).
   - otherwise: header, then `stream_record` per argument through a `stream_chunk_size` (16 KiB) stack chunk, gap
     scrub applied per chunk, stopping at the first sink failure; a dynamic image above the chunk is the one heap
     allocation a plain sink's `serialize` may make.
6. Returns `total`, or `detail::sink_failure(out)`.

`serialize_raw<T>(out, v, scrub)` writes the payload only (flat types; the caller owns framing, FR-S5).
`detail::serialize_noexcept<S, Ts...>` is true for `noexcept` sinks unless a dynamic argument meets a plain sink.
There is no registry access, lock, atomic or allocation on this path (C-5; ctest `serialize_inlines_at_O2`).

Dynamic images (`reflect.hpp`): `measure_image(v)` and `encode_image(v, image)` mirror each other. The image is the
root's fixed part (members at canonical offsets, `image_offset_of`, bases first, natural alignment of image sizes)
followed by tail blocks appended depth-first in member order; the whole measured image is zeroed first so alignment
gaps are 0 on every sink. An unordered container is sorted through an index of pointers (`key_less`: `operator<`
when the key has one, else the bytes of its scrubbed flat image) — 64 entries on the stack, a heap vector above
(the documented allocation, FR-W9).

### 4.3 The FILE container (`capture.hpp`, format 1.7)

`basic_capture_file<Scope = void, capture_policy P = {}>` (`capture_file`, `scoped_capture_file<Scope>`) is a
claiming sink over its own buffer and a `std::FILE*`. Both template parameters are part of the sink *type*: the
scope's registrations and the policy's `if constexpr` branches cost nothing per message (A-11).

`open(path, capture_options)`:
1. `detail::file_header(program_hash, dict_offset = 48, dict_len, extra flags)` — 48 bytes.
2. The opening dictionary: `export_dictionary(0)` — `export_image_scope(scope_hash_v<Scope>, 0)` for a scoped
   file, else `export_image_since(0)` — padded to 8.
3. The **CHAIN**: `u64 next_dictionary, u64 first_checkpoint`, written as zeros (`chain_offset_` remembered).
4. The writer-info frame when `writer_info` (default on): `write_info` emits library version, format `"1.7"`,
   program (if given), hostname/pid/start time (`info_host`), the opening thread id (`info_thread`), the loggers'
   clock (`log_clock`), `crc`, `checkpoint_bytes`, `checkpoint_ns`, `dictionary` (`"full"` or `"scoped:<name>"`)
   and `sync`. FILE flags: `le | dict`, plus `crc_all`, `checkpoints`, `info` as configured.

Per data frame: `claim(n)` → `before_frame()`: with `P.dictionary_updates` one relaxed load of
`registry::version()`; when it moved, `write_delta(v)` — `export_dictionary(dict_version_)`; if the delta holds no
entry (the registry moved for another scope) only the version is recorded; else a dictionary frame
(`detail::dictionary_frame_bytes`: `header | RRI delta | pad | u64 next = 0`) is written, `sync_if(dictionary)`,
and the previous link (`next_link_offset_`: the CHAIN's first word, or the last dictionary frame's trailing 8 bytes)
is **patched exactly once** with `patch(at, value)` — `fflush`, one aligned 8-byte `pwrite` (P-9, FR-W12). Then
`commit(n)` → the frame is written, `after_frame(n)` counts data frames and bytes, checks `checkpoint_bytes`, checks
`checkpoint_every` once every 64 frames, and `sync_if(frame)`.

`write_checkpoint()`: a meta frame (`frame_flag_checkpoint`, `checkpoint_sentinel`) with the 64-byte fixed payload —
`u16 1 | u16 flags | u32 rri_len | u64 prev | u64 next = 0 | u64 data_frames | u64 data_bytes | u64 now() | u64
latest_dictionary_offset | u64 registry_version` — followed by the full dictionary when `checkpoint_dictionary`;
`sync_if(checkpoint)`; the previous checkpoint's `next` (or the CHAIN's second word) is patched (P-11, FR-W11).
`checkpoint()` writes one on demand. `close()`: a final checkpoint when checkpoints are enabled, `fflush`,
`fdatasync` unless `sync == none`, `fclose`. **Nothing is ever rewritten**: a file truncated anywhere is readable up
to the cut. Not thread-safe: one writer.

`sync_policy{none, dictionary (default), checkpoint, frame}` is an ordered level: `sync_if(l)` syncs when the policy
is at least `l`. `capture_policy{.dictionary_updates = false}` removes the per-frame compare and the chain machinery
for a program whose registry cannot move.

Streams (`fd_sink`, rings): `write_dictionary_frame(out, since)` and `dictionary_tracker::update(out)` (first call
the full image, later calls deltas; one relaxed load when nothing changed) keep a non-seekable sink complete
(FR-W7).

### 4.4 Print frames (`print.hpp`, FR-P*)

`print(out, "fmt {}"_, args...)` / `println` (and `_with`, and the `<"fmt">` spellings) build
exactly the frame `serialize(out, lit<F>{}, args...)` would, under the call string `print(n){…}` / `println(n){…}`
and the call flags `print` / `print|newline`. `detail::check_format<F, Ts...>()` constructs
`std::format_string<render_t<Ts>...>` in a constant expression, so the standard's own checker validates the
placeholders against the rendered types (scalars as themselves; literals, strings, enums and classes as
`std::string_view`; `std::byte` and the wide char types are not `printable_arg`); `no_nested_fields<F>()` rejects
`{:>{}}`. Nothing is formatted on the writer side; `tools::render_frame` formats on the reader side (FR-P4).

### 4.5 The logger (`log.hpp`, A-8, P-15)

`logger<log_options Opts, sink S>` requires `Opts.dictionary == full || scoped_sink<S>`. `log_options{floor, thread,
sequence, clock, dictionary}` is a structural NTTP, so every policy is `if constexpr` and two loggers with different
policies are two call signatures; `mask()` packs thread/sequence/clock bits into the envelope constant.

- **Gate:** `enabled(l)` is `l >= floor && l >= min_.load(relaxed)`; below the compile-time floor (`Opts.floor`
  joined with the translation-unit macro `ES_SERREG_LOG_MIN_LEVEL`) it is `false` at compile time. The macro
  `ES_SERREG_LOG(lg, lvl, "fmt"_, args...)` is `enabled(lvl) && lg.write<lvl, fixed_string{__FILE__}, __LINE__>(…)`,
  so no argument is evaluated when the level is off (FR-G1).
- **Frame:** `write<L, File, Line>` below the floor compiles to nothing (no registration); otherwise `write_frame`
  calls `detail::serialize_frame<log_tag, constant<u8 L>, lit<File>, constant<Line>, constant<mask>, u64,
  [u32], [u64], lit<F>, Ts...>` with the timestamp from `detail::log_now(Opts.clock, user_clock_)`
  (`system_clock`, `steady_clock`, `__rdtsc`, or `set_clock`), `this_thread_id()` (`gettid`) for
  `thread_policy::per_message`, and `++seq_` for `sequence_policy::per_sink`. Level, file, line and mask travel as
  zero-byte constants of the call; only the timestamp (and by policy the thread id and sequence) cost bytes beyond
  `println`'s (FR-G3, FR-G8). The call ENTRY carries `call_flag_log`.
- **Results:** `write` and `trace…fatal` (file `"-"`, line 0) return `bool`; failures are counted (`failed()`) and the
  last code kept (`last_error()`).
- **Readers:** `log_layout` fixes the positions (`level_arg` 0, `file_arg` 1, `line_arg` 2, `mask_arg` 3,
  `time_arg` 4, then `format_arg(mask)`), used by `src/formatters.cpp` (`decode_log`, `render_message`), the
  Wireshark runtime and the Python reader.

### 4.6 Hot-path cost model (NFR-1)

| Operation | Cost | Notes |
|---|---|---|
| `serialize` of a flat 32-byte struct, compact, claiming or span sink | header store + `memcpy(32)` + 0–2 gap stores; ceiling **< 50 ns** mean in `tests/bench/bench.cpp` (release `libserreg`; 4× under a Debug library) | no registry access, no atomics, no allocation |
| + a literal or constant argument | nothing | named by the call hash |
| + a run-time string | one `strlen`/`size()`, `u32 len` + bytes | |
| + a dynamic argument | one measure pass + one encode pass over the object; the image is zeroed first | heap only on a plain sink above 16 KiB or when sorting > 64 unordered entries |
| `serialize_raw` | `memcpy` + gap scrub | equals a hand-written copy |
| `print` | as `serialize` of the arguments | nothing formatted |
| logger (`thread = once`, `sequence = none`, `clock = system`) | gate (one relaxed load) + `clock_gettime` + a `println` frame with one `u64` more | FR-G8 |
| `capture_file` per frame | one relaxed load of `registry::version()` (removable), buffer copy, `fwrite` | CRC: one CRC32C over the frame (SSE4.2); checkpoint: 64 bytes per checkpoint |
| `inspect` of a one-record frame | header validation + one registry lookup; ceiling **< 500 ns** | |

Zero-overhead statement: for flat types with a claiming sink or `serialize_raw`, the generated code equals a manual
`memcpy` plus explicit zeroing of the pads; `serialize()` leaves no call at `-O2`. Benchmarks gate as functional
smoke tests; numbers come from the reference box only (T-9).

---

## 5. Run time — reading

### 5.1 The reading styles (`deserialize.hpp`, `walk.hpp`, `router.hpp`)

Common ground: `read_frame_header(bytes)` reads the 16-byte header, refuses a length that is not a multiple of 8 or
exceeds the buffer, refuses unknown flag bits (`bad_version`), and when the frame carries a CRC checks the trailer
**before anything in the frame is believed** (`crc_mismatch`, FR-W10). `frame_header::meta()` is true for dictionary,
checkpoint and writer-info frames. `detail::resolver` finds call signatures and types through the registry first,
then an optional side `dyn::dictionary`.

1. **Typed** — `deserialize(bytes, outs...)` → `detail::read_typed<Sig...>`: `validate_frame` walks every record with
   one cursor (variable-length ones by their own length fields), checking each step against `frame_len` before
   anything is read, and compares the call hash (compact) or per-record hashes (full) against `call_hash_v<Sig...>`;
   only then `get_record` copies each output in the same pass (R-8). Outputs: flat objects (`get_object`: `memcpy`,
   bool bytes other than 0/1 repaired to `true` and reported as `warning::noncanonical_bool`, an out-of-range leaf of
   an enum without a fixed underlying type set to 0 and `invalid_value`), scalars, `std::string` (copy, any
   allocator) or `std::string_view` (a view into the frame), `lit<S>` (id checked), `constant<V>` (value checked:
   another value is `hash_mismatch`), dynamic types (`decode_value` over `decode_ctx` with the slot rules of
   `fmt::image_budget`). On failure `decode_error{code, expected, actual, failed}`. `deserialize_print` /
   `deserialize_println<F>` take the format literal first; `deserialize_raw<T>` reads a payload.
2. **Inspect** — `inspect(bytes, const dyn::dictionary* = nullptr) -> result<frame_view>`: validated up front; lazy
   `record_view`s `{hash, is_literal, desc, dyn_desc, payload, ak, k, is_const}`; a constant's payload is the value
   stored in the call entry.
3. **Dispatch** — `dispatch<Ts...>(bytes, f[, dict])` calls `f(const T&)` for the first `T` whose schema hash matches
   each record (zero-copy when aligned, else a stack copy), `literal_view` for literals, the `record_view` when
   nothing matches; up to `dispatch_linear_max` (4) types by compares, above that `detail::hash_index` — a
   compile-time perfect hash over the XXH3 keys (shift + mask + one compare) with a binary-search fallback.
   `dispatch_frame` matches a whole signature.
4. **Router** — `frame_router`: `on(h)` derives the signature from `h`'s parameter types (`detail::handler_params`),
   `on(lit, h)`, `on<Sig...>(h)`, `on_print`/`on_println(fmt, h)`, `on_log(handler(level, string_view line))`
   (A-12, FR-G7), `otherwise(dictionary, frame_ref)`, `on_error`; the call hash of each signature is a constant, so
   `route(frame)` is one `unordered_map` lookup and the typed reader of style 1; decoded values' storage is kept
   between frames; `callback()` plugs into `tools::for_each_frame`. One router per reading thread.
5. **Dictionary-driven** — `tools::visit_record(payload, type, visitor)` over a `decoded_value_visitor`
   (`begin_struct`, `scalar`, `enumerator`, `literal`, `string`, `begin_vector`, `null_value`, `begin_variant`,
   `begin_map`/`begin_entry`, `begin_set`, `begin_transparent`, `take_struct`, `noncanonical_bool`, …), needing no
   user types: what every formatter, `verify`, `srdump`, `to_string` and the Wireshark runtime follow.
   `tools::walk_values<Ts...>(capture | path, on_value, rest)` is the hybrid: a value whose schema hash equals
   `schema_hash_v<T>` of a listed `T` is decoded into `T` (`detail::decode_at`) and handed to `on_value(field, t)`;
   everything else goes field by field to `rest`.
6. **Zero-copy view** — `view<T>(record_view) -> result<const T*>` with `std::start_lifetime_as`; `misaligned`
   otherwise; a non-canonical bool or out-of-range enum leaf is `invalid_value` (it cannot repair in place).

### 5.2 The dictionary-driven decoder (`src/decoder.cpp`)

`visit_record` walks one record payload through the FIELDs of a `dyn::type`. A flat record is read at native
offsets; a dynamic record is `u32 len, u32 0, image`, and every dynamic member is a slot checked by the one shared
rule set `fmt::image_budget` (`format.hpp`): `n == 0 ⇔ off == 0`; a non-empty block starts at or after the root's
fixed part and lies inside the image; the blocks of all slots together reference at most image-size bytes (the
running budget, so shared tails cannot amplify); `take_n` refuses an `n × stride` product that wraps 64 bits (P-5).
Vector elements are at `slot_stride(size, align)`; a map's entries are `extent` bytes with the key at 0 and the
value at its element descriptor's offset; a set's entries are keys; a variant's `n` is `index + 1`; an optional's
`n` is 0 or 1. Depth is bounded by `decode_limits::image_depth` (two units per struct level, ceiling
`fmt::max_nesting` = 64). Enum values are read by the underlying kind with sign extension; a duplicate map or set key
is `invalid_value` (A-16 adds the range check of unfixed enums here, planned 2.2.x). `verify` and `deserialize`
accept exactly the same images.

### 5.3 Capture walking (`src/reader.cpp`)

Two walkers share `classify_frame` (the one reading of a frame header: length, alignment, flag bits, CRC room) and
`dictionary_problems`:

- **Span walker** — `internal::for_each_record(capture, …)` over memory (a `mapped_file`, a vector): a FILE header
  (`parse_file_header`: magic, major 1, minor in `[format_minor_oldest, format_minor]`, `hash_algo` 2, flags) and
  the opening RRI (a 1.6 back-patched file: the frames start after the original dictionary area, `rri_length`), or a
  bare frame stream; then frames in order.
- **Windowed file walker** — `tools::for_each_frame_in_file(path, f, block_options)`: `pread` windows of
  `block_bytes` (default `decode_limits::window_bytes`, 10 MiB); a frame that straddles a window is completed from
  the next; a frame larger than the window grows it to that frame within `frame_bytes`; pipes and `"-"` are read
  sequentially with a buffer that starts at 2 MiB and grows to 8 MiB. State is O(types): nothing per frame is kept
  (R-12, FR-D11). This is the default for every path-based reader (`verify`, `dump`, `for_each_frame(path)`,
  `walk_values(path)`); `file_options{blocks = false}` maps the whole file instead.

Both walkers: a dictionary frame's RRI (full or delta) is merged into the running `dyn::dictionary` and consumed;
checkpoint and writer-info frames are consumed; meta frames count in `frame_ref::index` but never reach a callback,
`verify_report::frames`/`bytes` or `frame_walk::frames` (P-13, FR-W14). A frame whose CRC fails is a problem
`"crc mismatch (frame skipped)"` and is never decoded. A bad `frame_len` stops the walk — unless
`block_options::resync` is set: the windowed walker then scans forward 8 bytes at a time for a header with
`frame_flag_checkpoint` and `call_hash == checkpoint_sentinel`, reports the skipped range as one problem and
continues there (FR-T2). Per-frame inconsistencies are problem lines (at most `decode_limits::problems`, then a
count); a container that cannot be read at all is an error.

`tools::info(path)` (FR-T1) reads the header, follows the dictionary chain (`CHAIN.next_dictionary`, then each
dictionary frame's trailing `u64 next`) and the checkpoint chain (`CHAIN.first_checkpoint`, then each checkpoint's
`next`) with seeks — O(links), never O(frames) — merging every delta into `capture_info::dict`, reading the
writer-info frame when the header announces one, and checking the CRC of every frame it reads; a 1.6 file yields
its dictionary and no chains; a pipe is `unsupported`. `to_text` / `to_json` render it.

The limits (`limits.hpp`, A-7): `decode_limits{frame_bytes 64 MiB, dictionary_bytes 256 MiB, dictionary_entries
10⁶, image_depth 64, input_bytes 1 GiB, window_bytes 10 MiB, problems 1000}`, process-wide behind a
`constinit` mutex (`get_decode_limits` / `set_decode_limits`), read once per walk, overridable per call
(`block_options::limits`).

### 5.4 Dictionaries (`src/dictionary.cpp`, `dictionary.hpp`)

`dyn::dictionary{types, literals, calls (std::map by id), program_hash, abi, abi_known, abi_mixed, conflicts,
id_conflicts, conflicts_omitted}`. `add_entries(span)` parses a bare ENTRY stream: every on-wire count is checked
against the remaining bytes at a minimum element size before anything is allocated; FIELD geometry (offset + size
inside the type, `extent × element size` inside the field) is validated so the decoder never sees an inconsistent
descriptor; a transparent entry must be non-flat with one unnamed field at 0 of size 8; unknown flag bits are
refused. The first definition of a hash wins; a later different body is a `type_conflict` (summaries only: name,
hashes, size, align) or an `id_conflict` (literal or call), at most 64 kept, the rest counted. `add_rri` parses the
40-byte header (minor 6 or 7, `hash_algo` 2, `n_entries ≤ dictionary_entries`, the ABI tag — a second, different tag
sets `abi_mixed`) then the entries. `merge(other)` is the same rule across sources. `record_size(call_arg)` gives a
compact record's size (0 for literals and constants; `scalar_size` uses the writer's `wchar` width from the ABI
tag). `to_json()` / `to_toml()` are canonical, sorted renderings (FR-W5); `to_rri(d, flags)` re-encodes a parsed
dictionary (keeping the source's ABI tag) for `--rri-out`. The in-house parsers are bounded, fuzzed (T-4), and never
on the serialization path (C-6).

### 5.5 Formatters and rendering (`src/formatters.cpp`)

- `text_visitor` and `json_visitor` implement `decoded_value_visitor`: block structs one field per line, anything
  inside an array inline; enums by enumerator name (JSON `{"name":…,"value":…}`); chars quoted, bytes as `0xNN`;
  JSON integers beyond ±2⁵³ as quoted decimal strings; maps as `{k: v}` / `[{"key":…,"value":…}]`; transparent types
  as their value; `--hex` adds raw bytes; `--flatten` inlines base members.
- `render_frame(frame, d)` (FR-P4): each `{:spec}` of the call's format literal is formatted through `std::vformat`
  with the argument type chosen at run time from the record's category and kind (`render_arg`; a bad spec becomes
  `invalid_argument`, nothing escapes); classes render as their one-line text (`struct_inline_text`); `'\n'` is
  appended for `println`. For a log call, `decode_log` reads the envelope at the `log_layout` positions and
  `render_message` prefixes `<time> <LEVEL> <file>:<line> [tid] #seq` (time as `YYYY-MM-DD HH:MM:SS.nnnnnnnnn` for the
  system clock, raw ticks otherwise).
- `dump` runs two structural passes (the dictionary is printed first and is "as of the end", so pass 1 collects
  dictionary frames without decoding records; pass 2 decodes and streams one frame at a time through `write_fn`);
  decode errors are problems, not stops; `dump_options` carries format, filters (`--frames`, `--type`), `logs_only`,
  `min_level`, `since`/`until`.
- `render_value(d, ak, k, schema, payload, format)` renders one record payload — what `to_string` / `to_json` use;
  `process_dictionary()` is the process's own image parsed once per registry version under a shared mutex
  (`process_dictionary_cache`).

### 5.6 `to_string.hpp` (A-10)

`render(v, format)`, `to_string(v)`, `to_json(v)`: `detail::record_of(v)` writes the compact record
(`put_value_record` into an 8-padded vector), then `tools::render_value` over `tools::process_dictionary()`.
Instantiating it runs the same `touch<T>` as a serialize call (the type is registered and in the ELF section).
`formattable<T>` specialised as `std::true_type` enables a `std::formatter<T>` (`{:j}` = JSON). It allocates, takes a
shared lock and links `es::serreg_tools`: not for the hot path.

---

## 6. Wire format 1.7, byte by byte

Normative source: `include/es/serreg/format.hpp`. All integers little-endian, fixed width, read and written with
`memcpy`; every frame, record and ENTRY is a multiple of 8 bytes; all ids are 64-bit (P-1, FR-H8). `python/serreg/
format.py` restates these tables and is the Python reader's contract; the two must agree (D-5 names them the draft
of the 3.0 wire specification).

### 6.1 FILE header (48 bytes)

| Offset | Field | Value |
|---|---|---|
| 0 | `char[4]` magic | `"ESSR"` |
| 4 | `u16 major` | 1 |
| 6 | `u16 minor` | 7 (writers); readers accept 6 and 7 (P-14) |
| 8 | `u32 flags` | bit0 little-endian, bit1 dictionary present, bit2 back-patched (1.6 files only; never written since 1.7), bit3 `crc_all`, bit4 checkpoints, bit5 writer info |
| 12 | `u32 hash_algo` | 2 = XXH3-64 |
| 16 | `u64 program_hash` | `fingerprint64_u64s` of the sorted type schema hashes |
| 24 | `u64 dict_offset` | 48 |
| 32 | `u64 dict_len` | bytes of the opening RRI |
| 40 | `u64 abi_tag` | `abi_tag{version 1, endian, ptr_size, long_size, wchar_size, char_signed, max_align, long_double_size}` packed little-endian (P-2) |

### 6.2 Opening dictionary block and CHAIN (1.7)

```
dict_offset:  RRI (dict_len bytes)
              pad to 8
              CHAIN: u64 next_dictionary | u64 first_checkpoint      (16 bytes, written as zeros; each patched once)
              [writer-info frame]                                    (FILE flag bit5)
              FRAME × N
```

A 1.6 file has no CHAIN; when its flag bit2 is set the header points at a rewritten dictionary at the end and the
frames run from after the original dictionary area to `dict_offset`.

### 6.3 FRAME header (16 bytes)

| Offset | Field | Value |
|---|---|---|
| 0 | `u32 frame_len` | whole frame, header included, multiple of 8 |
| 4 | `u16 flags` | bit0 full records, bit1 dictionary frame, bit2 CRC32C trailer, bit3 checkpoint frame, bit4 writer-info frame; any other bit is `bad_version` |
| 6 | `u16 n_records` | every argument counts, literals and constants included; 0 for meta frames |
| 8 | `u64 call_hash` | `call_hash_v<Sig...>`, or a meta sentinel |

### 6.4 Records

| Category | Compact record (each starts 8-aligned, padded to 8) | Full record (flag bit0): `u64 hash \| u32 payload_len \| u16 layout (0 = native) \| u16 rsv \| payload`, pad 8 |
|---|---|---|
| flat type | the object's bytes (size from the dictionary), padding zeroed, bools 0/1 | hash = schema hash; payload = the bytes |
| dynamic type (incl. a container argument) | `u32 len \| u32 0 \| canonical image` | hash = schema hash; payload = the same |
| literal | **nothing** | hash = literal id; payload = `u64 id` |
| constant | **nothing** (value in the call ENTRY) | hash and payload of the scalar or enum it stands for |
| scalar | the bytes | hash = `fingerprint64(kind name)`; payload = the bytes |
| string | `u32 len \| bytes` | hash = `fingerprint64("string")`; payload = the same |

### 6.5 Canonical image (format 1.2+, P-5)

The record's image = the root's **fixed part** (members in declaration order, bases first, each at
`align_up(cursor, align)` of its *image* size and alignment, the part ending at `align_up(cursor, type align)`)
followed by **tail blocks** appended depth-first in member order, each aligned to its element alignment, gaps
zero. `type_desc.size/align` of a dynamic type are the fixed part's.

| Member | Image in the fixed part | Size, align |
|---|---|---|
| scalar, enum, flat class, array of flat | native bytes, scrubbed | native |
| string, vector, optional, variant, map, set | slot `{u32 off, u32 n}` | 8, 4 |
| nested dynamic class | its fixed part inline | its fixed size/align |
| `T[N]` / `std::array<T,N>` of a dynamic `T` | `N` element images at the element stride | `N × stride`, `T`'s align |

| Kind | `n` | Block at `off` |
|---|---|---|
| string | byte count | the bytes, no terminator |
| vector | element count | `n` element images, stride `align_up(elem size, elem align)`, then their tails |
| optional | 0 / 1 | one element image when 1 |
| variant | 0 valueless / `i + 1` | one image of alternative `i` |
| map | entry count | `n` entries ascending by key; an entry = key image at 0, value image at `align_up(key size, value align)`; stride = FIELD `extent` |
| set | key count | `n` key images ascending, stride = FIELD `extent` |

Slot rules (`fmt::image_budget`, applied by both decoders): `n == 0 ⇔ off == 0`; `n != 0 ⇒ off ≥ fixed` and
`[off, off + bytes) ⊆ image`; the sum of all blocks ≤ image size; `n × stride` must not wrap. Breaking one is
`length_mismatch`.

### 6.6 Dictionary frame (flag bit1)

```
u32 frame_len | u16 flags = 2 | u16 0 | u64 0x4553535244494354 ("ESSRDICT")
RRI (full, or a delta with rri_flag_delta) | pad to 8
u64 next_dictionary            (when the RRI's minor ≥ 7; 0 = none yet, patched once)
```

Written inline before the first data frame that needs it (files) or by `dictionary_tracker` (streams).

### 6.7 CRC32C trailer (flag bit2, P-10)

The last 8 bytes of the frame: `u32 crc32c(frame[0, frame_len − 8))` (Castagnoli, reflected polynomial
`0x82F63B78`, seed 0; `crc32c("123456789") == 0xE3069283`), `u32 0`. Checked before the frame is believed; a mismatch
skips the frame.

### 6.8 Checkpoint frame (flag bit3, P-11)

Header with `call_hash = 0x45535352434b5054 ("ESSRCKPT")`, `n_records = 0`, then the 64-byte fixed payload:

| Offset in payload | Field |
|---|---|
| 0 | `u16 version` = 1 |
| 2 | `u16 flags` — bit0: a full RRI follows |
| 4 | `u32 rri_len` |
| 8 | `u64 prev_checkpoint` (file offset; 0 for the first) |
| 16 | `u64 next_checkpoint` (patched once; 0 for the last) |
| 24 | `u64 data_frames_before` |
| 32 | `u64 data_bytes_before` |
| 40 | `u64 wall_clock_ns` |
| 48 | `u64 latest_dictionary_offset` |
| 56 | `u64 registry_version` |
| 64 | `[RRI]` when flag bit0 |

### 6.9 Writer-info frame (flag bit4, P-12)

Header with `call_hash = 0x45535352494e464f ("ESSRINFO")`, payload `u16 version = 1 | u16 n_items | ITEM × n`,
`ITEM = u16 key | u16 len | bytes | pad to 2`, text values. Keys (`fmt::info_key`): 1 library_version, 2 format,
3 program, 4 hostname, 5 pid, 6 start_wall_ns, 7 scrub_padding, 8 full_records, 9 canonical_unordered, 10 crc,
11 checkpoint_bytes, 12 checkpoint_ns, 13 dictionary (`full` | `scoped:<name>`), 14 sync, 15 clock, 16 thread,
17 abi. Readers skip unknown keys.

### 6.10 RRI header (40 bytes)

| Offset | Field | Value |
|---|---|---|
| 0 | `char[4]` | `"ESRG"` |
| 4 | `u16 major`, `u16 minor` | 1, 7 (readers: 6 or 7) |
| 8 | `u32 flags` | bit0 delta |
| 12 | `u32 hash_algo` | 2 |
| 16 | `u32 n_entries` | bounds the walk; ≤ `decode_limits::dictionary_entries` |
| 20 | `u64 program_hash` | |
| 28 | `u32 reserved` | 0 |
| 32 | `u64 abi_tag` | the writer's ABI |

followed by `ENTRY × n_entries`. The section `es_serreg_dict` is an ENTRY stream without this header.

### 6.11 ENTRY, FIELD, ARG

Header (8 bytes): `u32 total_len` (multiple of 8, zero padded) | `u8 tag` | `u8 version` = 1 | `u16 reserved`.

| Tag | Body |
|---|---|
| 1 struct | `u64 schema_hash \| u64 layout_hash \| u32 size \| u32 align \| u8 flags (bit0 flat, bit1 transparent) \| u8 rsv \| u16 since \| u16 deprecated \| u16 name_len \| u16 n_fields \| name \| FIELD × n_fields` |
| 2 enum | `u64 schema_hash \| u64 layout_hash \| u32 size \| u32 align \| u8 underlying_kind \| u8 flags (bit0 fixed) \| u16 since \| u16 deprecated \| u16 name_len \| u16 n_enumerators \| name \| ENUMERATOR × n`, `ENUMERATOR = i64 value \| u16 name_len \| name` |
| 3 literal | `u64 id \| u32 len \| u32 rsv \| bytes` (text at entry offset 24) |
| 4 call | `u64 call_hash \| u16 n_args \| u16 flags (bit0 print, bit1 newline, bit2 log) \| u32 rsv \| ARG × n_args` |

`FIELD = u16 name_len | u8 kind | u8 rsv | u32 offset | u32 size | u16 align | u16 since | u16 deprecated |
u16 n_elem | u64 type_hash | u32 extent | name | FIELD × n_elem`. `n_elem` is 1 for an array, vector, optional or
set element, 2 for a map (key at 0, value at its entry offset), `extent` alternatives for a variant (each named by
the alternative's canonical name with its schema hash, FR-W8); `extent` is the element count of an array, the
alternative count of a variant, the entry stride of a map or set. A base is the field `<base:Name>` of kind
`struct_`.

`ARG = u8 arg_kind (0 type, 1 literal, 2 scalar, 3 string, 4 constant) | u8 kind (scalar kind; a constant's kind, 0
for an enum) | u8 rsv[6] | u64 hash` (schema hash | literal id | `fingerprint64(kind name)` |
`fingerprint64("string")`); a constant's ARG is followed by `u64 value` — 24 bytes.

Kinds (`descriptor.hpp`): `i8` 1 … `byte_` 17, `enum_` 32, `array_` 33, `struct_` 34, `string_` 48, `vector_` 49,
`optional_` 50, `variant_` 51, `literal_` 52 (reserved), `map_` 53, `set_` 54.

### 6.12 Canonical strings (what the hashes hash)

```
schema:  struct<corpus::Order>[32,8]{id:u64@0[8,8];px:i64@8[8,8];qty:u32@16[4,4];side:enum<corpus::Side,u8>@20[1,1];sym:array<char,8>@21[8,1];}
layout:  struct[32,8]{u64@0[8,8];i64@8[8,8];u32@16[4,4];enum<u8>@20[1,1];array<char,8>@21[8,1];}
dynamic: dyn<corpus::DynMsg>[20,4]{id:u32@0[4,4];name:string@4[8,4];xs:vector<i16>@12[8,4];}
map/set: dyn<corpus::DynMap>[36,4]{…;by_name:map<string,i32>@4[8,4];ids:set<u16>@12[8,4];…}
transp.: dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}
call:    call(2){lit:0x198e4c6da0847235,0x367f8cd8c6441dd2}   print(4){lit:…,string,i32,lit:…}
const:   call(5){lit:0x…,const:i32=42,0x367f8cd8c6441dd2,const:0x074e267cca5188e1=2,const:f64=0xbff8000000000000}
log:     log(n){…}  (log_tag form; the envelope constants are ordinary items)
```

Schema strings carry names and `;sN;dN` (omitted when both 0); layout strings do not (P-6). A string change is a
format-minor bump with a CHANGELOG note (C-7).

---

## 7. Tools

### 7.1 `srdump` and `es::serreg::cli` (`src/cli.cpp`)

`cli::parse(argc, argv, options&, app)` (`--config <file.toml>` first — the FR-D9 TOML subset: tables, `key =
value`, strings, integers, booleans, arrays of strings; no dates, inline tables or dotted keys — then the flags,
explicit flags overriding), `cli::execute(options, app)` (dispatch to `tools::`), `cli::run(argc, argv, app = {})`.
`cli::app{name, version, extra_usage, on_extra}` lets an application embed the whole command line under its own
name and consume its own flags (`examples/embedded_cli.cpp`). Exit codes: 0, 1 (verify problems, decode or tool
error), 2 (usage).

| Group | Flags |
|---|---|
| sources | `<capture.ser>`, `--stdin`, `--elf <bin>` (repeatable, merged), `--rri <image>`, `--dict <rri>` (side-loaded for bare streams), `--rri-out <file>` |
| output | `--text` (default), `--json`, `--toml`, `--dict-only`, `--verbose` (dictionary blocks and dictionary-frame markers), `--raw` (print frames as records), `--hex`, `--full` (no array elision), `--flatten`, `--program-hash`, `--version` |
| selection | `--frames A..B`, `--type <name\|hash>`, `--level <lvl>`, `--since <t>`, `--until <t>` (ns or ISO date, UTC) |
| verification, recovery | `--verify`, `--info`, `--resync`, `--window <size>`, `--frame-limit <size>` |
| comparison | `--diff <a> <b>`, `--direction old-data\|new-data\|both\|none` |
| generation | `--wireshark <out.lua>`, `--cxx <types.hpp\|->`, `--cxx-router`, `--namespace old=new` |
| configuration | `--config <file.toml>`, `--help` |

Text mode prints the dictionary, then every data frame decoded; print and log frames as their rendered message;
`--level` prints bare log lines (`--json`: `{"frame":N,"log":{level,time,file,line,[thread],[sequence],message}}`).
`--verify` walks every frame and every record through the descriptor walker (`counting_visitor`), checks literal
ids against the call signature, dictionary completeness (every call hash defined by a dictionary that precedes its
first use), dictionary conflicts and the ABI tags, with O(types) state. The CLI never does what `tools::` cannot
(A-2).

### 7.2 `--diff` (`src/diff.cpp`, A-5)

`tools::diff(a, b, direction)` compares by canonical type name: `identical | names-only | metadata-only | breaking |
added | removed | duplicate-name`; calls and literals by id: `call/literal added | removed | changed`; enumerators as
`(name, value)` pairs (a value that disappears or changes is breaking). `diff_direction` decides whether removals
(`old_data`, the default: a reader built from `b` reads `a`'s captures) or additions (`new_data`) break; layout
changes, changed calls and changed literals break in every direction; `diff_report::breaking` drives the exit
status. `--compat` (the projection report) is R-9, 2.3.

### 7.3 Wireshark (`src/wireshark.cpp`, G-13, T-5)

`tools::wireshark_dissector(d, wireshark_options{generator, sources})` emits one Lua file: a generated data part —
one `ProtoField` per filterable value (`serreg.<type>.<field>`), the types, enumerators, call signatures, literal
texts and the source facts (format, program hash, ABI tag) — and a fixed `lua_runtime` per library version: a FILE
reader (`.ser`), USER0 pcaps, TCP/UDP ports, the frame and record walk, a descriptor-driven value walker applying the
slot rules of `src/decoder.cpp`, print and log rendering (`LEVEL file:line [tid] #seq` prefix), and a comparison of
a capture's own dictionary against the generated one by hash (unknown entries and layout differences are expert
infos). The heuristic recognises frames by known call hashes and the 1.7 meta sentinels. No C++ type is compiled in;
a frame the dissector does not know is raw bytes, never decoded with the wrong layout. Parity with `srdump --json` on
every fixture is tested under tshark and under plain Lua 5.2 and 5.4.

### 7.4 `--cxx` (`src/cxxgen.cpp`, T-7, FR-D10)

`tools::cxx_header(d, cxx_options{router, namespaces, include})` declares every enum and struct of the dictionary in
dependency order inside their namespaces: fixed-width scalars from kinds, `enum class E : <kind>`, `std::array`,
`std::string`, `std::vector`, `std::optional`, `std::variant`, every `map` shape as `std::map` and every `set` as
`std::set`, bases from `<base:…>`, `since`/`deprecated`, a flat type's gaps as `[[=es::serreg::skip]]` padding
members, `[[=es::serreg::name("…")]]` for any name a plain declaration cannot spell; the std vocabulary is spelled
back, never defined; `static_assert`s on size, alignment, offsets and schema hash close the file. `--cxx-router`
adds `install_handlers(frame_router&)` with one handler per call. The generated header is compiled and run in the
test suite (`tests/cxxgen`).

### 7.5 The Python reader (`python/serreg`, T-6, FR-T4)

Stdlib only, Python ≥ 3.10, four modules: `format.py` (constants and the byte tables of §6, `Cursor`,
`scalar_value`), `dictionary.py` (ENTRY/RRI parsing into `Type`, `Field`, `Literal`, `Call`, `CallArg`,
`Dictionary` with the same first-wins/conflict rule, the same minimum-size checks before allocation, a field depth
limit of 32), `reader.py` (`Reader`: the FILE/stream walk, 1.6 back-patch and 1.7 CHAIN/deltas/CRC/checkpoints/
writer info, compact and full records, `Decoder` for the canonical image under the slot rules, plain Python
values — `Struct`, `Enum`, `Array`, `Str`, `Variant`, `Map`, `Set`, `Transparent`, `Null`, `Lit` — `record_json` /
`record_text`, `render_message` with a `std::format` subset (`cxx_format`), `decode_log`, `capture_info`,
`VerifyReport`), and `__main__.py` (`python3 -m serreg <file.ser> [--json|--text] [--verify] [--info] [--level LVL]
[--dict x.rri] [--rri] [--dict-only] [--hex]`, exit codes 0/1/2). It never raises on corrupt input: structural
problems go to `Reader.problems`, a record that does not decode carries `error`. Not covered: XXH3 recomputation,
`--elf`, `--diff`, `--resync`, pipes. `tests/python/parity.py` (ctest `python_reader_parity`) compares it
semantically with `srdump --json` over every golden and corpus fixture and a generated log capture.

### 7.6 `file(1)`

`share/serreg.magic` recognises `.ser` (`"ESSR"`) and `.rri` (`"ESRG"`) files.

---

## 8. Registry cost, and how to reduce it

### 8.1 What each phase does

| Phase | Work | Where |
|---|---|---|
| push (per registration, at image load) | `dladdr` + `main_image()` outside the mutex; one mutex; one `unordered_map` lookup; for the main program a view (no copy); for any other image a heap copy of blob and name and a deep copy of the descriptor | `registry::push`, `make_entry` |
| publish (once per burst, at the first query) | one `registry_index`: a vector of views, one sort by `(nk, hash)`, four `flat_map`s adopted from sorted vectors, the conflict list | `publish_locked` |
| lookup | one acquire load + one `flat_map` binary search | `registry_index::find` |
| export | header + concatenation of every blob (full or `added > since`); a scoped export first parses the full image into a `dyn::dictionary` and computes the closure | `export_image_since`, `export_image_scope` |
| parse (reader side) | `dyn::dictionary::add_rri`: one pass with per-element minimum-size checks and geometry validation | `src/dictionary.cpp` |

### 8.2 Measured (`tests/bench/registry_measure`, G-5, FR-R13)

`registry_measure [--entries N] [--bytes B] [--assert] [--plugin] [--elf <bin>] [--json]` synthesises N struct
entries whose blobs add up to about B bytes, pushes them exactly as guards do (descriptors in static pools of this
executable's image for the main-program path; on the heap with `--plugin` for the deep-copy path) and reports push,
publish, lookup, export, parse and RSS growth. `--assert` enforces the budgets in release builds; the ctests
`registry_measure_10k`, `registry_measure_100k`, `registry_measure_elf` carry the label `bench`.

Sandbox numbers (2 vCPU VM, GCC 16 `-O2`; reference-box numbers are the ones that count, T-9):

| Entries / blob bytes | push | publish | lookup | export | parse | RSS |
|---|---|---|---|---|---|---|
| 10 000 / 10 MB | 7.7 ms | 1.9 ms | 100 ns | 14 ms | 12 ms | — |
| 100 000 / 100 MB | 111 ms | 29 ms | 130 ns | 117 ms | 139 ms | +546 MB incl. the measurement's own copies |

Budgets: 10 ms for 10k and 100 ms for 100k (push + publish, release, reference box). Main-image views (C-11) halved
push time and removed one blob-size of RSS relative to copying.

### 8.3 Options a program with many types has

- **Fewer, larger message types.** Every type, literal, call signature and scope is one entry; an aggregate with
  ten members is one ENTRY where ten calls with one argument each are ten calls plus their literals.
- **Scoped capture files** (`scoped_capture_file<Scope>`, a logger over a scoped sink): the opening dictionary and
  the deltas hold only the scope's closure, so a log file carries its forty format strings, not the program's four
  thousand archive types (A-9).
- **`capture_policy{.dictionary_updates = false}`** when nothing is `dlopen`ed: no per-frame version compare, no
  chain machinery.
- **`checkpoint_dictionary = false`** (the default): checkpoints stay 64 bytes; the chain and `latest_dictionary_offset`
  find the dictionary.
- **Main-image views** (C-11) are automatic: put the message types in the executable, or accept the deep copy for
  plugin types (`--plugin` measures it).
- **Register only what is used.** Registration follows instantiation; a header full of types registers nothing
  until a `serialize`, `deserialize`, `dispatch`, router handler, `to_string` or `register_types` names them; a
  compile-time log floor (`ES_SERREG_LOG_MIN_LEVEL`, `log_options::floor`) removes the compiled-out calls'
  registrations entirely.
- **Keep literals and call sites moderate.** Each distinct format string and each distinct argument list is an
  entry; a logger's `thread`/`sequence` policy change is a new signature for every format.
- **Read the ELF section offline.** `srdump --elf` reads a binary's schema without running it; the program never
  parses its own section. Reader-side cost is `parse`, paid once per dictionary by tools, and once per registry
  version by `process_dictionary()` (only when `to_string`/`render` is used).
- **Prefer in-process rendering only off the hot path**: `to_string` takes a shared lock and allocates (A-10).

Open 3.0 items: **A-13 index reclamation** — `snapshot()` returns a plain reference, so every published generation
lives until exit (about 80 bytes per entry per publish; a reload-heavy program publishes one per `dlopen`/`dlclose`);
a `snapshot_guard` with epoch reclamation is planned beside `snapshot()`. **A-14 `forget_retained()`** — retained
entries stay in `export_image()` so old frames remain readable; a scoped export already omits what a scope does not
reach.

---

## 9. Build, packaging and conventions

### 9.1 CMake

`cmake_minimum_required(VERSION 4.0)`, `project(serreg VERSION 2.2.6)`. GCC ≥ 16 is required for the reflection
targets (`-std=c++26 -freflection`; clang-p2996 takes `-freflection-latest`); `ES_SERREG_IPC_ONLY` stops before the
compiler check.

| Option | Default | Effect |
|---|---|---|
| `ES_SERREG_BUILD_SHARED` | ON | `libserreg.so` / `libserreg_tools.so` (needed when `dlopen`ed DSOs serialise, FR-R3); OFF: static archives |
| `ES_SERREG_BUILD_TOOLS` | ON | `es::serreg_tools` and `srdump` |
| `ES_SERREG_BUILD_TESTS` / `_EXAMPLES` | top-level only | `tests/`, `examples/` |
| `ES_SERREG_WERROR` | ON | `-Werror` on every target (C-4) |
| `ES_SERREG_IMAGE_FORMAT` | `elf` | `none`: `image_none.cpp`, `ES_SERREG_EMBED_NONE`, no `-ldl`, no `-fno-gnu-unique`; refused with shared libraries |
| `ES_SERREG_IPC_ONLY` | OFF | configure and install `es::ipc` alone |
| `ES_SERREG_CATCH2_DIR` | — | a vendored Catch2 v3 for offline builds (test-only dependency, C-6) |

Targets: `es::serreg` (`ES_SERREG_SHARED` PUBLIC define when shared, `-fno-gnu-unique` PUBLIC on GCC/ELF,
`VERSION 2.2.6`, `SOVERSION 2.2`), `es::serreg_tools` (same SONAME rule), `es::ipc`, `srdump`.
Warnings (`cmake/warnings.cmake`, `es_serreg_apply_warnings`): `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wformat=2
-Wimplicit-fallthrough -Wmisleading-indentation`, plus `-Wduplicated-cond -Wduplicated-branches -Wlogical-op` on GCC;
not `-Wnull-dereference` (false positives at `-O2` on `std::expected::error()`).

Presets (`CMakePresets.json`, Ninja, `build/<preset>/`): `gcc16-debug` (`-D_GLIBCXX_ASSERTIONS`), `opt-gcc16-debug`
(`/opt/gcc-16/bin/g++-16`), `gcc16-release`, `gcc16-asan-ubsan`, `gcc16-static`, `gcc16-static-none`,
`gcc16-ubsan-trap`, `gcc16-tsan`, `clang-p2996` (`-stdlib=libc++`); matching build and test presets.

### 9.2 Install and consumption

`cmake --install` installs the three libraries, `srdump`, the headers under `include/es/{serreg,ipc}`,
`serregConfig.cmake` + `serregConfigVersion.cmake` (`COMPATIBILITY SameMinorVersion`: `find_package(serreg 2.2)`
accepts 2.2.x only, G-2, FR-V2), two export sets (`serregTargets`, `serregIpcTargets` for `COMPONENTS ipc`),
pkg-config files `serreg.pc` (carrying `-fno-gnu-unique` or `-DES_SERREG_EMBED_NONE`), `serreg-tools.pc`, `es-ipc.pc`,
and `share/serreg/serreg.magic`. Installed targets are linked with their final `$ORIGIN`-relative RPATH, so install
never relinks. `ports/serreg` is a vcpkg overlay port with a `gcc16` triplet; `FetchContent`/`add_subdirectory`
work because the targets carry `cxx_std_26`, PIC and the reflection flag themselves.

### 9.3 Versioning (G-2, P-14, P-20)

SONAME `major.minor` (`libserreg.so.2.2`; ctest `soname_major_minor`); the minor is the compatibility unit until
3.0 (a wire-format or ABI change bumps it; format 1.7 shipping in a patch release is the recorded exception, FR-V4);
`fmt::format_minor` 7, `format_minor_oldest` 6. Versions change only through `scripts/bump-version.sh` (it updates
`CMakeLists.txt`, `version.hpp`, the port and the Python package together). Deliverables are
`ser-reg-project-vX.Y.Z.tar.gz` with top-level `ser-reg-project-vX.Y.Z/`, built by `scripts/make-release.sh`
(refuses to overwrite, requires the CHANGELOG entry, excludes build/log/stray capture files); `--public` builds the
subset named by `public-files-list.txt` (every new file classified as a plain line, `#private:` or `#decide:`;
symbolic links kept; a public link to an unpublished file fails) (C-8).

### 9.4 Conventions the code follows

- `const` parameters by default; `std::expected<T, error>` (`result<T>`, `status`), `noexcept` where true; no
  exceptions, no RTTI; every mutable namespace-scope object `constinit`; nothing on the hot path allocates or locks
  (A-1, C-5).
- Output through `std::print`/`std::println`; library diagnostics through `detail::report_line`; every `main()` only
  parses the command line and runs an application class (C-1).
- Includes: package headers before third-party and system headers, every header self-contained with `#pragma
  once`; enforced by `.clang-format` (`IncludeBlocks: Regroup`), `tests/headers` (one generated TU per header, included
  twice) and `scripts/check-includes.py` (C-2). clang-format 22 is the reference; the tree is clean under 18–23;
  `scripts/format-all.sh` before every commit, `scripts/install-hooks.sh` installs the pre-commit hook (C-3).
- Byte-level format changes refreeze `tests/golden/blobs.cpp`, the golden fixtures (`orders.*`, `dynamic.*`,
  `features.ser`) and `tests/canary/` together with a `format_minor` bump and a CHANGELOG line (C-7).
- The book (`book/`, one chapter per topic and one page per source file) is kept in step by `scripts/check-book.py`
  (D-3); `scripts/run-all-tests.sh` is the whole verification protocol with logs under `logs/` (T-1).
- Scripts: `run-all-tests.sh`, `make-release.sh`, `bump-version.sh`, `check-book.py`, `check-includes.py`,
  `format-all.sh`, `install-hooks.sh`, `pre-commit`, `amalgamate-golden.py` (the Compiler Explorer golden check),
  `bench-hrtt-*.sh` (the `es::ipc` half-RTT benchmarks on the reference box).

### 9.5 `es::ipc` in one paragraph (G-4)

Header-only C++23 under `include/es/ipc/`: `region` (named shared memory on tmpfs or hugetlbfs — accepted only
there, checked with `fstatfs` — or an anonymous mapping; normal, 2 MiB or 1 GiB pages, `MAP_POPULATE`, optional
`mlock`), `ring` (`basic_ring<Unit, Align, DataAlign>`, layout 2, `ring_version` 2: header, cursor, producer word,
tickets, up to 256 gates, then records `{u32 size, u32 seq, u32 type, u32 check}` + payload in units of 16/32/64
bytes, every word accessed through `std::atomic_ref`, no pointers in shared memory), `producer` (`claim`/`commit`
in place; `es::ipc::sink` is a `claiming_sink` so `serialize` writes frames straight into records), `consumer`
(`peek`/`poll`/`message` in place; `ring_kind::spmc` broadcast gated by the slowest active consumer, `ring_kind::spsc`
queue that keeps messages while no consumer is attached; dead consumers reaped, a dead producer's role taken over).
Every header read from the ring is validated by its own never-crash rules before a span is formed. Design and
measurements: `docs/ipc/`.

---

## 10. Planned architecture changes

### 10.1 2.2.x (format 1.7, no byte changes)

A-16: the dictionary-driven enum range check moves into `src/decoder.cpp` (the `fixed` flag is already in every
enum ENTRY and `dyn::type::enum_fixed`), so `verify`, `walk_values` and `srdump` report `invalid_value` where the
typed readers already do; `std::list`/`std::deque` as the `vector` shape (a `dyn_member` trait each in
`reflect.hpp`); `serialize_options::canonical_unordered = false` to skip the sort for writers that do not need byte
identity (the writer-info key 9 exists for it).

### 10.2 2.3.0 — evolution level 1 (R-9, G-15)

A **projection engine** between the dictionary and the typed readers: given the writer's `dyn::type` (from the
capture's dictionary) and the reader's `descriptor_v<T>`, build once per `(schema hash, T)` a projection — member
by name, widening scalar conversions (`i16 → i32`, `f32 → f64`, unsigned → strictly wider signed), arrays that grew,
`[[=es::serreg::renamed_from("old")]]` on members and classes, variant alternatives matched by canonical name, nested
types projected by structure when the member name matches, enums by underlying kind — and apply it in `get_record`
/ `decode_value` when the layout hash differs. It lives in the runtime library beside the typed readers
(`deserialize.hpp` for the typed copy, a new source under `src/` for the cached projections keyed by the capture's
dictionary), with `dispatch` and `frame_router` taking a projection the same way; the equal-layout-hash case keeps
the `memcpy` fast path. `projection_report` and `srdump --diff --compat` render what a projection would do; a renamed
nested type is reported, not blocking; narrowing is never performed. No wire change; `since`/`deprecated` gain their
meaning. Also 2.3.0: **A-15 hidden visibility** for `libserreg` (`-fvisibility=hidden`, an exported-symbol list, a
ctest that checks `nm` against it).

### 10.3 3.0 — the frozen public contract

- **P-18 portable wire mode** beside the native one: fixed-width little-endian scalars, declared widths for `long`
  and `wchar_t`, canonical offsets for flat types; selected per sink or per call, flagged in the headers; the
  `memcpy` fast path stays when the ABI tags match. The place for it is a second `put_object`/`get_object` pair
  driven by the gap and leaf tables that already exist, and a layout id other than 0 in full records.
- **P-19 additive-only format** after 3.0: readers skip unknown frame flags, ENTRY tags, ARG kinds and info keys
  instead of refusing them (today unknown frame flags are `bad_version`), which is why the headers reserve room
  now. **D-5** a written wire specification grown from §6 and `format.py`.
- **A-13 index reclamation** (`snapshot_guard`, epoch-based; `snapshot()` kept), **A-14 `forget_retained()`**,
  retiring `es::ipc_old`, and the open items G-6 to G-11 (Windows/macOS/aarch64 through the image seam of §3.8 —
  a PE backend is a new `image_pe.cpp` and `embed_pe.hpp`, nothing in `registry.cpp` changes; license; neutral
  layer names `fingerprint`/`serialize`/`registry`/`tools`/`ipc`; provenance; the public repository).
