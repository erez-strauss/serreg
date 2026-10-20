# DECISIONS — es::serreg

**Refers to:** library **2.2.6**, wire format **1.7** · **Date:** 2026-10-19 · **Status of this file:** the one ledger
of every decision taken for the project — past, present and planned. It replaces the three earlier ledgers
(`directions-log.md` O/N numbering, `docs/cc-review-2.1.10-response.md` A–S lettering, the spec's §0 review log),
which are kept under `remove-me/` for one release as history only. New decisions are added here, never there.

Each entry has: an **ID** (section letter + number), the **decision** as it stands today (not how it was reached),
**status** (`implemented <version>` · `to be implemented <version>` · `open`), and **previously defined at** (the
place the decision was first recorded, for traceability). Sections: **R** requirements, **P** protocol (wire format
and dictionary), **A** API, **C** code specification (conventions and rules the code follows), **G** general
(scope, users, versioning, platform, community), **T** testing and tooling, **D** documentation. When a decision is
superseded, the entry is rewritten and the old text dropped — this file describes the present and the future, not the
path.

| Section | Entries | Implemented | To be implemented | Open |
|---|---|---|---|---|
| R — requirements | 12 | 11 | 1 | 0 |
| P — protocol | 21 | 18 | 1 | 2 |
| A — API | 16 | 13 | 2 | 1 |
| C — code specification | 11 | 11 | 0 | 0 |
| G — general | 16 | 9 | 1 | 6 |
| T — testing and tooling | 11 | 11 | 0 | 0 |
| D — documentation | 5 | 4 | 1 | 0 |
| **Total** | **92** | **77** | **6** | **9** |

The companion documents: `ser-reg-requirements.md` (what the library must do), `ser-reg-architecture.md` (how it is
built), `ser-reg-testing.md` (how it is verified), `ser-reg-stories.md` (how it is used), `ser-reg-rebuild-prompt.md`
(how to build it again from this ledger).

---

## R — Requirements

**R-1 Users: the public; the goal is a core C++ library.** The library is written for as many teams as possible, with
the aim of bringing most of it into Boost, Beman or the standard. Every design choice is judged against that, not
against one team's convenience. — *implemented* (drives everything) — *previously defined at:*
directions-log B-O1 (2026-09-30); response H.

**R-2 Trust model: one trust domain, and the code never crashes on wrong data.** Input is corrupt or buggy, not
hostile, but: every known bug is fixed; reasonable size limits apply by default and the caller can override them
(`decode_limits`, process-wide or per call); for any sequence of input bytes every reading API returns a value or an
error and never crashes, aborts, asserts, executes undefined behaviour, or allocates or loops beyond the limits.
Captures may be several TB (a trading week); frames are limited to 64 MiB; pipe buffers start at 2 MiB and grow to 8
MiB. — *implemented 2.1.16* (FR-D5, `limits.hpp`, `docs/SECURITY.md`) — *previously defined at:* directions-log B-O2;
spec §0.00000 "O2 (in code)".

**R-3 Every non-static member is serialised, private members included; `as_flat<T>` maps a type, `[[=skip]]` drops
a member.** The dictionary documents exactly what was serialised. `as_flat` applies to top-level arguments only. —
*implemented 1.0* — *previously defined at:* spec round 1 #12, D2; directions-log A2.

**R-4 Padding is scrubbed by default; `bool` is normalised.** Equal values give equal bytes (captures, hashes, dedup);
`serialize_options{.scrub_padding = false}` opts out. A non-canonical `bool` byte reads as `true` with an optional
warning; `view<T>` returns `invalid_value` instead. — *implemented 1.0 / 2.1.3* — *previously defined at:* spec D3,
D4; directions-log A3, A23.

**R-5 Registration by instantiation, with a dictionary copy in every binary.** odr-using `registrar<T>::g` registers a
type before `main()` and at `dlopen`, with no macros and no calls; every ENTRY is also in the `es_serreg_dict` ELF
section so tools read a binary's schema offline. `register_types<Ts...>()` / `register_call<Sig...>()` register
without a call site. — *implemented 1.0 / 2.1.16* — *previously defined at:* spec round 1 #8/#9, D6; directions-log
A4; response A1.

**R-6 Literals travel as ids; compile-time constants travel as nothing.** A string literal in a compact frame costs 0
bytes (the call signature names it), its text lives in the dictionary; `42_`, `-7_`, `1.5_`, `k<V>` are constants of
the call signature, stored once in the call's ENTRY. — *implemented 1.1 / 2.1.15* — *previously defined at:* spec
round 2 Q1 (FR-L1..L5); directions-log A7, B-O15.

**R-7 Compact frames by default; the dictionary is always complete.** One 64-bit call hash per frame; record
boundaries come from the dictionary; full records are opt-in. Completeness: dictionary frames on streams, and since
1.7 inline dictionary deltas in files (P-9) — never a rewrite at close. — *implemented 1.0 / 2.2.1* — *previously
defined at:* spec round 2 Q2 (FR-W2/W7); directions-log A8.

**R-8 Typed readers report expected versus actual in one pass.** `deserialize` and friends return
`std::expected<std::size_t, decode_error>` with the code, the number of items expected, the number decoded and the
first item that failed; no second validation pass, no silent partial output. — *implemented 2.1.15* — *previously
defined at:* response C; spec §0.0000 C.

**R-9 Schema evolution level 1: projection by member name, widening, `renamed_from`, in both directions.**
A reader built from a newer or an older definition reads the other side's frames through the dictionary: members
matched by name; widening allowed, narrowing never; R3 (unsigned → strictly wider signed) and R12 (an array that
grew) kept; variant alternatives matched by the alternative type's canonical name (`std::string` regardless of
allocator; a renamed alternative needs `renamed_from` on the class); nested types project by structure when the
member name matches — a renamed nested type is reported (`projection_report`, `--diff --compat`), not blocking;
enums match by underlying kind. No wire change. **Shipped first for flat types** (`projection.hpp`: the
`renamed_from` annotation, `tools::project<T>`, `deserialize(dict | projection_context, bytes, outs…)`, a plan
per (writer type, T) built once; R1–R9, R12–R14). Still to come: dynamic members (R10, R11, R15), `frame_router`
and `dispatch` by projected argument types, `walk_values{.project}`, `srdump --diff --compat` / `--project`, and
`renamed_from` recorded in the dictionary (an additive ENTRY, format 1.8). — *implemented 2.2.3 (flat types);
the rest 2.3.0* — *previously defined at:* spec D5; directions-log B-O5..O8; response L; decided 2026-10-07 (L1 a,
L2 a, L3 a, L4 b); maintainer 2026-10-13 (E1: level 1 starts in 2.2.3).

**R-10 Evolution levels 2 and 3.** Level 2 (structural changes beyond level 1) is unscheduled; level 3
(dictionary-less evolved reads) is not planned: every stream carries its dictionary, emitted when the registry
version moves. — *implemented* (as a non-goal) — *previously defined at:* directions-log B-O4; response I (2026-10-07).

**R-11 Binary logging is a primary use.** The logger (A-8), scoped dictionaries (A-9), the registry budgets (G-5) and
the log views of srdump exist because log sites are the largest source of registry entries and frames. —
*implemented 2.2.1* — *previously defined at:* response H; directions-log B-O16, B-N5.

**R-12 Captures of unlimited size with bounded memory.** Every capture reader works on a window
(`decode_limits::window_bytes`, 10 MiB); nothing reads a capture whole or keeps per-frame state; `verify`
statistics are O(types). — *implemented 2.1.16* — *previously defined at:* directions-log B-O23; response A6.

## P — Protocol (wire format and dictionary)

**P-1 64-bit ids everywhere.** Schema, layout, call and literal identities are 64-bit XXH3 fingerprints (`hash_algo`
2); FNV-1a was removed and pre-1.3 files are refused. — *implemented 2.1.0* — *previously defined at:* spec round 2
Q4 (FR-H8), §0.0 D1; directions-log A10, A20.

**P-2 An ABI tag in the FILE and RRI headers.** Eight bytes recording endianness, pointer, `long`, `wchar_t`, `char`
signedness, `max_align_t`, `long double`; typed reads across ABIs fail by hash, dictionary readers use the tag. —
*implemented 2.1.0 (format 1.3)* — *previously defined at:* spec §0.0 D2; directions-log A21.

**P-3 Canonical names are hand-built and library-independent.** Names walk `parent_of`, never compiler spellings;
builtins by kind name; `{anon}` and `{fn:}` for anonymous and local scopes; a class nested in a template
specialisation is named through its parent's canonical arguments; std types use a frozen vocabulary without inline
namespaces or allocators; user inline namespaces are kept; a type without a linkage-unique name must be named
explicitly. Shipped names are frozen by golden `STATIC_REQUIRE`s and the Compiler Explorer lanes. — *implemented
2.1.14 (format 1.4)* — *previously defined at:* spec D8, §6.2b, §0.000 B; directions-log A6, B-N6; response B.

**P-4 Base classes are `<base:Name>` pseudo-fields.** Hashes compose; `srdump --flatten` shows the flat view; virtual
bases are rejected. — *implemented 1.0* — *previously defined at:* spec D7 option d; directions-log A5.

**P-5 The canonical image: one `{u32 off, u32 n}` slot for every dynamic member; dynamic records are prefixed `u32
len, u32 0`.** Identical values give identical bytes; both decoders share one set of slot rules
(`fmt::image_budget`). A valueless variant decodes to `std::monostate` when the variant has one, otherwise
`invalid_value`, and decoding continues. — *implemented 2.0 / 2.1.3* — *previously defined at:* plan v2.0 §2;
directions-log A17, A22; spec §0.0 D3.

**P-6 Variants carry each alternative's name and hash; `since`/`deprecated` are in the schema hash only.** —
*implemented 1.0* — *previously defined at:* spec round 2 Q6/Q7 (FR-W8, FR-S11); directions-log A12, A13.

**P-7 Maps and sets are wire shapes, not container types.** `std::map`, `std::unordered_map`, `std::flat_map` are
`map` (`kind::map_ = 53`, sorted by key); `std::set`, `std::unordered_set`, `std::flat_set` are `set` (`kind::set_ =
54`); unordered containers are sorted before encoding; a duplicate key decodes as `invalid_value`; a container passed
as an argument is a transparent synthetic type; the enum ENTRY carries `enum_flag_fixed`. — *implemented 2.2.0
(format 1.6)* — *previously defined at:* spec §0.000000; `docs/plans-for-containers.md`.

**P-8 Enums without a fixed underlying type: kind from the enumerators, range-checked on decode.** The underlying
kind is the unsigned kind of `sizeof(E)`, or the signed kind when an enumerator is negative (GCC's, Clang's, the
Itanium ABI's rule, so no hash moved). Typed readers reject values outside the enumerators of such an enum; the
dictionary-driven readers do the same from the `fixed` flag. — *implemented 2.1.16 / 2.2.0* — *previously defined at:*
response D; spec §0.00000 D.

**P-9 Format 1.7: the dictionary is a forward-linked chain of blocks; nothing is ever rewritten.** The opening block
is `RRI | pad8 | CHAIN(u64 next_dictionary, u64 first_checkpoint)`; each later dictionary frame holds a delta
(`rri_flag_delta`: the entries whose registry version is newer than the last written) and ends with `u64 next`; every
link is zero when written and patched exactly once with one 8-byte `pwrite` when its target exists. A file
truncated anywhere is readable up to the cut; the old back-patch at close (FILE flag bit 2) is read from 1.6 files
and never written. — *implemented 2.2.1 (format 1.7)* — *previously defined at:* directions-log B-O23; maintainer
2026-10-07 ("avoid back-patching", "offset to the next delta dictionary frame … linked list").

**P-10 Format 1.7: an optional CRC32C trailer per frame.** Frame flag bit 2; the last 8 bytes are `u32
crc32c(frame[0, len-8))`, `u32 0`; a frame whose CRC does not match is reported and skipped, never decoded nor
delivered; typed readers return `crc_mismatch`. Enabled by `capture_options::crc`, `serialize_options::crc`, or a
sink's `frame_crc()`. Default off. — *implemented 2.2.1* — *previously defined at:* directions-log B-O17 (review
C-11); response O17 (2026-10-07: "all four in 1.7, all optional").

**P-11 Format 1.7: checkpoint frames.** Frame flag bit 3, sentinel "ESSRCKPT"; a fixed 64-byte payload (version,
flags, rri_len, prev, next (patched), data frames before, data bytes before, wall clock, latest dictionary offset,
registry version) and optionally the full dictionary; written every N bytes and/or every T (checked every 64 frames)
and once at close; linked both ways; `--resync` scans forward 8 bytes at a time to the next one after an unparsable
frame. Default off. — *implemented 2.2.1* — *previously defined at:* directions-log B-O23; response O17/O23.

**P-12 Format 1.7: a writer-info frame records the options a file was written with.** Frame flag bit 4, sentinel
"ESSRINFO"; `u16 key, u16 len, bytes` items (`fmt::info_key`: library version, format, program, hostname, pid, start
time, scrub/full-record options, canonical_unordered, crc, checkpoint settings, dictionary scope, sync policy, log
clock, writer thread, ABI); unknown keys are skipped. `srdump --info` prints it by seeks. On by default
(`capture_options::writer_info`, `info_host = false` for reproducible files). — *implemented 2.2.1* — *previously
defined at:* maintainer 2026-10-07 ("srdump should be able to extract the serialize_options that was used").

**P-13 Meta frames are consumed, never delivered.** Dictionary, checkpoint and writer-info frames count in a frame's
`index` and are invisible to callbacks, `verify` counts, dumps and routers. — *implemented 2.2.1* — *previously
defined at:* this release (2.2.1 harmonisation of the span and windowed readers).

**P-14 Readers accept the current format minor and the previous one (1.6 and 1.7).** Older minors are refused
(`bad_version`), newer ones are not guessed at. — *implemented 2.2.1* (`format_minor_oldest`) — *previously defined
at:* spec FR-H5 (one minor), widened for 1.7.

**P-15 Log frames are print frames with an envelope of constants.** Signature `log_tag, constant<u8 level>,
lit<File>, constant<u32 Line>, constant<u8 mask>, u64 time, [u32 thread], [u64 sequence], lit<Format>, args…`;
`call_flag_log` (bit 2) marks the call; only the timestamp and, by policy, the thread id and sequence cost bytes. —
*implemented 2.2.1* — *previously defined at:* directions-log B-O16 (review C-14); maintainer 2026-10-07.

**P-16 Scoped dictionaries are computed at compile time, written per file.** A `scoped_sink` declares
`dictionary_scope`; every call serialised into it registers a `(scope, call)` node; a scoped capture file's
dictionary (opening block and deltas) is the closure of those calls over their argument types and literals. No
per-frame cost, no runtime bitmap. — *implemented 2.2.1* — *previously defined at:* response P; maintainer 2026-10-07
(rejected the runtime set; "specific logger_sink type … at compile time").

**P-17 The `[INDEX]` footer of spec §6.5 is replaced by checkpoint frames.** A footer written at close does not
survive a crash; checkpoints do. — *implemented 2.2.1* (footer never built) — *previously defined at:* directions-log
B-O23 recommendation (b).

**P-18 A canonical-width encoding beside the native one (3.0) — so the typed read crosses ABIs; the file itself is
already portable by specification.** Fixed-width little-endian scalars, declared widths for
`long`/`wchar_t`, canonical offsets for flat types; the `memcpy` fast path stays when the ABI tags match; a written
wire specification, not header comments. — *to be implemented 3.0* — *previously defined at:* directions-log B-O19.

**P-19 After 3.0 the wire format changes only additively.** New frame flags, ENTRY tags, ARG kinds and info keys may
appear within a major; a reader skips what it does not know; removing or re-meaning a field is a new major. —
*open* (the 3.0 contract; stated now so 2.x design does not preclude it) — *previously defined at:* maintainer
2026-10-07 (G-2 discussion).

**P-20 Format minors in 2.x: the minor is the compatibility unit; 2.2.1 is the recorded exception.** A wire-format
change bumps the library minor (2.3 would carry 1.8); 2.2.1 ships format 1.7 as a patch because the library is not
yet outside its development tree ("we are allowed to break the versioning rules for now"). — *implemented 2.2.1* —
*previously defined at:* maintainer 2026-10-07.

**P-21 `since` / `deprecated` leave the schema hash (format 1.8).** Both stay on the ENTRY/FIELD records (u16
fields) and in `srdump --diff`; the schema string stops interpolating `;s…;d…`; golden hashes of annotated types
move with `format_minor = 8` in one commit (C-7). No third hash. Until then adding either annotation to a shipped
member moves the schema hash and every call hash naming the type while no byte changes (P-6 as it stands;
`docs/schema-evolution-specs.md` §0). — *open (maintainer decision pending)* — *previously defined at:* review of
2.2.5 §7.3; response document 2026-10-19 (`docs/claude-response-to-review-and-presentation.md` §3 7.3, §7).

## A — API

**A-1 No exceptions, no RTTI; errors are `std::expected`; `const` parameters by default.** — *implemented 1.0* —
*previously defined at:* spec §3, AGENTS.md §4.

**A-2 Services in the library, `srdump` a one-line `main`; applications may embed the whole CLI (`cli::app`).** —
*implemented 1.0* — *previously defined at:* spec round 1 #4 (FR-D0); directions-log A15.

**A-3 `dlclose` never aborts; entries are retained; plugins reload.** Each registration is a heap entry (for the main
program a view of its blob, name and descriptor — G-5; for other images copies and a deep copy of the descriptor);
`dlclose` drops references, lookups still find the entry, frames still decode; a reload revives it; a changed layout
is a new entry beside the retained one. No `dlopen`/`dlclose` interposers. — *implemented 2.1.13 / 2.2.1* —
*previously defined at:* response A; spec §0.00 A; directions-log A9/B-N1 (superseded).

**A-4 Three libraries, one package.** `es::serreg` (runtime), `es::serreg_tools` (CLI, renderers, Wireshark, diff,
C++ generator, `render_value`, `process_dictionary`), `es::ipc` (header-only C++23, no dependency on serreg).
`find_package(serreg COMPONENTS ipc serreg tools)`; `ES_SERREG_IPC_ONLY`. — *implemented 2.1.13* — *previously
defined at:* response G; spec §0.00 G; directions-log B-N2.

**A-5 `srdump --diff` takes a direction.** `--direction old-data | new-data | both | none`, default `old-data`;
reused by `--compat`. — *implemented 2.1.16* — *previously defined at:* response E; spec §0.00000 E.

**A-6 Compile-time constants with `_`.** `42_`, `-7_`, `1.5_`, `k<V>` at any argument position; ARG kind
`arg_const`. — *implemented 2.1.15 (format 1.5)* — *previously defined at:* directions-log B-O15; spec §0.0000 O15.

**A-7 `decode_limits`, process-wide and per call.** Frame 64 MiB, image bytes, nesting depth 64, walk steps,
dictionary entries, conflicts kept, window 10 MiB, pipe buffer 2→8 MiB. — *implemented 2.1.16* — *previously defined
at:* directions-log B-O2 table.

**A-8 The logger is a separate header with compile-time policies.** `log.hpp`: `logger<log_options, Sink>`; the gate
(`enabled()`, one relaxed load) runs before any argument is evaluated through `ES_SERREG_LOG(lg, level, "fmt"_,
args…)`; `ES_SERREG_LOG_MIN_LEVEL` and `log_options::floor` compile calls out; `log_options` is an NTTP so thread
id (`none | once | per_message`), sequence (`none | per_sink`), clock (`system | steady | tsc | user`) and
dictionary (`scoped | full`) are `if constexpr`; a logger over a plain sink needs `dictionary_policy::full`, over a
`scoped_sink` it uses the sink's scope; member calls `trace..fatal` (file "-", line 0) return `bool`, failures are
kept in `last_error()`/`failed()`. — *implemented 2.2.1* — *previously defined at:* directions-log B-O16; maintainer
2026-10-07 ("log wrapper in separate header file, b & c", "log_options{} are compile time constant … policy").

**A-9 Per-sink dictionaries through sink types, not runtime state.** `scoped_sink` concept, `scoped_capture_file<Scope>`,
`registry::export_image_scope`, `registry::scope_calls`; the policy of a logger chooses the full dictionary or the
sink type's scoped one. — *implemented 2.2.1* — *previously defined at:* response P; maintainer 2026-10-07.

**A-10 `to_string` / `to_json` / `render` for any serialisable value; `formattable<T>` opts into `std::formatter`.**
Rendering is dictionary-driven (`tools::render_value` over `tools::process_dictionary()`), so the text equals what
print and srdump show; instantiating it registers the type like a serialize call; it links `es::serreg_tools` and
is not for the hot path. — *implemented 2.2.1* — *previously defined at:* maintainer 2026-10-07 ("do we have a
to_string for any registered type?" — "Yes for both").

**A-11 `capture_options` / `capture_policy`: every 1.7 feature is opt-in; defaults write what 1.6 wrote.** CRC,
checkpoints (by bytes, by time, with dictionary), sync policy, program name, host info, writer thread, log clock,
clock function; `capture_policy{.dictionary_updates}` as an NTTP removes the per-frame version compare. —
*implemented 2.2.1* — *previously defined at:* maintainer 2026-10-07 ("all extra overhead are optional … so by
default as today").

**A-12 `frame_router::on_log`.** Log frames that no typed handler takes arrive rendered as `(level, line)`;
`on_log(level, h)` splits them by level into `tools::log_record`s (envelope fields + the message without the
prefix): the handler with the highest floor at or below the frame's level takes it, frames below every floor are
dropped without rendering (the level is a constant of the call entry), a plain `on_log(h)` keeps the rest.
`tools::render_log(frame, dict)` is the same decomposition for one frame. —
*implemented 2.2.1* — *previously defined at:* this release.

**A-13 Index reclamation (documented limit; `forget_retained()` in 2.3.0 — maintainer 2026-10-18).** `snapshot()` returns a plain reference, so published indexes live until exit (about 80
bytes per entry per publish). A `snapshot_guard` with epoch reclamation is added when a reload-heavy user appears,
keeping `snapshot()` for compatibility. — *open (3.0 API)* — *previously defined at:* response O.

**A-14 Retained entries in exported dictionaries.** Retained (unloaded) entries stay in `export_image()` so old
frames remain readable; a scoped dictionary (A-9) exports only what the scope's calls reach;
`registry::forget_retained()` is deferred. — *implemented 2.2.1 (scoped) / to be implemented 3.0 (forget)* —
*previously defined at:* response Q.

**A-15 Hidden visibility for the runtime library.** `-fvisibility=hidden` with an exported-symbol list checked by a
ctest. — *to be implemented 2.3.0* — *previously defined at:* response R.

**A-16 The read-side enum range check is a compile-time policy, off by default.** An enum without a fixed
underlying type holds only the values of the minimal-width integer of its enumerators; checking a leaf against that
range is *value validation*, not a format rule. `ES_SERREG_VALIDATE_READ=1` (one value for the whole program —
every TU and libserreg — like `ES_SERREG_LOG_MIN_LEVEL`; a mismatch fails to link since 2.2.6) turns the check
on: a leaf outside its range reads as 0 and the call reports
`invalid_value`; `view<T>` refuses the frame. With the default (0) the tables `enum_ranges_v<T>` are not
instantiated and the bytes are copied as they are. The dictionary readers (`srdump --verify`, Python) do not check
enum values. The write-side check and declared ranges belong to the value-validation feature
(`docs/values-validation.md`). — *implemented 2.2.3* — *previously defined at:* 2.2.1 suggestions (2026-10-07);
maintainer 2026-10-13 ("part of value validation, compile-time policy, default off"); wording corrected 2.2.6
(review B3).

**A-17 The standard-library vocabulary is closed and complete for the common containers.** Serialisable `std` types
are exactly: `string`, `vector`, `deque`, `list`, `forward_list`, `array`, `optional`, `variant`, `pair`, `tuple`,
`map`, `unordered_map`, `flat_map`, `multimap`, `unordered_multimap`, `set`, `unordered_set`, `flat_set`, `stack`.
`deque`/`list`/`forward_list`/`stack` take the `vector<E>` shape (same layout hash as `vector<E>`, their own name
in the schema string). `pair`/`tuple` are positional dynamic classes (`first`/`second`, `_0`…`_N`) at canonical
offsets, always image-encoded (libstdc++ stores a tuple in reverse); `tuple<>` is rejected. `multimap`/
`unordered_multimap` are a `vector<pair<K,V>>` sorted by key, duplicates kept (schema kindspec
`multimap<K,V>`, layout kindspec `vector<dyn<L:…>>`). Every other `std` class (`bitset`, `complex`, `expected`,
`priority_queue`, `chrono::*`, …), every pointer and every smart pointer is rejected at compile time with a
diagnostic that names the rule and the alternative (`as_flat<T>`, `type_properties<T>`, `optional<T>`). A `std`
class that was accepted before this rule only because it happened to be an aggregate had a hash that is not
preserved; that is recorded as the one exception to P-3. — *implemented 2.2.3* — *previously defined at:*
plans-for-v2.2.3 D1/S12/Q2/Q3/Q4; maintainer 2026-10-13.

**A-18 `schema_string_v<T>` / `layout_string_v<T>`.** The exact strings the two hashes are computed from, as
`std::string_view` constants (one copy per type per executable; nothing is rebuilt at run time). —
*implemented 2.2.3* — *previously defined at:* plans-for-v2.2.3 N1.

**A-19 `print` / `println` are the print-frame writers.** `namespace sr = es::serreg; sr::println(out, "…"_, …)`
reads like `std::println`; unqualified `println(out, …)` is found by ADL through the sink argument; the first
parameter is constrained by `sink`, so neither collides with `std::print`/`println`. `serialize_print` /
`serialize_println` (and `_with`) are gone: the rename is the one source-incompatible change of the 2.2 line,
made while the API is unfrozen (G-2 covers the ABI, not the spelling) and recorded here. `deserialize_print` /
`deserialize_println`, `on_print` / `on_println` are unchanged. — *implemented 2.2.4* — *previously defined at:*
maintainer 2026-10-18.

**A-20 `serialize_options::canonical_unordered`.** Default `true`: an unordered container is sorted by key on the
wire (identical values ⇒ identical bytes). `false`: written in its iteration order — no sort, no allocation above
64 entries; same value, possibly different bytes. Every reader accepts both (the reader's container decides its
own order; a duplicate key is still `invalid_value`). An ordered container is unaffected; hashes are unaffected.
Writer-info key 9 stays reserved for a sink that fixes the option per file. — *implemented 2.2.4* — *previously
defined at:* 2.2.1 suggestions (A-16c); maintainer 2026-10-18 (option: serialization policy, readers take any
order).

**A-21 Evolution projection is a typed-reader feature.** The dictionary-driven readers (`srdump`, `walk_values`
without types, the Python reader) read a frame of any version generically from its dictionary and do not
project; Python parity covers that generic read, not projection. — *decided 2026-10-18 (Q6: a)* — *previously
defined at:* plans-for-v2.2.3 Q6.

**A-22 Registration by namespace: `ES_SERREG_REGISTER_NAMESPACE(ns[, selector])` / `register_namespace<^^ns, Select>`
(`register_namespace.hpp`) registers every serialisable class and enum type of a namespace — nested namespaces
included — before `main()`, with no `serialize()` call site.** The walk is `std::meta::members_of` at the point of
instantiation (declared-so-far, this TU; documented); the default selector takes all types, name selectors
(`name_prefix`, `name_suffix`, `name_glob`, `qualified_glob`), `annotated<A>` (`[[=es::serreg::message]]`) and the
combinators (`all_of`, `any_of`, `not_`) narrow it; `selected_type_names<…>()` shows the selection at compile time so a
user dumps everything first and then selects a sub-group with a pattern. Types the library cannot serialise are
skipped silently (`detail::supported_type<T>`, `check_supported()` as a predicate); `[[=skip]]` on a type excludes
it from selection and registration. Registration goes through the same `detail::touch<T>()` as `serialize()`:
no new registry path, no run-time cost. `register_types<Ts…>()` (A1) stays the per-type spelling. — *implemented
2.2.5* — *previously defined at:* roadmap §5.8; maintainer, 2026-10-19.

## C — Code specification

**C-1 Output with `std::print`/`std::println` only; library diagnostics through `detail::report_line`; every `main()`
parses the command line and runs an application class.** — *implemented 2.1.9* — *previously defined at:*
directions-log A25; AGENTS.md §4.

**C-2 Include rules.** Package headers before system headers; every header self-contained; `#pragma once`; enforced
by `.clang-format`, `tests/headers` and `scripts/check-includes.py`. — *implemented 2.1.13* — *previously defined
at:* spec §0.00 I.

**C-3 clang-format 22 is the reference formatter; the tree is clean under versions 18–23; `scripts/format-all.sh`
before every commit.** — *implemented 2.1.9* — *previously defined at:* directions-log A25.

**C-4 Warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
-Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
-Wmisleading-indentation` (not `-Wnull-dereference`: false positives at -O2) (+ `-Wduplicated-cond -Wduplicated-branches -Wlogical-op` on GCC), with `-Werror` on by
default (`ES_SERREG_WERROR=ON`; every preset, every target including tests and examples).** — *implemented 2.2.1* — *previously defined at:* spec §3;
maintainer 2026-10-07 ("add the flags -Wall -Wextra -Wshadow -Werror and any other that you find useful").

**C-5 Nothing on the hot path allocates or locks; `constinit` globals; `[[gnu::always_inline]]` fill lambdas so
`serialize()` leaves no call at -O2 (checked by `serialize_inlines_at_O2`).** — *implemented 2.1.12* — *previously
defined at:* review C-08.

**C-6 No new dependencies; in-house minimal parsers (TOML subset for `--config`, JSON/TOML writers); Catch2 v3 for
tests only.** — *implemented 1.0* — *previously defined at:* spec round 2 Q5 (FR-D9), §10.1; directions-log A11.

**C-7 Byte-level format changes refreeze `tests/golden/blobs.cpp`, the golden fixtures and the canary together with a
`format_minor` bump and a CHANGELOG line — never silently; a run is never made green by deleting, disabling or
loosening a test.** — *implemented* (rule) — *previously defined at:* CLAUDE.md; test protocol.

**C-8 Version bumps only through `scripts/bump-version.sh`; versioned deliverables only
(`ser-reg-project-vX.Y.Z.tar.gz`, built by `scripts/make-release.sh`); every new file classified in
`public-files-list.txt`.** — *implemented 2.1.11* — *previously defined at:* CLAUDE.md.

**C-9 Executable-format seam.** Image identity and the embedded section live behind `src/image.hpp` /
`detail/embed_*.hpp` (ELF and `none` backends) so a PE backend is a new file, not a rewrite. — *implemented 2.1.8* —
*previously defined at:* directions-log A19.

**C-10 Spec §11 defaults are used as written** (sentinel value, `uint16_t` since/deprecated, TOML subset, 100 ms
signal poll — deviated to a `sigwait` thread and recorded, abort diagnostic text); a wrong default is proposed in a
session report, never silently diverged from. — *implemented* — *previously defined at:* spec §11; CLAUDE.md.

**C-11 Main-program registrations are viewed, not copied.** The executable is never unmapped before exit, so its
ENTRY blobs, names and descriptors are referenced in place by the registry; only other images' entries are copied. —
*implemented 2.2.1* — *previously defined at:* this release (G-5 measurement).

## G — General (scope, versioning, platform, community)

**G-1 Toolchain: GCC 16 with P2996 (`-std=c++26 -freflection`) is the reference; clang-p2996 + libc++ is the
cross-check lane; Linux x86-64 ELF first; CMake 4.** EDG rejects `template for`; clang-p2996 + libstdc++ is
unavailable (libstdc++'s `<meta>` is GCC-intrinsic-only). — *implemented* — *previously defined at:* spec D4, D9, §3;
CLAUDE.md lanes.

**G-2 Versioning: the minor is the compatibility unit until 3.0.** SONAME `major.minor` (`libserreg.so.2.2`),
`find_package` compatibility `SameMinorVersion`; a wire-format or ABI change bumps the minor; patch releases never
change bytes (exception recorded in P-20). After 3.0: semantic versioning with an additive-only wire format (P-19). —
*implemented 2.2.1* — *previously defined at:* directions-log B-N7; response F; maintainer 2026-10-07 ("ok - a").

**G-3 Flat first, dynamic designed up front.** v1 shipped trivially-copyable types with the dynamic kinds already in
the dictionary schema; 2.0 added the canonical image without moving a flat hash. — *implemented 1.0 / 2.0* —
*previously defined at:* spec round 1 #2/#11, D1; directions-log A1.

**G-4 `es::ipc` is its own header-only C++23 library** with its own `ring_version` and gates; `es::ipc_old` (the
earlier fixed-slot rings) is kept unused. — *implemented 2.0.5 / 2.1.13a* — *previously defined at:*
directions-log A18, B-N2; maintainer Q1 (2.1.13a).

**G-5 Registry budgets: 10 000 entries / 10 MB / 10 ms and 100 000 entries / 100 MB / 100 ms (push + publish, release
build), measured by `tests/bench/registry_measure` and asserted under the `bench` label; the architecture document
lists the optimisation options a program with many types has.** — *implemented 2.2.1* (the 10k budget is met in the
sandbox; the 100k one measures 140 ms on the 2-vCPU sandbox and is to be confirmed on the reference box; the scoped
delta export parses the full image once per delta — O(registry), acceptable at dlopen rate) — *previously defined at:* directions-log B-N5; response K;
maintainer 2026-10-07 ("add measurement app, and document optimization options").

**G-6 Windows, macOS, aarch64: not before 3.x (x ≥ 1).** Linux x86-64 is the only platform through 3.0; the
portable wire mode (P-18) is designed in 3.0 so that the first port needs no format change. Order of ports to be
decided then; `docs/windows-porting.md` holds the plan; exit detection on Windows is moot since A-3. — *deferred
(maintainer 2026-10-18)* — *previously defined at:* directions-log B-O12/O13.

**G-7 License: MIT.** The project stays under MIT as a stand-alone repository; the license may change if the
library is submitted to a target that requires another (BSL-1.0 for Boost, Apache-2.0 WITH LLVM-exception for
Beman) while there is one copyright holder, and `README.md` says so. — *decided 2026-10-18 (maintainer: option c)*
— *previously defined at:* directions-log B-O20.

**G-8 Layering and names for submission: layer now under neutral names (`fingerprint`, `serialize`, `registry`,
`tools`, `ipc`), rename at submission.** — *open* — *previously defined at:* directions-log B-O21.

**G-9 Provenance of AI-assisted code: ask the target's maintainers early; record provenance from now on.** — *open*
— *previously defined at:* directions-log B-O22.

**G-10 An SG7 experience paper on canonical type identity with P2996 and load-time registration, aimed at a 2027
mailing.** — *open* — *previously defined at:* directions-log B-O14.

**G-11 Public repository contents: the `public-files-list.txt` set; CI lanes gcc and clang-p2996 + libc++;
OSS-Fuzz later.** — *open* (no repository action yet) — *previously defined at:* response J; directions-log B-O3.

**G-12 Executed assumptions ratified:** the next feature after T0–T3 was `pmr::string` then evolution; writers may
change the bytes of formerly non-canonical frames; a release counts only after a reference-box full run; a one-line
diagnostic for two copies of libserreg; one tranche per release. — *implemented* — *previously defined at:*
directions-log A.5 / B-O18.

**G-13 Wireshark 4.2+ with parity tests on both Lua 5.2 and 5.4; 5.2 is dropped when 4.2 leaves the major
distributions.** — *implemented 2.1.11* — *previously defined at:* directions-log B-N4.

**G-14 One place for decisions: this file.** The O/N numbering and the A–S lettering are history; new decisions get
the next number in their section here. — *implemented 2.2.1* — *previously defined at:* response R1/B3; maintainer
2026-10-07.

**G-15 2.2.x are patch releases on format 1.7; 2.3.0 carries format 1.8 and the rest of evolution level 1; 3.0 is
the frozen public contract (P-18, P-19, A-13, A-15, G-7, G-8).** Level 1 for flat types ships in 2.2.3 (R-9) with
no wire change; 2.3.0 adds the additive dictionary records that need a format minor (`renamed_from` metadata,
constraint records of `docs/values-validation.md`), the remaining level-1 readers and tools, and A-15. —
*to be implemented* — *previously defined at:* release-state-2.2.0 §4/§5; maintainer 2026-10-07 (L4 b);
maintainer 2026-10-13 (Q1).

**G-16 The registry's definition of a conflict and its policy: same schema hash, different layout, both loaded →
the first definition stays, the later one is reported (`ES_SERREG_CONFLICT=warn|abort`), and a loaded definition
takes over when the kept one unloads.** — *implemented 2.1.13* — *previously defined at:* spec FR-R4; response A.

**G-17 `es::ipc_old` leaves the build: from 2.2.6 on, nothing compiles, tests or installs the fixed-slot rings**
(`include/es/ipc_old/`, the `es::ipc_old` target, `serreg_ipc_old_tests`, the `2.0.5 ipc_old` huge-page test — the
one SKIP row of every run since 2.1.13a). The headers, the old suite and `ipc_compare_bench.cpp` go under
`remove-me/` for one release and are deleted in 2.3.0; `es::ipc` is the only ring library (G-4). The four region
cases of the old suite (tmpfs, huge pages, I-06, I-08) test es::ipc's `region` and moved into `serreg_ipc_tests` as
"ipc region: …" — the huge-page SKIP row stays, now under its real owner. — *implemented 2.2.6* — *previously
defined at:* maintainer, 2026-10-18 (after the 2.2.4 Fedora run), 2026-10-19 (after the 2.2.5 run).

## T — Testing and tooling

**T-1 The verification protocol is one script** (`scripts/run-all-tests.sh`: every preset, format and include
checks, srdump checks, fuzz, Wireshark parity, bench, install + consumer check; `--quick` for a smoke run); every
stage logs under `logs/serreg-tests-<stamp>/`; a SKIP is reported as unverified; a release counts only after a
reference-box full run. — *implemented 2.1.11* — *previously defined at:* AGENTS.md §6; `docs/test-and-log-protocol.md`.

**T-2 Golden tests lock what the compiler decides:** canonical names and hashes (`hashes.cpp`), ENTRY blobs
(`blobs.cpp`), the byte-exact fixtures (`orders.*`, `dynamic.*`, `features.ser`), the format canary that decodes
`orders.rri` from literal offsets with no library header. — *implemented 1.0 / 2.2.1* — *previously defined at:* spec
§10.2; CLAUDE.md.

**T-3 The Compiler Explorer examples (eight since 2.2.6) are part of every build and test run:** each compiled as one translation
unit against its own header symlinks with `-O1 -W -Wall -Wextra -Wshadow -Werror` and run (ctest label `ce`); the
lanes on godbolt (`ce-run.py`) remain the cross-compiler gate for canonical-string changes. — *implemented 2.2.1* —
*previously defined at:* directions-log B-O3; maintainer 2026-10-07 ("add to your build / testing phase compiling
all the 5 ce-examples").

**T-4 Fuzz targets for every reader (decoder, RRI, ELF, JSON/TOML, capture) under ASan/UBSan, seeded from the
corpus; the never-crash contract is what they test.** — *implemented 2.1.16* — *previously defined at:* R-2.

**T-5 Wireshark parity: the generated dissector and srdump must agree on every record of the fixtures, under tshark
and under plain Lua 5.2/5.4 stubs; the heuristic recognises frames by call hash and the 1.7 meta sentinels.** —
*implemented 2.1.11 / 2.2.1* — *previously defined at:* G-13; review T-01/T-02.

**T-6 A pure-Python reader with semantic parity against `srdump --json`** (`python/serreg`, stdlib only; ctest
`python_reader_parity` over every fixture and a generated log capture): the second, independent reading of the
format that R-1 requires. — *implemented 2.2.1* — *previously defined at:* directions-log B-O1 consequences ("a
second reader (Python/Rust)"); maintainer 2026-10-07 ("stand alone python reader to 2.2.1").

**T-7 `srdump --cxx` generates C++ types and a `frame_router` skeleton from any dictionary source, self-checked by
hash `static_assert`s; the generated code is compiled and run in the test suite.** — *implemented 2.1.16* —
*previously defined at:* response A5.

**T-8 `srdump --info`, `--resync`, `--level`, `--since/--until`** are the 1.7 tool views (header, chain and
checkpoints by seeks; recovery; the log view). — *implemented 2.2.1* — *previously defined at:* P-9..P-15.

**T-9 Benchmarks are gating only as functional smoke tests; numbers come from the reference box
(`scripts/bench-hrtt-*.sh`, `registry_measure`), never from the sandbox.** — *implemented 2.0.4* — *previously defined
at:* tests/bench/CMakeLists.txt; CLAUDE.md.

**T-10 Link, dlopen (keep, reload, changed layout, conflict take-over, churn) and TSan runs are mandatory for any
change to the registry or the image backend.** — *implemented 2.1.13* — *previously defined at:* CLAUDE.md working
style.

**T-11 Sanitizer and TSan presets are part of the full run; a test that flakes is fixed in the test or the code, never
retried into green.** — *implemented* — *previously defined at:* `docs/test-and-log-protocol.md` §3.

## D — Documentation

**D-1 The flattened set:** `DECISIONS.md` (this file), `ser-reg-requirements.md`, `ser-reg-architecture.md`,
`ser-reg-testing.md`, `ser-reg-stories.md`, `ser-reg-rebuild-prompt.md`; written from the current state, history
removed; the earlier spec and plans stay as history, the superseded reviews and ledgers are kept under
`remove-me/` for one release. — *implemented 2.2.1* — *previously defined
at:* maintainer 2026-10-07.

**D-2 The rebuild prompt is generated:** `scripts/make-rebuild-prompt.py` assembles `ser-reg-rebuild-vX.Y.Z.md`
from the flattened documents, the format header and the golden values, so an agent can rebuild the library from
one file; the two earlier rebuild prompts are retired (kept under `remove-me/`). — *implemented 2.2.1* — *previously defined
at:* response S; maintainer 2026-10-07 ("9. C but name the file docs/ser-reg-rebuild-prompt.md and provide a
script").

**D-3 The book (`book/`) describes the code file by file and is checked by `scripts/check-book.py` (citations by
content; prose markers); symbol citations for prose, line citations only after quoted code.** — *implemented
2.1.12* — *previously defined at:* response M.

**D-4 Every release ends with: verified (and how) / not verified / deviations / next step, and a test report in
`docs/test-reports/`.** — *implemented* — *previously defined at:* CLAUDE.md; test protocol.

**D-5 A written wire specification for 3.0 (not `format.hpp` comments)** — the Python reader's `format.py` and the
architecture document's byte tables are its draft. — *to be implemented 3.0* — *previously defined at:* directions-log
B-O19.
