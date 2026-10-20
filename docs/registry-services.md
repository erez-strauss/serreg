# Registry services — what the type registry provides, how it is fed, and how it meets the ELF image and the RRI

> **Companion:** `serreg-registry.md` is the per-function reference of `registry.hpp` / `registry.cpp` (one
> paragraph each, with file:line). This document explains the mechanisms and the formats; that one is the lookup
> table.

The registry is the half of `es::serreg` that lives at run time: the process-wide table of every type, literal
and call signature the program can serialize, built *before `main()`* from the compile-time descriptors
(`reflection-services.md`), queryable without locks, exportable as a byte image — the **RRI** — that is the
dictionary of every capture file and the payload of the ELF section `es_serreg_dict`. This document is ordered
so that each section uses only what came before it: the unit of information (the ENTRY), how it reaches the
registry (guards and `touch`), what the registry does with it (storage, publication, conflicts, unload), the
services it exposes, the two byte formats it reads and writes (RRI, ELF section), and the tools built on them.

Code: `include/es/serreg/registry.hpp` (the public class, the registrars, `touch`), `src/registry.cpp` (storage,
index, export, hooks), `include/es/serreg/detail/embed_elf.hpp` (the section), `src/image_elf.cpp` (image
identity), `src/elf_reader.cpp` (reading the section back from a file), `include/es/serreg/format.hpp` (the byte
layouts), `src/dictionary.cpp` (parsing an RRI into `dyn::dictionary`). `serreg-registry.md` has one paragraph
per function with line numbers.

---

## 1. The unit: an ENTRY

Everything the registry stores, exports or reads is a sequence of **ENTRY** blobs. One ENTRY describes one of
four things; its bytes are produced at compile time by `detail::type_blob_bytes<T>()`, `literal_blob_bytes<S>()`
and `call_blob_bytes<Ts…>()` from the reflected descriptor, and are the same bytes wherever they appear
(`.rodata`, the ELF section, an RRI, a dictionary frame).

```
ENTRY header (8 bytes)
  u32 total_len      whole entry incl. header, multiple of 8, zero-padded
  u8  tag            1 struct  2 enum  3 literal  4 call
  u8  version        1
  u16 reserved       0

tag 1 struct:   u64 schema_hash, u64 layout_hash, u32 size, u32 align,
                u8 flags (bit0 is_flat, bit1 transparent), u8 rsv, u16 since, u16 deprecated,
                u16 name_len, u16 n_fields, name bytes, FIELD × n_fields
tag 2 enum:     u64 schema_hash, u64 layout_hash, u32 size, u32 align,
                u8 underlying_kind, u8 flags (bit0 fixed underlying type), u16 since, u16 deprecated,
                u16 name_len, u16 n_enumerators, name bytes, ENUMERATOR × n
                ENUMERATOR = i64 value, u16 name_len, name bytes
tag 3 literal:  u64 id, u32 len, u32 rsv, text bytes
tag 4 call:     u64 call_hash, u16 n_args, u16 flags (bit0 print, bit1 newline, bit2 log), u32 rsv, ARG × n
                ARG = u8 arg_kind (0 type, 1 literal, 2 scalar, 3 string, 4 constant), u8 scalar kind,
                      u8 rsv[6], u64 hash  [+ u64 value when arg_kind == 4]
FIELD:          u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16 since, u16 deprecated,
                u16 n_elem, u64 type_hash, u32 extent, name bytes, FIELD × n_elem (element descriptors)
```

All integers are little-endian and written with `memcpy`. The *hash* fields are the identities: a struct or
enum is keyed by its **schema hash** (XXH3-64 of the schema string — names, kinds, offsets, sizes, enumerators),
a literal by its **id** (XXH3-64 of its text), a call by its **call hash** (XXH3-64 of the call string
`call(lit,ns::Order,u32)`). A `FIELD` whose kind is `struct`, `enum`, `vector`, `optional`, `variant`, `map`,
`set` points at another ENTRY through `type_hash`; a dictionary is therefore a graph of ENTRYs keyed by hash,
and the order of ENTRYs in a stream does not matter.

Readers of ENTRY streams follow two forward-compatibility rules (`dictionary::add_entries`): an unknown
`version` byte is skipped, and an unknown `tag` is skipped (the `total_len` makes that possible without
understanding the body). A later format minor can add a tag — constraint records, `renamed_from` metadata — and a
current reader will read everything else.

## 2. How an ENTRY reaches the registry: nodes, guards, `touch`

### 2.1 The node

For every registered thing, each image (the main program or a shared object) that uses it holds one
constant-initialised `reg_node` in its `.data`:

```cpp
struct reg_node {
    const void* payload;              // the type_desc* / call_desc* of this image (nullptr for a literal)
    std::span<const std::byte> blob;  // the ENTRY bytes in this image's .rodata
    std::uint64_t hash;               // schema hash | literal id | call hash
    std::string_view name;            // canonical name | literal text | "call(n){…}"
    node_kind nk;                     // type, literal, call, scope
    void* entry; const void* image_base; std::uint32_t refs; bool linked;   // set by the registry
};
```

The node is a *view of the image*: nothing in it is allocated.

### 2.2 The guard: registration by static initialisation

`registrar<T>::g` is a `const guard` with static storage duration whose constructor calls
`registry::instance().push(&node)` and whose destructor calls `on_guard_destroyed(&node)`. The template is
never instantiated by itself: it is **odr-used** — `static_cast<void>(&registrar<T>::g)` — from the body of
`serialize`, `deserialize`, `dispatch`, the router, `register_types<T>()`. An odr-use of a static data member of
a class template instantiates its definition, and a static with dynamic initialisation runs its constructor
during the static initialisation of the image that instantiated it: **before `main()`** for the executable,
**inside `dlopen()`** for a plugin. There is no registration call at run time and no call-site cost: the
`static_cast<void>(&…)` compiles to nothing.

Three registrar flavours exist: `registrar<T>` (a type), `registrar_lit<S>` (a literal), `registrar_call<Ts…>`
(a call signature); a fourth, `registrar_scoped<Scope, Ts…>`, records that a call signature was serialized into
a sink with a *dictionary scope* (§4.6).

### 2.2a The idiom in general: odr-use as a registration tool

The mechanism of §2.2 is not specific to serialization; it is a general way to build a **self-populating
registry of anything a program instantiates** — handlers, codecs, plugins, metrics, test cases — with no
registration call and no central list. The parts, and why each is there:

```cpp
template<class T> struct registrar {
    static reg_node node;              // constant-initialised: a view of static data, nothing to run
    static const guard g;              // dynamic initialisation: its constructor registers `node`
};
template<class T> reg_node registrar<T>::node{ &descriptor_v<T>, blob_v<T>, hash_v<T>, … };
template<class T> const guard registrar<T>::g{&registrar<T>::node};

template<class T> void use() { static_cast<void>(&registrar<T>::g); }   // the odr-use: zero code
```

1. **A static data member of a class template is instantiated only when odr-used** ([temp.inst]); taking its
   address in a function that is itself instantiated for `T` is the cheapest odr-use there is and compiles to
   nothing. So "the program serializes `T` somewhere" and "`registrar<T>::g` exists" become the same fact.
2. **A variable with dynamic initialisation runs its initialiser during the static initialisation of its
   image** — before `main()` for the executable, inside `dlopen()` for a shared object ([basic.start.dynamic]).
   That is what makes the registry complete before the first line of user code, and what lets a plugin add its
   entries when it loads and drop them when it unloads (the destructor).
3. **The node is constant-initialised** (`constinit`-able: pointers to `constexpr` data and integers), so there is
   no ordering problem between the node and the guard that points at it, and no ordering problem between two
   registrars — each one only touches the registry object, which is itself constant-initialised (`constexpr`
   constructor, `constinit` state) and therefore usable from any initialiser in any order.
4. **The payload is a template-dependent constant** (`descriptor_v<T>`, the ENTRY bytes): the compiler produces
   the description, the guard only files a pointer to it. The registry never computes anything per type at run
   time.

The idiom's failure modes, each of which the library had to meet:

- **Static libraries drop unreferenced objects.** The linker pulls an object file out of a `.a` only when
  something references a symbol in it. A registrar in a TU that nothing else references never links. The odr-use
  from `serialize()` avoids this for the serializer (the TU that serializes `T` is the TU that references
  `g`), but a "register by merely compiling this file" design does not survive a static library without
  `--whole-archive` or an explicit reference. `register_types<T>()` exists for exactly the case where no call
  references the type.
- **`--gc-sections` and LTO remove unreferenced data.** The guard survives (it has a constructor the init array
  references); plain data placed in a custom section does not — hence `[[gnu::used, gnu::retain]]` on the
  `es_serreg_dict` copies and binutils ≥ 2.36 (`SHF_GNU_RETAIN`).
- **COMDAT and custom sections.** An inline/template variable is COMDAT: one definition is kept per program.
  A COMDAT variable that names a section drags the *whole group* into that section's first-seen TU and breaks
  multi-TU links on GCC; the section copies are therefore TU-local (`static`) and deduplicated by content
  (hash) rather than by the linker.
- **One registry per process, but possibly several copies of the library.** Two copies of `libserreg` (static
  into a plugin and shared in the host) would each have a registry. The library exports one marker function to
  detect that; the fix is the shared library.
- **`STB_GNU_UNIQUE`.** GCC marks inline/template statics `GNU_UNIQUE` by default; glibc then pins any DSO that
  defines one (`NODELETE`), so the plugin can never unload. `-fno-gnu-unique` is required for the destructor half
  of the idiom to mean anything.
- **Destructors run at exit and at `dlclose()`, indistinguishably.** The idiom cannot tell the two apart (and
  need not: the registry keeps copies and retains entries, §3.3). A design that frees registry memory in the
  destructor would race the exit sequence.
- **Hidden visibility and the ODR.** `registrar<T>` is `ES_SERREG_HIDDEN`: each image gets its own node and
  guard for `T`, which is what the per-image reference count wants. With default visibility the dynamic linker
  would merge them across images and a plugin's unload could remove the host's registration.
- **Instantiation is per TU.** Ten TUs that serialize `Order` each define `registrar<Order>::g` (COMDAT folds
  them to one), but each TU that *embeds* the ENTRY has its own section copy; that is the price of the
  linker-independent section (bytes, not correctness).

Used with those rules, the idiom gives a program a table of exactly the things it uses, filled by the compiler
and linker, visible before `main()`, extended by plugins, and inspectable offline — which is what this registry
is.

### 2.3 `touch<T>()`: the closure of a type

Registering `Order` must also register `Side` (its enum member), `Px` (its nested struct), the element type of
its `std::vector<Fill>`, each alternative of its `std::variant`, the key and value of its `std::map`. `touch<T>()`
is a compile-time walk over the *parts* of `T` (`part_t<T, I>`, `base_t<T, B>`, the `dyn_member` traits) that
odr-uses the registrar of every component type reachable from `T`; `touch_arg` does the same for a call's
arguments. Because the walk is a fold of empty inline functions it has no run-time body; its effect is which
guards exist in the image.

### 2.4 The ENTRY's two homes in the image

The same `std::array<std::byte, N>` of ENTRY bytes is emitted twice per TU that instantiates it:

- `type_blob_bytes_v<T>` in `.rodata`, pointed at by `descriptor_v<T>.blob` and by the node — what `push()`
  copies and what `export_image()` writes;
- `type_blob_copy_v<T>` in the section **`es_serreg_dict`** (`[[gnu::section("es_serreg_dict"), gnu::used,
  gnu::retain]]`, 8-aligned, internal linkage) — what an *offline* reader finds in the file (§6).

The second copy is TU-local on purpose: GCC merges every COMDAT variable naming a section into one section
carrying the group of whichever TU came first, which breaks multi-TU links; TU-local copies avoid it, and
duplicates across TUs cost bytes but no correctness because readers key by hash. `gnu::used` + `gnu::retain`
(binutils ≥ 2.36) keep the copies alive under `--gc-sections` although no code references them.

## 3. What the registry does with a node

### 3.1 Storage: heap entries, one per (kind, hash)

`push()` (under one mutex; the loader lock is never taken while it is held) looks the node's key up in
`by_key`. One **entry** per logical definition is allocated on the heap and never freed: it owns a copy of the
ENTRY bytes and the name, and — for every image other than the main program — a deep copy of the descriptor
(fields, element descriptors, enumerators, gap/bool tables, call arguments), so that the registry *never reads an
image's memory after that image's guard is destroyed*. The main program's descriptors are viewed in place: they
live as long as the process.

The four outcomes of a push: a **new** entry; an **alias** (another TU or image brought the same definition —
`refs` grows); a **revival** (same key and layout as a retained entry whose images had unloaded); a **conflict**.

### 3.2 Conflicts (FR-R4, DECISIONS G-16)

Same schema hash, different layout hash, both definitions loaded: the first stays the registry's entry, the
second is recorded in `conflicts` and reported once (`ES_SERREG_CONFLICT=warn` by default; `abort` ends the
process with the diagnostic text of spec §11). When the kept definition unloads, a still-loaded conflicting
definition takes its place. Readers can ask `has_conflict(hash)`; `srdump --verify` lists conflicts of a file's
dictionary; the Catch2 test `serreg_conflict` injects one.

### 3.3 Unload and retention (FR-R10/R12)

A guard destructor runs at `dlclose()` of its image and at process exit; it only drops the reference. C++
cannot tell the two cases apart and the registry does not need to: an entry whose references reach zero is
**retained** — lookups still find it, pointers into it stay valid, frames the unloaded plugin wrote still decode,
and `export_image()` still carries it (DECISIONS A-14), so a capture that outlives a plugin is still
self-describing. A reload revives the entry; a changed type has a new schema hash and is a new entry.

Image identity (`src/image_elf.cpp`) comes from `dladdr` on the node's payload (`dli_fbase` is the image base),
with the main program's base computed once through `dl_iterate_phdr` (load bias + lowest `PT_LOAD` vaddr).
`can_unload()` probes whether a base belongs to an image glibc can unload: not the main program, and not a DSO
marked `DF_1_NODELETE` (which is also why the library builds with `-fno-gnu-unique`: an `STB_GNU_UNIQUE` symbol
makes glibc pin the DSO, and plugin reloading needs real unloads).

### 3.4 Publication: a lock-free index

A change (push, unload, conflict) only marks the registry stale and bumps `version_`. The **next reader** builds
one immutable `registry_index` — a vector of `registry_entry` views sorted by (kind, hash) plus one
`std::flat_map<hash, position>` per kind — publishes it with a release store, and runs the `on_change` callbacks
once for that generation, outside the mutex. Every lookup (`find_type`, `find_call`, `find_literal`, the
decoders' `registry::instance().find…`) acquire-loads the current index and binary-searches it: no lock, no
allocation, and an index is never freed before process exit, so a `snapshot()` reference stays valid for the
life of the process (DECISIONS A-13). A burst of `dlopen`s costs one index build at the first read after it,
not one per registration.

## 4. The services

### 4.1 Lookup

| Service | Returns | Used by |
|---|---|---|
| `find_type(schema_hash)` | `const type_desc*` (fields, enumerators, scrub tables, canonical name, blob) | the typed readers (`view<T>`, `dispatch`, `frame_router`), `inspect`, the projection planner, `--elf` |
| `find_type_by_name(name)` | the same by canonical name | tools, the diff |
| `find_call(call_hash)` | `const call_desc*` (argument kinds and hashes, flags) | `inspect` of a compact frame (the record layout comes from the call), the router, `walk_values` |
| `find_literal(id)` | the literal's text (a view into the ENTRY) | rendering print/log frames |
| `snapshot()`, `size()`, `version()`, `generations()`, `conflicts()`, `images()`, `retained()` | the published index and its statistics | tests, `registry_measure`, diagnostics |

### 4.2 Export: the registry as bytes

| Service | What it writes |
|---|---|
| `export_image()` | a full **RRI** (§5): header + every ENTRY (loaded and retained), `program_hash` over the loaded types |
| `export_image_since(version)` | a **delta RRI** (`flags` bit0): only the ENTRYs whose `added` version is newer — what a capture's dictionary-delta frame carries after a plugin loaded |
| `export_image_scope(scope_hash, since)` | the RRI of one *dictionary scope* (§4.6): the calls that scope carries, their types and literals, nothing else |
| `export_image(fd)` / `export_image(path)` | the full RRI to a descriptor or a file (`.rri`) |
| `to_json()`, `to_toml()` | the same content rendered for humans and other tools |
| `program_hash()` | XXH3-64 over the sorted schema hashes of the loaded types (FR-H3): the identity of "this program's type set", stamped into every FILE and RRI header |

### 4.3 Notification

`on_change(callback) → id`, `remove_on_change(id)`: run once per published generation, outside the mutex.
`capture_file` uses it to learn that a plugin brought new types, so the next frame it writes is a dictionary
delta (the mechanism `examples/dictionary_deltas/` shows byte by byte).

### 4.4 Registration without a call site

`register_types<Ts…>()` and `register_call<Sig…>()` odr-use the registrars of the named types / signature from
any TU, for programs that only *read* a type (a plugin host, a `srdump`-like tool with compiled types) — so the
type is in `find_type`, in this binary's `es_serreg_dict`, and in `export_image()`. Empty at run time.

A whole namespace at once (A-22, `register_namespace.hpp`): `ES_SERREG_REGISTER_NAMESPACE(app::messages);` walks
the namespace with `std::meta::members_of` (nested namespaces included), keeps the complete class and enum types
a selector accepts — all of them by default; `name_prefix<"Msg">{}`, `name_glob<"*Request">{}`,
`qualified_glob<"app::*::v2::*">{}`, `annotated<message_t>{}` for `struct [[=message]] T`, composable with
`all_of`/`any_of`/`not_` — and odr-uses `registrar<T>::g` through the same `detail::touch<T>()` for every one the
library can serialise (`detail::supported_type<T>`, the predicate form of `check_supported()`; a helper with a
pointer member is listed but skipped, a `[[=skip]]` type is neither). `selected_type_names<^^ns, Select>()` is the
selection as compile-time strings, so a program can print the whole namespace once and then narrow it;
`registered_type_count<^^ns, Select>` is the number that will register, for a `static_assert` next to the macro.
The walk sees what is declared before the point of instantiation in that translation unit: one line, after the
message headers, once per program.

### 4.5 Process hooks (FR-R9, §6.11)

Read once at static initialisation of `libserreg`:

| Variable | Effect |
|---|---|
| `ES_SERREG_DUMP_REGISTRY=path` / `ES_SERREG_DUMP_REGISTRY_FD=n` | write the registry at exit (`atexit`) to a file / descriptor |
| `ES_SERREG_DUMP_FORMAT=rri\|json\|toml` | the format of that dump (default RRI) |
| `ES_SERREG_DUMP_ON=…` / `ES_SERREG_DUMP_SIGNAL=USR1\|USR2\|RTMIN+n` | dump on demand: a helper thread with the signal blocked everywhere else polls (100 ms, spec §11) and writes the dump outside any signal handler |
| `ES_SERREG_DUMP_EXIT=1` | `_Exit(0)` after the dump (a "print the schema and quit" mode for build scripts) |
| `ES_SERREG_CONFLICT=warn\|abort` | the FR-R4 policy |

### 4.6 Dictionary scopes (DECISIONS A-9)

A sink type that declares `using dictionary_scope = Tag;` makes every call signature serialized into it register
a `scope` node `(scope_hash, call_hash)` beside the call. `export_image_scope(scope_hash)` then produces the
dictionary of *that sink type only*: a log file's dictionary is the forty format strings it uses, not the
program's four thousand archive types. Compile time, zero bytes per message; the scoped capture file writes the
scope's RRI as its opening dictionary and scope deltas afterwards.

## 5. The RRI — Raw Registry Image

The RRI is the registry serialized: a 40-byte header followed by ENTRYs. It is the opening dictionary block of a
capture **FILE** (at `dict_offset`, `dict_len` bytes), the payload of every dictionary frame (full or delta), the
optional full copy inside a checkpoint frame, the content of a `.rri` file (`export_image(path)`,
`srdump --rri-out`, the `ES_SERREG_DUMP_REGISTRY` dump), and the concatenation a reader rebuilds from an ELF
section (§6).

```
offset  size  field
0       4     magic            "ESRG"
4       2     major            1
6       2     minor            7         the format minor the writer stamps; readers accept 6 and 7
8       4     flags            bit0 = delta (this image lists only ENTRYs added since an earlier one)
12      4     hash_algo        2 = XXH3-64 (the only algorithm; 1 = the retired FNV of format 1.0–1.2 → bad_version)
16      4     n_entries        number of ENTRYs that follow
20      8     program_hash     XXH3-64 over the sorted schema hashes of the loaded types (0 if unknown)
28      4     reserved         0
32      8     abi_tag          packed: u8 version(1), endian(0 LE), ptr_size, long_size, wchar_size,
                               char_signed, max_align, long_double_size
40      …     ENTRY × n_entries   each a multiple of 8 bytes (§1)
```

What each header field is for:

- **magic / major / minor / hash_algo** — a reader refuses anything but `ESRG` 1.x with `format_minor_oldest ≤
  minor ≤ format_minor` and `hash_algo == 2` (`error::bad_version`, `bad_magic`), before touching an ENTRY.
- **flags.delta** — a delta RRI is *merged* into a dictionary already holding the opening one; its
  `program_hash` is that of the whole registry at export time, not of the delta.
- **n_entries** — the loop bound; `total_len` of each ENTRY is the stride, bounds-checked against the buffer
  (the RRI fuzz target drives this parser with mutated inputs under ASan/UBSan).
- **program_hash** — lets a reader tell "same program, same types" at a glance and `srdump --elf
  --program-hash` print the identity of a binary; the FILE header carries the same value.
- **abi_tag** — the facts of the writer's C++ ABI the ENTRYs do not already carry (member sizes and offsets are
  in every `FIELD`): `wchar_t` width (a `wchar` kind has no width in a call string), `char` signedness (how a
  `char` renders), pointer/`long` sizes and `max_align_t` (diagnostics for a file from another ABI). A
  dictionary that merges RRIs of different ABIs is flagged `abi_mixed`; typed reads across ABIs already fail by
  hash.

Three exports produce it: `export_image()` (full), `export_image_since(v)` (delta: the registry's `added`
version of each entry decides membership), `export_image_scope(h, v)` (one scope). `dictionary::encode_rri`
(`src/dictionary.cpp`) re-encodes a *parsed* dictionary — `srdump --rri-out` of a capture — keeping the source's
`program_hash` and `abi_tag` rather than this process's.

Reading: `tools::load_dictionary_rri(bytes)` / `load_dictionary_file(path)` parse the header, then
`dictionary::add_entries` walks the ENTRYs into `dyn::dictionary` (`types`, `literals`, `calls` keyed by hash,
`scopes`), applying the FR-R4 rule from the tooling side (the first definition of a hash is kept, a different
body under the same hash is a `type_conflict`) and validating every `FIELD`'s geometry (offset + size within
the type, extent × element size within the field) so that a hostile RRI cannot drive a decoder out of bounds.

## 6. The ELF section — the registry without running the program

Every TU that instantiates a serializable type, literal or call writes the ENTRY copy into the section
`es_serreg_dict` (§2.4). Linked, the section is the concatenation of those 8-aligned blobs: an ENTRY stream with
no header, duplicates included. That makes any binary built with the library **self-describing offline**:

```
srdump --elf ./your-app                  # the embedded dictionary of an executable or a .so
srdump --elf ./your-app --rri-out app.rri
srdump --elf ./your-app --program-hash   # the identity of its type set
srdump --elf ./your-app --json | --toml | --cxx | --wireshark
```

`tools::extract_embedded_dictionary(path | image bytes)` (`src/elf_reader.cpp`) is a self-contained ELF64
little-endian section reader — no libelf: it reads the section header table (`e_shoff`, `e_shentsize`,
`e_shnum`, overflow-checked), finds `es_serreg_dict` (or `.es_serreg_dict`) through the section-name string
table, rejects `SHT_NOBITS`, caps the input at `internal::input_limit()`, and hands the section's bytes to
`add_entries` exactly as if they were the ENTRY part of an RRI. Duplicates collapse by hash; the result is the
same `dyn::dictionary` a running process would export, minus retained entries of plugins never loaded.

Why a section and not a symbol table or a generated file: the data is produced by the compiler from the same
`consteval` descriptor that the running program uses (no generator, no drift), it survives stripping (it is
`SHF_ALLOC` read-only data, not a symbol), it needs no code to run (a Wireshark dissector or a Python reader can
be generated from the binary on a machine where it cannot execute), and `--gc-sections` keeps it because of
`gnu::retain`. The `image=none` build (`ES_SERREG_EMBED_NONE`, `-DES_SERREG_IMAGE_FORMAT=none`) omits the
section for formats without one; the running registry is unaffected.

## 7. How the three images of the registry relate

| Image | Where | Header | Contains | Produced by | Read by |
|---|---|---|---|---|---|
| **running registry** | heap entries + published index | — | every loaded and retained definition of the process; descriptors with scrub tables | guards during static init / `dlopen` | `find_*`, the typed readers, `inspect` |
| **RRI** | a capture's dictionary block/frames, a `.rri` file, a dump | 40 bytes (§5) | the ENTRYs of the export (full / delta / scope), `program_hash`, ABI | `export_image*`, `encode_rri` | `load_dictionary_*`, every capture reader, the Python reader |
| **ELF section** | `es_serreg_dict` in each executable / `.so` | none | the ENTRYs of every TU of that image, duplicates included | the compiler (`embed_elf.hpp`) | `extract_embedded_dictionary`, `srdump --elf` |

A capture file is read with the RRI it carries; a binary is read with its section; a live process answers from
its index — and all three agree because they are the same ENTRY bytes, keyed by the same hashes.

## 8. Costs and limits (DECISIONS G-5)

Registration is O(1) per node under one mutex (a `by_key` lookup, one heap copy per *new* definition, none for
an alias); publication is one index build per burst, at the first read after it; lookups are a binary search
over keys. The budgets asserted under the `bench` label: 10 000 entries / 10 MB of ENTRYs / 10 ms and 100 000 /
100 MB / 100 ms for push + publish in a release build (`tests/bench/registry_measure`), met on the reference box.
Memory: one heap copy of each ENTRY and name, a deep descriptor copy for plugin images only, indexes retained
until exit (about 80 bytes per entry per generation). A program with many types keeps the image small with
dictionary scopes (§4.6) and delta exports.
