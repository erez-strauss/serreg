# Executables

Every program the tree builds, grouped by purpose. Target names are the CMake target names; the binaries land in
`build/<preset>/` (srdump) or the matching sub-directory (`tests/unit/`, `examples/`, …). Unless stated, exit
code 0 means success and non-zero means failure.

## Shipped tool

### `srdump`  (`tools/srdump/main.cpp`, installed to `bin/`)

`int main(int argc, char** argv) { return es::serreg::cli::run(argc, argv); }` — the whole behaviour is in the
library (chapter 10, `files/src-cli.cpp.md`); `srdump` links `es::serreg_tools` (decision G)
(`libserreg_tools`, which brings `libserreg`). Options:

| option | effect |
|---|---|
| `<capture.ser>` | a FILE capture or a bare frame stream (the latter needs `--dict` / `--elf` / `--rri`) |
| `--dict-only` | print only the dictionary |
| `--verbose` | with the dictionary: its blocks (the opening block, every dictionary frame: offset, what it added); in the frame dump a marker line per dictionary frame (chapter 10.1d) |
| `--text` / `--json` / `--toml` | output format (default text; records are JSON lines under `--toml`) |
| `--verify` | check every frame and record against the dictionary; exit 1 on problems (CRC trailers checked; meta frames not counted) |
| `--info` | (format 1.7) the file's header, dictionary chain, checkpoints and writer-info items, by seeks — no frame walk; `--json` for one object; a pipe is a usage error (chapter 10.1d) |
| `--resync` | after a frame header that cannot be parsed, continue at the next checkpoint frame (one problem line for the skipped range) instead of stopping; files read in windows only |
| `--level <lvl>` | only log frames at or above `trace`\|`debug`\|`info`\|`warn`\|`error`\|`fatal`, one rendered line each (`--json`: `{"frame":N,"log":{…}}`); an unknown level is exit 2 (chapter 10.1e) |
| `--since <t>`, `--until <t>` | log frames whose timestamp is in range (inclusive); `<t>` = a number in the writer's clock units or `YYYY-MM-DD[THH:MM[:SS[.fff]]]` UTC; alone they imply `--level trace` |
| `--dict <file.rri>` | extra dictionary (repeatable) |
| `--elf <binary>` | extra dictionary from the binary's `es_serreg_dict` section (repeatable) |
| `--rri <image.rri>` | read an RRI (dictionary only) |
| `--rri-out <file>` | with `--elf`/`--rri` and no capture: write the merged dictionary as an RRI |
| `--wireshark <f.lua\|->` | write a Wireshark Lua dissector for the merged dictionary of the capture, `--dict`, `--elf` and `--rri` (`-` = stdout); conflicting sources → exit 1, no source → exit 2 — except in an application embedding `cli::run` whose own registry has entries: then the dissector describes that registry (chapter 10.4, `docs/wireshark.md`) |
| `--cxx <types.hpp\|->` | write C++ definitions of the merged dictionary's types (the sources and refusals of `--wireshark`), ending in `static_assert`s of every size, offset, name and hash, so compiling the file verifies the regeneration (chapter 10.4b, `files/src-cxxgen.cpp.md`); two types of one name → exit 1 |
| `--cxx-router` | with `--cxx`, append `install_handlers(es::serreg::frame_router&)`, one handler per call signature |
| `--namespace old=new` | with `--cxx`, declare the types of namespace `old` in `new` (repeatable); the canonical name is kept through `[[=es::serreg::name("…")]]` |
| `--direction <d>` | with `--diff`, whose reader the verdict is for — `old-data` (default: a reader built from the new side reads old data; removals break), `new-data` (an old reader reads new data; additions break), `both`, `none` (neither breaks); layout, call and literal changes break in every direction (chapter 10.1b) |
| `--window <size>` | read the capture this many bytes at a time (default `10M`; `K`/`M`/`G` suffixes, powers of two; at least 4096, else exit 2); a capture's size is bounded by the file system, not by the tool (chapter 10.1a) |
| `--frame-limit <size>` | the largest frame accepted for this run (`decode_limits::frame_bytes`, default `64M`; at least 16 bytes); a larger `frame_len` is a `bad frame_len` problem and the walk stops (or resyncs with `--resync`) |
| `--frames A..B` | only frames A..B (inclusive, 0-based; `A` alone = one frame); the numbering counts meta frames too (a 1.7 file's frame 0 is its writer-info frame) |
| `--type <name\|0xhash>` | only records of this type |
| `--hex` | also print raw payload bytes |
| `--raw` | print frames as records too, not only the rendered message |
| `--flatten` | present base-class members inline |
| `--full` | do not elide arrays longer than 16 elements |
| `--stdin` | read the capture from standard input |
| `--config <file.toml>` | settings file (TOML subset: keys = long option names without dashes); explicit flags win |
| `--program-hash` | print the program hash of the capture / RRI / ELF dictionary and exit |
| `--diff <a> <b>` | compare two dictionaries (each an `.rri` or an ELF binary); exit 1 if breaking for the `--direction` (`old-data` by default, so a removed type, call or literal is breaking) |
| `--version` | `srdump 2.2.6 (format 1.7, hash_algo 2)` |
| `--help` | usage |

Exit codes: 0 ok, 1 problems/decode error/breaking diff/schema conflict, 2 usage error. Every path over a capture reads it in windows (`--verify` streams a pipe too); a *dump* of standard input or a pipe is the
one input still read whole (within `decode_limits::input_bytes`, 1 GiB), because the dictionary is printed first
and a pipe cannot be read twice. `--verify` keeps at most 1000 problem lines (`problem: ... and N more`;
`"problems_omitted"` in `--json`).

**Environment honoured by every program linked with `libserreg`** (read by the priority-101 constructor
`serreg_init` in `src/registry.cpp`, so it must be set before the process starts; with either image backend):

| variable | values / default | effect |
|---|---|---|
| `ES_SERREG_DUMP_REGISTRY` | path or `-` | write the registry at exit (default format JSON) |
| `ES_SERREG_DUMP_REGISTRY_FD` | fd number | write the registry to an open descriptor (default format RRI) |
| `ES_SERREG_DUMP_FORMAT` | `rri` \| `json` \| `toml` | format for the two above |
| `ES_SERREG_DUMP_ON` | `exit` (default) \| `change` \| `both` | when to dump; `change` = at every published change (e.g. after each `dlopen`/`dlclose`, on the thread that reads next or calls `registry::refresh()`) |
| `ES_SERREG_DUMP_EXIT` | `1` | `_exit` right after the first dump (schema extraction) |
| `ES_SERREG_DUMP_SIGNAL` | `USR1` \| `USR2` \| `RTMIN` \| `RTMIN+n` | a `sigwait` thread dumps on that signal; anything else is ignored with one stderr line |
| `ES_SERREG_ALLOW_DLCLOSE` | any | ignored (one stderr line at startup): `dlclose` never aborts and the unloaded image's entries are retained; it once selected an unlink mode instead of an abort |
| `ES_SERREG_CONFLICT` | `warn` (default) \| `abort` \| `silent` | reaction to a same-schema/different-layout registration |

## Examples (built when `ES_SERREG_BUILD_EXAMPLES=ON`, the default for a top-level build)

Each example's `main()` only parses the command line and calls an application class
(`files/examples.md`); output uses `std::print`/`std::println`.

### `file_store`  (`examples/file_store.cpp`)
`file_store [path]` (default `file_store.ser` in the current directory). Writes three `("new order"_, Order)`
frames, one `("cancel"_, Cancel, "user"_)` and one `("eod"_)`, closes (format 1.7: nothing is rewritten), reads the file back,
verifies and dumps it (`file_store::app::run()`). Prints `<path>: 5 frames, 10 records, <n> bytes, ok` then the
dump. ctest: `example_file_store` (writes into the build tree).

### `print_sinks`  (`examples/print_sinks.cpp`)
`print_sinks [dir]` (default `.`). `app::program::run(dir)` writes an archive (`capture_file`, whole-registry
dictionary) and a scoped log (`scoped_capture_file<log_scope>`), the log through `sr::println`, unqualified
`println` (ADL), `sr::println<"…">` and `sr::print`, then prints both dictionaries' sizes (`tools::info`): the
log's is `0 types, 4 literals, 4 calls`. ctest: `example_print_sinks`.

### `register_namespace`  (`examples/register_namespace.cpp`)
`register_namespace [dir]` (default `.`). `ES_SERREG_REGISTER_NAMESPACE(app::messages);` at namespace scope registers
the five serialisable types of `app::messages` (a `Helper` with a pointer is listed but skipped, a `[[=skip]]`
type is neither); `app::program::run(dir)` prints the selection, the `Msg*` and the `[[=message]]` sub-groups,
the registry's answer for types no code serialises, and writes `register_namespace.ser`, a capture with no frames
whose dictionary has `5 types, 0 calls`. ctest: `example_register_namespace`.

### `ipc_ring`  (`examples/ipc_ring.cpp`)
No arguments; `main()` is `ipc_ring::app{}.run()`. An `es::ipc` SPSC ring (`ring_kind::spsc`) with 128 KiB of
records in 64-byte units in an anonymous shared region, shared with a forked child. The parent process serializes 100 000 frames — `("order"_, Order)` three
times out of four, `("cancel"_, Cancel)` otherwise, `claim()` spinning while the queue is full — directly into ring
records through `es::ipc::sink`. The child process polls each record in place and decodes it with
`dispatch<Order, Cancel>`. Prints `orders=75000 cancels=25000 literals=100000 total_qty=…`. ctest:
`example_ipc_ring` (timeout 120 s). Links `es::serreg` and `es::ipc` (`es::serreg` does not bring `es::ipc`).

### `ipc_log_producer` / `ipc_log_consumer`  (`examples/ipc_log/`)
Two programs over a named `es::ipc` SPSC queue (`files/examples.md`). `ipc_log_producer --expect FILE [--name REGION]
[--rounds N] [--linger-ms MS]` (region default `serreg_ipc_log`, 3 rounds, 20 000 ms linger) sends its dictionary in
band, then 102 distinct `println` formats, typed `Fill` frames, one large batch per round, a foreign frame
and an end record, and writes the reference text to `FILE`. `ipc_log_consumer [--name REGION] [--out FILE]
[--timeout-ms MS]` (stdout, 20 000 ms) decodes `Fill` with a typed handler and renders everything else from the
in-band dictionary. Usage errors exit 2; each prints a status line on stderr. ctest: `example_ipc_log`
(`run_ipc_log.cmake`: both at once, a random region name, the two texts compared byte for byte; timeout 120 s).
Both link `es::serreg` and `es::ipc`.

### `dictionary_deltas_host` + `dictionary_deltas_plugin_{tick,quote}.so`  (`examples/dictionary_deltas/`)
`dictionary_deltas_host <out.ser> <plugin.so>... [--frames N]` (default 3 frames per phase) writes a reproducible
1.7 capture whose dictionary grows while it is written (`files/examples.md`, the directory's `README.md`): its own
frames, then — after each `dlopen` and `registry::instance().refresh()` — the plugin's frames, each plugin's first
one preceded by a dictionary delta frame; checkpoints with full dictionaries every 96 bytes. Prints `srdump --info`'s
text (`tools::info`) and a summary line; usage errors exit 2. ctests: `example_dictionary_deltas`
(`run_dictionary_deltas.cmake`: the host, then `srdump --info` must show `3 block(s)` and `--verify` must pass) and
`example_dictionary_deltas_fixture_{verify,json,info}` on the checked-in copy
`tests/fixtures/corpus/capture/dictionary_deltas-1.7.ser`. Needs the shared `libserreg`.

### `embedded_cli`  (`examples/embedded_cli.cpp`)
The srdump command line inside an application via `cli::app`, held by `embedded_cli::application` (`main()`
calls its `parse(argc, argv)` and `run()`). `embedded_cli --version` →
`embedded_cli 0.1 (format 1.7, hash_algo 2)`; `embedded_cli --trace --selftest out.ser --verify out.ser`
writes three `serialize<"tick">` frames and verifies them (`ok: 3 frames, 6 records`); no arguments prints the
program's own registry; `embedded_cli --wireshark <f.lua|->` with no other source writes a Wireshark
dissector for that registry. Links `es::serreg_tools`. ctests: `example_embedded_cli_version`, `_selftest`,
`_default`, `_wireshark`.

## Test executables

| target | source | run by (ctest) | what it does |
|---|---|---|---|
| `serreg_tests` | `tests/unit/test_*.cpp` + `tests/golden/hashes.cpp`, `blobs.cpp` | one ctest per Catch2 test case (`catch_discover_tests`) | the main unit suite, multi-TU; `serreg_tests "[golden-print]"` prints every frozen hash/blob value |
| `serreg_conflict` | `tests/unit/test_conflict.cpp` | its Catch2 cases | FR-R4: a hand-built node with the same schema hash and another layout is recorded as a conflict |
| `serreg_alloc_tests` | `tests/unit/test_alloc.cpp` | its Catch2 case | the canonical sort's order table is the one allocation of `serialize` into a claiming sink: a fail-on-demand global `operator new` shows 64 entries allocate nothing and 65 throw `std::bad_alloc` out of `serialize` (review B1); own executable because it replaces the allocator |
| `serreg_validate_off_tests` | `tests/unit/test_validate_off.cpp` | its Catch2 case | FR-D5: the read-side enum range check with the default policy (`ES_SERREG_VALIDATE_READ=0`): the bytes `serreg_tests` (built with the policy on) rejects read through unchanged; a separate executable because the policy is per translation unit |
| `premain_check` | `tests/unit/premain_check.cpp` | `premain_registry_count`, `premain_env_dump`, and the `srdump_*` tests that read its capture | `main()` is `premain_check::app{argv[1] or "premain.ser"}.run()`; the app's first member initialiser reads the registry count before anything else runs, then it checks the entities and writes a capture (argv[1]) |
| `runtime_selftest` | `tests/unit/runtime_selftest.cpp` | `runtime_selftest` | framework-free test of the runtime/tools layer on hand-built dictionaries (`runtime_selftest::app`, 14 member functions); `runtime_selftest <out.ser> <fixtures-dir>` also regenerates `tests/fixtures/corpus/*` and `negative/*` |
| `signal_dump_test` | `tests/unit/signal_dump_test.cpp` | `signal_dump_usr2` | forks, sets `ES_SERREG_DUMP_SIGNAL=USR2`, re-execs itself, signals the child, checks the dumped RRI |
| `registry_scale` | `tests/unit/registry_scale.cpp` | `registry_scale_one_index_for_startup` | `main()` runs `registry_scale::app{}.run()`: 128 types registered before `main()`, one index published at the first read; prints the counts, exit 1 otherwise |
| `local_in_main` | `tests/unit/local_in_main.cpp` | `local_class_in_main_serialises` | `main()` defines a local struct (the point of the test) and runs `local_in_main::app<Local>{}.run()`: the canonical name must be the explicit `local_in_main::Local` and a `serialize`/`deserialize` round trip must succeed; prints both, exit 1 otherwise (review C-05: GCC 16 cannot take `parent_of(main)`) |
| `make_fixtures` | `tests/golden/make_fixtures.cpp` | `golden_fixtures_generate` (+ `golden_fixture_*_byte_exact`, `golden_fixture_features_byte_exact`) | `make_fixtures <dir>` writes `orders.{ser,rri,text,json,toml}` (20 registry entries, 11 data frames, 34 records) and `features.ser` (the same frames with CRC trailers and checkpoints); fixed capture options (no host items, a fixed clock) keep them reproducible |
| `make_dynamic_fixtures` | `tests/golden/make_dynamic_fixtures.cpp` | `golden_dynamic_fixtures_generate` | `make_dynamic_fixtures <dir>` writes `dynamic.{ser,text,json,toml}` |
| `rri_canary` | `tests/canary/rri_canary.cpp` | `format_canary_rri` | decodes an RRI from literal offsets without any library header; args `<rri> [minor n_entries n_literals n_calls]` (the ctest: `7 20 8 9`); it also checks the ARGs of the one call with constants |
| `serreg_fuzz` | `tests/fuzz/fuzz_driver.cpp`, `fuzz_targets.cpp` | `fuzz_rri`, `fuzz_capture`, `fuzz_elf`, `fuzz_config` | `serreg_fuzz <rri\|capture\|elf\|config> <corpus-dir> [iterations=2000] [seed=1]`; replays the corpus then mutates; RLIMIT_AS 2 GiB outside sanitizers |
| `serreg_libfuzz_<t>` | `fuzz_targets.cpp` | — | only with `-DES_SERREG_LIBFUZZER=ON` and clang (`-fsanitize=fuzzer`) |
| `serreg_ipc_tests` | `tests/unit/test_ipc.cpp` | one ctest per Catch2 case (`ipc: …`, `ipc SPSC: …`, `ipc SPMC: …`, `ipc threads: …`, `ipc processes: …`) | `es::ipc`: the variable-size SPSC queue and SPMC broadcast between threads and processes, layout 2, validation of forged headers, the regions (tmpfs, huge pages — SKIP without a pool, I-06, I-08) (chapter 11); separate from `serreg_tests` so its types stay out of the golden registry; links `es::ipc` only |
| `serreg_hrtt_raw`, `serreg_hrtt_serreg` | `tests/bench/hrtt_raw.cpp`, `hrtt_serreg.cpp`, `hrtt_common.hpp` | `hrtt_{raw,serreg}_smoke_{process,thread}` (2 000 round trips, functional only) | half-RTT between two processes over two `es::ipc` rings (SPMC with one reader, or SPSC with `--ring spsc`), raw zero-copy packets vs es::serreg frames; options below; driven by `scripts/bench-hrtt-*.sh`; `main()` parses the options (`hrtt::parse`) and runs `hrtt::app<Codec>` |
| `writer`, `reader1`, `reader2`, `reader3` (targets `user_example1_*`) | `examples/user_example1/*.cpp` | `user_example1_writer`, `_reader1`, `_reader1_blocks`, `_reader2`, `_reader2_blocks`, `_reader3`, `_reader3_blocks` | a user-style example: a complex capture with `println`, read back typed (a `frame_router`), generically, and hybrid (`files/examples.md`); argument: the file (default `user_example1.ser`); every reader reads in windows by default, `--mapped` maps the whole file and `--blocks` (the default, kept for the `_blocks` ctests) changes nothing (`ue1::reader_options`) |
| `registry_measure` | `tests/bench/registry_measure.cpp` | `registry_measure_10k`, `registry_measure_100k` (`--assert`), `registry_measure_elf` (label `bench`) | `registry_measure [--entries N] [--bytes B] [--assert] [--plugin] [--elf <binary>] [--json]`: the registry at scale — push, publish, lookup, export, parse and RSS of N synthetic entries; `--assert` enforces DECISIONS G-5 (10k/10 MB ≤ 10 ms, 100k/100 MB ≤ 100 ms push + publish) in release builds; exit 1 over budget, 2 usage or inconsistency (`registry-measure.md`) |
| `python_log_capture` | `tests/python/log_capture.cpp` | `python_reader_parity` | `python_log_capture <out.ser>`: a 1.7 capture of log frames under two envelope policies, a println frame and a record frame, with CRC trailers and a fixed user clock — the log input of the Python parity test |
| `ce_example<N>` | `ce-examples/ce-exampleN/main.cpp` | `ce_example1_builds_and_runs` … `ce_example5_builds_and_runs` (label `ce`) | not CMake targets: `tests/ce/build_and_run.cmake` compiles each example with `-I.` and `-O1 -W -Wall -Wextra -Wshadow -Werror` into the build tree, runs it and matches its verdict line (`ce-examples-tests.md`) |
| `serreg_bench` | `tests/bench/bench.cpp` | `bench_hot_path_ceilings` (label `bench`) | mean `serialize()` of a 32-byte struct < 50 ns, `inspect()` < 500 ns; Catch2 benchmarks |
| `probe_depth4` | `tests/probe/depth4.cpp` | `compile_probe_depth4` (compile-time budget), `compile_probe_depth4_runs` | 4-level × 4-member nesting: memoised hashes keep compile time bounded |
| link tests `link_plain`, `link_alt_order`, `link_reversed`, `link_gc`, `link_nopie`, `link_lto` | `tests/link/*.cpp` | `link_two_tu_dedup`, `link_alt_registration_order`, `link_program_hash_order_independent`, `link_reversed_order`, `link_program_hash_reversed_equal`, `link_gc_sections`, `link_no_pie_main_image`, `link_no_pie_program_hash_equal`, `link_lto` | FR-R2/R3/H3 across TU orders, `--gc-sections`, `-no-pie`, `-flto` |
| `link_image_none` (Debug builds only; in the `gcc16-static-none` preset linked against `es::serreg` itself, test `link_image_none_backend` only) | `tests/link/image_none_main.cpp`, `tu_a.cpp`, `tu_b.cpp`, linked against the private static `serreg_image_none` | `link_image_none_backend`, `link_image_none_program_hash_equal` | the null image backend (`ES_SERREG_IMAGE_FORMAT=none` sources, `ES_SERREG_EMBED_NONE`): 5 entries, one image, nothing unloadable, no `es_serreg_dict` section, same program hash as `link_plain`; the optional argument is the file it writes the program hash to |
| `dlopen_host` + plugins `serreg_plugin`, `_b`, `_c`, `_v2`, `_nested`, `_conf_a`, `_conf_b`, `_nounique`, `_alias` | `tests/dlopen/*.cpp` | `dlopen_A_dlclose_keeps_entries`, `dlopen_B_reload_revives_entries`, `dlopen_C_rtld_nodelete`, `dlopen_D_nested_dlclose`, `dlopen_E_changed_layout_reload`, `dlopen_F_concurrent_dlclose`, `dlopen_H_conflicting_plugins`, `dlopen_I_plugin_without_flags_unloads`, `dlopen_J_readers_vs_dlclose_churn`, `dlopen_K_alias_only_plugin_notifies`, `dlopen_L_two_thread_load_unload`, `srdump_elf_conflicting_plugins_*`, `srdump_elf_single_plugin_exit_0` | `host <plugin.so> keep\|reload\|nodelete\|nounique\|churn\|alias`, `host <a.so> nested\|changed\|concurrent\|conflict\|churn2 <b.so>` (header of `host.cpp`, `files/tests.md`) |
| `registry_second_copy` | `tests/dlopen/second_copy.cpp` | `registry_second_copy_diagnostic` | defines and exports its own `es_serreg_registry_marker_v3()` function and serialises one `Probe` from `second_copy::app::run()`; libserreg.so must print the "two copies of libserreg" line, and under ASan two exported functions are no ODR violation |
| `python_reader_parity` | `tests/python/parity.py`, `python/serreg` | the same name (label `python`) | not an executable of its own: `parity.py --srdump … --package python/ --log-writer python_log_capture <captures…>` compares `python3 -m serreg --json` with `srdump --json` (records, messages, log lines, `--verify`, `--info`); SKIP (77) without Python 3 |
| `wireshark_generate`, `wireshark_dissector_parity` | `tests/wireshark/CMakeLists.txt`, `parity.py` | the same names (label `wireshark`) | not executables of their own: `srdump --wireshark` checked for the expected filter field, and `parity.py` (Python 3, tshark, optionally `text2pcap`) comparing tshark's decoding through generated dissectors with `srdump --json`; the parity test is disabled without tshark/Python 3 and exits 77 (skip) when tshark cannot run Lua (`files/tests.md`) |
| `wireshark_print_capture` | `tests/wireshark/print_capture.cpp` | `wireshark_dissector_parity` (check `prints:`) | `wireshark_print_capture <out.ser>` writes 15 `print`/`println` frames covering std::format replacement fields; the parity test requires the generated dissector's messages to equal srdump's; built only with `srdump` |
| header checks `serreg_headers_ipc` (`include/es/ipc`), `serreg_headers_public`, `serreg_headers_internal`, `serreg_headers_tests` | generated, one TU per header (`tests/headers/CMakeLists.txt`) | `headers_include_rules` (runs `scripts/check-includes.py`) | object libraries, not executables: building them proves every header compiles alone (included twice); the ctest checks `#pragma once` and that package headers precede system headers (`files/tests.md`) |
| negative compile tests | `tests/negative/neg_*.cpp` | `negative_neg_<name>`, `negative_control_compiles` | the compiler itself is the executable: each file must fail with the expected diagnostic (checked at configure time too) |

### `serreg_hrtt_raw` / `serreg_hrtt_serreg` options

| option | meaning |
|---|---|
| `--role both\|ping\|pong` | `both` (default): ping here, pong forked (`--mode process`, default) or a thread (`--mode thread`, anonymous regions). `ping`: create the two named regions and rings and measure. `pong`: open them by name (on `/dev/shm` or any hugetlbfs mount), read the geometry and kind from the rings, echo |
| `--slots N`, `--slot-bytes B` | ring room for `N` messages of up to `B` bytes: a capacity of at least `N × align_up(16 + B, 64)` bytes, rounded up to a power-of-two unit count by `ring::for_capacity` (default 1024 × 256: 512 KiB of records); only the creator needs them |
| `--ring spmc\|spsc` | SPMC rings with one reader each (default) or SPSC queues; pong learns the kind from the ring header |
| `--huge 2m\|1g` | rings on explicit huge pages (hugetlbfs file for processes, `MAP_HUGETLB` for threads); fails if the pool is empty |
| `--lock` | `mlock()` the rings |
| `--name NAME` | region base name; the rings are `NAME-a2b` and `NAME-b2a` (default `serreg-hrtt-<pid>`) |
| `--iters N`, `--warmup N` | measured round trips (default 1 000 000) and discarded warm-up (100 000) |
| `--cpus A,B`, `--cpu N` | pin ping to A and pong to B; with `--role ping\|pong`, `--cpu` pins this process |
| `--pause` | spin with a CPU pause hint instead of a pure busy loop |
| `--timeout SEC` | how long pong waits for the rings and ping waits for pong (default 30) |
| `--samples FILE` | write every half-RTT sample (ns), one per line |
| `--quiet` | print only `ok: N round trips` (the ctests) |

Exit codes: 0 ok, 2 usage (including a message that does not fit the ring), 3 cannot attach/join, 4 a reply failed
to decode or carried the wrong sequence/timestamp, 5 a consumer was lapped, 6 rings or peer not available within
the timeout. The report is described in chapter 11 §11.6.

The link-test executables are created by `es_serreg_link_exe()` in `tests/link/CMakeLists.txt` (`link_image_none`
by its own block there);
`ctest -N -R link_` lists their tests. The dlopen tests are not built in the `gcc16-static` preset.

## The pure-Python reader: `python3 -m serreg`

Not built: `python/serreg` is a standard-library Python package (Python ≥ 3.10) shipped in the tarball;
`PYTHONPATH=python python3 -m serreg …` from the tree, or `pip install ./python` (which also installs the script
`serreg-dump`).

| invocation | effect |
|---|---|
| `python3 -m serreg <file.ser> [--json \| --text] [--dict x.rri …]` | the dictionary, then every record — srdump's text or JSON lines; `--dict-only`, `--hex` (JSON: payload bytes) |
| `python3 -m serreg <file.ser> --verify [--json]` | counts and problems (`ok: 11 frames, 34 records, 576 bytes`); exit 1 on problems |
| `python3 -m serreg <file.ser> --info [--json]` | 1.7: header, dictionary chain, checkpoints, writer info |
| `python3 -m serreg <file.ser> --level LVL [--json]` | log frames at or above `LVL`, one line each |
| `python3 -m serreg --rri <image.rri> [--json]` | a dictionary on its own |

`-` reads standard input. Exit codes: 0 ok, 1 problems or an unreadable capture, 2 usage. No `--elf`, `--diff`,
`--resync`, TOML, or code generation (`python-serreg.md`).
