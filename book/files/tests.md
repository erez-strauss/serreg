# `tests/` — the test tree

The `gcc16-debug` build registers about 370 ctests (372 at the time of writing; `ctest -N` lists them).
`scripts/run-all-tests.sh` runs the `bench` label as a stage of its own; the two `link_image_none_*` tests are built
in Debug only, `gcc16-static` does not build the dlopen tests, and `gcc16-static-none` replaces the ELF-section tests
by `srdump_elf_none_has_no_section` while `tests/link` runs `link_image_none_backend` against `es::serreg`.
`wireshark_dissector_parity` and `python_reader_parity` are disabled or skip where tshark or Python 3 is missing.
The Catch2 cases are discovered when ctest runs (`catch_discover_tests(... DISCOVERY_MODE PRE_TEST)`, B-08), and
every test with a `PASS_REGULAR_EXPRESSION` also has a `FAIL_REGULAR_EXPRESSION` for sanitizer reports (B-09,
`files/build-system.md`). Every test names the requirement or review item it locks (`FR-S1`, `FR-L6`, `gk 1.4`,
`cc F2`, …) in its name or head comment. `tests/CMakeLists.txt` fetches Catch2 v3.8.1 (or uses
`ES_SERREG_CATCH2_DIR`), defines `ES_SERREG_CORPUS_DIR` (= `tests/golden`), `ES_SERREG_TEST_SUPPORT_DIR`,
`ES_SERREG_GOLDEN_FIXTURES`, builds `runtime_selftest`, registers `soname_major_minor`, and adds the
sub-directories below. The executables that test the CLI, the Wireshark generator, the C++ generator or the diff
(`runtime_selftest`, `serreg_tests`, `serreg_fuzz`, `wireshark_print_capture`) link `es::serreg_tools`;
`serreg_ipc_tests` links `es::ipc` alone. The canary expects minor 7, and
the readers accept minors 6 and 7 and refuse 5 and 8. `serreg_tests` is compiled with `ES_SERREG_VALIDATE_READ=1`
(the read-side enum range check on; `test_limits.cpp` exercises it); `serreg_validate_off_tests`
(`test_validate_off.cpp`) is a second Catch2 executable with the default policy, because the policy is per
translation unit.

Run one test: `ctest --preset gcc16-debug -R <regex> --output-on-failure`, or the Catch2 binary directly:
`build/gcc16-debug/tests/unit/serreg_tests "[dynamic]"`.

**Minimal `main()`.** Every test executable that is not a Catch2 binary has a `main` that at most parses
the command line and calls an application class: `runtime_selftest::app{runtime_selftest::options::parse(argc,
argv)}.run()` (the former 650-line body split into member functions, one per area: `dictionary_parsing`,
`rri_round_trip`, `parser_bounds`, `review_2_1_10_decoders`, `dictionary_conflicts`, `capture_verify`, `dump_text_json`,
`json_64bit_integers`, `diff_reports`, `elf_reader`, `write_all_errors`, `cli_surface`, `negative_fixtures`,
`generate_fixtures`), `premain_check::app`, `signal_dump_test::app`, `make_fixtures::app` and
`make_dynamic_fixtures::app` (after the usage check), `rri_canary::app`, `fuzz_driver::app`, `host::app`,
`second_copy::app`, `link_test::app`, `image_none::app`, `hrtt::app<Codec>` (was `hrtt::main<Codec>`),
`probe::app` and `local_in_main::app<Local>`, whose `main` defines the local struct it tests. All output uses `std::print`/`std::println`; no test uses the `printf`/`puts` family.

## `tests/golden` — frozen values (format lock)

### `corpus.hpp`
The shared corpus: every shape the library supports (scalars, padded, nested, bases incl. multiple and empty,
enums, arrays, `[[=skip]]`, `since`/`deprecated`, `as_flat` (`Stamp` → `StampFlat`), anonymous-namespace type,
templates with value arguments `t::W<…>`), the message types `Order`/`Cancel`/`Side`, and the dynamic types `DynMsg`,
`DynNested`, `DynArr`, `DynBase` with `sample_*()` values.

### `hashes.cpp` (compiled into `serreg_tests`)
`STATIC_REQUIRE`s over schema/layout hashes (FR-H1/H4), canonical names (§6.2b), descriptor field tables,
padding-gap tables (FR-S6), frame sizes (FR-S3, including FR-L6's 0-byte literals), the argument
categories and print/println call hashes, and the dynamic types' hashes and fixed layouts. Two cases lock
compile-time constants (format 1.5): "FR-A5 the numeric _ literal and k<V> build constant<V>" (the
type of every spelling: `42_`, `-7_`, `0x10_`, `1'000'000_`, `1.5_`, `2.5e-3_`, `k<true>`, …) and "FR-A5 a
constant's type and value are in the call string and the call hash" (the `const:` items and nine call hashes that
were computed outside the library). Two cases lock enums without a fixed underlying type (decision D): "2.1.16 decision D: an enum without a fixed underlying type takes its kind from its enumerators"
(`has_fixed_underlying_type_v` false for `Plain`, `Neg` and `Big`, true for `enum class` and `enum : T`;
`Plain` → `u32`, `Neg` → `i32`, a value above 32 bits → `u64`; the derived kind equals `std::underlying_type_t`'s
kind for those and for `corpus::Venue` on GCC; the canonical string
`enum<d_rule::Neg,i32>[4,4]{n=-1;p=1;}`) and "2.1.16 decision D: the value range of an enum without a fixed
underlying type" (`enum_ranges_v`: one range per such leaf, the two's-complement bounds, none for a fixed
type). Format 1.6: "2.2.0 golden strings and hashes of the map/set shapes and the transparent types (format
1.6)" — the member kindspecs `map<u16,u8>`, `set<i16>`, `map<string,struct<L:…>>` spelled out, a `std::map` and a
`std::unordered_map` member giving one layout string and hash, the transparent types' names and strings
(`dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}` → `0x02c87ef3a015eb45`, computed outside the library),
`map_entry_layout` for three key/value pairs, and `corpus::DynMap`'s hashes and fixed size 36. "golden strings of
std::deque/list/forward_list, std::pair/tuple, multimaps and std::stack" locks the rest of the vocabulary: the
three sequences as `vector<…>` members and their names, a `std::deque<i32>` argument sharing the vector's layout
hash and not its schema hash, `std::pair<i32,f64>`'s string `dyn<std::pair<i32,f64>>[16,8]{first:i32@0[4,4];second:f64@8[8,8];}`
and hash, a tuple's `_0`/`_1`/`_2` string, a class with a pair member, the `multimap<K,V>` schema kindspec against the
`vector<dyn<L:…>>` layout kindspec, `std::stack<i32>`'s name and layout hash, and
`fingerprint64(schema_string_v<corpus::Order>) == schema_hash_v<corpus::Order>` (the public strings). A failure is a
wire-format change: fix the code, or refreeze with a `format_minor` bump and a CHANGELOG entry.
`serreg_tests "[golden-print]"` prints the current values for a deliberate refreeze.

### `blobs.cpp`
XXH3-64 and size of the exact ENTRY bytes of every corpus type, the `"evt"` literal, the `("evt",Order)` call,
a print call, and the dynamic types — the lock on the ENTRY/FIELD encoding itself (cc F1). Also the
call ENTRY of `("evt", 42_, Order, k<Side::sell>)`: 104 bytes, with asserts on both constant ARGs (`arg_kind` 4,
the `kind` byte, the hash bytes of `fingerprint64("i32")`, the value bytes 42 and 2), byte 33 of every enum blob (the flags byte of format 1.6: `enum_flag_fixed` for
`corpus::Side` and `corpus::Venue`, 0 for an enum without a fixed underlying type), and the `struct_flag_transparent` bit:
set in `std::vector<std::int32_t>`'s blob, clear in `corpus::DynMap`'s.

### `make_fixtures.cpp`, `make_dynamic_fixtures.cpp`, `compare_srdump.cmake`, `compare_program_hash.cmake`
Generators of `tests/fixtures/golden/{orders,dynamic}.*`. `make_fixtures` writes `orders.ser` with 11 frames and
34 records and checks that its registry holds exactly 20 entries (3 types, 8 literals, 9 calls) before it exports
`orders.rri`. Three of the frames carry
constants: `serialize(f, "fill"_, 42_, order, k<Side::sell>, -1.5_)` compact and with full records, and
`println(f, "limit={} on={}"_, 5'000'000'000_, k<true>)`. ctests: `golden_fixtures_generate`,
`golden_dynamic_fixtures_generate` (fixture setup), `golden_fixture_<ext>_byte_exact`,
`golden_fixture_dynamic_<ext>_byte_exact` (regenerated bytes equal the checked-in ones),
`golden_srdump_verify[_dynamic]`, `golden_srdump_<fmt>_matches_fixture` (srdump output equals the checked-in
renderings), `golden_srdump_program_hash_agrees` (capture vs RRI), `golden_srdump_program_hash_elf_agrees`
(ELF dictionary recomputation). `golden_srdump_verify` expects `ok: 11 frames, 34 records`. The
captures are format 1.7 and carry a writer-info frame; `make_fixtures` opens them with fixed options
(`info_host = false`, a fixed clock, `program = "make_fixtures"`) so they stay reproducible, and also writes
`features.ser`: the same 11 frames with a CRC32C trailer on every frame and a checkpoint (with a full dictionary)
every 256 bytes — `golden_fixture_features_byte_exact`, `golden_srdump_verify_features` (`ok: 11 frames, 34
records`: meta frames are not counted) and `golden_srdump_info_features` (`srdump --info`). The writer-info frame
contains `library_version`, so a version bump changes these files. `make_dynamic_fixtures`
writes `dynamic.ser` with 9 frames and 18 records: four of dynamic classes (`DynMsg`; `DynNested` + `DynArr`;
`DynBase` with full records; a `println` with a `DynMsg` and a string), then `("map"_, corpus::DynMap)` — an
ordered map with string keys, a set, an unordered map written in key order, a map of a dynamic value — a
top-level `std::vector<i32>` `{3, 1, 2}` compact, empty, and with full records, and `println("ids={} map={}"_,
ids, map)`; `golden_srdump_verify_dynamic` expects `ok: 9 frames, 18 records`. Every format other
than 1.6 and 1.7 is refused, not decoded ("FR-H5 captures of an older minor or hash algorithm are refused, not
guessed at" in `test_print.cpp`; "P 1.7 the readers still read 1.6 files, and refuse 1.5 and 1.8" in
`test_format17.cpp`).

## `tests/unit` — behaviour

| file | locks |
|---|---|
| `test_roundtrip.cpp` | "B4 a mutable member serialises like any other; a [[=skip]] const member of a dynamic class is accepted and left alone"; FR-S1 typed round trip of every flat shape, FR-L1 literals (all four spellings), FR-L2 literal footprint, FR-S3 sizes, FR-S6 scrub determinism, FR-W2 full records, FR-S5 raw, FR-S8 `as_flat`, error paths; G2-1: hostile compact/full frames whose `frame_len` lies (strings, images, truncated tails, inner length ≠ `payload_len`) refused with no output written, read from a guard-page copy so an over-read faults; "FR-S4 a failure reports expected items, decoded items and the first that failed (2.1.15)" (the `decode_error` of a missing header, a compact string overrun, a full-record hash and literal mismatch, the print readers), "FR-A5 constants: no bytes in a compact frame, the value's record in a full one (1.5)" (`deserialize`, `inspect`, `dispatch`, full records, plain sinks) and "FR-A5 constants through the dictionary: RRI round trip, verify, dump, JSON, print (1.5)" |
| `test_readers.cpp` | `inspect` (lazy range, no record cap), `view<T>` (aligned/misaligned), `dispatch` (routing, unmatched, gk 1.1 short records), 41- and 300-record frames (gk 1.2); "2.1.9 dispatch over more than four types uses the compile-time hash index" (`detail::hash_index` is perfect for twelve `wide::tick<N>` hashes, `find` for present, absent and duplicated keys, a twelve-type `dispatch` delivering five records in order and one unmatched `corpus::Cancel`); "2.1.10 frame_router routes each frame to the handler of its call signature" (seven frames through `router.callback()`: `on(h)`, `on(lit, h)`, `on<Sig...>` with a trailing literal, `on_println`, a dynamic `corpus::DynMsg` twice through the kept storage, one frame to `otherwise`; the counters; sections for `route()` on raw frames — handled, a corrupted image as an error, no handler as `false` — and `on_error` during a walk) and "2.1.10 for_each_frame(path) and walk_values(path): mapped and in blocks agree" (a 100-frame `capture_file`: the same frame offsets mapped and with 256-byte blocks, `not_found` for a missing file; `walk_values(path, visitor)` mapped, in blocks and over a span count the same values; `walk_values<corpus::DynMsg>(path, …)` delivers 50); "2.1.12 T-10 frame_router::route counts like the walk callback and reports decode errors" (`route()` on a handled frame, a frame without a handler and a cut-short record: `handled()`, `unhandled()` and `failed()` are 1 each, `on_error` is called once with the frame's call hash) |
| `test_registry.cpp` | FR-R1/R2 contents, FR-R5 lookups, FR-H3 program hash, FR-R9 export round trip, JSON, version stability; "2.1.16 A1 register_types / register_call register without a serialize call site" (a type, its component type, an enum and a literal named only in a never-called `register_types<…>()`, a signature with a constant in `register_call<…>()`: all found by `find_type`/`find_literal`/`find_call` and listed by `export_image()`) |
| `test_capture.cpp` | FILE container round trip, full records, bare streams with dictionary frames, `extract_registry_from_elf(/proc/self/exe)`, `ES_SERREG_DUMP_REGISTRY_FD` through a pipe (fork) |
| `test_sinks.cpp` | "B1 serialize of a type that may sort an unordered container is not noexcept on any sink" (`may_sort_v`, `serialize_noexcept`); 8 MiB object on a 1 MiB stack (claiming and fd sinks), stop at the first refused write, `state()` propagation, bool normalisation, conditional `noexcept` (G2-7: dynamic / `std::string` outputs and dynamic records to plain sinks are not `noexcept`), `capture_file` sticky errors, `dictionary_tracker`; C-09: "2.1.12 C-09 a claiming sink whose commit() returns false makes serialize() fail" (a `refusing_sink` whose `commit` returns `bool`) and "2.1.12 C-09 capture_file: a frame the file system refuses is reported by that serialize()" (in a forked child with a 64 KiB `RLIMIT_FSIZE` and `SIGXFSZ` ignored, a 1 MiB frame: that `serialize` and `close()` both fail) |
| `test_tools.cpp` (`for_each_frame`; ABI tag) | verify's literal-id checks (full records; compact literals have no bytes), streaming `dump`, JSON 64-bit integers, `diff` verdicts, dictionary conflicts, `--version`, `cli::app`/`parse`/`execute`; FR-W4 ABI tag in the headers, a foreign tag sizing a top-level `wchar_t` and shown by `dump`, mixed tags as `abi_mismatch`; "2.1.16 decision E: --diff --direction decides whether removals or additions are breaking" (a removed type, call and literal and an added call and literal under `old_data`, `new_data`, `both` and `none`: the counts, `breaking_of` each verdict, the `BREAKING (direction …)` line, `(direction none)`, a layout change breaking under `none`, `--direction` through `cli::parse` and its usage error), "2.1.16 A5 tools::cxx_header regenerates the process dictionary's types with their hashes" and "2.1.16 A5 cxx_header spells names a declaration cannot, and srdump --cxx refuses two types of one name" (`[tools][cxx]`) |
| `test_limits.cpp` (FR-D5, A6, decision D) | "2.1.16 A3 decode_limits: defaults, process-wide set/get, and what each one bounds" (the seven defaults, `set`/`get` round trip); "2.1.16 A3 a frame above decode_limits::frame_bytes is a problem, not an allocation" (a 200 MiB `frame_len` after two honest frames: `bad frame_len 209715200`, two frames counted, over a span and over a file; a 32-byte limit makes the honest frame the problem); "2.1.16 A3 the problem list is capped at decode_limits::problems; the rest are counted" (ten unknown calls under a cap of 3: 3 lines, `problems_omitted == 7`, span and file); "2.1.16 A3 decode_limits::dictionary_entries bounds what a dictionary may declare" (the process RRI refused with `corrupt_dictionary` under a cap of 2); "2.1.16 A6 every window size gives the whole-file output, byte for byte" (text, JSON and TOML of `orders.ser` and `dynamic.ser` with windows of 64 bytes — rounded up to the 4 KiB floor — 4 KiB and the default, equal to the span dump; `verify` agrees in frames, records, bytes and `program_hash`); "2.1.16 A6 a frame larger than the window grows it; a 64 MiB capture walks within a bounded RSS" (1024 frames of a 64 KiB string with one 4 MiB frame in the middle, a 1 MiB window: 1025 frames, one above the window, `verify` ok, `ru_maxrss` grows by less than 24 MiB); "2.1.16 A6 srdump reads a capture in windows: --window and --frame-limit" (`--window 4096 --verify` and `--window 10M --json` exit 0, `--window 12` and `--window ten` exit 2, `--frame-limit 32 --verify` exits 1 and sets the process-wide limit, `--frame-limit 64M` exits 0); "2.1.16 decision D: an out-of-range value of an enum without a fixed underlying type reads as 0 and is reported as invalid_value" (`HasPlain{u32, Plain, u32}`: value 3 — inside the 2-bit range, no enumerator — accepted; value 7: `deserialize` returns `{invalid_value, 3, 2, 1}` with the leaf 0 and the later `u32` read, `view` → `invalid_value`, `dispatch` delivers nothing and returns it, `deserialize_raw` reports it) |
| `test_hash.cpp` | FR-H5 XXH3-64 known answers of the xxHash 0.8.3 reference for every length class, in `STATIC_REQUIRE` and at run time; the constant-evaluation and run-time paths agree for lengths 0–299; FR-H3 program hash byte order |
| `test_skip.cpp` | `[[=skip]]` members are never instantiated; a skipped type is absent from registry and ELF section |
| `test_register_namespace.cpp` | A-22 `ES_SERREG_REGISTER_NAMESPACE`: the namespace walk (templates, aliases, incomplete and `[[=skip]]` types excluded; recursion on/off), the selectors and the glob, `supported_type` on a pointer and a polymorphic type, `registered_type_count`, the types in the registry and in `export_image()` before `main()` without a `serialize()` call site |
| `test_anon_tu.cpp`, `test_empty_member.cpp` | `corpus::Local` (an anonymous-namespace type, named explicitly); `[[no_unique_address]]` empty members |
| `test_format17.cpp` (format 1.7; `[format17]`) | "P 1.7 a capture file is written as 1.7: header, CHAIN, writer-info frame, no rewrite at close"; "P 1.7 a registry change between frames writes a dictionary delta inline and links the chain"; "P 1.7 CRC32C trailers: written per frame, checked by every reader, a corrupt frame is skipped" (also the `crc32c` check value in a `STATIC_REQUIRE`); "P 1.7 checkpoint frames: a forward chain by size, a final one at close, tools::info follows it"; "P 1.7 --resync: after a corrupt frame_len the windowed reader continues at the next checkpoint"; "P 1.7 the readers still read 1.6 files, and refuse 1.5 and 1.8" (`orders-1.6.ser`, `orders-1.6.rri`); "P 1.7 serialize_options{.crc} on a plain sink, and frame_size accounts for the trailer" |
| `test_log.cpp` (`[log]`) | the logger over a `scoped_capture_file`: "A log: the gate evaluates nothing below the level; the floor compiles the call out", "A log: the envelope by policy — level, file:line, time, thread id, sequence — renders in srdump" (also `--level`, `--since`/`--until`, `--json`, `frame_router::on_log(h)` receiving every log frame rendered; `on_log(level::warn, h)` + `on_log(level::error, h)` splitting by floor as `tools::log_record`s — envelope fields present, the `info` frame below both floors dropped and counted as handled; a line handler beside a floor; `tools::render_log` on one frame: `level`, `message`, `format`), "A log: the scoped dictionary holds the scope's calls, their literals and types — nothing else", "A log: a logger over a plain sink needs dictionary_policy::full; the frames decode with the registry" |
| `test_to_string.cpp` (`[to_string]`) | `to_string`/`to_json`/`render` and `formattable<T>`: a type registered by `to_string` alone, enums, scalars, 64-bit JSON strings, quoted strings, dynamic members, a container as the value, `std::format("{}")` and `{:j}` |
| `test_print.cpp` | FR-A1..A4 scalar/string arguments, FR-L6, FR-P1..P6 print/println, `render_frame`, `srdump` text/JSON/`--raw`, refusal of older minors, other hash algorithms and unknown flags; "FR-P1 print/println: sr::println, println<…> and ADL println write the same frame" (`[names]`: `sr::println(out, "…"_, …)`, the template-argument spelling `sr::println<"…">(out, …)` and an unqualified `println(out, …)` found by ADL through the sink produce identical bytes, read back by `deserialize_println`) |
| `test_containers.cpp` (format 1.6; `[containers]`) | "FR-W9 B2 golden: unordered_map<double, i32> keys are written in std::strong_order (-inf, +0, +inf, NaN)" (96 frozen bytes), "B7 a sequence whose size() x element stride overflows size_t is refused with invalid_argument, not wrapped into a small image" (`add_block`, a `std::stack` over a lying sequence); "FR-A6 maps and sets round-trip as members: ordered, unordered, flat, nested (1.6)" (the corpus type `DynMap`; `flat_map`/`flat_set`; ordered and unordered sets; a map of vectors, a vector of maps, a map of maps; a trivially copyable key without `operator<`, ordered by its bytes; empty containers as `{0, 0}` slots); "FR-A6 canonical order: identical values give identical bytes whatever the container (1.6)" (an `unordered_map` of 300 entries filled in two insertion orders — the heap path above 64 — and a `map` of the same values give one image; equal layout hash, different schema hash for the two containers; an `unordered_set<std::string>` written as the sorted set is); "FR-A6 a duplicate key on the wire is invalid_value; the first entry is kept (1.6)" (an image with two equal keys: `deserialize` reports `invalid_value` into a `map` and an `unordered_map`, the output holds the first value, `verify` accepts the well-formed bytes); "FR-A6 a container is an argument of its own: the transparent wrapper type (1.6)" (`serialize(out, "ids"_, ids)` is a 48-byte frame, `arg_type` by the vector's schema hash, `dynamic_type<std::vector<int>>` and `<std::map<…>>` but not `<std::string>`; `deserialize` of the compact, empty and full-records frames and of a map, an optional, a variant and a set in one call; the registry's `type_desc` — `transparent`, not flat, size 8, one unnamed field at 0 — and the dictionary's, JSON `"transparent": true`, an RRI round trip; `inspect`, `dispatch` and `frame_router::route`); "FR-A6 srdump shows maps as {key: value}, sets as {…}, a container argument as the value (1.6)" (text `{"a": 1, "b": 2}` / `{10, 20}` / `[3, 1, 2]`, JSON `[{"key":…,"value":…}]`, `--print` rendering `ids=[3, 1, 2]`); "FR-A6 serialize_options::canonical_unordered: sorted by default, iteration order on request, readers take both" (`[options]`: a 200-entry `unordered_map` — above the 64-entry stack table — written with the default equals the same values in a `map` after the header's call hash; with `.canonical_unordered = false` the frame has the same length, `deserialize` and `tools::dump` read it back, an ordered container's bytes do not change, and another bucket count gives the same canonical bytes) |
| `test_dynamic.cpp` (with the `walk_values` cases) | the byte-exact plan §2 image, every dynamic shape, empty/engaged forms, large vectors (heap and claiming paths agree), readers, corrupt-image rejection, tools output, registration of element types; `walk_values` (whole record / inner values / none typed; flat base; native flat record), mmap vs block reader on a 60 KB stream, block reader vs in-memory walker on every capture fixture and a hand-made back-patched FILE; G2-2: an empty vector measured as encoded, identical bytes from a dirty `span_sink`, a reused `capture_file` buffer and an `fd_sink` stack buffer; T2: `[g2-6]` `deserialize` and `verify` reject the same corrupt images (an empty vector with `off` past the image, `n = 0` with `off ≠ 0`, a block inside the fixed part, 64 slots over one tail — the byte budget), the valueless-variant rule (`std::monostate`, else `invalid_value` with the other outputs still read; `dispatch`, `dump` as `null`, `walk_values` fallback reported); `[g2-12]` non-canonical `bool` bytes repaired in flat records, a top-level `bool` and a dynamic image, with the warning handler, `view` → `invalid_value`, `verify` warnings; `[g2-9]`/`[g2-10]` an unknown frame flag and a literal conflict seen the same way by both walkers, `dump`'s error code, a back-patched FILE whose original area has an entry that is not a multiple of 8; `[pmr]`: `std::pmr::string`/`std::pmr::vector` members write the `std::string` image and decode keeping the output's memory resource, a `std::pmr::string` argument writes the same frame as a `std::string`; "FR-S4 a record that fails while it is decoded: expected, actual and the failed item (2.1.15)" (a dynamic image corrupt on the inside: the argument before it is complete, the one after it untouched) |
| `test_ipc.cpp` → `serreg_ipc_tests` | `es::ipc` (chapter 11; test names "ipc: …", "ipc SPSC: …", "ipc SPMC: …", "ipc threads: …", "ipc processes: …"): geometry and attach validation (every header field, the 256-reader limit, memory aligned to max(64, Align), a compile-time unit refusing another), runtime units 16/32/64 and `Align` 16/4096, payload sizes 0..600, every boundary of the skip logic (a record that exactly fills the end, one unit too big, skips released by `peek` and `poll`), records of `max_message()`, rings above 4 GiB, positions wrapping across 2³², types and reserved types, `full_policy::fail` and `try_claim`, cancel/abandon/short commits, deferred commits and `flush`, `publish` with abandon, the producer role, peek/release/wait/poll, SPSC persistence and hand-over, SPMC broadcast and gating, eviction (the evicted consumer can never move the gate; an evicted gate no longer holds the producer), layout 2's plain-store progress, corrupt and forged headers (never delivered), threads (200k mixed sizes, 1 SPSC or 3 SPMC consumers; joins, leaves and evictions while running, every byte checked), `fork`ed processes (SPSC and SPMC, named and anonymous regions; dead consumers reaped or keeping the SPSC position; a reaper never pairing a stale pid with a new ticket; producer take-over; handles copied by `fork()`), `basic_sink` with es::serreg frames, the Q6b gate scan, deferred records versus a joiner, `DataAlign` 64, `emplace<T>`/`poll_as<T>`; the regions (test names "ipc region: …", from the old suite with G-17): create/open/remove on tmpfs and only on tmpfs or hugetlbfs, `mlock`, only the creator unlinks (I-06), `foreign_owner` (I-08), explicit 2 MiB huge pages named and anonymous (SKIP without a pool or a writable mount); links `es::ipc` only |
| `test_conflict.cpp` → `serreg_conflict` | FR-R4 conflict recording with a hand-built node (separate executable: the conflict persists) |
| `test_alloc.cpp` → `serreg_alloc_tests` | replaces global `operator new`/`delete` with a fail-on-demand allocator (its own executable for that reason): 64 unordered entries allocate nothing, 65 throw `std::bad_alloc` out of `serialize` into a `span_sink` and the sink is untouched, `canonical_unordered = false` allocates nothing (review B1) |
| `test_evolution.cpp` (R-9; `[evolution]`) | schema evolution level 1 for flat types (`projection.hpp`): "R-9 level 1: an old Order projects into the new one by member name (widen, default, rename, R12)" (`evo_old::Order`, carrying `[[=es::serreg::name("evo::Order")]]`, into `evo::Order` through `deserialize(projection_context{&d, &rep}, …)` in compact and full-records frames: `qty` u32 → u64 and `weight` f32 → f64 widened, `px` from `renamed_from("price")`, `flags` and `venue = 7` defaulted, `lots[2]` → `lots[4]`, `legacy` skipped; level-0 `deserialize` still says `hash_mismatch`; the counters, and the notes only with the frame that built the plan); "R-9 level 1: tools::project<T> and the R7 rule" (an `i64 id` into a `u64`: not projected, the default kept, `conflicts == 1`, the note `evo::Order.id: i64 -> u64 not projected (R7)`; another type by name → `hash_mismatch`, 4 bytes → `truncated`); "R-9 level 1: renamed_from on a class, exact arguments beside a projected one, call-shape rules" (`evo::NewName` with `renamed_from("evo::OldName")` next to a literal and a `u32` through `deserialize(dict, …)`; another literal → `hash_mismatch`, another argument count → `length_mismatch`; the exact path through the dictionary overload) |
| `test_validate_off.cpp` → `serreg_validate_off_tests` | "FR-D5 ES_SERREG_VALIDATE_READ=0: out-of-range enum leaves read through unchanged" (`STATIC_REQUIRE(!detail::validate_read_enums)`; the bytes `test_limits.cpp` rejects — a `Plain` leaf holding 7 — read through `deserialize`, `view` and `dispatch` unchanged, no error) |
| `premain_check.cpp` → `premain_check` | FR-R1/R2/R3: registry size is final when `main()` starts — `main()` constructs `premain_check::app`, whose first member initialiser reads `registry::instance().size()` (and `version()`) before any other code runs (before, `main()`'s first statement); then checks the entities by name and hash and produces the capture used by `srdump_*` ctests, checking that `serialize()` registered nothing |
| `runtime_selftest.cpp` (top-level `tests/CMakeLists.txt`; `runtime_selftest::app`, 14 member functions) | framework-free tests of the runtime/tools layer on hand-built ENTRY streams: parser bounds, bombs, conflicts, walker, verify (incl. a 1.0-minor stream), dump, diff, ELF reader (`extract_embedded_dictionary` on the in-memory
ELF — 3 types and 1 call — on an `MZ` blob → `unsupported`, on 3 bytes → `truncated`), cli, config parser. `review_2_1_10_decoders()` — D-01 (`take` accepts a wrapped `n × stride`, `take_n` refuses it, the walker returns `length_mismatch`, and the parser refuses a `u8` FIELD of `0xFFFFFFFF` bytes with alignment `0xFFFE` and an alignment of 12), D-02 (30 levels of 1-byte types with 8 overlapping struct fields: `length_mismatch` within 5 s and fewer than 200 000 visitor events), D-03 (201 redefinitions: 64 summaries without fields, the rest in `conflicts_omitted`), D-04 (an ELF image with 20 000 section headers named by a 1 MiB string: both lookups within 5 s), T-13 (`y` and `y_` get `serreg.t_Y.y` and `serreg.t_Y.y_1`), D-08 (JSON of a literal with stray bytes is valid UTF-8); `diff_reports` T-09 (versions of one name pair by schema hash); `cli_surface` D-09 (commas between array items, a comment after a table header), T-07 (`/dev/full` as stdout: exit 1 for a dump, `--verify` and `--wireshark -`), T-03 (a capture file name with newlines stays in the Lua comment), T-08 (a dictionary frame after the data frames is used; a bare stream without one is exit 1). With `<ser> <fixtures-dir>` it regenerates `tests/fixtures/corpus/*` and `negative/*` |
| `local_in_main.cpp` → `local_class_in_main_serialises` (C-05) | `main()` defines a local struct and runs `local_in_main::app<Local>`: it is named with `[[=es::serreg::name("local_in_main::Local")]]` (a local class has no linkage-unique name) and it round-trips through `serialize`/`deserialize` |
| `signal_dump_test.cpp` → `signal_dump_usr2` | `ES_SERREG_DUMP_SIGNAL=USR2` end to end (fork + re-exec so the environment precedes the constructor) |
| `registry_scale.cpp` → `registry_scale_one_index_for_startup` | 128 `tick<N>` types and their call signatures register during static initialisation; `registry_scale::app` checks `registry::generations()`: 0 indexes published before `main()`'s first read, 1 after it, still 1 after a later lookup (deferred publication; publishing at every registration would be O(n²) at startup) |

`tests/unit/CMakeLists.txt` also registers `srdump_verify_capture`, `srdump_json_capture`,
`srdump_env_dump_parses`, `srdump_elf_premain`, `srdump_elf_serreg_tests`, `srdump_elf_no_skipped_type`,
`srdump_version`, `srdump_program_hash_capture`, `premain_env_dump`. The three `srdump_elf_*` tests that read
`premain_check`'s or `serreg_tests`' section are ELF-only; with `ES_SERREG_IMAGE_FORMAT=none`
`srdump_elf_none_has_no_section` requires `srdump --elf premain_check` to report `not_found` instead. Likewise
`golden_srdump_program_hash_elf_agrees` and the parity test's `elf:` checks exist only for the ELF backend.

## `tests/link` — the link model
`main.cpp`, `tu_a.cpp`, `tu_b.cpp`, `tu_extra_first.cpp`, `link_types.hpp`, and `image_none_main.cpp`.
`main.cpp`'s `main()` is `link_test::app{link_test::options::parse(argc, argv)}.run()`; the count at
`main()` is read in `app`'s first member initialiser, so it is still taken before anything else runs.
Executables `link_plain`, `link_alt_order`, `link_reversed`, `link_gc` (`-Wl,--gc-sections`), `link_nopie`
(`-no-pie`), `link_lto` (`-flto`). Tests: dedup across TUs (FR-R2), count at `main()` (FR-R3), program hash
independent of registration/link order (FR-H3), the `es_serreg_dict` section survives `--gc-sections` and LTO,
main-image identification without PIE. `validate_read_consumer.cpp` is compiled by ctest itself against the
built `libserreg` with this build's flags: `validate_read_match_links` (the library's `ES_SERREG_VALIDATE_READ`,
links and runs) and `validate_read_mismatch_fails_to_link` (the other value, passes only on `undefined reference
to validate_read_build<N>`) — the A-16 fuse (review B3).

The null image backend (review addendum A4), Debug builds with the default `elf` backend only: a private static library
`serreg_image_none` (the library sources with `src/image_none.cpp` and `ES_SERREG_EMBED_NONE`) and the executable
`link_image_none` (`image_none_main.cpp` + `tu_a.cpp` + `tu_b.cpp`). `link_image_none_backend`: 5 entries at
`main()`, exactly one non-null image, every entry in it with `refs == 1`, `unload_guard_active` false, the frames
verify against `export_image()`, and `extract_embedded_dictionary("/proc/self/exe")` is `not_found` (no section);
it writes the program hash. `link_image_none_program_hash_equal` compares that hash with `link_plain`'s.

## `tests/dlopen` — plugins, unload and reload
`host.cpp` (modes A–L, see `executables.md`), `plugin.cpp` (built four times: `serreg_plugin`, `_b` and `_c` with
different namespaces, and `serreg_plugin_v2` with `ES_PLUGIN_V2`, which gives `plugin::Tick` one more
member), `plugin_nested.cpp` (closes another plugin from its destructor), `plugin_conf_a/b.cpp` (conflicting
definitions of `conf::Msg`), `serreg_plugin_nounique` (built without the recommended flags), `plugin_alias.cpp`
(only entries the host already has), `second_copy.cpp` (an executable that exports its own libserreg
marker — its own `extern "C" es_serreg_registry_marker_v3()` function, so the test is no ODR violation under
ASan). The host calls `registry::refresh()` after each `dlopen`/`dlclose` whose notification it
checks. No test sets `ES_SERREG_ALLOW_DLCLOSE` and none expects an abort (decision A):

| test | checks |
|---|---|
| `dlopen_A_dlclose_keeps_entries` | the plugin's entries appear at `dlopen` (one published generation, `on_change` once, entry `refs == 1`, blob, name, descriptor, field names outside the plugin image); after `dlclose` the image is unmapped, the entries are retained (`size()` unchanged, `retained()` = the plugin's count, `refs == 0`), `images()` and `program_hash()` are back to their earlier values, `find_type`/`find_type_by_name` return the same descriptor with unchanged names, fields and blob, an old snapshot is unchanged, and the plugin's frame still decodes (`inspect`, `tools::dump`) |
| `dlopen_B_reload_revives_entries` | 50 load/unload cycles: the same descriptor every time, no growth, `retained()` 0 while loaded and the plugin's count while unloaded, the first load's frame decodes in every cycle, one generation per load and one per unload |
| `dlopen_C_rtld_nodelete` | `RTLD_NODELETE`: `dlclose` changes nothing (no retained entry, version unchanged) |
| `dlopen_D_nested_dlclose` | a plugin's destructor closes another: both unmapped, both retained, `images()` back to the baseline |
| `dlopen_E_changed_layout_reload` | `serreg_plugin_v2` after `serreg_plugin`: `plugin::Tick` has a new schema hash, two new entries (Tick and its call), no conflict, the loaded definition found by name, the old one by hash, frames of both versions decode; loading the first build again revives its entry |
| `dlopen_F_concurrent_dlclose` | two threads unload two plugins at once; every entry retained, the shared literal `"tick"` too |
| `dlopen_H_conflicting_plugins` | a conflicting second plugin: stderr warning naming both images, one conflict record, the first definition kept; a `dlclose` of the first ends the conflict and the second definition takes its place, the first descriptor stays readable |
| `dlopen_I_plugin_without_flags_unloads` | cc F12: a plugin built without `-fno-gnu-unique`/hidden visibility is unloadable (`unload_guard_active`), really unmapped by `dlclose`, its entries retained |
| `dlopen_J_readers_vs_dlclose_churn` | cc F13: `export_image`, snapshots, lookups, `program_hash`, `to_json` on one thread during 200 ms of load/unload |
| `dlopen_K_alias_only_plugin_notifies` | G2-10: a plugin of aliases adds no entry, moves the version, notifies once at load and once at unload |
| `dlopen_L_two_thread_load_unload` | G2-5: two threads load and unload two plugins for 300 ms (ends itself with `alarm` on a deadlock); reloads add nothing, every published generation notifies exactly once |

Plus `registry_second_copy_diagnostic` (Q10), `srdump_elf_conflicting_plugins_*` and
`srdump_elf_single_plugin_exit_0`. Not built in `gcc16-static`.

## `tests/headers` — include rules
`CMakeLists.txt` only. One generated translation unit per header, containing that header included twice, compiled
in four object libraries: `serreg_headers_ipc` (`include/es/ipc/*.hpp` as C++23 against `es::ipc` alone),
`serreg_headers_public` (`include/es/serreg/*.hpp` and `detail/*.hpp`), `serreg_headers_internal` (`src/*.hpp`) and
`serreg_headers_tests` (headers under `tests/golden`, `tests/support`, `tests/bench`, `tests/dlopen`, `tests/fuzz`,
`tests/link` and `examples/user_example1`). A header that misses an include or `#pragma once` breaks the build.
The ctest `headers_include_rules` runs `scripts/check-includes.py` (`#pragma once` first, package headers before
system headers) and passes on `include rules: 0 problem`; it is registered only when Python 3 is found.

## `tests/wireshark` — generated dissectors
`CMakeLists.txt`, `parity.py` (Python 3 standard library), `print_capture.cpp` and
`wslua_stub.lua`. Only when `srdump` is built. Label `wireshark`.

- `wireshark_print_capture` (`print_capture.cpp`): `wireshark_print_capture <out.ser>` writes a capture of
  15 `print`/`println` frames whose format strings exercise std::format's replacement fields (width, fill and
  alignment, sign, `#`, `0`, precision, integer bases including 64-bit extremes, floating-point types, strings,
  `bool`, `char`, an enum, a 64-bit enum with and without an enumerator name and with a
  value above 2^53 (T-02), a line of `float` arguments (`0.1f`, `3.14f`; T-04), argument ids); `main()` runs `ws_print::writer`. It is not a ctest of its own: the parity
  check `prints:` runs it.

- `wireshark_generate`: `srdump --wireshark - tests/fixtures/golden/orders.ser` must print
  `ProtoField.uint32("serreg.corpus_Order.qty", "qty", base.DEC)`. Needs no Wireshark.
- `wireshark_dissector_parity`: runs `parity.py --srdump … --tshark … [--text2pcap …] --fixtures … <checks>`
  when `tshark` and Python 3 are found (else registered `DISABLED`); `SKIP_RETURN_CODE 77`, timeout 300 s. Each
  check generates a dissector with `srdump --wireshark`, decodes a capture with `tshark -X lua_script:… -T pdml`, and
  compares every value field of every frame, in order, with `srdump --json`, and the rendered print messages.
  It accepts an enumerator value that `srdump --json` writes as a string (a 64-bit value beyond 2^53).
  Checks: `fixture:<orders.ser|dynamic.ser>` (the FILE reader; 1.7 files), `backpatched`
  (`corpus/capture/orders-1.6.ser` with its dictionary relocated to the end — a 1.6 shape; 1.7 writers never
  rewrite), `user0` (a pcap with link type 147: a dictionary frame, then the frames), `udp` and `tcp`
  (with `text2pcap` only; TCP splits one frame across two segments), `mismatch` (a dissector from orders.ser reading
  dynamic.ser: 2 known and 10 unknown dictionary entries, no value decoded, the "unknown to this dissector" expert
  infos, no Lua error), `fuzz` (3000 corrupted frames, seed 2190, in one USER0 pcap: every packet dissected, none
  with a Lua error), `elf:<binary>:<capture>` (dissectors generated with `--elf` from `make_fixtures` and
  `make_dynamic_fixtures`), and `prints:<wireshark_print_capture>` (every rendered message equal to
  srdump's), `udp-heuristic` and `tcp-heuristic` (with `text2pcap` only: the UDP and split-segment TCP captures on
  other ports with no port preference, recognised by the heuristic dissectors). As root it runs tshark through `runuser -u nobody` (Wireshark does not run Lua for
  root), or exits 77 without `runuser`.

- `wireshark_dissector_lua52`, `wireshark_dissector_lua54`: `parity.py --lua <interpreter>` with the checks
  `fixture:` (both), `backpatched`, `prints:` and, for the ELF backend, `elf:`. Instead of tshark, the dissector runs
  under a plain Lua interpreter through `wslua_stub.lua`, which implements the part of Wireshark's Lua API the
  generated file uses (`Proto`, `ProtoField`, `TvbRange`, `TreeItem`, `UInt64`/`Int64` as exact 32-bit halves,
  `FileHandler`, and `ByteArray.new(hex):tvb(name)`, which the dissector uses to show a constant of a
  call), drives the file reader over the capture and prints PDML. As in Wireshark 4.6, `le_uint()` returns
  an integer and `tonumber()` a float under Lua 5.4. Wireshark up to 4.4 embeds Lua 5.2, 4.6 and later 5.4; the
  integer/float distinction shows only under 5.4 (as with tshark 4.6.8), and `wireshark_dissector_lua54`
  reproduces that environment. The interpreters are found as `lua5.4`/`lua54`/`lua5.2`/`lua52`
  or `lua` by their `_VERSION`; a missing one registers its test `DISABLED`.

`scripts/run-all-tests.sh` records the tshark version (or "not found") and the Lua interpreters in
`00-environment.txt`.

## `tests/negative` — must not compile
`neg_unnamed_struct`, `neg_pointer_member`, `neg_nested_as_flat`, `neg_enum_u64_overflow`,
`neg_print_bad_spec`, `neg_print_arg_count`, `neg_multimap_member` (by its historical name: a `std::priority_queue`
member meets the closed-vocabulary message "this standard-library type is not in the serialisable vocabulary";
`std::multimap` itself is a member kind now), `neg_smart_pointer_member` (a `std::unique_ptr` member: "smart
pointers (std::unique_ptr, std::shared_ptr, std::weak_ptr) are not serialisable"), `neg_empty_tuple_member`
(`std::tuple<>`: "has no parts to serialise"),
`neg_string_view_member`, `neg_router_no_params`,
`neg_print_nested_field` ("nested replacement fields", T-05), `neg_router_generic_lambda` ("the handler has no
single call signature", T-10), `neg_zero_size_element` ("must occupy bytes in the image", D-05),
`neg_pointer_argument` (B10: a container is an argument of its own, so the deleted overload is met by an `int*`
— "never serialised"), `neg_const_member` and `neg_volatile_member` ("const and volatile members are not
serialisable", review B4), and the
control `neg_control_compiles`. Each is checked at configure time with `try_compile` (a file that compiles, or
fails without the expected text, stops the configure) and again as a ctest `negative_neg_<name>` that re-runs the
compiler and matches the diagnostic.

## `tests/cxxgen` — regenerated types (plan A5)
`CMakeLists.txt`, `compile_generated.cmake`, `check_orders.cpp`, `check_dynamic.cpp`, `check_namespace.cpp`,
`roundtrip_orders.cpp`, `roundtrip_dynamic.cpp`. Only when `srdump` is built. Label `cxxgen`. Every test is one
`cmake -P compile_generated.cmake` run: `srdump … --cxx <build>/x.hpp --cxx-router` at test time (a build does not
run binaries), then the project compiler with the project's warnings and `-Werror` on a source that includes the
output — compiled and linked in two steps, as a build does, because GCC 16.2 given `-fsanitize` twice
on one command line (the sanitizer presets carry it in both the compile and the link flags) miscompiles the
constant evaluation. The generated header carries its own `static_assert`s (sizes, offsets, canonical names, schema and layout
hashes), so compiling it is the regeneration check.

- `cxxgen_golden_compiles`: `orders.rri` → `orders.hpp`; `check_orders.cpp` (`-fsyntax-only`) constructs a
  `frame_router` and calls the generated `install_handlers`, which must register the nine signatures.
- `cxxgen_golden_dynamic_compiles`: the dictionary embedded in `dynamic.ser` → `dynamic.hpp` (dynamic types,
  `std::monostate` spelled back; `corpus::DynMap` with `std::map`/`std::set` members — the `unordered_map`
  member comes back as `std::map` with the same hashes — and the transparent `std::vector<i32>` spelled, not
  defined); `check_dynamic.cpp`.
- `cxxgen_golden_namespace_compiles`: `orders.rri` with `--namespace corpus=regen` → `orders_ns.hpp`, compiled
  beside `corpus.hpp`: `check_namespace.cpp` asserts `schema_hash_v<regen::Order> == schema_hash_v<corpus::Order>`
  (and layout hash, size, `offsetof`, the enum's underlying type).
- `cxxgen_golden_roundtrip` / `cxxgen_golden_dynamic_roundtrip`: `roundtrip_orders.cpp` / `roundtrip_dynamic.cpp`
  are compiled, linked against the built `serreg` and `serreg_tools` and run on the golden capture: the generated
  `install_handlers` must accept every frame (11 / 9 handled, none unhandled or failed), and handlers registered
  over its signatures compare the decoded values with the fixtures' (`new order` ids 42 and 43, the `fill`
  signature with its three constants, `px=2.50 qty=7 side=sell`; `DynMsg{7, "ab", {1, -2, 3}}`, the nested
  optional/variant values, `DynBase`'s base and note, the println with a string; `DynMap`'s four containers
  decoded into the regenerated `std::map`/`std::set` members, the top-level vector `{3, 1, 2}` and the empty one,
  and the println with a vector and a map). Output
  `cxxgen roundtrip: N frames handled, ok`.

## `tests/canary` — format drift
`rri_canary.cpp` includes no library header and links nothing from it; it decodes an RRI from literal offsets
documented in `format.hpp`. `format_canary_rri` checks the current 1.7 image (`rri_canary orders.rri 7 20 8 9`:
minor 7, 20 entries, 8 literals, 9 calls): the 40-byte header, `hash_algo` 2, the
ABI tag bytes, and the `corpus::Cancel` ENTRY (found by name) against its golden hashes. It also reads
the one call with five arguments, `("fill", 42_, Order, k<Side::sell>, -1.5_)`: the ENTRY must be 128 bytes
(8 + 16 + 16 + 24 + 16 + 24 + 24), and each constant's ARG must have `arg_kind` 4, the right `kind` byte (3 = `i32`,
0 = an enum, 10 = `f64`) and the value 42, 2 and `0xbff8000000000000` (the bits of -1.5) at offsets 56, 96 and 120
of the ENTRY (`rri_canary.cpp:180-203`). An encoder+decoder change that stays self-consistent but moves the bytes fails here.

## `tests/fuzz`
`fuzz_driver.cpp` (portable driver: replay the corpus, then seeded mutations — bit flips, overwrite, insert,
delete, truncate, duplicate), `fuzz_targets.cpp` (RRI/ENTRY parser; capture walker + decoder + formatters; ELF
section finder and `extract_embedded_dictionary` on the same bytes; `--config` parser). ctests `fuzz_rri`, `fuzz_capture`, `fuzz_elf`, `fuzz_config` with
`ES_SERREG_FUZZ_ITERATIONS` (300) per file; `scripts/run-all-tests.sh` runs 200 000 under ASan/UBSan.
The driver (FR-D5, maintainer answer O2) gates every input on **time and memory** as well as on
crashes: a single input that takes longer than `max_ms` (2000; 20 000 under ASan or TSan, which are 5–20× slower)
or raises the process's RSS high-water mark (`getrusage` `ru_maxrss`, compared with its value when the run
started, so an input that frees everything before returning is still caught) by more than `max_rss_mib` (512)
fails the run and is written next to the corpus as `<target>-slow-N.bin` / `<target>-big-N.bin` — amplification is
a bug under the never-crash contract even when nothing crashes. The two limits are the fifth and sixth
command-line arguments (`serreg_fuzz <target> <corpus-dir> [iterations] [seed] [max_ms] [max_rss_mib]`), and the
summary line reports the slowest input and the RSS growth. The address-space limit (`RLIMIT_AS`, 2 GiB) stays,
except under a sanitizer, whose shadow memory does not fit under it.
`fuzz_capture` also runs the **typed** readers (G2-1) — `deserialize`, `deserialize_print`,
`deserialize_println` and `dispatch` with the exact signatures of the golden fixtures (`corpus.hpp`) — on every
frame of the mutated capture and on every 8-aligned offset that reads as a frame header; each frame is copied so it
ends at a `PROT_NONE` page, so an over-read faults even without a sanitizer runtime (fuzzing only `verify`/`dump` once missed G2-1; this
mode found it within seconds).
It also checks that the two image decoders agree (`decoders_agree`): for every record of a corpus
dynamic type (`DynMsg`, `DynNested`, `DynArr`), `detail::decode_image` and `tools::visit_record` over the process
dictionary must both accept or both reject the image (`invalid_value`, a valueless variant the C++ type cannot hold,
counts as accepted); a disagreement aborts. In a 3 000-iteration check run it compared about 4 000 images, a tenth
of them rejected by both. The fuzzers run under the ASan/UBSan preset, else under `gcc16-ubsan-trap`, else in the
debug build.
Optional libFuzzer builds with `-DES_SERREG_LIBFUZZER=ON` (clang).

## `tests/bench`
`bench.cpp` → `serreg_bench`, ctest `bench_hot_path_ceilings` (label `bench`, timeout 300): `serialize()` of a
32-byte struct into a `span_sink` < 50 ns mean, `inspect()` of a one-record frame < 500 ns (a plain
`steady_clock` loop over 1e5 iterations, `REQUIRE`d against the ceiling; 4× looser with a debug libserreg).
Each case also reports Catch2 distributions: `inspect 1-record frame` and `dispatch<Order> 1-record frame` as
plain `BENCHMARK`s, and `serialize Order -> span_sink`, `serialize lit + Order -> span_sink` and `deserialize
Order` as `BENCHMARK_ADVANCED` rows whose input changes per sample (`Order::id` set from the sample index — for
the decode row, the byte at offset 16 of the frame) and whose output is consumed (`keep_memory(buf)`, the
returned value), so the optimiser cannot hoist the frame out of the loop (without that the serialize rows read
0.7 ns with "est run time 0 ns"). Excluded under sanitizers; WARN (non-gating) in the full run.

C-08: `hot_inline.cpp` and `check_inline.cmake`, ctest `serialize_inlines_at_O2` (not
labelled `bench`, so it gates). The ctest runs `check_inline.cmake`, which compiles `hot_inline.cpp` — `extern "C"
hot()` serialising `"new order"_lit` and a `corpus::Order` into a `span_sink` — at `-O2` to assembly and fails when
the body of `hot` contains a `call` or a tail `jmp` to a symbol.

The half-RTT benchmarks over the `es::ipc` rings: `hrtt_common.hpp` (the
half-RTT harness: roles, named/anonymous regions with optional huge pages, SPMC rings or, with `--ring spsc`, SPSC
queues sized by `--slots`/`--slot-bytes`, ping/pong loops, percentile report), `hrtt_raw.cpp` → `serreg_hrtt_raw`
(64-byte POD built in the claimed record and read in place), `hrtt_serreg.cpp` → `serreg_hrtt_serreg` (the same
content as a reflected `hrtt::msg::Tick`, `serialize` through `es::ipc::sink` / `deserialize` in the record). ctests
`hrtt_raw_smoke_process`, `hrtt_raw_smoke_thread`, `hrtt_serreg_smoke_process`, `hrtt_serreg_smoke_thread`: 2 000
round trips each, **gating but functional only**. They check that every reply arrives in order with the right
timestamp and nobody is lapped, never how fast. The numbers come from `scripts/bench-hrtt-raw.sh` and
`scripts/bench-hrtt-serreg.sh` (chapter 11).

DECISIONS G-5: `registry_measure.cpp` → `registry_measure`, the registry at scale — N synthetic struct
entries pushed as guards push them, timing push, publish, lookup, export and parse and the RSS growth; ctests
`registry_measure_10k` and `registry_measure_100k` (`--assert`: push + publish within 10 ms and 100 ms in a release
build) and `registry_measure_elf` (the reader side on `serreg_bench`), label `bench`. Its own page:
`book/registry-measure.md`.

## `tests/python`
`log_capture.cpp` → `python_log_capture` (a 1.7 capture of log frames under both envelope policies, a println frame
and a record frame, with CRC trailers) and `parity.py`: ctest `python_reader_parity` runs `srdump --json` and the
pure-Python reader `python3 -m serreg --json` on the golden captures (`orders.ser`, `dynamic.ser`, `features.ser`),
the 1.6 corpus captures, `selftest.ser`, two bare streams with `selftest.rri` and the log capture, and compares
them semantically — dictionary, records, messages, log lines, `--verify` counts, `--info` for 1.7 files,
`--level trace`. Label `python`; SKIP (77) without Python 3. `book/python-serreg.md`.

## `tests/ce`
`CMakeLists.txt` and `build_and_run.cmake`: `ce_example1_builds_and_runs` … `ce_example5_builds_and_runs` compile
each `ce-examples/ce-exampleN/main.cpp` with `-I.` only (the example's header symlinks) and `-O1 -W -Wall -Wextra
-Wshadow -Werror`, run it and match its verdict (`0 golden mismatches`, `0 mismatches`, `ok: 1000 messages`).
Label `ce`. `book/ce-examples-tests.md`.

## `tests/probe`
`depth4.cpp`: 4 levels × 4 members of nested structs; `compile_probe_depth4` compiles it (compile-time budget
for the memoised hashes), `compile_probe_depth4_runs` runs it.

## `tests/support`
`test_support.hpp`: `all_records(frame_view)`, the `ES_SERREG_TEST_PLAIN_SINK` static check, a
`static_assert` that `serialize()` into `fd_sink` is `noexcept` by the library's own derivation, and
`embedded_dictionary_of_self()`: the dictionary of the running test binary — its `es_serreg_dict` section
with the ELF backend; with `ES_SERREG_EMBED_NONE` it requires that no section exists and returns the registry's own
RRI (`registry::export_image()`), so `test_registry.cpp`, `test_capture.cpp` and `test_skip.cpp` run unchanged in
the `gcc16-static-none` preset.

## `tests/fixtures`

| directory | contents | regenerated by | rule |
|---|---|---|---|
| `golden/` | `orders.{ser,rri,text,json,toml}`, `dynamic.{ser,text,json,toml}`, `features.ser` — current format (1.7) | `make_fixtures <dir>`, `make_dynamic_fixtures <dir>` | regenerate only for a deliberate format/rendering change + CHANGELOG |
| `corpus/{rri,capture,elf,config}/` | fuzz seeds, incl. `orders-1.7.ser`, `orders-1.7.rri`, `dynamic-1.7.ser`, `features-1.7.ser` (copies of the golden files, renamed with each format minor), `dictionary_deltas-1.7.ser` (what `examples/dictionary_deltas` writes: three dictionary blocks) and the 1.6 copies `orders-1.6.ser`, `orders-1.6.rri`, `dynamic-1.6.ser` kept as the compatibility inputs of the oldest accepted minor (and of the Wireshark `backpatched` check) | `runtime_selftest <ser> tests/fixtures` (selftest files) + copies of the golden files | add, don't replace; a crashing input found by fuzzing is added here |
| `negative/` | `bomb_array_extent.ser`, `entry_n_elem_65535.bin` | `runtime_selftest <ser> tests/fixtures` | must equal the in-memory bombs of `runtime_selftest` |
