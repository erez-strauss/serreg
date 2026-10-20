# Build system, packaging and project scripts

## `CMakeLists.txt`

`cmake_minimum_required(VERSION 4.0)`, `project(serreg VERSION 2.2.6 LANGUAGES CXX)`. The compiler gate
rejects GCC < 16 (`-freflection` is required) and any compiler other than GCC 16+ or clang-p2996
(`-freflection-latest`).

### Three libraries, one package (decision G)

| target (alias) | kind | contents | links |
|---|---|---|---|
| `serreg` (`es::serreg`) | shared (`libserreg.so.2.2`, SONAME major.minor) or static | the runtime: registry, image backend, dictionary, ELF reader, file io, reader, decoder, formatters (`ES_SERREG_SOURCES`) | `Threads::Threads` PUBLIC; for `elf` also `${CMAKE_DL_LIBS}` |
| `serreg_tools` (`es::serreg_tools`) | shared (`libserreg_tools.so.2.2`) or static, like `serreg` | srdump's command line (`es::serreg::cli`, `src/cli.cpp`), the Wireshark generator (`src/wireshark.cpp`), the dictionary diff (`src/diff.cpp`), the C++ generator (`src/cxxgen.cpp`) (`ES_SERREG_TOOLS_SOURCES`); also what `to_string.hpp` needs (`tools::render_value`, `tools::process_dictionary`, compiled in `src/formatters.cpp` of `serreg`, reached through this target) | `serreg` PUBLIC |
| `es_ipc` (`es::ipc`, export name `ipc`) | `INTERFACE`, header-only | `include/es/ipc/` — regions and the variable-size SPSC queue / SPMC broadcast; `cxx_std_23`, no reflection | nothing |

`es::serreg` does not link `es::ipc`: a program that uses both links both. `srdump`,
`embedded_cli`, `runtime_selftest`, `serreg_tests`, `serreg_fuzz` (and the libFuzzer targets) and
`wireshark_print_capture` link `es::serreg_tools`; `ipc_ring`, the `ipc_log` programs, `serreg_hrtt_*` and the header
checks link `es::ipc` explicitly, and `serreg_ipc_tests` link `es::ipc` only.

`es::ipc` is defined first, before the compiler gate, together with the function
`es_serreg_install_package_files()`, which installs `es::ipc` (its headers and
its own export set `serregIpcTargets`), `serregConfig.cmake`, `serregConfigVersion.cmake` and `es-ipc.pc`. With
**`ES_SERREG_IPC_ONLY=ON`** the top-level file calls that function and `return()`s: only `es::ipc`
is configured and installed, and no reflection compiler is needed. A full build calls the same function after installing `serreg`,
`serreg_tools` and `srdump` (export set `serregTargets`).

Directory-wide settings are made only when ser-reg is the top-level project (B-02, B-04): `CMAKE_CXX_STANDARD 26` (required, no extensions), `CMAKE_POSITION_INDEPENDENT_CODE ON` and the default
build type `Release`. As a sub-project (`add_subdirectory`, `FetchContent`) it leaves the parent's standard and
build type alone, and the targets carry what they need: `serreg` has `cxx_std_26` and the properties
`POSITION_INDEPENDENT_CODE ON` and `CXX_EXTENSIONS OFF`. `CMAKE_CXX_SCAN_FOR_MODULES` is `OFF` in every case: the
tree has no C++20 modules.

| option | default | effect |
|---|---|---|
| `ES_SERREG_BUILD_SHARED` | ON | `libserreg.so` (required when `dlopen`'ed DSOs serialize: one registry per process); OFF → `libserreg.a` |
| `ES_SERREG_BUILD_TOOLS` | ON | build `srdump` (`es::serreg_tools` is always built) |
| `ES_SERREG_IPC_ONLY` | OFF | configure and install only `es::ipc` (header-only, C++23); everything below the IPC block is skipped |
| `ES_SERREG_BUILD_TESTS` | `PROJECT_IS_TOP_LEVEL` | tests (fetches Catch2 v3 unless `ES_SERREG_CATCH2_DIR` points at a checkout) |
| `ES_SERREG_BUILD_EXAMPLES` | `PROJECT_IS_TOP_LEVEL` | examples |
| `ES_SERREG_WERROR` | **ON** | `-Werror` on every target the tree builds (DECISIONS C-4: the library, the tools, the tests and the examples compile clean under the full warning set). The `base` configure preset also sets it `ON` explicitly. A packager whose newer compiler adds a warning passes `-DES_SERREG_WERROR=OFF` |
| `ES_SERREG_IMAGE_FORMAT` (cache) | `elf` | the registry's image backend (`src/image.hpp`, chapter 9.4): `elf` compiles `src/image_elf.cpp` (`dladdr`, `dl_iterate_phdr`, `DT_FLAGS_1`) and the headers emit the `es_serreg_dict` section; `none` compiles `src/image_none.cpp` (one image, nothing unloads) and adds the PUBLIC define `ES_SERREG_EMBED_NONE` (no section, `detail/embed_none.hpp`). Any other value is a `FATAL_ERROR`; `none` with `ES_SERREG_BUILD_SHARED=ON` is a `FATAL_ERROR` too (the configure message: a shared `libserreg` must observe the image unloads it can suffer). |
| `ES_SERREG_LIBFUZZER` (tests/fuzz) | OFF | libFuzzer executables (clang only) |
| `ES_SERREG_FUZZ_ITERATIONS` (cache) | 300 | mutations per corpus file in the ctest fuzz runs |

Library sources (`ES_SERREG_SOURCES`): `src/registry.cpp`, `src/image_${ES_SERREG_IMAGE_FORMAT}.cpp`,
`src/dictionary.cpp`, `src/elf_reader.cpp`, `src/file_io.cpp`, `src/reader.cpp`, `src/decoder.cpp`,
`src/formatters.cpp`. Tools sources (`ES_SERREG_TOOLS_SOURCES`): `src/cli.cpp`, `src/wireshark.cpp`,
`src/diff.cpp` and `src/cxxgen.cpp`. `tests/link/CMakeLists.txt` reuses the first list, with `image_elf`
replaced by `image_none`, for the private static `serreg_image_none` library (Debug builds with the `elf` backend
only; `files/tests.md`).

Target `serreg` (alias `es::serreg`, export name `serreg`): `SOVERSION` = major.minor (`2.2`; DECISIONS G-2: until 3.0 the minor is the compatibility unit, so `libserreg.so.2.2` and a future
`libserreg.so.2.3` are different libraries; ctest `soname_major_minor` checks it), `VERSION` = 2.2.6;
`cxx_std_26`; PUBLIC link to `Threads::Threads`; PUBLIC compile option `-freflection`, and for the `elf` backend also `-fno-gnu-unique` (only for GCC, `$<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>`, B-01b: clang has no `STB_GNU_UNIQUE` and rejects the
flag) and the PUBLIC link to `${CMAKE_DL_LIBS}` (consumers inherit them — chapter 9 explains why `-fno-gnu-unique` must reach every image
that instantiates registrars, and it is what keeps plugins unloadable, which reloading needs; a `none` build
carries neither, nothing unloads there); `ES_SERREG_SHARED` /
`ES_SERREG_BUILDING` definitions select `ES_SERREG_API` visibility. Target `serreg_tools` (alias
`es::serreg_tools`, export name `serreg_tools`): the same `VERSION`/`SOVERSION`, PUBLIC link to `serreg`.
`serreg`, `serreg_tools` and `srdump` use `BUILD_WITH_INSTALL_RPATH ON` and
`INSTALL_RPATH "$ORIGIN;$ORIGIN/../<libdir>"` so installation never relinks (the Ninja generator refuses to rewrite RPATHs otherwise) and an
installed tree is relocatable. Install rules: the `es/serreg` headers, `serreg` and `serreg_tools` in the export set
`serregTargets` (namespace `es::`), `bin/srdump`, `serreg.pc` and `serreg-tools.pc`, `share/serreg/serreg.magic`;
and through `es_serreg_install_package_files()` the `es/ipc` headers, the export set `serregIpcTargets`,
`serregConfig.cmake` + `serregConfigVersion.cmake` (`SameMinorVersion`: `find_package(serreg 2.2)` accepts 2.2.x only) and `es-ipc.pc`.

`cmake/warnings.cmake`: `es_serreg_apply_warnings(tgt)`, applied to every target the tree builds. The set (maintainer) is `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
-Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation`,
plus for GCC `-Wduplicated-cond -Wduplicated-branches -Wlogical-op`, plus `-Werror` with `ES_SERREG_WERROR` (on by
default). `-Wnull-dereference` is deliberately absent: at `-O2` GCC reports it on `std::expected::error()` after a
`has_value()` check, a documented false positive (the comment in `warnings.cmake` says so).

`es_serreg_fail_on_sanitizer_reports(dir)` (B-09), at the end of the top-level `CMakeLists.txt` when tests
or examples are built, walks every test directory recursively and gives each test that has a
`PASS_REGULAR_EXPRESSION` the `FAIL_REGULAR_EXPRESSION` `"ERROR: [A-Za-z]+Sanitizer|WARNING: ThreadSanitizer|runtime
error:"`. A test with a pass expression passes on its output alone, whatever its exit code, so a sanitizer report
printed after the matched line (LeakSanitizer at exit, a crash in an `atexit` handler) would otherwise go
unnoticed.

## Test sub-directories

`tests/CMakeLists.txt` adds `headers`, `unit`, `golden`, `link`, `dlopen`, `canary`, `negative`, `probe`,
`bench`, `fuzz`, `wireshark`, `cxxgen`, `python` and `ce` (`files/tests.md`).
`tests/python/CMakeLists.txt` builds `python_log_capture` (`log_capture.cpp`, a 1.7 capture of log frames)
and registers `python_reader_parity` (`parity.py`: `python3 -m serreg --json` against `srdump --json` on the golden
and corpus captures; label `python`, SKIP 77 without Python 3; `book/python-serreg.md`). `tests/ce/CMakeLists.txt`
(DECISIONS T-3) registers `ce_example1_builds_and_runs` … `ce_example5_builds_and_runs`: each compiles one
`ce-examples/ce-exampleN/main.cpp` with `-I.` only and `-O1 -W -Wall -Wextra -Wshadow -Werror`, runs it and matches
its verdict line (label `ce`; `book/ce-examples-tests.md`). `tests/bench/CMakeLists.txt` adds `registry_measure`
(DECISIONS G-5) and the ctests `registry_measure_10k`, `registry_measure_100k` and `registry_measure_elf`
(label `bench`; `book/registry-measure.md`). `tests/headers/CMakeLists.txt` (include
rules) generates one translation unit per header — the header included twice, nothing else — and builds four object
libraries from them: `serreg_headers_ipc` (`include/es/ipc/*.hpp`, C++23,
linked to `es::ipc` only, so no reflection flag reaches it), `serreg_headers_public` (`include/es/serreg/*.hpp` and `detail/*.hpp`),
`serreg_headers_internal` (`src/*.hpp`) and `serreg_headers_tests` (the test, bench, fuzz, link and example
headers). Building them is the check that every header is self-contained and has `#pragma once`; with Python 3
found, the ctest `headers_include_rules` runs `scripts/check-includes.py`. The Catch2 executables register their cases with
`catch_discover_tests(... DISCOVERY_MODE PRE_TEST)` (B-08): the cases are listed when ctest runs, not
in a post-build step, so a build never runs a test binary and a sanitizer build that cannot run them here still
builds. `tests/wireshark/CMakeLists.txt` returns early without the `srdump`
target. It builds the executable `wireshark_print_capture` (`print_capture.cpp`, linked to `es::serreg`,
passed to the parity test's `prints:` check) and always registers `wireshark_generate`; `wireshark_dissector_parity`
is registered with `find_program(tshark)`, `find_program(text2pcap)` (optional: without it the `udp`, `tcp`,
`udp-heuristic` and `tcp-heuristic` checks are left out) and
`find_package(Python3 COMPONENTS Interpreter)`, and is `DISABLED` when tshark or Python 3 is missing. Both carry the
label `wireshark` (`ctest -L wireshark`); the parity test has `SKIP_RETURN_CODE 77` and a 300 s timeout. tshark and
Python 3 are optional: nothing else in the build needs them. The file also looks for Lua interpreters
(`lua5.4`, `lua54`, `lua5.2`, `lua52`, `lua`, told apart by running `-e "io.write(_VERSION)"` at configure time) and
registers `wireshark_dissector_lua52` and `wireshark_dissector_lua54` (`parity.py --lua`, the FILE-capture checks
through `wslua_stub.lua`), each `DISABLED` when its interpreter or Python 3 is missing.

`tests/cxxgen/CMakeLists.txt` (plan A5) registers the five `cxxgen_golden_*` tests only when the `srdump`
target exists, with the label `cxxgen` and a 600 s timeout each: generation happens at test time (a build does not
run binaries) through `compile_generated.cmake`, which runs `srdump --cxx` and then the project compiler with the
project's flags, `-std=c++26`, the reflection flag, the project warnings and `-Werror` — the generated header must
be warning-clean — and, for the two roundtrip tests, links the result against `serreg` and `serreg_tools` with an
RPATH to the build tree (`files/tests.md`). `tests/unit/CMakeLists.txt` adds `test_limits.cpp` and `test_containers.cpp` to
`serreg_tests`, and `tests/negative/CMakeLists.txt` the negative tests `neg_pointer_argument` ("never serialised")
and, for the std classes that stay rejected, `neg_multimap_member` (a `std::priority_queue` member: "this
standard-library type is not in the serialisable vocabulary"), `neg_smart_pointer_member` ("smart pointers … are
not serialisable") and `neg_empty_tuple_member` ("std::tuple<> has no parts to serialise"). `tests/unit/CMakeLists.txt`
also compiles `serreg_tests` with `ES_SERREG_VALIDATE_READ=1` and builds `serreg_validate_off_tests`
(`test_validate_off.cpp`) with the default policy, since the policy is per translation unit.

## `CMakePresets.json`

All presets use Ninja and `build/<presetName>`, and export `compile_commands.json`. The compiler comes from
`CXX` (presets deliberately do not set it, so one file serves every box); `opt-gcc16-debug` pins
`/opt/gcc-16/bin/g++-16`.

| preset | build type | notes |
|---|---|---|
| `gcc16-debug` | Debug, `-D_GLIBCXX_ASSERTIONS` | the development default; libstdc++ bounds checks abort on any out-of-range `span`/`vector` access |
| `gcc16-release` | Release | benchmark ceilings meaningful here |
| `gcc16-static` | Release, `ES_SERREG_BUILD_SHARED=OFF` | dlopen tests are not built (they need the shared registry) |
| `gcc16-static-none` | Release, `ES_SERREG_BUILD_SHARED=OFF`, `ES_SERREG_IMAGE_FORMAT=none` | the build without any ELF dependency (chapter 9.4): no section, no `-fno-gnu-unique`/`-ldl`; the ELF-section tests become no-section tests, `tests/link` runs `link_image_none_backend` only |
| `gcc16-ubsan-trap` | Debug, `-fsanitize=undefined -fsanitize-trap=undefined -fno-sanitize=null,nonnull-attribute,returns-nonnull-attribute,vptr -D_GLIBCXX_ASSERTIONS` | no runtime library: UB executes a trap instruction, so it runs where `libubsan` is missing; `vptr` needs the runtime (and the library uses no RTTI); `run-all-tests.sh` runs it (with the fuzzers) only where the ASan/UBSan preset cannot run |
| `gcc16-asan-ubsan` | Debug, `-fsanitize=address,undefined -fno-sanitize=null,nonnull-attribute,returns-nonnull-attribute -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS` | the three exclusions: GCC 16.2 rejects constant evaluation of `enum_string<E>` with them on (`docs/deviations.md`) |
| `gcc16-tsan` | Debug, `-fsanitize=thread` (also module linker flags for plugins) | run with `-LE bench` |
| `opt-gcc16-debug` | Debug | `CMAKE_CXX_COMPILER=/opt/gcc-16/bin/g++-16` |
| `clang-p2996` | Debug, `-stdlib=libc++` | for the golden cross-check with Bloomberg's clang |

Build presets exist for every configure preset except `opt-gcc16-debug` (use `cmake --build build/opt-gcc16-debug`);
test presets exist for the seven `gcc16-*` presets.

## Packaging for consumers

- `cmake/serregConfig.cmake.in` — components `ipc`, `serreg`, `tools`. `ipc` is always available;
  `serreg` and `tools` when `serregTargets.cmake` is installed (not in an `ES_SERREG_IPC_ONLY` installation).
  Without components, every available one is loaded. `ipc` includes `serregIpcTargets.cmake`; `serreg` or `tools`
  run `find_dependency(Threads)` and include `serregTargets.cmake`; `check_required_components(serreg)` fails a
  required component the installation lacks. `find_package(serreg 2.2)`; `find_package(serreg CONFIG REQUIRED
  COMPONENTS ipc)` for `es::ipc` alone.
- `cmake/serreg.pc.in` — relocatable (`prefix=${pcfiledir}/<up>`, where `<up>` is the path from the installed
  `<libdir>/pkgconfig` to the prefix, computed at configure time — `../..` for `lib`, `../../..` for a
  multiarch `lib/x86_64-linux-gnu`; B-05), `Cflags: -I… -std=c++26 -freflection
  -fno-gnu-unique`, `Libs: -L… -lserreg -ldl -lpthread` for the `elf` backend; the backend-specific
  parts come from `ES_SERREG_PC_CFLAGS` / `ES_SERREG_PC_LIBS`, so a `none` build installs `Cflags: … -freflection
  -DES_SERREG_EMBED_NONE` and `Libs: … -lserreg -lpthread`.
- `cmake/serreg-tools.pc.in` — `Requires: serreg = <version>`, `Libs: -L… -lserreg_tools`.
- `cmake/es-ipc.pc.in` — header-only: `Cflags: -I… -std=c++23`, `Libs: -lpthread`. The prefix is computed
  the same way (`ES_SERREG_PC_UP`, set in `es_serreg_install_package_files()`).
- `vcpkg.json` (manifest for the tree), `ports/serreg/{portfile.cmake,vcpkg.json,usage}` (overlay port; installs
  with examples off; `tools` and `tests` are port features; linkage follows the triplet;
  `vcpkg-cmake` and `vcpkg-cmake-config` are declared as host dependencies, `"host": true`, B-01d),
  `ports/triplets/x64-linux-gcc16.cmake` + `gcc16-toolchain.cmake` (dynamic linkage,
  GCC 16 chainload toolchain).
- `share/serreg.magic` — `file(1)` rules for `ESSR` (capture: format, hash algorithm, ABI, the back-patched, CRC,
  checkpoint and writer-info flags, program hash) and `ESRG` (RRI: format, delta flag, entry count, program hash).

## Style and hooks

- `.clang-format` — Microsoft base, `AccessModifierOffset: -4`, `PointerAlignment: Left`, `ColumnLimit: 100`,
  `SpaceAfterCStyleCast: false`, `SpaceAfterTemplateKeyword: false`, `DerivePointerAlignment: false`,
  `ReflowComments: false` (keeps aligned example comments intact). For the include rules (`AGENTS.md` §4):
  `IncludeBlocks: Regroup` with the categories `"…"` (1), `<es/…>` (2), `<catch2/…>` (3), `<*.h>` (4) and any other
  `<…>` (5), and `SortIncludes: CaseSensitive`: package headers come first, then Catch2, then C/POSIX headers, then
  the standard library, each block sorted. The whole tree was reformatted with it. **clang-format 22 is the reference formatter**
  (the maintainer's box and CI); older majors format a few constructs differently. `AGENTS.md` §4b lists the
  spellings to avoid because versions 18 to 23 parse them differently.
- `scripts/format-all.sh` — formats `include src tools tests examples` in place (`CLANG_FORMAT` overrides the binary;
  otherwise `clang-format`, then `clang-format-22`). It prints a warning when the binary's major version
  is below 22 (`pip install clang-format==22.1.8` provides the reference).
- `scripts/check-book.py` — the book ↔ code sync check: a `book/files/` page per source file under
  `include/es/`, `src/` and `tools/srdump/` (and none for a file that no longer exists); every `name.ext:N[-M]`
  citation within the file; a citation that follows a code block points at that block's first line (±3, or inside a
  cited range); no page names a symbol removed from the code (`STALE` list). `--fix` rewrites drifted citations
  when the quoted line occurs once (following it to another file if the code moved). Python 3 standard library
  only; a gating stage of `run-all-tests.sh`.
- `scripts/check-includes.py` — the include rules: every header's first code line is `#pragma once`, and in
  every source and header under `include src tools tests examples` each package include (`<es/…>` or `"…"`) comes
  before every other include. A line may opt out with a trailing `// include-order: after <reason>` comment. Prints
  `include rules: N problem(s)` and exits 1 when N > 0. Standard library only; run by the
  ctest `headers_include_rules`. That every header compiles on its own is checked by `tests/headers`.
- `scripts/pre-commit` — refuses a commit whose staged C++ files are not clang-format clean;
  `scripts/install-hooks.sh` links it into `.git/hooks`. POSIX `sh` (B-01f): no bash process substitution, which
  would be a syntax error where `/bin/sh` is dash. It uses `CLANG_FORMAT`, else
  `clang-format-22`, else `clang-format`, and warns when that is not version 22.

## `scripts/run-all-tests.sh`

The verification protocol of `AGENTS.md` §6 in one command; triage rules in `docs/test-and-log-protocol.md`.

```
scripts/run-all-tests.sh [--presets a,b,c] [--quick] [--fuzz-iters N] [--fuzz-seed N] [--tsan-repeat N]
                         [--jobs N] [--keep-build] [--no-install] [--out DIR]
```

Default presets: `gcc16-debug`, `gcc16-release`, `gcc16-static`, `gcc16-static-none`, `gcc16-asan-ubsan`,
`gcc16-tsan`, and `gcc16-ubsan-trap` only where the ASan/UBSan preset cannot run (name it in `--presets` to force it).
Stages: environment capture → clang-format check → per preset fresh configure/build/ctest (release and
sanitizer presets `-LE bench`) → on debug: golden-print, `srdump --version/--verify/--elf`, `file(1)` magic →
on release: bench (non-gating WARN), `file_store` example with a registry dump → on ASan/UBSan: the four fuzz
targets (on debug instead, with `_GLIBCXX_ASSERTIONS`, when ASan/UBSan cannot run) → on TSan:
`dlopen_[EFGJL]|signal_dump|concurrent|hrtt_.*_thread` repeated until-fail → install + a `find_package`
consumer + `pkg-config`. The install stage checks the package layout: the consumer's `CMakeLists.txt`
fails unless `find_package(serreg)` without components provides `es::serreg`, `es::serreg_tools` and `es::ipc` and
`es::serreg`'s `INTERFACE_LINK_LIBRARIES` does not mention ipc; it runs the serreg consumer and an `es::ipc`-only
consumer (C++23, no reflection flags); it configures and installs the tree with `ES_SERREG_IPC_ONLY=ON` and builds a
consumer that asks for `COMPONENTS ipc` and fails if `es::serreg` or `es::serreg_tools` exist; and it runs
`pkg-config --cflags --libs serreg serreg-tools es-ipc`. Output: `logs/serreg-tests-YYYYMMDD-HHMMSS/` (`00-summary.txt`, `00-environment.txt`,
per-stage logs, `LastTest.log`, `checks/golden-print.txt`) and the same directory as `.tar.gz`.
`00-environment.txt` records the tool versions, including clang-format and tshark (or
`not found: wireshark_dissector_parity is disabled`). A preset whose
sanitizer runtime is missing is SKIP with the reason — not a pass. TSan is probed with 40 runs of a trivial
binary: if they die with "incompatible memory layout / unable to disable ASLR" (high
`vm.mmap_rnd_bits` plus a seccomp profile that blocks `personality(ADDR_NO_RANDOMIZE)`, typical of containers on
recent kernels) the TSan stages run under `setarch $(uname -m) -R` when that works, and are otherwise SKIP
`ENV: TSan vs ASLR` with the host fix (`sudo sysctl vm.mmap_rnd_bits=28`, or
`--security-opt seccomp=unconfined`). Exit 0 iff every gating stage passed or was
skipped.

## `scripts/bench-hrtt-raw.sh`, `scripts/bench-hrtt-serreg.sh`, `scripts/bench-hrtt-lib.sh`

The two half-RTT benchmark drivers (chapter 11). Each is three lines: it sources `bench-hrtt-lib.sh` and calls
`hrtt_main raw|serreg "$@"`. The shared body builds `serreg_hrtt_<kind>` in `gcc16-release` (`--preset`,
`--no-build`), picks two CPUs on different physical cores of one package (never CPU 0; `--cpus A,B` or `none`),
records the latency-relevant machine state (CPU model, SMT, governor and frequency of the two CPUs,
`no_turbo`, `isolcpus`/`nohz_full`/… from the kernel command line, load, huge-page pool, hugetlbfs mounts,
`ulimit -l`, compiler), removes ring files left by earlier runs whose process is gone (`/dev/shm` and every
hugetlbfs mount), and runs `--runs` times
(default 3): pong started in the background, then ping, as two separately launched processes. It ends with a
per-run summary (min/p50/p99/p99.9/max half RTT) and `==== result: ok|FAILED`. With `--threads`, one
thread-mode reference run is added. `--samples` keeps the per-sample files.

```
scripts/bench-hrtt-raw.sh    [--iters N] [--warmup N] [--runs N] [--cpus A,B|none] [--pause] [--threads]
scripts/bench-hrtt-serreg.sh [--slots N] [--slot-bytes N] [--huge 2m|1g] [--lock]
                             [--samples] [--preset P] [--no-build] [--out DIR]
```

Output: `logs/bench-hrtt-<kind>-YYYYMMDD-HHMMSS.txt` (+ `-runN.samples`), also shown on the terminal. The exit
status is 0 only when every run and every pong succeeded.

## `scripts/bump-version.sh`

`scripts/bump-version.sh X.Y.Z` reads the current version from the `project()` line and replaces it only in the
places that state the current release: `CMakeLists.txt`, `library_version` (`include/es/serreg/version.hpp`),
`python/serreg/__init__.py`'s `__version__` and `python/pyproject.toml`, the `**Refers to:**` line of
`docs/DECISIONS.md`, `vcpkg.json` and `ports/serreg/vcpkg.json`, the version checks in `test_tools.cpp` and `runtime_selftest.cpp`, the
`srdump --version` examples (README, `book/executables.md`), `book/README.md`, chapter 1, `package-contents.md`
and this file. Each substitution must change its file, or the script stops with the file and the pattern.
It also updates (B-18) the `GIT_TAG v…` of the README's `FetchContent` example and the `REF v…` comment in
`ports/serreg/portfile.cmake`. Historical mentions (test names, CHANGELOG) are never touched. Then add the
`## vX.Y.Z` CHANGELOG entry, which `make-release.sh` requires. It does not touch the golden captures (they carry
the version in their writer-info frame, so they need `make_fixtures`, `make_dynamic_fixtures` and
`runtime_selftest` after a bump; `files/include-es-serreg-version.hpp.md`).

## `scripts/make-release.sh`

Builds `ser-reg-project-v<version>.tar.gz` (version from `CMakeLists.txt`) with the top-level directory of the
same name from an explicit exclude list (build trees, logs, caches, `Testing/`, capture/RRI files outside
`tests/fixtures/`, `.ccache*`), refuses to overwrite an existing tarball, requires the CHANGELOG
entry, and prints the file count and sha256. `--out DIR` chooses the destination (default: the repository's
parent); `--public` packs the subset `public-files-list.txt` selects.

Checks and reproducibility (B-01a, B-17):

- The format and book checks are required, not optional: without `clang-format` on the `PATH` the script stops;
  the tree must pass `clang-format --style=file --dry-run --Werror` and `scripts/check-book.py --quiet`.
- The tarball is reproducible: `tar --sort=name --owner=0 --group=0 --numeric-owner`, every mtime set to the
  release date of the version's `## vX.Y.Z — YYYY-MM-DD` CHANGELOG line (1970-01-01 when there is none), and
  `gzip -9n` (no name, no timestamp). The same tree gives the same bytes, and so the same sha256 for the vcpkg
  port.

`__pycache__` directories are deleted from the working tree before packing, and after the project
tarball (not the `--public` one) the script runs `scripts/make-rebuild-prompt.py --out DIR`, which writes
`ser-reg-rebuild-v<version>.md` next to the tarball.

## `scripts/make-rebuild-prompt.py`

Assembles the single-file rebuild prompt (DECISIONS D-2) from `docs/ser-reg-rebuild-prompt.md`, the five flattened
documents (`docs/DECISIONS.md`, `docs/ser-reg-requirements.md`, `-architecture.md`, `-testing.md`, `-stories.md`),
the normative sources, the public headers' leading comments and the checksums of every fixture and ce-example.
Its own page: `book/make-rebuild-prompt.md`.

## `.github/workflows/ci.yml`

A `gcc:16` container matrix over `gcc16-debug`, `gcc16-release`, `gcc16-static`, `gcc16-static-none`,
`gcc16-asan-ubsan` (configure/build/ctest; the debug job uploads `golden-print.txt`), a `book` job
(`check-book.py`), a `full-sandbox` job (`run-all-tests.sh` with `gcc16-debug,gcc16-ubsan-trap`), a `tsan` job
(`run-all-tests.sh` with `gcc16-tsan`), a `format` job, and a `clang-p2996` job that compiles the golden file with
Bloomberg's clang when a runner provides it (continue-on-error until pinned).

The `gcc:16` containers (B-01e) install CMake 4 from pip (`'cmake>=4.0,<5'`); the matrix runs `ctest -LE bench` with `ASAN_OPTIONS
detect_leaks=1:abort_on_error=1` and `UBSAN_OPTIONS print_stacktrace=1:halt_on_error=1`; `full-sandbox` installs
`lua5.2` and `lua5.4` (for `wireshark_dissector_lua52`/`_lua54`) and `clang-format==22.1.8` from pip; the `format`
job, on the plain runner, installs `clang-format==22.1.8` with `pipx`, since Ubuntu 24.04 ships 18 and its pip is
PEP 668-locked, then runs `--dry-run --Werror` over `include src tools tests examples`.
