# Deviations from `ser-reg-requirements-v3.md` — 2.2.6

Recorded per AGENTS.md §1. Each item says what the code does where it differs from the spec, and why. **Trust the
code first, this file second, the spec third.** `include/es/serreg/format.hpp` is the wire contract where it
disagrees with the spec; `docs/plans-for-v2.0.md` §2 is the dynamic-image contract where it disagrees with spec
§6.5; the maintainer decisions after v3 are in `docs/DECISIONS.md`.

**Upkeep:** this file is kept in step with each release; a release's `CHANGELOG.md` entry lists the bullets it
changed. A bullet that says "not implemented" describes the release named in the title, not a permanent state.

## Wire format and hashing (format.hpp is normative)

- **XXH3-64 (spec §0.0 D1, D2).** Every identity is XXH3-64 (`hash_algo` 2); FNV-1a-64 is not supported
  and files of minors 1.0–1.5 are refused (`bad_version`), not read (readers accept `format_minor_oldest` 6 to
  `format_minor` 7). A FILE header `u64` (reserved in the spec) and
  an RRI `u64` at +32 carry the writer's ABI tag; the RRI header is 40 bytes (spec §6.7 says 32).
- **Dynamic record prefix** is `u32 len, u32 0` (8 bytes, keeps the image 8-aligned), not the `u32 len` of spec
  §6.5. Dynamic members are the reference slot `{u32 off, u32 n}` for all four kinds (plan v2.0 §2); the spec §4.7
  sketches (`optional = {u8 has, pad, T}`, `variant = {u16 index, max alternative}`) did not ship.
- **Canonical images are not ABI-free.** Nested *flat* members inside an image are native bytes (`sizeof`,
  `memcpy`), and `long`/`wchar_t` follow the host. The ABI tag (FR-W4) records the writer's ABI; typed reads
  across ABIs fail by schema hash, dictionary-driven readers decode by the file's FIELD sizes and report the
  difference (they do not refuse). A top-level `wchar` argument is sized from the tag.
- **Readers of images apply one set of slot rules** (review G2-6): `n == 0 ⇔ off == 0`, blocks after the
  fixed part and inside the image, a byte budget of the image size. That rejects some images no writer of this
  library produces, which the dynamic-image plan had not ruled out; it is a reader tightening, not a format change.
- **ENTRY/RRI details**: the ENTRY header carries a `u8 version`; there are no alias/image tags; enum bodies carry
  `layout_hash/size/align`; FIELD stores `name_len` first and the name bytes after the fixed part; there is no index
  footer.
- **Literals in compact frames occupy 0 bytes** (FR-L6, format 1.1): the call signature names them.

## Registry and loader (FR-R*)

- **FR-R6/R10/R11 (decision A)**: `push()` copies every registration into a heap entry (blob and name for
  every image, a deep copy of the descriptor for every image other than the main program, whose descriptors are
  used in place). Reads are lock-free over a published, immutable index of views into those entries, with one
  `std::flat_map` per kind for the lookup; indexes and entries are never freed before exit, so every pointer the
  registry hands out stays valid for the life of the process. `dlclose` never aborts: guard destructors drop
  references, and an entry without references is *retained* (still listed and found, refs 0); a reload revives it.
  This replaces the spec's abort (FR-R10) and `ES_SERREG_ALLOW_DLCLOSE` unlink mode (FR-R11);
  the variable is ignored with one stderr line. **Publication is deferred:** a change only marks the index
  stale (and bumps `version()`); the next reader publishes one index for all changes since the last one, and runs
  the `on_change` callbacks once for it on its own thread (`registry::refresh()` does it at once). Startup
  publishes one index, a `dlopen` one more (`registry::generations()`; ctests `registry_scale_one_index_for_startup`,
  `dlopen_A`–`C`). **Not done:** retired indexes are kept until exit, so a process that loads and unloads plugins in
  a loop keeps one index per publish (an index is about 80 bytes per entry, no blob copies); reclaiming them needs a
  reader handle (open question). Retained entries are bounded by the distinct definitions ever loaded.
- **FR-R4**: `serialize()` never returns `error::schema_conflict` — there is no registry access on the hot path, by
  design; do not "fix" this with a lock. Conflicts are reported at load time (`ES_SERREG_CONFLICT=warn|abort|silent`)
  and by `registry::conflicts()`/`has_conflict()` and the tools. Literal and call id collisions in a
  dictionary are recorded too (`dyn::dictionary::id_conflicts`).
- **FR-R10**: libserreg does not interpose `dlopen`/`dlclose` (an earlier design did, with `thread_local` depth
  counters and a re-armed `atexit` marker, to tell an unload from process exit, and had holes with
  `RTLD_DEEPBIND`, `-Bsymbolic` and exit ordering). Consequence for notifications: a registration cannot
  know whether it runs under the loader lock, so `on_change` callbacks are delivered when the next index is
  published, by whichever thread reads first (FR-R11). A callback whose code lives in a plugin must be removed
  (`remove_on_change`) before that plugin is unloaded; the registry cannot protect the plugin's own code, objects or
  threads. `registry::unload_guard_active()` stays as a diagnostic (can the image unload at all: not the main
  program, not `DF_1_NODELETE`).
- **FR-R9**: `ES_SERREG_DUMP_ON=exit|change|both` (default `exit`; the spec's `init` is `exit` in practice — the
  library cannot observe "all static initialisation done" before `main()`). Signal dumps use a `sigwait` thread, not
  a 100 ms poll (§11 default changed): the signal is blocked at library load (`serreg_init`), and the
  thread and the exit hook start there too. `pthread_sigmask` is per thread, so load libserreg before creating threads.
- **FR-R12**: `images()` lists the images of entries with loaded registrations (refs > 0), one image per entry (the
  first registering image while it is loaded, then another loaded one); an image that only contributed aliases is not
  listed. `program_hash()` covers loaded types only, so it returns to its earlier value after a plugin unloads.
- **Executable-format backend (A4)**: image identity is in `src/image_elf.cpp` behind `src/image.hpp`; the section copy is in
  `include/es/serreg/detail/embed_elf.hpp`. CMake `-DES_SERREG_IMAGE_FORMAT=none` (static library only) selects
  `src/image_none.cpp` and `detail/embed_none.hpp`: one image, no descriptor copies, no `es_serreg_dict` section (offline
  tools need an `.rri`). No PE/Mach-O backend exists; `tools::extract_embedded_dictionary` returns
  `error::unsupported` for those formats.
- **One registry per process** is guaranteed only with the shared `libserreg`. A static `libserreg` in a host and in
  a `dlopen`'ed DSO gives two registries (the DSO's dies with it); the second copy (review Q10) prints
  `es::serreg: two copies of libserreg in one process (…)` when it starts, unless its marker is not in the
  global scope (a static executable without `-rdynamic`). The marker is a function
  (`es_serreg_registry_marker_v3()`), not an exported object: two copies of one exported object are an ODR
  violation that AddressSanitizer aborts on.
- **Library diagnostics** are written with `detail::report_line` (`sink.hpp`: `std::format_to_n` into a stack
  buffer, then `write(2)`), not `std::println`: they run in destructors, in libserreg's load-time constructor and
  inside `dlclose()`, in processes that may have closed stderr, where `std::println` would allocate and throw.
  Tools, tests and examples use `std::print`/`std::println`; nothing uses the `printf`/`puts` family.
- **Per-image entities** (`descriptor_v`, `call_desc_v`, `registrar*`, blob arrays) have hidden visibility, and the
  `es::serreg` target adds `-fno-gnu-unique` (PUBLIC): `STB_GNU_UNIQUE` symbols make glibc mark a DSO
  `DF_1_NODELETE`, which would silently disable `dlclose` — and plugin reloading (the old code would
  stay mapped). The flag and `-ldl` belong to the ELF
  backend only (`ES_SERREG_IMAGE_FORMAT=elf`, the default); with `none` the target and `serreg.pc` carry neither
  (and `serreg.pc` adds `-DES_SERREG_EMBED_NONE`). The preset `gcc16-static-none` builds and tests that
  configuration (tests that read the `es_serreg_dict` section use the registry's RRI there, or check that no
  section exists).
- **`es_serreg_dict`** holds one TU-local copy of each blob per translation unit that instantiates the type
  (~250 B per type per TU, linear in TU count): GCC puts every COMDAT variable naming one custom section into a
  single section with the first symbol's group, which breaks multi-TU links. Readers deduplicate by hash. The
  pointer-table design that removes the copies (review G2-17) is **not done**.

## Reflection and structure

- **`template for`** is used in two places — over the template arguments of a canonical name (`reflect.hpp`
  `canonical_name`) and over the enumerators of an enum (`enumerators_fit`, the u16-count and value checks). Member iteration uses `index_sequence` expansion
  over `define_static_array(...)[I]` (the §6.1 fallback), which keeps the dependency on P2996 companion papers small.
- **Reflection outside `reflect.hpp`**: `registry.hpp` (`touch_components` splices member types to register
  components) and `blob.hpp` (includes `reflect.hpp`). Spec §9 asked for reflection only in `reflect.hpp`.
- **File layout** differs from spec §9: there are no `annotations/dispatch/visit/platform.hpp` (their content is in
  `reflect.hpp`, `deserialize.hpp`, `tools.hpp`); added headers are `capture.hpp`, `dictionary.hpp`, `tools.hpp`,
  `walk.hpp`, `print.hpp`, `literal.hpp`, `detail/embed_{elf,none}.hpp`, `detail/blob_bytes.hpp`,
  and the `es::ipc` headers; `src/` is `registry, image_{elf,none}, dictionary, elf_reader, file_io, reader, decoder,
  formatters, diff, cli` (`cli`, `diff` and `wireshark` form the second library `es::serreg_tools`, and
  `es::ipc` is a third, header-only library; decision G). `tests/` has `golden, unit, headers, link, dlopen,
  negative, canary, fuzz, bench, probe, support, fixtures`.

- **The never-crash contract and the windowed readers (§0.00000; FR-D5, FR-D11).**
  - **Windows are read, not mapped.** The 10 MiB window is filled with `pread`/`read` (the block reader, the
    default for files too); `mmap`-ed windows with `madvise` are not implemented — the whole-file mapping
    (`mapped_file`, `file_options{.blocks = false}`) remains for callers who want zero-copy over a file that fits
    the address space. Measured difference is a copy per window; correctness and memory bounds are the same.
  - **`decode_limits` cover the readers, not `es::ipc`.** The ring library validates its headers by its own rules
    (`docs/ipc/ipc-design.md` §5) and has no notion of the process-wide limits.
  - **The enum range check is off by default and typed-only.** It is a compile-time policy
    (`ES_SERREG_VALIDATE_READ=1`, DECISIONS A-16); the enum ENTRY carries the `fixed` flag (`fmt::enum_flag_fixed`;
    `dyn::type::enum_fixed`, `"fixed"` in the JSON/TOML dumps, `enum_fixed` in `type_desc`) but no dictionary-driven
    reader (`verify`, `walk_values`, `srdump`) uses it: those print an out-of-range value as a number.
  - **A-16's "per translation unit" is corrected to "one value for the whole program"** (2.2.6, review B3; a
    wording correction, not a reopening). `ES_SERREG_VALIDATE_READ` and `ES_SERREG_LOG_MIN_LEVEL` are
    whole-program switches set by the build, never in a header: `validate_read_enums` is an `inline constexpr`
    that inline functions of every TU branch on, so two values in one program would be an ODR violation. A TU
    built with a value other than the one `libserreg` was built with fails to link (an undefined
    `validate_read_build<V>`), which makes the mismatch loud instead of silent. The CMake option
    `ES_SERREG_VALIDATE_READ` sets it for `libserreg` and, through the exported target, for every consumer.
    `serreg_tests` (built with the check on against the default library) supplies the `<1>` instantiation itself,
    because the library's own objects instantiate none of the switched templates. `ES_SERREG_LOG_MIN_LEVEL` has the
    same shape (`logger::enabled()` is inline) and no fuse yet: `ce-example7` documents a per-TU floor, so adding
    one is a maintainer decision.
  - **`copy_flat` keeps the in-place `bool` repair** (memcpy, then patch the byte; review B8): the ASan/UBSan lane
    runs "G2-12 a bool byte other than 0/1 reads as true" with `-fsanitize=bool` (part of `-fsanitize=undefined`)
    as the standing check; a copy buffer for `sizeof(U) <= 256` is the fallback if it ever fires.
  - **`const` and `volatile` members are rejected** (review B4; `member_field`'s `static_assert`): a reader would
    have to modify a `const` subobject. `[[=skip]]` members are exempt because they are never instantiated; for a
    *flat* type the reader still `memcpy`s the whole native image, so a skipped `const` member of a flat type is
    overwritten with zero on read — the one remaining case, left as designed (a flat type with a skipped `const`
    member is rare; use a dynamic member or drop the qualifier).
  - **A dump of a pipe still reads it whole** (within `decode_limits::input_bytes`, 1 GiB): the text dump needs the
    dictionary before the frames and a pipe cannot be read twice; a file or `--dict` avoids it.
- **Containers (§0.000000; FR-A6, FR-W9).**
  - **The canonical order sorts with `operator<` when the key has one, else by the bytes of the key's flat
    image** (`es::serreg::key_less`): the plan (`docs/plans-for-containers.md` §3) proposed byte order for every
    key type. With `operator<` an `std::map` and an `std::unordered_map` of the same values encode identically
    without re-sorting the ordered one, and a key whose `<` is not the byte order (a signed integer, a
    floating-point number, a string) iterates in its natural order in the decoded `std::map`. A key type with
    neither `operator<` nor a trivially copyable image is refused (`static_assert`). A custom comparator that is
    not `std::less<K>` enters the canonical name of a transparent type (never a member's kindspec) but not the
    order: a map ordered by `std::greater` is sorted by `operator<` on the wire like an unordered one, and decodes
    correctly into any container of that shape (`emplace_hint(end())` with a wrong hint is only slower).
  - **Floating-point keys are ordered by `std::strong_order`, everything else by `<`** (2.2.6, review B2):
    `double` satisfies `std::totally_ordered` syntactically, but `<` is not a strict weak order once a NaN key is
    present (an unordered container admits one), which is undefined behaviour for the sort. `strong_order` is
    IEEE `totalOrder`: `-0.0` before `+0.0`, NaNs ordered by payload, the same bytes for the same multiset of
    keys. Integer, string and user-defined keys keep `<`, so no existing golden file moved; a user-defined key
    must provide a strict total `<`. `key_less` is `noexcept` only when the key's `<` is.
  - **Sorting an unordered container allocates above 64 entries.** Up to 64 entries the index of pointers lives
    on the stack; above it a heap buffer is used — the same documented exception as the > 16 KiB image on plain
    sinks. `serialize_options{.canonical_unordered = false}` (DECISIONS A-20) switches the sort off for a call:
    the container is written in iteration order, no sort, no allocation, and the same value may give different
    bytes; every reader accepts both orders.
  - **`serialize_noexcept` is false for an argument that reaches an unordered container or a multimap**
    (`std::multimap`, `std::unordered_multimap`; 2.2.6, review B1): the canonical sort may allocate above 64
    entries, and a `noexcept` `serialize()` into a claiming sink (`span_sink`, `es::ipc::sink`) would turn that
    `bad_alloc` into `std::terminate`. The `noexcept` specification is narrower than before for those arguments only;
    flat, scalar, string, literal and ordered-container arguments into a claiming sink stay `noexcept`.
    `put_record`, `put_value_record` and `serialize_frame`'s fill carry the same conditional specification, so the
    `bad_alloc` propagates; `canonical_unordered = false` avoids the allocation at run time but not the
    specification. The multimap walk sorts `(entry, original index)` with `std::sort` and the index as tie-break —
    stable without `std::stable_sort`'s buffer, same bytes.
  - **Multimaps are a sorted `vector<pair<K,V>>` with duplicates kept, not a `multi` flag** (DECISIONS A-17):
    `std::multimap` / `std::unordered_multimap` have the schema kindspec `multimap<K,V>` and the layout kindspec
    `vector<dyn<L:…>>`. `std::deque`, `std::list`, `std::forward_list` and `std::stack` take the `vector<E>` shape
    (the same layout hash as `vector<E>`, their own name in the schema string). `std::multiset` and
    `std::unordered_multiset` are not in the vocabulary and are refused with the vocabulary diagnostic.
  - **The transparent type's name is the exact container type** (`std::unordered_map<i32,std::string>`), so two
    programs that send the same values in a `std::map` and in a `std::unordered_map` as top-level arguments have
    different call hashes (as they would with two different member types). The dictionary records the layout hash
    equal for both; the evolution rules (2.3) can project one onto the other.
  - **`srdump --cxx` spells every `map` shape as `std::map<K, V>` and every `set` as `std::set<K>`** for members
    (the dictionary records the shape, not the container, so the original `unordered_map` is not recoverable —
    and not needed: the regenerated owner has the same schema and layout hashes and decodes the same frames), and
    the recorded vocabulary name (`spell_std`) for transparent types, where the container is the type.
  - **The Wireshark dissector shows a map entry as `[i]` with `key` and `value` sub-fields** and the whole member
    as `{k: v, …}`; a nested dynamic value inside an entry (a map of vectors) renders through the generic image
    walker and is not individually filterable.
- **`srdump --cxx` (FR-D10).** The generated header reproduces every type whose canonical name the
  `[[=es::serreg::name]]` annotation can carry (letters, digits, `_`, `:`, `<`, `>`, `,`, `-`). A name outside that
  set — a template argument that is an enum value renders as `enum(7)` — is declared, noted at the top of the
  header, and checked by layout hash only. Access (`private`) and comments are not in the dictionary and are not
  regenerated; members come back public. Types from a dictionary written on another ABI compile, but their
  `offsetof` asserts fail on a host whose layout differs — the asserts are the point.
- **Typed readers report how far they got (§0.0000 C; FR-S4).** `deserialize` and the print readers return
  `std::expected<std::size_t, decode_error>`, not `…, error>`: the spec's original signature is replaced. The
  guarantee is the one-pass one the maintainer chose, not the "nothing is written on any failure" reading of the
  original FR-S4: when a dynamic image fails *while it is decoded*, the arguments before it are complete and that
  one is valid but unspecified. `decode_error::actual` and `failed` say which. `dispatch`, `inspect`, the router
  and `deserialize_raw` still return `result<…>` (they deliver records one at a time, so there is no "how many").
- **Constants (§0.0000 O15; FR-A5; format 1.5).** Current limits:
  - **Literal types:** `42_` is `int`, else `long long`, else `unsigned long long` — also for hexadecimal, octal
    and binary literals, where C++ itself would try `unsigned int` first. Use `k<0xFFFFFFFFu>` for another type.
  - **Floating literals** are parsed by the library at compile time: at most 15 significant digits and a decimal
    exponent within ±22 (the range in which one multiplication or division is correctly rounded, so the value
    equals the compiler's). Anything else, and hexadecimal floating literals, are a compile error that says to
    write `k<value>`.
  - **No class-type constants and no `'c'_`:** a class-type value needs a canonical value rendering (planned
    with the log envelope, O16); a character literal reaches a literal operator as a run-time value, so its
    spelling is `k<'c'>`.
  - **`k` is a one-letter name in `es::serreg`.** It was chosen to keep call sites short; it can collide with a
    user's own `k` after `using namespace es::serreg`. `constant<V>{}` is the long form.
  - **Value in the dictionary as `u64` bytes:** JSON and TOML show a constant's value as the hexadecimal `u64` of
    its bytes (`0x000000000000002a`), not as a typed number; the frame renderers show the typed value.
- **Canonical names (§0.000 B; cc review C-03, C-04, C-05, C-07; format 1.4).** Names are
  independent of the compiler and standard library: the golden corpus and ce-example1–4 give identical values on
  GCC 16 + libstdc++ and clang-p2996 + libc++. Remaining limits:
  - **Explicit names can't be checked across programs.** Uniqueness is checked only where two definitions meet:
    the registry reports one name with two layouts as a conflict (FR-R4). Two programs that give different types the
    same explicit name, with equal layouts, cannot be told apart.
  - **Std types outside the vocabulary** use the general rule: std's implementation namespaces are dropped, and
    default template arguments are still spelled out (for example a `std::less<…>` or `std::allocator<…>` argument).
    These names are the same across libraries, but not short.
- **es::ipc_old's gates share cache lines (never fixed).** Its `detail::gate` is 16 bytes and `gates_[i]` is indexed by
  that size, although its layout reserves 64 bytes per gate. SPMC readers of the old slot rings therefore false-share.
  The old rings are out of the build since 2.2.6 (DECISIONS G-17; sources under `remove-me/` until 2.3.0), so
  this is history, not a defect of a shipped component; es::ipc does not have this mismatch.
- **Zero-size elements are rejected (cc review D-05).** A vector, optional or variant element must occupy
  bytes in the canonical image; `std::array<T, 0>` of a dynamic `T` as such an element is a compile error. The
  dictionary format never admitted it.

## Serialisation and reading guarantees

- **FR-S4 holds for flat records only (cc review C-06; decision C pending).** A flat record is validated
  (lengths, bool bytes) before any output is written. A dynamic record is checked against the slot rules *while*
  it is decoded, and a multi-record frame is decoded output by output, so a failed `deserialize(bytes, a, b, c)`
  can leave earlier outputs written and the failing one partly written: a basic guarantee (valid, unspecified
  values), not the strong one the spec states.
- **A refused commit is a failed serialize (C-09).** `claiming_sink::commit` may return `bool`;
  `capture_file` and `es::ipc::producer` do. `serialize()` then returns the sink's error for that frame.
- **Print frames:** nested replacement fields (`{:>{}}`) are a compile error (T-05): no reader renders
  them. A user's `std::formatter<T>` is never consulted by readers (C-14).

## API naming and scope

- **Sinks**: `file_sink` is `capture_file`; there is no `file_sink{.archival = true}` — full records are chosen per
  call: `serialize_with(serialize_options{.full_records = true}, sink, ...)`.
- **FR-R5**: `find` → `find_type`, `write_json/toml` → `to_json/to_toml`; `for_each`, `calls()`, `literals()` are
  served by `snapshot()` (filter on `registry_entry::nk`).
- **FR-S3**: `frame_size<Ts...>()` is exact and constexpr for flat arguments and the *minimum* for runtime strings
  and dynamic types; `frame_size(options, vs...)` is exact for every argument.
- **FR-S5**: `serialize_raw` / `deserialize_raw` accept flat types only. Plan v2.0 §4's sentence about raw *images*
  is not implemented (it would be new API without a call hash; it needs a format note first).
- **FR-S7**: `static_assert` messages do not interpolate the type or member name (P2741 string messages are not
  used yet); the offending member is visible in the instantiation backtrace.
- **FR-S8**: `as_flat<T>` applies to top-level arguments only; a *nested* member whose type has `as_flat` is
  rejected at compile time. `[[=skip]]` works for trivially-copyable-but-unsupported members; the enclosing type
  must still be trivially copyable for the flat path (a skipped `std::string` in a flat type is not supported).
  Skipped members are zero on the wire and never registered.
- **Strings**: `std::basic_string<char, std::char_traits<char>, A>` with any allocator is a string (member,
  argument, output); the allocator is not encoded. Other character types are rejected.
- **Typed decode of odd images (spec §0.0 D3/D4)**: a valueless variant decodes to `std::monostate` when the
  variant has it, otherwise `error::invalid_value` (the rest of the frame is still read); a `bool` byte other than 0/1
  reads as `true` with an optional warning (`set_warning_handler`); `view<T>` over such a byte is `invalid_value`.
- **FR-W7 / FR-W12 (format 1.7)**: dictionary frames carry a **delta** (`rri_flag_delta`: the entries newer
  than the last written registry version) and end with the `u64 next` link; capture files write deltas inline
  before the first frame that needs them and **never back-patch** the opening dictionary at close — the §11
  back-patch default is withdrawn (DECISIONS P-9), and FILE flag bit 2 is read from 1.6 files only. Non-seekable
  sinks still use `dictionary_tracker::update(sink)` from the application's idle path (one relaxed atomic load
  when nothing changed) rather than an automatic frame from `serialize()`.
- **Spec §6.5 `[INDEX]` footer**: never built; checkpoint frames (P-11, FR-W11) replace it because a footer written
  at close does not survive a crash.
- **FR-H5 acceptance window**: readers accept the current minor and the previous one (1.6 and 1.7), not
  "exactly the current minor": 1.7 adds only opt-in features, so a 1.6 file is a 1.7 file without them.
- **G-5 registry budget**: the 100k-entries / 100 MB budget is asserted by `registry_measure_100k` under
  the `bench` label and measures 140 ms on the 2-vCPU sandbox (10k/10 MB: 9.6 ms, within budget); the reference
  box decides. `registry_measure --plugin` (the deep-copy path of a dlopen'd image) is 2× the main-program path.
- **Writer-info keys (P-12)**: `scrub_padding`, `full_records` and `canonical_unordered` are defined in
  `fmt::info_key` but not written by `capture_file`, whose per-call `serialize_options` it cannot know (the three
  are per `serialize()` call, not per file; key 9 stays reserved for a sink that fixes `canonical_unordered` per
  file — DECISIONS A-20).
- **FR-D2/D8/D9**: `--dict` accepts RRI images only (no JSON dictionary reader); `--toml` renders the dictionary in
  TOML and records as JSON lines; `--full` means "do not elide long arrays". TOML subset per FR-D9.
- **FR-D5 / NFR**: the fuzz driver (`tests/fuzz`, a portable mutation driver with seed corpora) is what
  `run-all-tests.sh` runs, under ASan/UBSan where available, else UBSan trap mode, else
  `_GLIBCXX_ASSERTIONS`. A libFuzzer build is optional (`-DES_SERREG_LIBFUZZER=ON`, clang). `tests/bench` holds the
  hot-path ceilings; the half-RTT benchmarks are scripts (`scripts/bench-hrtt-*.sh`).
- **M6 evolution: level 1 for flat types shipped in 2.2.3, the rest is 2.3.0.** Spec §1.3 says "evolution
  semantics — v2+" and §12 puts it in M6 with a v4 spec; there is no v4 spec — the specification is
  `docs/schema-evolution-specs.md` and the decision is DECISIONS R-9. What ships: `projection.hpp` (the
  `renamed_from` annotation, `tools::project<T>`, `deserialize(dict | projection_context, bytes, outs…)`,
  projection by member name, widening, defaults, grown arrays) for flat types. Still to come (2.3.0): dynamic
  members, `frame_router`/`dispatch` by projected types, `walk_values{.project}`, `srdump --diff --compat` /
  `--project`, `renamed_from` in the dictionary. `since`/`deprecated` are stored in the dictionary and in the
  schema string but do not change decoding: adding one to a shipped member moves the schema hash and every call
  hash naming the type while no byte changes (DECISIONS P-21, open, proposes taking them out of the hash in
  format 1.8).
- **Wire format 1.7 shipped in a patch release**: the recorded exception to G-2 (P-20); the next format change
  bumps the library minor.

## Environment and verification

- **Golden hashes are verified on GCC 16.2 locally; the clang-p2996 cross-check (FR-H4, review G2-18) runs on
  Compiler Explorer.** The godbolt.org API is reachable from the sandbox; the amalgamated golden check and the
  `ce-examples` pass on both lanes (`docs/ce-links.md` holds the links, results and the procedure). Earlier text
  recorded the sandbox as unable to reach Compiler Explorer; that is no longer the case. The remaining gap is the
  clang + libstdc++ lane (`CLAUDE.md`), not the cross-check itself.
- **Sanitizers**: the `gcc16-asan-ubsan` preset excludes `-fsanitize=null,nonnull-attribute,returns-nonnull-attribute`
  (GCC 16.2 rejects constant evaluation of `enum_string<E>` with them on). A `/opt/gcc-16` built from source may
  lack `libasan`/`libtsan`; `run-all-tests.sh` then reports those presets as SKIP and runs `gcc16-ubsan-trap`,
  which needs no runtime. Release reports from such a box list ASan and TSan as not verified.
- **Compile time**: descriptor generation is linear in the number of members (the gap table is interval-based); a
  1 MiB buffer member compiles in about 3 s. `tests/probe` bounds a depth-4 nesting at 60 s.
