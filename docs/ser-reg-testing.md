# ser-reg testing — what is verified, how, and how to add to it

Library 2.2.6, wire format 1.7 (readers accept 1.6 and 1.7). This is the maintainer's current description of the
test tree, the golden values, the verification protocol and the conventions for writing a test. It is written from
the tree as it is; the decision ledger behind it is `docs/DECISIONS.md` (T-1 … T-11, C-7), the requirement IDs are
indexed in `docs/ser-reg-requirements.md` Appendix A, and the day-to-day procedure for a full run is
`docs/test-and-log-protocol.md`.

Measured on the `gcc16-debug` tree of this version (`ctest -N`): **371 tests**, of which 242 are Catch2 cases
discovered from three executables (`serreg_tests` 160, `serreg_ipc_tests` 59,
`serreg_conflict` 1) and 129 are CMake-level tests (golden byte compares, link, dlopen, negative compiles, srdump
invocations, scripts). `gcc16-release` lists 369 (the Debug-only `link_image_none_*` pair is absent). Labels:
`bench` 4, `wireshark` 4, `cxxgen` 5, `ce` 5, `python` 1; everything else is unlabelled and gating.

---

## 1. What is verified, and by what

| Requirement area | Test kind | Files / ctest names |
|---|---|---|
| Hashes, canonical names, descriptors, frame sizes (FR-H1/H4/H7, FR-S3/S6/S11, §6.2b) | compile-time golden (`STATIC_REQUIRE`) | `tests/golden/hashes.cpp` → cases `FR-H1/FR-H4 golden schema and layout hashes`, `6.2b canonical names`, … inside `serreg_tests`; `golden_hashes_standalone_compiles` (the same file as plain `static_assert`s, no Catch2) |
| ENTRY blob bytes (cc F1, FR-H8, FR-P3) | compile-time golden | `tests/golden/blobs.cpp` → `cc F1 golden ENTRY blob hashes and sizes` |
| Byte-exact files and renderings (FR-W1..W5, FR-D8) | regenerate + byte compare | `tests/golden/make_fixtures.cpp`, `make_dynamic_fixtures.cpp` → `golden_fixture_{ser,rri,text,json,toml}_byte_exact`, `golden_fixture_dynamic_*`, `golden_fixture_features_byte_exact`, `golden_srdump_{json,toml,text}_matches_fixture`, `golden_srdump_verify[_dynamic|_features]`, `golden_srdump_info_features`, `golden_srdump_program_hash_agrees`, `golden_srdump_program_hash_elf_agrees` |
| Format drift independent of the library (FR-H8) | header-free decoder | `tests/canary/rri_canary.cpp` → `format_canary_rri` |
| Serialization, readers, literals, constants, print (FR-S1..S10, FR-A1..A6, FR-L1..L6, FR-P1..P6) | Catch2 unit | `tests/unit/test_roundtrip.cpp`, `test_readers.cpp`, `test_print.cpp`, `test_sinks.cpp`, `test_skip.cpp`, `test_containers.cpp`, `test_dynamic.cpp`, `test_empty_member.cpp`, `test_anon_tu.cpp` |
| Hash algorithm (FR-H5) | known answers, consteval = run time | `tests/unit/test_hash.cpp` |
| Registry contents and API (FR-R1..R5, FR-R9, FR-R12, FR-H3) | Catch2 unit + framework-free | `tests/unit/test_registry.cpp`; `premain_check.cpp` → `premain_registry_count`, `premain_env_dump`; `registry_scale.cpp` → `registry_scale_one_index_for_startup`; `local_in_main.cpp`; `signal_dump_test.cpp` → `signal_dump_usr2` |
| Conflicts (FR-R4) | separate executable | `tests/unit/test_conflict.cpp` → `serreg_conflict`; `dlopen_H_conflicting_plugins`; `srdump_elf_conflicting_plugins_*` |
| Link model (FR-R2/R3/R7, FR-H3) | multi-TU executables | `tests/link/` → `link_two_tu_dedup`, `link_alt_registration_order`, `link_program_hash_order_independent`, `link_reversed_order`, `link_program_hash_reversed_equal`, `link_gc_sections`, `link_no_pie_main_image`, `link_no_pie_program_hash_equal`, `link_lto`, `link_image_none_backend`, `link_image_none_program_hash_equal` |
| dlopen / dlclose / reload (FR-R6/R7/R10/R11/R12/R16) | host + plugin modules | `tests/dlopen/` → `dlopen_A_dlclose_keeps_entries` … `dlopen_L_two_thread_load_unload`, `registry_second_copy_diagnostic` |
| Capture files, streams, dictionary completeness (FR-W1/W2/W7, FR-D1/D6) | Catch2 unit | `tests/unit/test_capture.cpp`, `test_tools.cpp` |
| Format 1.7: chain, deltas, CRC32C, checkpoints, writer info, `--resync`, 1.6 acceptance (FR-W6, FR-W10..W14, FR-T1/T2, FR-V3) | Catch2 unit + golden | `tests/unit/test_format17.cpp` (`[format17]`); `features.ser` fixture |
| Logger (FR-G1..G8, FR-T3, FR-R15) | Catch2 unit | `tests/unit/test_log.cpp` (`[log]`) |
| `to_string` / `to_json` / `render` / `formattable` (FR-A7..A11) | Catch2 unit | `tests/unit/test_to_string.cpp` (`[to_string]`) |
| Reader limits, windowed readers (FR-D5, FR-D11) | Catch2 unit | `tests/unit/test_limits.cpp` (`[limits]`, `[window]`) |
| Never-crash contract (FR-D5, FR-D9) | fuzz | `tests/fuzz/` → `fuzz_rri`, `fuzz_capture`, `fuzz_elf`, `fuzz_config` (short in ctest; long in `run-all-tests.sh`) |
| Runtime + tools layer without reflection (FR-W2/W3/W5, FR-D1..D3, FR-L3) | framework-free self-test | `tests/unit/runtime_selftest.cpp` → `runtime_selftest` (also regenerates the fuzz seeds) |
| Rejections at compile time (FR-S7, FR-S8, FR-P2, router traits, canonical-name rules) | negative compile | `tests/negative/` → `negative_neg_*`, `negative_control_compiles` |
| Compile-time cost (cc F9) | probe | `tests/probe/depth4.cpp` → `compile_probe_depth4`, `compile_probe_depth4_runs` |
| Hot path (NFR-1, FR-P6, C-5) | bench, inline check | `tests/bench/bench.cpp` → `bench_hot_path_ceilings` (label `bench`); `hot_inline.cpp` → `serialize_inlines_at_O2`; `hrtt_{raw,serreg}_smoke_{process,thread}`; `ipc_compare_bench_smoke` |
| Registry at scale (FR-R13, FR-R14, FR-T6) | measurement app | `tests/bench/registry_measure.cpp` → `registry_measure_10k`, `registry_measure_100k`, `registry_measure_elf` (label `bench`) |
| Wireshark dissector (T-5) | parity script | `tests/wireshark/parity.py` → `wireshark_generate`, `wireshark_dissector_parity`, `wireshark_dissector_lua52`, `wireshark_dissector_lua54` |
| `srdump --cxx` (FR-D10, T-7) | generate + compile + run | `tests/cxxgen/` → `cxxgen_golden_compiles`, `cxxgen_golden_dynamic_compiles`, `cxxgen_golden_namespace_compiles`, `cxxgen_golden_roundtrip`, `cxxgen_golden_dynamic_roundtrip` |
| Pure-Python reader (FR-T4, T-6) | parity script | `tests/python/parity.py` → `python_reader_parity` |
| Cross-compiler stability (FR-H4, FR-T5, T-3) | CE examples as tests; godbolt lanes | `tests/ce/` → `ce_example{0..7}_builds_and_runs`; `ce-examples/ce-run.py` (not a ctest) |
| Header hygiene (C-2) | generated TUs + script | `tests/headers/` → object libraries `serreg_headers_{ipc,public,internal,tests}` (build-time), `headers_include_rules` |
| `es::ipc` rings (docs/ipc) | separate executable | `tests/unit/test_ipc.cpp` → `serreg_ipc_tests` (rings, regions, huge pages) |
| Versioning (FR-V1/V2) | ctest + install stage | `soname_major_minor` (`tests/CMakeLists.txt`); the consumer project of `run-all-tests.sh` (`SameMinorVersion`, SONAME `major.minor`) |
| Examples keep working | ctest | `example_file_store`, `example_ipc_ring`, `example_embedded_cli_*`, `example_ipc_log`, `user_example1_*`, `example_dictionary_deltas[_fixture_*]` (from `examples/`) |

Every row of Appendix A in `docs/ser-reg-requirements.md` names the test files that cite an ID; `grep -rl FR-W10 tests/`
is the way to find them from the code side.

---

## 2. The test tree

All test directories are added by `tests/CMakeLists.txt`, which also fetches Catch2 v3.8.1 (`ES_SERREG_CATCH2_DIR`
for an offline copy), defines `runtime_selftest` and `soname_major_minor`, and sets three variables the
subdirectories use: `ES_SERREG_CORPUS_DIR` (= `tests/golden`), `ES_SERREG_TEST_SUPPORT_DIR`, `ES_SERREG_GOLDEN_FIXTURES`.
Every test target goes through `es_serreg_apply_warnings()` (`cmake/warnings.cmake`: `-Wall -Wextra -Wpedantic
-Wshadow -Wconversion -Wsign-conversion … -Werror`), so a test file is held to the library's warning set.

Run one directory's tests with a name filter, for example `ctest --preset gcc16-debug -R '^dlopen_'`; a single
Catch2 case with its executable, for example `build/gcc16-debug/tests/unit/serreg_tests "[format17]"`. When GCC 16
is not the system compiler, prefix every run with `LD_LIBRARY_PATH=/opt/gcc-16/lib64`.

### 2.1 `tests/unit` — Catch2 cases and framework-free programs

Purpose: behaviour of every public header, one translation unit per concern. `es_serreg_catch2_exe()` builds a
Catch2 executable linked against `es::serreg` and `es::serreg_tools`, with `tests/golden` and `tests/support` on
the include path and two definitions: `ES_SERREG_TEST_TMPDIR` (the build directory of `tests/unit`, where tests
write files) and `ES_SERREG_TEST_SOURCE_DIR` (to reach `tests/fixtures/corpus`). Discovery is
`catch_discover_tests(... DISCOVERY_MODE PRE_TEST)`: the case list is produced when ctest runs, never as a build
step, so a sanitizer build that cannot run binaries still builds.

Executables:

- `serreg_tests` — the main suite: `test_roundtrip`, `test_readers`, `test_registry`, `test_capture`,
  `test_anon_tu`, `test_empty_member`, `test_sinks`, `test_tools`, `test_skip`, `test_print`, `test_dynamic`,
  `test_hash`, `test_limits`, `test_containers`, `test_format17`, `test_log`, `test_to_string`, plus
  `tests/golden/hashes.cpp` and `blobs.cpp`. Linking all of them into one binary is itself part of the test: the
  registry contents of this executable are what the golden tests and `srdump_elf_serreg_tests` lock.
- `serreg_alloc_tests` — `test_alloc.cpp` alone, because it replaces the global allocator (review B1).
- `serreg_conflict` — `test_conflict.cpp` alone, because it pushes a hand-built conflicting node that stays in the
  process registry for the process lifetime.
- `serreg_ipc_tests` — `es::ipc`; separate so the types it serializes do not enter `serreg_tests`' registry.
- `premain_check` (`premain_registry_count`, `premain_env_dump`): `main()`'s first statement reads
  `registry::size()`; also produces the capture the `srdump_*_capture` tests consume and the RRI written through
  `ES_SERREG_DUMP_REGISTRY`.
- `registry_scale` (one index published for 128 types registered before `main`), `local_in_main`,
  `signal_dump_test` (fork + exec + `SIGUSR2`, parses the dump as an RRI), `runtime_selftest` (1561 lines of plain
  asserts over hand-built ENTRY streams; `runtime_selftest <out.ser> <fixtures-dir>` regenerates the fuzz seeds).

What each unit file proves is in its first comment; the files most often touched now:

| file | proves |
|---|---|
| `test_format17.cpp` | 1.7 header and CHAIN, writer-info frame, no rewrite at close; a registry change between frames writes a dictionary delta inline and links it; CRC32C per frame, every reader reports `crc_mismatch` and skips the frame; checkpoints by size, the final one at close, `tools::info` follows the chain; `--resync` after a corrupt `frame_len`; 1.6 files read, 1.5 and 1.8 refused; `serialize_options{.crc}` on a plain sink |
| `test_log.cpp` | the gate evaluates nothing below the level, the floor compiles the call out; the envelope by policy renders in `--level`, `--since/--until`, JSON, `frame_router::on_log`; the scoped dictionary holds only the scope's calls, literals and types; a logger over a plain sink needs `dictionary_policy::full` |
| `test_to_string.cpp` | `to_string`/`to_json` register the type and render; dynamic members and containers; `formattable<T>` gives `std::format` and `{:j}` |
| `test_registry.cpp` | contents and lookups, `program_hash`, `export_image()` parses back, `to_json()`, version stability and `on_change` |
| `test_limits.cpp` | `decode_limits` defaults and bounds, a frame above `frame_bytes` is a problem not an allocation, the problem list is capped, every window size gives the whole-file output, a 64 MiB capture walks in a 1 MiB window, `--window`/`--frame-limit` |
| `test_ipc.cpp` | geometry and attach validation, skip-logic boundaries, wrap-arounds across 2^32, threads and processes with every byte verified, reaping and producer take-over, sink + `serialize` |
| `runtime_selftest.cpp` | dictionary parse/round trip, parser bounds, conflicts, capture walking, verify with the decoder, dump, diff, JSON 64-bit rendering, ELF reader, `cli` |

Run alone:

```sh
ctest --preset gcc16-debug -R 'premain|registry_scale|signal_dump|runtime_selftest'
build/gcc16-debug/tests/unit/serreg_tests "[log]" --success        # one tag, verbose
build/gcc16-debug/tests/unit/serreg_tests --list-tests | tail -1   # "160 test cases"
```

### 2.2 `tests/golden` — the compile-time and byte-exact locks

Purpose: freeze what the compiler decides (names, hashes, descriptors, blob bytes) and what the writers and
renderers produce (the fixtures). `hashes.cpp` and `blobs.cpp` are compiled into `serreg_tests`; with
`-DES_SERREG_GOLDEN_STANDALONE` `hashes.cpp` compiles without Catch2 (every `STATIC_REQUIRE` a `static_assert`),
which `golden_hashes_standalone_compiles` runs with `-fsyntax-only` and which Compiler Explorer uses for the second
compiler. `make_fixtures` and `make_dynamic_fixtures` write the fixtures into the build tree at test time
(`golden_fixtures_generate`, a `FIXTURES_SETUP`), and `cmake -E compare_files` compares each with
`tests/fixtures/golden/`. `corpus.hpp` is the shared corpus: one type per supported shape, used by every test
directory so that counts, hashes and frames all speak about the same types. What it would catch: any change to a
canonical string, the hash input, a descriptor table, the ENTRY/FIELD encoding, a frame's bytes or a renderer's
output. Run alone:

```sh
ctest --preset gcc16-debug -R '^golden_'
build/gcc16-debug/tests/unit/serreg_tests "[golden-print]"          # prints every current value (the hash oracle)
```

### 2.3 `tests/link` — link models

Purpose: registration survives every way a program can be linked. `main.cpp`, `tu_a.cpp`, `tu_b.cpp` (and
`tu_extra_first.cpp`) are linked as `link_plain`, `link_alt_order`, `link_reversed`, `link_gc`
(`-ffunction-sections -fdata-sections -Wl,--gc-sections`), `link_nopie` (`-no-pie`: `main_image` equals
`dladdr(&main).dli_fbase`), `link_lto` (when `check_ipo_supported` passes; otherwise a DISABLED placeholder) and,
in Debug only, `link_image_none` against a private static `serreg_image_none` library built from
`src/image_none.cpp`. Each writes its `program_hash` to a file; `compare_files` tests prove it is independent of
registration order and backend. Catches: a `[[gnu::retain]]` section dropped by the linker, a guard removed by
LTO, a program hash that depends on link order. Run alone: `ctest --preset gcc16-debug -R '^link_'`.

### 2.4 `tests/dlopen` — plugins

Purpose: decision A — entries are copied to the heap at `push()`, retained after `dlclose`, revived on
reload, never freed. Needs `ES_SERREG_BUILD_SHARED=ON` (one registry per process). `plugin.cpp` builds five images
(`serreg_plugin`, `_b`, `_c`, `_v2` with one more `Tick` member, `_nounique` without the recommended flags);
`plugin_nested.cpp`, `plugin_conf_a/b.cpp` (conflicting layouts of one schema hash), `plugin_alias.cpp` (only
aliases). `host.cpp` takes a mode: `keep`, `reload` (50 cycles, no growth), `nodelete`, `nested`, `changed`,
`concurrent`, `conflict`, `nounique`, `churn` (readers vs. unload for 200 ms), `churn2` (two threads load and
unload, alarm 60 s), `alias`. `second_copy.cpp` checks the "two copies of libserreg" diagnostic. Catches: a DSO
that becomes `DF_1_NODELETE` (an `STB_GNU_UNIQUE` symbol), a registry that still points into an unmapped image, a
missed `on_change`, a lock-order inversion. Run alone, and under TSan whenever `registry.*` or `src/image_*.cpp`
change:

```sh
ctest --preset gcc16-debug -R '^dlopen_|registry_second_copy|srdump_elf_conflicting'
ctest --preset gcc16-tsan --repeat until-fail:20 -R 'dlopen_[ABDEFJL]'
```

### 2.5 `tests/canary` — the format-drift canary

`rri_canary.cpp` includes no `es/serreg` header and links nothing: it decodes `tests/fixtures/golden/orders.rri`
from literal byte offsets taken from the layout comment at the top of `format.hpp`, and checks the frozen hashes
and the geometry of `corpus::Cancel`. `format_canary_rri` passes `7 20 8 9` (minor, entries, literals, calls).
Catches: an encoder and decoder changed consistently with each other but away from the documented layout. Run
alone: `ctest --preset gcc16-debug -R format_canary`.

### 2.6 `tests/negative` — rejections

Each `neg_*.cpp` must fail to compile with the expected diagnostic substring (`es_serreg_negative_test(name
"text")` in its `CMakeLists.txt`). Checked twice: `try_compile` at configure time (a file that compiles is a
configure error) and as the ctest `negative_<name>` (the compiler rerun with `-fsyntax-only`, the diagnostic
matched by `PASS_REGULAR_EXPRESSION`). `neg_control_compiles.cpp` proves the harness accepts a valid file. Catches:
a `static_assert` that silently stopped firing, a diagnostic text that moved. Run alone:
`ctest --preset gcc16-debug -R '^negative_'`.

### 2.7 `tests/probe` — compile-time cost

`depth4.cpp` (depth-4 nesting, 4 members per level) must compile within the test timeout (300 s; ~14 s and
885 MB alone) and the result must link and run. `compile_probe_depth4` invokes the compiler directly so every ctest
run recompiles. Excluded from the UBSan-trap preset (a wall-clock limit is not a UB test).

### 2.8 `tests/bench` — ceilings, smoke runs and the registry measurement

Always compiled with `-O2` whatever the preset. `bench_hot_path_ceilings` (label `bench`): `serialize()` of a
32-byte struct into a `span_sink` < 50 ns mean, `inspect()` of a one-record frame < 500 ns (×4 under a Debug
library). `serialize_inlines_at_O2` compiles `hot_inline.cpp` to assembly and checks that `serialize()` leaves no
call. `hrtt_{raw,serreg}_smoke_{process,thread}` and `ipc_compare_bench_smoke` are functional smoke runs (every
reply arrives, in order, nobody lapped), gating, no timing. `registry_measure` (G-5) synthesises N entries of B
bytes, pushes them as guards do and reports push / publish / lookup / export / parse / RSS; `--assert` enforces
10k + 10 MB → ≤ 10 ms and 100k + 100 MB → ≤ 100 ms in release builds only; `--elf <bin>` measures the reader
side. Numbers for the record come only from the reference box (`scripts/bench-hrtt-*.sh`, `registry_measure`
under `gcc16-release`), never from a sandbox (T-9). Run alone:

```sh
ctest --preset gcc16-release -L bench --output-on-failure
build/gcc16-release/tests/bench/registry_measure --entries 100000 --bytes 104857600 --assert --json
```

### 2.9 `tests/fuzz` — the never-crash contract

`fuzz_targets.cpp` defines four `extern "C"` entry points with the libFuzzer signature: `fuzz_rri`
(`dyn::dictionary::add_rri`, re-encode, JSON, TOML, and the same bytes as a bare ENTRY stream), `fuzz_capture`
(`verify`, `dump` text/JSON, `walk_values`, the block reader, every frame through the typed readers of the corpus
types, and every 8-aligned offset that reads as a frame header), `fuzz_elf` (section finder, embedded dictionary),
`fuzz_config` (`cli::parse_config`). `fuzz_capture` aborts when the typed decoder and the
dictionary-driven walker disagree on an image. `fuzz_driver.cpp` is the portable driver (GCC has no libFuzzer):
replay every file of `tests/fixtures/corpus/<target>/`, then seeded mutations (bit flips, overwrite, insert,
delete, truncation, chunk duplication); every input is also gated on time (2 s, ×10 under a sanitizer) and RSS
growth (512 MiB); an offender is written next to the corpus as `<target>-slow-N.bin` / `<target>-big-N.bin`.
`-DES_SERREG_LIBFUZZER=ON` (clang) builds `serreg_libfuzz_<target>` instead. The ctests `fuzz_<target>` run
`ES_SERREG_FUZZ_ITERATIONS` (300) mutations per seed; the protocol run does 200 000 under ASan/UBSan. Run alone:

```sh
build/gcc16-asan-ubsan/tests/fuzz/serreg_fuzz capture tests/fixtures/corpus/capture 200000 1
build/gcc16-asan-ubsan/tests/fuzz/serreg_fuzz rri tests/fixtures/corpus/rri 200000 <seed-from-the-log>
```

### 2.10 `tests/wireshark` — dissector parity

`wireshark_generate` checks `srdump --wireshark -` writes the expected `ProtoField`s (no tshark needed).
`wireshark_dissector_parity` runs `parity.py` with tshark: for each check (`fixture:`, `backpatched`, `user0`,
`udp`/`tcp` and their `-heuristic` forms when `text2pcap` exists, `mismatch`, `fuzz`, `prints:` from
`print_capture.cpp`, `elf:` from the fixture generators' sections) every value field of every frame in tshark's
PDML must equal `srdump --json`. `wireshark_dissector_lua52` / `_lua54` run the same dissector under a plain Lua
interpreter through `wslua_stub.lua`. Where tshark, Lua or Python 3 is missing the test is DISABLED or exits 77
(ctest SKIP). Run alone: `ctest --preset gcc16-debug -L wireshark`.

### 2.11 `tests/cxxgen` — `srdump --cxx`

At test time (never at build time) `compile_generated.cmake` runs `srdump --cxx` on `orders.rri` or on
`dynamic.ser`'s embedded dictionary, then compiles the generated header with the project's flags and `-Werror`;
the header proves itself with `static_assert`s for every size, offset, name and hash. `cxxgen_golden_roundtrip` and
`_dynamic_roundtrip` link the regenerated types and route the golden captures through them (`11 frames handled,
ok` / `9 frames handled, ok`). `--namespace corpus=regen` is checked beside `corpus.hpp`. Label `cxxgen`.

### 2.12 `tests/python` — the pure-Python reader

`python_reader_parity` runs `tests/python/parity.py`: `srdump --json` and `python3 -m serreg --json` on
`orders.ser`, `dynamic.ser`, `features.ser`, the 1.6 corpus captures, `selftest.ser`, `dictionary_deltas-1.7.ser`
(three dictionary blocks), two bare frame streams with
their RRI (`PATH:DICT.rri`), and a log capture written by `python_log_capture` (both envelope policies, under
CRC). The comparison is semantic (floats within tolerance), dictionary first, then frame by frame and record by
record, then `--verify` counts, `--info` blocks and checkpoints for 1.7 files, and `--level trace`. Exit 77 = SKIP
when srdump cannot run. Label `python`. Run alone: `ctest --preset gcc16-debug -L python --output-on-failure`.

### 2.13 `tests/ce` — the Compiler Explorer examples as tests

`ce_example{0..7}_builds_and_runs` compile `ce-examples/ce-exampleN/main.cpp` as one translation unit against the
example's own header symlinks (`-O1 -W -Wall -Wextra -Wshadow -Werror -I.`) and run it; the output must match
`0 golden mismatches` (1, 2), `0 mismatches` (3, 4) or `ok: 1000 messages` (5). A header added to the library needs
a new symlink in every example that includes it (`g++ -MM`; `ce-examples/README.md`) — this test fails first. The
godbolt lanes themselves (`ce-run.py`, `docs/ce-links.md`) are not ctests. Label `ce`.

### 2.14 `tests/headers` — self-contained headers and include rules

One generated TU per header, containing only that header included twice, compiled into object libraries
`serreg_headers_ipc` (C++23, no reflection flag), `serreg_headers_public`, `serreg_headers_internal` (`src/*.hpp`)
and `serreg_headers_tests`. A missing include or a missing `#pragma once` breaks the build. `headers_include_rules`
runs `scripts/check-includes.py` (order: package headers before system headers; `#pragma once` first; the one
opt-out is a `// include-order: after …` comment).

### 2.15 `tests/support` and `tests/fixtures`

`test_support.hpp`: `es::serreg::test::all_records(frame_view)`, `guarded_copy(span)` (a copy ending at a
`PROT_NONE` page, so one byte over-read faults without a sanitizer), `embedded_dictionary_of_self()` (the ELF
section, or the exported image in a `none` build), and the `ES_SERREG_TEST_PLAIN_SINK` static check.
`tests/fixtures/golden/` (the frozen outputs), `tests/fixtures/corpus/{rri,capture,elf,config}/` (fuzz seeds and
compatibility files), `tests/fixtures/negative/` (the review bombs `runtime_selftest` asserts byte for byte).

---

## 3. Golden values and fixtures

**What is frozen.** Three layers, each catching what the one above cannot:

1. `hashes.cpp` and `blobs.cpp`: `STATIC_REQUIRE`s on `schema_hash_v<T>`, `layout_hash_v<T>`, canonical names,
   descriptor field tables, padding gap tables, `frame_size<…>()`, call strings, and the XXH3-64 and size of
   `detail::type_blob_bytes_v<T>`. A failure is a compile error in `serreg_tests`.
2. `tests/fixtures/golden/`: `orders.{ser,rri,text,json,toml}` (11 frames, 34 records, 20 registry entries:
   3 types, 8 literals, 9 calls), `dynamic.{ser,text,json,toml}` (9 frames, 18 records), `features.ser`
   (the same 11 frames with a CRC trailer on every frame, a checkpoint with a full dictionary every 256 bytes, the
   writer-info frame). Compared byte for byte with what `make_fixtures` / `make_dynamic_fixtures` write now, and
   through `srdump --verify` / `--json` / `--toml` / `--text` / `--info` / `--program-hash`.
3. `tests/canary/rri_canary.cpp`: the layout as documented in `format.hpp`, decoded without the library.

**When they may change (C-7).** A byte-level format change refreezes `tests/golden/blobs.cpp`, the golden fixtures
and the canary **together with a `format_minor` bump and a CHANGELOG line** — never silently, never one of them
alone. The `.text` fixtures are *renderings* (FR-D8), not wire bytes: a deliberate change of `srdump`'s text form
refreezes `orders.text` / `dynamic.text` with a CHANGELOG line and no format bump, and the `.ser`/`.rri`/`.json`/
`.toml` files must come out identical (`cmp` them). A run is never made green by refreezing a value, loosening an expectation or disabling a test. A changed
canonical string or hash input is a wire-format change even when no byte of a frame moves (the dictionary and
every hash move). When a golden test fails, first diff `[golden-print]` against the last green run's
`checks/golden-print.txt` (kept in `docs/test-reports/<version>-<date>.md`), find the change, and either revert
it or make it a deliberate format change.

**How to regenerate after a deliberate change** (in this order, from a green build of the new code):

```sh
B=build/gcc16-debug
export LD_LIBRARY_PATH=/opt/gcc-16/lib64
$B/tests/unit/serreg_tests "[golden-print]"             # paste the new values into hashes.cpp / blobs.cpp
$B/tests/golden/make_fixtures tests/fixtures/golden          # orders.* and features.ser
$B/tests/golden/make_dynamic_fixtures tests/fixtures/golden  # dynamic.*
$B/tests/unit/runtime_selftest /tmp/selftest.ser tests/fixtures   # corpus/{rri,capture,elf,config}/* and negative/*
# the corpus seeds that carry the format minor in their name:
cp tests/fixtures/golden/orders.ser   tests/fixtures/corpus/capture/orders-1.7.ser
cp tests/fixtures/golden/dynamic.ser  tests/fixtures/corpus/capture/dynamic-1.7.ser
cp tests/fixtures/golden/features.ser tests/fixtures/corpus/capture/features-1.7.ser
cp tests/fixtures/golden/orders.rri   tests/fixtures/corpus/rri/orders-1.7.rri
# the canary: update the expected minor/entries/literals/calls in tests/canary/CMakeLists.txt and, if the
# layout moved, the literal offsets in rri_canary.cpp from the new comment at the top of format.hpp
ctest --preset gcc16-debug -R 'golden|canary|fuzz_|python|wireshark|cxxgen|ce_'
```

**Compatibility files.** `tests/fixtures/corpus/capture/orders-1.6.ser`, `dynamic-1.6.ser` and
`corpus/rri/orders-1.6.rri` are the 1.6 writers' output and are **kept unchanged**: `test_format17.cpp` ("the
readers still read 1.6 files, and refuse 1.5 and 1.8"), `test_dynamic.cpp` and `python_reader_parity` read them,
and the fuzz seeds include them. A new format minor adds `*-1.<new>.*` files next to them and keeps the old ones
for as long as the acceptance window (FR-V3) includes that minor. The unversioned seeds (`selftest.ser`,
`bare_frames.bin`, `big_bare.bin`, `dict_frame_stream.bin`, `selftest.rri`, `big.rri`, `minimal.o`,
`selftest.toml`) are rewritten by `runtime_selftest` on every regeneration.

---

## 4. The verification protocol

`scripts/run-all-tests.sh` is AGENTS.md §6 as one command (T-1). Stages, in order, each with its own log:

| stage | what | gating |
|---|---|---|
| `format/clang-format --dry-run --Werror` | over `include src tools tests examples` | yes |
| `book/check-book.py` | book ↔ code sync (pages, citations, stale names, quoted test names) and cited document paths (`docs/*.md`, `book/*.md`) that exist | yes |
| `<preset>/configure`, `/build`, `/ctest` | fresh `cmake --preset` (build dir deleted; `--keep-build` reconfigures), `cmake --build -j`, `ctest -j --output-on-failure --timeout 600`; `-LE bench` under release and the sanitizers; `-E compile_probe_depth4` under UBSan-trap | yes |
| `gcc16-debug/golden-print` | `serreg_tests "[golden-print]"` → `checks/golden-print.txt` | yes |
| `gcc16-debug/srdump …` | `--version`, `--verify` on `orders.ser`, `--elf <serreg_tests> --program-hash`, `--elf … --json` | yes |
| `gcc16-debug/file(1) magic` | `file -m share/serreg.magic` names `.ser` and `.rri` | yes |
| `gcc16-release/bench`, `example file_store` | `ctest -L bench`; `ES_SERREG_DUMP_REGISTRY=- ES_SERREG_DUMP_FORMAT=toml` | WARN / yes |
| `fuzz/<target>` | `serreg_fuzz rri|capture|elf|config` for `--fuzz-iters` (200 000) under ASan/UBSan, else UBSan-trap, else the debug build with `_GLIBCXX_ASSERTIONS`; seed in the first log line | yes |
| `gcc16-tsan/concurrency` | `ctest --repeat until-fail:<--tsan-repeat>` over `dlopen_[ABDEFJL]|signal_dump|concurrent|hrtt_.*_thread` | yes |
| `install/…` | `cmake --install`; a generated consumer project: `find_package(serreg)` provides `es::serreg`, `es::serreg_tools`, `es::ipc` and `es::serreg` does not link `es::ipc`; `find_package(serreg <major>.<minor>)` succeeds and `<major>.<minor+1>` is refused (`SameMinorVersion`), the imported SONAME ends in `.so.<major>.<minor>`; an `es::ipc` consumer at C++23 without reflection flags; the installed `srdump --version`; `ES_SERREG_IPC_ONLY` configure + install + consumer; `examples/user_example1` built standalone and run; `pkg-config --cflags --libs serreg serreg-tools es-ipc` | yes |

Presets (`CMakePresets.json`, all Ninja, all `ES_SERREG_WERROR=ON`): `gcc16-debug` (`-D_GLIBCXX_ASSERTIONS`),
`gcc16-release`, `gcc16-static` (`ES_SERREG_BUILD_SHARED=OFF`, so no dlopen tests), `gcc16-static-none`
(`ES_SERREG_IMAGE_FORMAT=none`: the ELF-only tests are replaced by `srdump_elf_none_has_no_section` and
`link_image_none_backend`), `gcc16-ubsan-trap` (`-fsanitize=undefined -fsanitize-trap=undefined`, no runtime
needed; skipped by default where ASan runs), `gcc16-asan-ubsan` (`-fsanitize=address,undefined
-fno-sanitize=null,nonnull-attribute,returns-nonnull-attribute`), `gcc16-tsan` (`-fsanitize=thread`),
`clang-p2996` (`-stdlib=libc++`; only when named and `CXX` is a P2996 clang), `opt-gcc16-debug` (the compiler at
`/opt/gcc-16`). The script sets `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:allocator_may_return_null=1`,
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`, and
probes TSan 40 times first (ASLR entropy problems are retried under `setarch -R`, else SKIP `ENV: TSan vs ASLR`).

```sh
scripts/run-all-tests.sh --quick                     # gcc16-debug only, 5000 fuzz iterations, no install: the session smoke run
scripts/run-all-tests.sh                             # every preset, 200 000 fuzz runs, TSan ×5, install + consumer
scripts/run-all-tests.sh --presets gcc16-asan-ubsan,gcc16-tsan --fuzz-iters 1000000 --tsan-repeat 20
CXX=/opt/gcc-16/bin/g++-16 scripts/run-all-tests.sh --jobs 8 --keep-build
```

**Logs layout.** `logs/serreg-tests-YYYYMMDD-HHMMSS/` (git-ignored) and the same as `.tar.gz`:
`00-summary.txt` (one row per stage: `PASS|FAIL|WARN|SKIP`, seconds, log path, reason; ctest totals per preset;
failed tests per preset; the last 40 lines of every FAIL/WARN log; every sanitizer report verbatim),
`00-environment.txt` (compiler, cmake, ninja, binutils, glibc, host, options, sanitizer option strings, huge-page
facts), `<preset>/{configure,build,ctest}.log` + `LastTest.log` + `LastTestsFailed.log`, `checks/*.log` and
`checks/golden-print.txt`, `fuzz/<target>.log`, `install/*.log`.

**Reading a run.** `00-summary.txt` top to bottom, then `00-environment.txt` (the compiler patch level is the
first suspect when a previously green stage fails), then the stage logs the summary points at. Exit status 0 means
every gating stage passed **or was skipped with a reason**: read the table, never just the exit code.
**SKIP is not PASS.** A SKIP row (no `libasan`/`libtsan` for `CXX`, TSan vs ASLR, no tshark, no huge pages) is
reported as *unverified* with the exact rerun command for the box that can run it. Inside ctest, a DISABLED test
or a 77 exit (`SKIP_RETURN_CODE`) is also a skip.

**Triage** (`docs/test-and-log-protocol.md` §3 in full): configure → environment, fix `CXX`/`ES_SERREG_CATCH2_DIR`,
never patch CMake around it; build → check the toolchain's `<meta>` first, a new warning is fixed at the source;
`golden_*`/`blobs`/`canary_*` → never refreeze to pass (§3 above); `link_*`/`dlopen_*` → `readelf -d` (`FLAGS_1`),
`readelf -S` (`es_serreg_dict`), binutils/glibc facts; `negative_*` → update the expected substring only after
confirming the rejection still happens for the right reason; sanitizer report → fix the code, no suppressions
beyond the three `-fno-sanitize` entries the preset carries; TSan → real unless proven a loader-lock false
positive (then a one-line `no_sanitize("thread")` with a comment); `fuzz/<target>` → reproduce with the seed from
the log, minimise, add the input to the corpus, fix, rerun ≥ 200 000; `gcc16-tsan/concurrency` → 1/20 is a bug,
not flakiness; bench WARN → informational on a loaded machine, record the numbers; `install/*` → packaging
regression, treated as a build failure.

**Reference box vs. sandbox.** The reference box is the maintainer's machine with GCC 16 as `g++`, the sanitizer
runtimes, tshark, Lua 5.2 and 5.4, hugetlbfs, and ≥ 8 quiet cores: it is the only place bench numbers, the
`registry_measure` budgets and a no-SKIP full run are valid. A sandbox run is still useful (it is how the
`docs/test-reports/*-sandbox-*.md` records were made) but its report must say which rows were SKIP or
environment-limited and what remains for the reference box.

**What a release needs.** A green full run on the reference box with the default presets, no SKIP rows, fuzz at
≥ 200 000 iterations, its record committed as `docs/test-reports/<version>-<YYYYMMDD>.md` (verbatim
`00-environment.txt`, the stage table and ctest totals, `checks/golden-print.txt`), the `CHANGELOG.md` entry
(`make-release.sh` refuses without it), the Compiler Explorer lanes rerun when any canonical string changed
(`docs/ce-links.md`), and the tarball from `scripts/make-release.sh`.

---

## 5. Writing a test

### 5.1 Conventions

- **Catch2 v3**, `#include <catch2/catch_test_macros.hpp>`; `REQUIRE` for preconditions, `CHECK` for the
  assertions after them, `STATIC_REQUIRE` for anything that is a constant expression, `SECTION` for variants of
  one setup.
- **Names carry requirement IDs** (or the decision/review ID that introduced the behaviour): `"FR-W1 capture_file:
  compact frames read back, verify and dump"`, `"P 1.7 CRC32C trailers: …"`, `"A log: the gate evaluates …"`,
  `"2.1.16 A3 decode_limits: …"`. Tags group by header or feature (`[capture]`, `[format17]`, `[log]`,
  `[golden]`). `scripts/check-book.py` checks that every test name the book quotes still exists, so renaming a
  case means updating the book.
- **One translation unit per concern**, added to the `serreg_tests` list in `tests/unit/CMakeLists.txt` with a
  one-line comment. A test that pollutes the registry in a way other tests would see (a hand-built node, a
  conflicting definition, hundreds of synthetic types) gets **its own executable** through `es_serreg_catch2_exe`
  or a plain `add_executable`.
- **Corpus types come from `tests/golden/corpus.hpp`** (`corpus::Order`, `sample_order()`, `corpus::Cancel`,
  `corpus::Side`, …); a type defined inside a test file is fine when its presence in `serreg_tests`' registry is
  acceptable (it will appear in `srdump --elf serreg_tests`), and its canonical name must be unique
  (anonymous-namespace or local classes need `[[= es::serreg::name("…")]]`).
- **Helpers:** `tests/support/test_support.hpp` (`all_records`, `guarded_copy`, `embedded_dictionary_of_self`);
  files go under `ES_SERREG_TEST_TMPDIR` and are unlinked at the end of the case; fixtures are reached through
  `ES_SERREG_TEST_SOURCE_DIR "/../fixtures/…"`.
- **No Catch2 assertion nested inside another Catch2 assertion.** Each macro declares a local in its own block;
  with `-Wshadow -Werror` a `CHECK(...)` inside a lambda that is itself the argument of a `CHECK(...)` fails to
  compile. Evaluate the outer expression into a named `const auto` first:

  ```cpp
  const auto delivered = c->poll([&](const dyn::message& m) {
      CHECK(m.data.size() == 12 * 64 - 15);   // assertions inside the lambda: fine
  }) == 1;                                     // not nested in a CHECK (-Wshadow)
  CHECK(delivered);
  ```

- **Determinism:** inject the clock (`capture_options::clock`, `logger::set_clock`), disable host-specific
  writer items (`co.info_host = false`) when bytes are compared, and never depend on test order within an
  executable (Catch2 may shuffle; the registry is shared).
- **Style rules apply to tests too:** `const` parameters and locals, `std::print`/`std::println` only, `main()`
  of a framework-free test hands over to an application class, includes in package → third-party → system order
  (`scripts/format-all.sh` arranges them), `#pragma once` in any test header (it is compiled by `tests/headers`).

### 5.2 (a) A golden `STATIC_REQUIRE` for a hash and a blob

Write the test first with placeholder values, run `serreg_tests "[golden-print]"` (or compile and read the
`static_assert` message), then paste the printed values. In `tests/golden/hashes.cpp`:

```cpp
#define ES_GOLDEN(T, schema, layout)                                                               \
    STATIC_REQUIRE(schema_hash_v<T> == schema##ull);                                               \
    STATIC_REQUIRE(layout_hash_v<T> == layout##ull)

TEST_CASE("FR-H1/FR-H4 golden schema and layout hashes", "[golden]")
{
    ES_GOLDEN(corpus::Order, 0x367f8cd8c6441dd2, 0x0661bb3e388c01fe);
    ES_GOLDEN(corpus::Cancel, 0x35af6843b770c519, 0x99c125fd4fe8673c);
    // a new type: add its line here and in print_one<> of the [golden-print] case
}
```

In `tests/golden/blobs.cpp` the unit is the ENTRY blob (hash of the exact bytes, and the size):

```cpp
template<class T> constexpr std::span<const std::byte> blob_of()
{
    return std::span<const std::byte>(detail::type_blob_bytes_v<T>);
}
template<class T> constexpr std::uint64_t blob_hash() { return fingerprint64(blob_of<T>()); }
template<class T> constexpr std::size_t blob_size() { return blob_of<T>().size(); }

#define ES_BLOB(T, hash, size)                                                                     \
    STATIC_REQUIRE(blob_hash<T>() == hash##ull);                                                   \
    STATIC_REQUIRE(blob_size<T>() == size)

TEST_CASE("cc F1 golden ENTRY blob hashes and sizes", "[golden][blob]")
{
    ES_BLOB(corpus::Scalars, 0x5b676741452a6b58, 192);
    ES_BLOB(corpus::Padded, 0x9c624beb5c8b5b77, 168);
}
```

Both files must also compile in standalone mode: `hashes.cpp` is checked by `golden_hashes_standalone_compiles`
(`-DES_SERREG_GOLDEN_STANDALONE -fsyntax-only`), so use nothing from Catch2 beyond `TEST_CASE`, `STATIC_REQUIRE`
and `SUCCEED()` there.

### 5.3 (b) A capture round trip with verify and dump checks

Adapted from `tests/unit/test_capture.cpp`:

```cpp
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cstring>
#include <string>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace
{
std::string tmp_path(const char* const name)
{
    return std::string(ES_SERREG_TEST_TMPDIR) + "/" + name;
}
} // namespace

TEST_CASE("FR-W1 capture_file: compact frames read back, verify and dump", "[capture][file]")
{
    const std::string path = tmp_path("compact.ser");
    {
        capture_file f(path);
        REQUIRE(f.is_open());
        REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
        REQUIRE(serialize(f, corpus::sample_cancel(), "why"_lit));
        REQUIRE(serialize(f, corpus::Padded{9, 8, 7}, corpus::Side::sell));
        REQUIRE(f.close());
    }
    const auto bytes = tools::read_file(path);
    REQUIRE(bytes.has_value());
    CHECK(std::memcmp(bytes->data(), fmt::file_magic.data(), 4) == 0);

    const auto rep = tools::verify(*bytes);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 3);  // data frames only: the writer-info frame is not counted
    CHECK(rep->records == 6);
    CHECK(rep->bytes == frame_size<decltype("evt"_lit), corpus::Order>() +
                            frame_size<corpus::Cancel, decltype("why"_lit)>() +
                            frame_size<corpus::Padded, corpus::Side>());

    SECTION("text dump has field names and values")
    {
        std::string text;
        REQUIRE(tools::dump(*bytes, nullptr, tools::dump_options{}, text));
        for (const char* const needle : {"corpus::Order", "id: 42", "side: corpus::Side::buy (1)",
                                         "corpus::Cancel", "reason: 7"})
        {
            INFO(needle);
            CHECK(text.find(needle) != std::string::npos);
        }
    }
    SECTION("json dump quotes 64-bit integers")
    {
        std::string js;
        tools::dump_options o;
        o.fmt = tools::format::json;
        REQUIRE(tools::dump(*bytes, nullptr, o, js));
        CHECK(js.find("\"id\":\"42\"") != std::string::npos);
    }
    SECTION("a truncated capture is reported, not crashed on")
    {
        std::vector<std::byte> bad = *bytes;
        bad.resize(bad.size() - 4);
        const auto r = tools::verify(bad);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->ok());
        CHECK(r->frames == 2);
    }
    ::unlink(path.c_str());
}
```

### 5.4 (c) A format-1.7 feature test: CRC corruption → `crc_mismatch`

Adapted from `tests/unit/test_format17.cpp`. The fixed clock and `info_host = false` keep the file deterministic;
the frame header is read with the public `read_frame_header`, then one payload byte is flipped and every reader is
asked:

```cpp
namespace
{
std::uint64_t fixed_clock() noexcept
{
    return 1'600'000'000'000'000'000ull;
}
std::uint32_t u32_at(const std::vector<std::byte>& b, const std::size_t at)
{
    std::uint32_t v{};
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
} // namespace

TEST_CASE("P 1.7 CRC32C trailers: written per frame, checked by every reader, a corrupt frame is skipped",
          "[format17][crc]")
{
    STATIC_REQUIRE(crc32c(std::string_view("123456789")) == 0xE3069283u); // the standard check value
    const std::string path = tmp_path("f17_crc.ser");
    capture_options co;
    co.crc = true;
    co.clock = &fixed_clock;
    co.info_host = false;
    {
        capture_file f(path, co);
        REQUIRE(f.is_open());
        REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
        REQUIRE(serialize(f, corpus::sample_cancel(), "why"_lit));
        REQUIRE(serialize(f, corpus::Side::buy));
        REQUIRE(f.close());
    }
    auto bytes = *tools::read_file(path);
    CHECK((u32_at(bytes, 8) & fmt::file_flag_crc_all) != 0);

    std::size_t n = 0, first_data = 0;
    const auto w = tools::for_each_frame(
        std::span<const std::byte>(bytes), [&](const dyn::dictionary&, const tools::frame_ref& fr) {
            if (!first_data)
                first_data = fr.offset;
            ++n;
            return true;
        });
    REQUIRE(w.has_value());
    CHECK(n == 3);
    const auto h = read_frame_header(std::span<const std::byte>(bytes).subspan(first_data));
    REQUIRE(h.has_value());
    CHECK(h->crc());
    CHECK(h->frame_len == frame_size<decltype("evt"_lit), corpus::Order>(serialize_options{.crc = true}));
    CHECK(h->payload_end() == h->frame_len - fmt::crc_trailer_size);

    // flip one payload byte of the first data frame
    bytes[first_data + fmt::frame_header_size + 2] ^= std::byte{0x40};
    const auto rep = tools::verify(bytes);
    REQUIRE(rep.has_value());
    CHECK_FALSE(rep->ok());
    CHECK(rep->frames == 2);                                   // the bad frame is skipped, not delivered
    REQUIRE(!rep->problems.empty());
    CHECK(rep->problems[0].find("crc mismatch") != std::string::npos);
    corpus::Order o{};
    CHECK(deserialize(std::span<const std::byte>(bytes).subspan(first_data), "evt"_lit, o).error() ==
          error::crc_mismatch);
    CHECK(read_frame_header(std::span<const std::byte>(bytes).subspan(first_data)).error() ==
          error::crc_mismatch);
    ::unlink(path.c_str());
}
```

### 5.5 (d) A logger test: gate and rendering

Adapted from `tests/unit/test_log.cpp`. The scope type is the logger's dictionary scope; `log_options` is an NTTP;
`ES_SERREG_LOG` is the only form that skips argument evaluation:

```cpp
namespace logtest
{
struct app_scope
{
};
} // namespace logtest

namespace
{
std::uint64_t ticks = 1'000;
std::uint64_t test_clock() noexcept
{
    return ticks += 10;
}
using file_t = scoped_capture_file<logtest::app_scope>;
constexpr log_options plain{.clock = clock_policy::user};
constexpr log_options floored{.floor = level::warn, .clock = clock_policy::user};
} // namespace

TEST_CASE("A log: the gate evaluates nothing below the level; the floor compiles the call out", "[log][gate]")
{
    const std::string path = tmp_path("log_gate.ser");
    capture_options co;
    co.info_host = false;
    co.clock = &test_clock;
    co.log_clock = "user";
    file_t f(path, co);
    REQUIRE(f.is_open());
    logger<plain, file_t> lg{f, level::info};
    lg.set_clock(&test_clock);
    int evaluated = 0;
    const auto cost = [&] {
        ++evaluated;
        return 7;
    };
    CHECK_FALSE(ES_SERREG_LOG(lg, debug, "debug {}"_, cost()));
    CHECK(evaluated == 0);                                   // the argument was never evaluated
    const int line_info = __LINE__ + 1;
    CHECK(ES_SERREG_LOG(lg, info, "info {}"_, cost()));
    CHECK(evaluated == 1);

    logger<floored, file_t> fl{f};
    STATIC_REQUIRE(decltype(fl)::floor == level::warn);
    CHECK_FALSE(fl.enabled(level::info));
    CHECK_FALSE(ES_SERREG_LOG(fl, info, "never {}"_, cost()));
    CHECK(evaluated == 1);                                   // compiled out
    CHECK(fl.warn("floored {}"_, 2));                        // no call site: file "-", line 0
    REQUIRE(f.close());

    const auto rep = tools::verify(path);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 2);                                 // info, warn

    // rendering: srdump --level is tools::dump with logs_only
    tools::dump_options o;
    o.logs_only = true;
    std::string text;
    REQUIRE(tools::dump(path, nullptr, o, text));
    const std::string file = __FILE__;
    const std::string base = file.substr(file.find_last_of('/') + 1);
    CHECK(text.find("INFO  " + base + ":" + std::to_string(line_info) + " info 7\n") != std::string::npos);
    CHECK(text.find("WARN  floored 2\n") != std::string::npos);
    o.min_level = 3;                                         // --level warn hides the info line
    text.clear();
    REQUIRE(tools::dump(path, nullptr, o, text));
    CHECK(text.find("info 7") == std::string::npos);
    ::unlink(path.c_str());
}
```

### 5.6 (e) A dlopen / reload test sketch

The real test is `tests/dlopen/host.cpp` (one mode per ctest) with the plugins of its `CMakeLists.txt`; a new
scenario is a new `mode` there and one `add_test` line. The skeleton of a mode:

```cpp
// host <plugin.so> <mode>: the host serializes its own type first so the plugin's entries are additive
int run_keep(const char* const path)
{
    using namespace es::serreg;
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    const std::size_t images0 = reg.images().size();
    if (reg.find_type_by_name("plugin::Tick"))
        return fail("host must not know plugin::Tick before dlopen");

    void* const h = ::dlopen(path, RTLD_NOW);
    if (!h)
        return fail(::dlerror());
    reg.refresh();                           // publish and notify on this thread
    const auto expected = reinterpret_cast<int (*)()>(::dlsym(h, "plugin_expected_entries"));
    const std::size_t added = static_cast<std::size_t>(expected());
    if (reg.size() != size0 + added)         // FR-R6: registered by dlopen itself
        return fail("registry did not grow by the plugin's entry count");
    const type_desc* const tick = reg.find_type_by_name("plugin::Tick");
    if (!tick || reg.images().size() != images0 + 1)
        return fail("plugin type or image missing (FR-R12)");

    if (::dlclose(h) != 0)                   // never aborts, needs no environment variable
        return fail("dlclose failed");
    reg.refresh();
    if (reg.size() != size0 + added || reg.retained() != added)
        return fail("entries must be retained after dlclose (FR-R11)");
    if (reg.find_type_by_name("plugin::Tick") != tick)
        return fail("the retained descriptor must be the same object");
    if (reg.images().size() != images0)
        return fail("images() still lists the unloaded plugin");
    std::println("ok: dlclose kept the plugin's entries");
    return 0;
}
```

The plugin side is `plugin.cpp`: a namespace chosen by `-DES_PLUGIN_NS=…`, `plugin_run()` serializing its type
into a `vector_sink`, `plugin_expected_entries()` returning the count of distinct new entities (types, components,
literals, calls — an alias of a literal another image already registered adds nothing). Build it with
`es_serreg_plugin(name src [DEFINES…])` (a `MODULE`, hidden visibility, no prefix). Any new mode that touches the
registry under concurrency joins the TSan regex in `run-all-tests.sh` (`dlopen_[ABDEFJL]|…`).

### 5.7 (f) A Python parity invocation

What the ctest runs (from `tests/python/CMakeLists.txt`), usable by hand on any capture:

```sh
python3 tests/python/parity.py \
    --srdump build/gcc16-debug/tools/srdump/srdump \
    --package python \
    --log-writer build/gcc16-debug/tests/python/python_log_capture \
    tests/fixtures/golden/orders.ser tests/fixtures/golden/features.ser \
    tests/fixtures/corpus/capture/orders-1.6.ser \
    tests/fixtures/corpus/capture/bare_frames.bin:tests/fixtures/corpus/rri/selftest.rri
# exit 0 parity, 1 the first mismatch (printed), 77 srdump cannot run here
python3 -m serreg tests/fixtures/golden/features.ser --info          # the reader alone (cwd: python/)
```

A new fixture or a new rendering (a new `log` field, a new container shape) is added to `es_serreg_py_captures`
and, when the JSON shape changes, to the comparison in `parity.py` and to the reader in `python/serreg/`.

### 5.8 (g) A negative compile test

A file that must not compile, with the expected diagnostic in a comment and in the CMake line:

```cpp
// tests/negative/neg_pointer_member.cpp — negative (FR-S7): a pointer member without [[=skip]] is not
// serialisable; the diagnostic names the rule.
// expected diagnostic: "pointers are not serialisable"
#include <es/serreg/serreg.hpp>

#include <vector>
namespace neg
{
struct HasPtr
{
    std::uint64_t key;
    const char* name;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasPtr{1, "x"}) ? 0 : 1;
}
```

```cmake
# tests/negative/CMakeLists.txt
es_serreg_negative_test(neg_pointer_member "pointers are not serialisable")
```

The substring must be part of the `static_assert` message, not of a compiler-generated line (those change between
GCC versions). Before adding one, make sure the control file `neg_control_compiles.cpp` still compiles — a harness
that rejects everything would pass every negative test.

### 5.9 (h) A fuzz target skeleton

A target is an `extern "C"` function with the libFuzzer signature that returns normally for any input. Adding one
means: the declaration in `fuzz_targets.hpp`, the body in `fuzz_targets.cpp`, a line in the driver's name table
(`fuzz_driver.cpp`, `if (name == "rri") o.target = &fuzz_rri; …`), the name in the `foreach(t rri capture elf
config)` of `tests/fuzz/CMakeLists.txt` and of `run_fuzz()` in `run-all-tests.sh`, and a seed directory
`tests/fixtures/corpus/<target>/` written by `runtime_selftest`:

```cpp
// fuzz_targets.hpp
extern "C" int fuzz_rri(const std::uint8_t* data, std::size_t size); // dyn::dictionary::add_rri

// fuzz_targets.cpp
extern "C" int fuzz_rri(const std::uint8_t* const data, const std::size_t size)
{
    using namespace es::serreg;
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    dyn::dictionary d;
    if (d.add_rri(bytes))
    {
        (void)dyn::to_rri(d); // re-encoding what was accepted must be safe too
        (void)d.to_json();
        (void)d.to_toml();
    }
    dyn::dictionary e;
    (void)e.add_entries(bytes); // the same bytes as a bare ENTRY stream (ELF section content)
    return 0;
}
```

Run it under the sanitizer preset before claiming anything: `build/gcc16-asan-ubsan/tests/fuzz/serreg_fuzz rri
tests/fixtures/corpus/rri 200000 1`. A crash, a sanitizer report, an input over 2 s or over 512 MiB of RSS growth
fails the run and leaves the input next to the corpus.

---

## 6. Checks that are not tests

| check | command | what it enforces |
|---|---|---|
| Formatting | `scripts/format-all.sh` (format in place); the run script and `make-release.sh` use `clang-format --dry-run --Werror` | `.clang-format` (Microsoft base, 100 columns); reference clang-format 22, tree clean under 18–23; `IncludeBlocks: Regroup` orders the includes. Install the hook once with `scripts/install-hooks.sh` |
| Include rules | `scripts/check-includes.py` (also ctest `headers_include_rules`) | `#pragma once` first; package headers before system headers; one opt-out per line with `// include-order: after …` |
| Book sync | `scripts/check-book.py [--fix]` (gating stage `book/check-book.py`) | a page per source file; `file:N` citations in range and on their code block (`--fix` moves drifted ones); no name from the STALE list; every quoted test name exists in `tests/`; **docs**: every `docs/<name>.md` / `book/<name>.md` path cited in `README.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `docs/**/*.md` and `book/**/*.md` exists in the tree (a plain regex over back-quoted or bare paths; a retired document still under `remove-me/<path>` satisfies a historical citation, placeholders such as `vX.Y.Z.md` and paths under `tests/fixtures/` are skipped, and a path followed by "(planned …)" / "(to be written …)" is an announcement, not a citation). A problem prints as `docs: <file>:<line>: cites <path> which does not exist`; it catches a renamed or removed document that a public page still points at |
| Public subset | `public-files-list.txt`, checked by `scripts/make-release.sh --public` | every file classified as a plain, `#private:` or `#decide:` line; no file in two classes; no stale public line; warnings for public files citing excluded ones. A new test file or fixture is classified in the change that adds it |
| Version bump | `scripts/bump-version.sh X.Y.Z` | changes only the places that hold *the current* version (`CMakeLists.txt`, `version.hpp`, the vcpkg manifests, `test_tools.cpp`'s `library_version` check, `runtime_selftest.cpp`'s and `README.md`'s `srdump X.Y.Z (format` lines); never a blanket `sed` |
| Release | `scripts/make-release.sh [--public] [--out DIR]` | reads the version, refuses to overwrite, requires a `## vX.Y.Z` CHANGELOG entry, runs the format check and `check-book.py`, excludes build/log/stray `.ser`/`.rri` files, prints file count and sha256 |

---

## 7. Known gaps and what the maintainer must verify on the reference box

- **TSan dlopen runs.** The sandbox records (`docs/test-reports/*-sandbox-*.md`) ran `gcc16-tsan` with ×5 repeats
  on two cores; the reference run should use `--tsan-repeat 20` on ≥ 8 cores, in particular
  `dlopen_F_concurrent_dlclose`, `dlopen_J_readers_vs_dlclose_churn`, `dlopen_L_two_thread_load_unload` and
  `hrtt_*_thread`, which exercise the push path (main-program entries viewed in place, C-11).
- **Registry 100k budget (FR-R13, G-5).** `registry_measure_100k --assert` is release-only and non-gating; the
  sandbox numbers (2 vCPU, -O2) were push 111 ms + publish 29 ms against a 100 ms budget, 10k well inside its
  10 ms. The reference-box figure decides whether the budget or the code moves; record it in
  `docs/ser-reg-architecture.md` and the test report.
- **Compiler Explorer lanes (FR-H4).** `ce-run.py` for the eight examples and the amalgamated golden check of
  `docs/ce-links.md` must be rerun after any change to a canonical string, a blob or a frame layout; the
  `clang-libstdcxx` lane is unavailable (libstdc++'s `<meta>` is GCC-intrinsic-only) and EDG rejects `template
  for`, so today the gate is `gcc` + `clang-libcxx`. Short links are permanent and public.
- **Reference records.** The latest records in `docs/test-reports/` are sandbox runs; every release needs its own
  green full run on the reference box and its record, and `make-release.sh` refuses until the release's
  `CHANGELOG.md` entry exists.
- **Environment-dependent skips** that are SKIP everywhere but the reference box: the huge-page `es::ipc` test
  (needs a writable 2 MiB hugetlbfs mount and two free pages), `link_lto` (DISABLED where CMake's IPO check
  fails), `wireshark_dissector_parity` and `_lua5x` (tshark, `lua5.2`, `lua5.4`), `python_reader_parity` (Python
  ≥ 3.10), `gcc16-asan-ubsan`/`gcc16-tsan` (the runtimes for the chosen `CXX`).
- **Not covered by the Python reader:** XXH3 recomputation, `--elf`, `--diff`, `--resync`, pipes; parity is on
  decoded values, not on those tool paths.
- **Fuzz depth.** The ctest fuzz runs are 300 mutations per seed; only the protocol run (200 000, ASan/UBSan) and
  a longer `--fuzz-iters 1000000` after reader changes count as coverage of the never-crash contract.
- **Stale comments noticed while writing this (not test defects):** `tests/golden/make_fixtures.cpp` says the 1.0 files are kept in
  `tests/fixtures/golden-1.0/`, a directory that no longer exists (the compatibility files are the `*-1.6.*`
  corpus seeds, and `check-book.py` already lists `golden-1.0` as a stale name); the `tests/golden/CMakeLists.txt`
  header names only `orders.*` as what `make_fixtures` writes (it also writes `features.ser`).
