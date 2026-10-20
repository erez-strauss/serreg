# 1. Introduction and terminology

`es::serreg` (ser-reg) is a C++26 library for binary serialization of C++ objects to files and IPC channels,
with one property no other C++ serializer offers today: **every type you serialize is described by the compiler
(P2996 reflection) and registered in a process-wide registry before `main()` runs**, purely because you called
`serialize()` on it somewhere in the program. No IDL, no code generation, no macros, no manual registration,
no reflection at run time. The description ("dictionary") travels with the data, can be exported from a running
process, and is also embedded as a plain ELF section in every binary — so a tool that has never seen your
headers can decode your captures, and `srdump --elf ./your-app` prints the schema of any program built with it.

Library version 2.2.6 writes wire format 1.7 and reads 1.6 and 1.7. The format grew in steps: dynamic members (1.2), XXH3-64 ids and the writer's ABI tag (1.3), the canonical-name rules (1.4),
compile-time constants as call arguments (1.5), containers (1.6: maps and sets as members — the wire shapes `map`
and `set`, entries sorted by key, FR-W9 —, any container as an argument of its own — the transparent type, FR-A6 —,
and the enum ENTRY's `fixed` flag), and integrity and crash safety for captures of any size (1.7, every feature
opt-in: dictionary deltas written inline and linked forward so nothing is rewritten at close, an optional CRC32C
trailer per frame, checkpoint frames for seeking and resynchronisation, a writer-info frame shown by
`srdump --info`). Around the format sit the reader limits and the never-crash contract (`limits.hpp`,
`docs/SECURITY.md`, FR-D5), windowed reading of captures of any size (FR-D11), `register_types`, the range check for
enums without a fixed underlying type (decision D), `--diff --direction` (decision E), `srdump --cxx` (FR-D10), the
logger (`log.hpp`), dictionary scopes, `to_string`/`to_json`, the pure-Python reader and the registry measurements.
Readers accept the current minor and its predecessor (DECISIONS G-2: until 3.0 the minor is the compatibility unit,
and the SONAME is `major.minor`). The std vocabulary is closed and complete for the common containers:
`std::deque`/`list`/`forward_list`/`stack` are the `vector` shape, `std::pair`/`tuple` positional dynamic classes,
`std::multimap`/`unordered_multimap` a vector of pairs sorted by key, and any other standard-library class is
rejected with a diagnostic that names the vocabulary. The canonical strings behind the hashes are public
(`schema_string_v<T>`, `layout_string_v<T>`), and flat types have schema evolution level 1: a reader with a
dictionary reads an older or newer build's frames by member name (`renamed_from`, `deserialize(dict, …)`,
`tools::project`, chapter 4 §4.1d). The book documents the current state of the tree.

## 1.1 What it is for

- **Captures**: a trading engine writes every message it sends and receives to a file; a year later, a
  different program — or `srdump` — reads it, with the exact schema of the binary that wrote it.
- **IPC**: a producer writes frames into a shared-memory ring; consumers dispatch on them at `memcpy` speed.
- **Binary logging**: `println("px={:.2f} qty={}"_, px, qty)` costs a 16-byte header plus the raw
  arguments; the reader runs `std::format`. A `logger` adds level, source location and time
  (`ES_SERREG_LOG(lg, warn, "…"_, …)`) for 8 more bytes, and `srdump --level` prints log lines.
- **Schema tooling**: hashes per type, per call signature and per program let you compare two builds
  (`srdump --diff a b`), detect a layout change before it corrupts a file, and verify a capture against the
  binary that should have written it.

## 1.2 Design in one paragraph

A **flat type** (trivially copyable class/enum, arrays, nested classes, private members, bases) is written as
its native bytes, with padding zeroed and `bool`s normalised so identical values give identical bytes. A
**dynamic type** (a class holding `std::string`, `std::vector`, `std::optional` or `std::variant` anywhere) is
written as a **canonical image**: a fixed part with reference slots plus a tail, laid out by a rule that does not
depend on the compiler's `offsetof`. Each `serialize(sink, args...)` call writes one **frame**: a 16-byte header
carrying the 64-bit **call hash** of the argument categories, followed by one **record** per argument. String
**literals** passed as `"text"_` are interned: only their id lives in the call signature, zero bytes on the wire.
Numeric **constants** passed as `42_` are part of the call signature in the same way.
Every type, literal and call signature has a compile-time **ENTRY blob**; blobs are collected into the
**registry** at image load, exported as an **RRI**, embedded in the **`es_serreg_dict`** ELF section, and
written at the head of every capture file — followed, in a 1.7 file, by a **delta** whenever a plugin adds types
while the file is open, so a capture describes itself up to its last complete frame at every instant.

## 1.3 Terminology

| term | meaning |
|---|---|
| **flat type** | `flat_type<T>`: trivially copyable class or enum (after `as_flat` mapping); record = native bytes |
| **dynamic type** | `dynamic_type<T>`: class with a dyn member anywhere, or a container passed as an argument; record = `u32 len, u32 0, canonical image` |
| **dyn member** | `std::string`, `std::vector<T>`, `std::optional<T>`, `std::variant<Ts...>`, and (format 1.6) `std::map`/`std::unordered_map`/`std::flat_map` (wire shape `map`) and `std::set`/`std::unordered_set`/`std::flat_set` (shape `set`); `std::deque`/`std::list`/`std::forward_list`/`std::stack` (shape `vector`) and `std::multimap`/`std::unordered_multimap` (a `vector` of `std::pair<K,V>` sorted by key; schema kindspec `multimap<K,V>`) — an 8-byte reference slot `{u32 off, u32 n}` in the fixed part. `std::pair`/`std::tuple` are not dyn members but **positional dynamic classes** (parts `first`/`second`, `_0`, `_1`, …, inline in the fixed part at canonical offsets) |
| **std vocabulary** | the standard-library classes the library maps (the rows above plus `std::array`, `std::monostate`); closed: any other std class — `std::priority_queue`, a smart pointer, `std::tuple<>` — is a compile-time rejection naming the rule (FR-S7) |
| **projection (level 1)** | reading a flat type's frame written by another version of the type, member by member, by name, from the writer's dictionary (`renamed_from`, `deserialize(dict, …)`, `tools::project<T>`, `projection_report`; chapter 4 §4.1d) |
| **transparent type** | (FR-A6) the synthetic struct type of a container passed as an argument: canonical name = the container's vocabulary name (`std::vector<i32>`), one unnamed slot field at 0, size 8, flag `transparent`; renderers show the value, not a struct |
| **canonical order** | (FR-W9) the entries of a map or set are written ascending by the key's `operator<` (its flat-image bytes when it has none), whatever the C++ container's iteration order, so equal values are equal bytes |
| **canonical image** | fixed part (members at canonical offsets) + tail (string bytes, element blocks) referenced by image-relative offsets |
| **literal** | `lit<"text">` — a compile-time string whose identity is in the type; id = XXH3-64 fingerprint of the text; 0 bytes in compact frames |
| **runtime string** | `const char*`, `std::string_view`, `std::string` as an *argument*: `u32 len, bytes` on the wire (data, never interned) |
| **scalar argument** | `int`, `double`, `bool`, … as a top-level argument: its bytes, padded to 8 |
| **constant** | (format 1.5) `constant<V>`, written `42_`, `-7_`, `1.5_` or `k<V>`: a scalar or enum value fixed at compile time; type and value are in the call string, the value is stored once in the call's ENTRY; 0 bytes in compact frames |
| **argument category** | type / literal / scalar / string / constant — what the call string names (`0x<schema>`, `lit:<id>`, `i32`, `string`, `const:i32=42`) |
| **frame** | one `serialize()` call: 16-byte header + records; **compact** (default) or **full records** (self-describing, archival) |
| **record** | one argument inside a frame |
| **call hash** | XXH3-64 of the canonical call string, e.g. `call(2){lit:0x…,0x…}`, `print(4){…}` |
| **schema hash** | XXH3-64 of the schema string (names, types, offsets, `since`/`deprecated`) — identifies a type on the wire |
| **layout hash** | XXH3-64 of the layout string (no names) — two types with equal layout hashes are bit-compatible |
| **program hash** | order-independent hash of all type schema hashes of a process (FR-H3); in FILE and RRI headers |
| **ENTRY / blob** | one type, literal or call serialised as a TLV blob (`format.hpp`); `constexpr`, in `.rodata`, and copied into `es_serreg_dict` |
| **dictionary** | a set of ENTRYs; `dyn::dictionary` is its runtime (parsed) form |
| **RRI** | Raw Registry Image: 40-byte header (incl. the writer's ABI tag) + ENTRY stream; what `registry::export_image()` produces and what captures embed |
| **registry** | the process-wide table of every type/literal/call the program can serialize, populated at image load (`registry::instance()`) |
| **registrar / guard** | `registrar<T>::g`: the static object whose constructor pushes `T`'s node — instantiated by odr-use inside `serialize()` |
| **image** (loader sense) | the executable or one DSO; the registry counts references per image (`dladdr`) for `dlopen`/`dlclose` |
| **heap entry** | the registry's record of one registration — for the main program views of its ENTRY blob, name and descriptor, for any other image copies of the blob and name and a deep copy of the descriptor; one per (kind, hash), never freed before exit |
| **retained entry** | an entry whose images have all been unloaded (`refs == 0`); it stays in lookups, exports and the index, so old frames still decode (`registry::retained()`) |
| **reload** | loading an unloaded plugin again: registrations with the same hash and layout revive their retained entries; a changed type has a new schema hash and becomes a new entry |
| **sink / claiming sink** | where bytes go: `write(p, n)`; a claiming sink also hands out an exact-size slot (`claim/commit`) so frames are built in place |
| **capture file** | `capture_file`: FILE header + opening dictionary (RRI) + CHAIN + writer-info frame + frames (format 1.7); nothing is rewritten at close — dictionary deltas are written inline (an earlier format back-patched the dictionary on close) |
| **capture options / policy** | `capture_options` (run time, per file: CRC, checkpoints, writer info, sync policy) and `capture_policy` (compile time: `dictionary_updates`) |
| **dictionary frame** | a frame carrying an RRI inside a stream (pipes, sockets) or, in a 1.7 file, inside the file after a registry change |
| **delta dictionary** | (format 1.7) an RRI with `rri_flag_delta` holding only the entries added since the last dictionary written (`registry::export_image_since`, `registry_entry::added`); readers merge it into what they have |
| **CHAIN** | (1.7) the 16 bytes after a file's opening dictionary: `u64 next_dictionary`, `u64 first_checkpoint` — the heads of the two forward-linked chains, each link patched once with an 8-byte `pwrite` |
| **CRC32C trailer** | (1.7) optional 8 bytes at the end of a frame, `u32 crc32c(frame before it), u32 0` (frame flag bit 2); a frame that fails it is reported and skipped, never decoded (`error::crc_mismatch`) |
| **checkpoint** | (1.7) a meta frame written every N bytes or T seconds: counts of the data frames and bytes before it, wall clock, latest dictionary offset, optionally the full dictionary; checkpoints are forward-linked for seeking (`srdump --info`) and are where `--resync` resumes after a damaged frame |
| **writer info** | (1.7) the meta frame after the CHAIN: key/value items — library version, format, program, host, pid, start time, the options the file was written with (`srdump --info`) |
| **meta frame** | (1.7) a dictionary, checkpoint or writer-info frame: no records, consumed by every reader, counted in the frame numbering but not in `--verify`'s totals, never listed in dumps |
| **scope / scoped sink** | a dictionary scope is a tag type; a sink that declares `using dictionary_scope = Tag;` (`scoped_sink`, e.g. `scoped_capture_file<Tag>`) registers, at compile time, which calls it carries, and its file's dictionary holds only those calls and the types and literals they reach (`registry::export_image_scope`) |
| **log envelope** | the leading arguments of a logger's call — level, file, line, options mask as zero-byte constants, then the `u64` timestamp and, by policy, a thread id and a sequence number — under the call flag `log` (`log.hpp`) |
| **srdump** | the CLI; a one-line `main` over `es::serreg::cli::run`, which is itself a thin layer over `es::serreg::tools` |
| **print call** | `print/println`: a call whose first argument is a format literal; flags in the call ENTRY; rendered by the reader |
| **never-crash contract** | (FR-D5, `docs/SECURITY.md`) for any sequence of input bytes every public reading API returns a value or an error — no crash, no undefined behaviour, no allocation or loop beyond the **reader limits** |
| **reader limits** | `es::serreg::decode_limits` (`limits.hpp`): frame 64 MiB, dictionary 256 MiB / 1 000 000 entries, image depth 64, whole-input read 1 GiB, window 10 MiB, 1000 problem lines; process-wide (`set_decode_limits`) or per call |
| **window** | the slice of a capture a reader holds at a time (`decode_limits::window_bytes`, `--window`); every reader over a path works window by window, so a capture's size is bounded by the file system, not by the tools |

## 1.4 What is unique about the registry (and why it matters)

1. **Registration by instantiation.** `serialize(sink, T{})` odr-uses `detail::registrar<T>::g`. That static data
   member of a class template is instantiated in whatever translation unit calls `serialize`, and its dynamic
   initialisation runs when that image loads — before `main()` for the executable, at `dlopen()` for a plugin.
   Nothing is looked up lazily; there is no "first call" cost and no way to forget to register a type.
2. **Recursive and exact.** `touch<T>()` walks members, bases, array elements, vector/optional elements and
   variant alternatives, so every type reachable from an argument has its own ENTRY — the dictionary is
   complete by construction, and exactly as large as what the program actually serializes.
3. **One registry per process, per-image accounting.** Storage lives in `libserreg` (shared); each node knows
   its image base (`dladdr`), aliases across TUs/images collapse by hash, and a genuine layout conflict
   (same schema hash, different layout hash) is reported (`ES_SERREG_CONFLICT=warn|abort|silent`).
4. **Lock-free readers.** Every registration becomes a heap entry when it is pushed (the main program's entries
   view their blobs in place, other images' entries are copies); mutations
   (load/unload time only) take a mutex and mark the index stale, and the next reader publishes one immutable,
   sorted index of views into those entries. Readers acquire-load it and binary-search.
5. **Unloading keeps what frames need.** `dlclose` of a plugin drops its references and never aborts; its entries
   are retained, so frames it wrote still decode and descriptor pointers stay valid. Loading the plugin again
   revives the same entries; a rebuilt plugin with a changed type adds a new entry next to the old one
   (decision A). An earlier design interposed `dlclose` and aborted on such an unload; retaining entries is
   simpler and keeps every descriptor valid (`ES_SERREG_ALLOW_DLCLOSE` is now ignored with a warning).
6. **Three carriers, one truth.** The same blobs are in the ELF section (offline: `srdump --elf`), the exported
   RRI (`ES_SERREG_DUMP_REGISTRY`, signal, API) and the capture — a dictionary can be recovered from any of them.

## 1.5 How to read this book

Chapters 2–6 are the user's path (build, serialize, read back, registry, hashes). Chapters 7–8 are the bytes
(wire format, canonical images). Chapter 9 explains the shared-library and loader machinery a maintainer must
understand. Chapter 10 is the tooling (including `--info`, `--resync`, the log options, the Python reader).
Chapter 3 §3.9 is the logger, chapter 7 §7.9 format 1.7. `executables.md`, `package-contents.md` and `files/`
document every program, the delivery, and every source file — one page per header and source, among them
`files/include-es-serreg-limits.hpp.md` for the reader limits,
`files/include-es-serreg-log.hpp.md`, `files/include-es-serreg-to_string.hpp.md` and
`files/include-es-serreg-version.hpp.md`; the pages on the Python reader,
`registry_measure`, the CE-example tests and the rebuild-prompt script sit at the top of `book/`.
