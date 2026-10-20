# Package contents and delivery

## The release tarball

`ser-reg-project-v2.2.6.tar.gz` unpacks to one directory, `ser-reg-project-v2.2.6/`, produced by
`scripts/make-release.sh` from a clean tree (no build trees, logs, caches, or capture files outside
`tests/fixtures/`). Release file names are never reused; each version has a `CHANGELOG.md` entry and a test
record in `docs/test-reports/`.

| path | what it is |
|---|---|
| `README.md` | overview, examples, build and consumption instructions |
| `CHANGELOG.md` | one entry per version, newest first; wire-format and API changes called out |
| `LICENSE` | MIT |
| `docs/THIRD-PARTY-NOTICES.md` | the xxHash (XXH3) BSD 2-Clause notice for the algorithm and constants in `hash.hpp`; Catch2 (test-only) |
| `docs/SECURITY.md` | the threat model (one trust domain; input is not hostile but is wrong at times), the never-crash contract (spec FR-D5) with the `decode_limits` it holds within, how the contract is checked (the four fuzz targets gated on time and memory, `tests/unit/test_limits.cpp`, the sanitizer runs), how to report a crash, a hang or an amplification reached from input bytes, and which versions are supported (the current release only) |
| `docs/repo-get-ready.md` | what to settle before the tree becomes a git repository (the public subset, tags, CI gaps, open decisions) |
| `remove-me/` | superseded documents kept for one release under their original relative paths: the two earlier rebuild prompts (retired by DECISIONS D-2), the earlier decision logs and reviews, old plans, per-release status reports and session notes (`remove-me/README.md`); `docs/DECISIONS.md`, the flattened `docs/ser-reg-*.md`, `CHANGELOG.md` and `docs/test-reports/` replace them. `#private` |
| `public-files-list.txt` | which files go into the public repository, which stay private and which wait for a decision; read and checked by `scripts/make-release.sh --public`. Not itself public |
| `AGENTS.md`, `CLAUDE.md` | rules for coding agents (and humans): conventions, verification protocol, deliverables |
| `CMakeLists.txt`, `CMakePresets.json` | the build (chapter 2, `files/build-system.md`) |
| `cmake/` | `warnings.cmake`, `serregConfig.cmake.in` (components `ipc`, `serreg`, `tools`), `serreg.pc.in`, `serreg-tools.pc.in` and `es-ipc.pc.in` |
| `include/es/serreg/` | the public headers (24; `serreg.hpp` is the umbrella; `limits.hpp` holds the reader limits `decode_limits`, FR-D5; `log.hpp` is the logger, `projection.hpp` schema evolution level 1 (`renamed_from`, `tools::project`), `to_string.hpp` (`to_string`/`to_json`) links `es::serreg_tools` and is not in the umbrella, `version.hpp` holds `library_version`), and `detail/embed_elf.hpp` / `detail/embed_none.hpp` (the embedded-section hooks, included by `blob.hpp`) and `detail/blob_bytes.hpp` (the ENTRY blob bytes, split out of `blob.hpp`) |
| `include/es/ipc/` | `es::ipc`, header-only: the variable-size rings — `layout.hpp`, `ring.hpp`, `producer.hpp`, `consumer.hpp`, `sink.hpp`, umbrella `ipc.hpp` — with the shared `region.hpp` and `common.hpp` (chapter 11) |
| `src/` | `libserreg` sources: registry (heap entries — views for the main program —, scopes and delta exports, unload and reload, env and signal hooks), image backend (`image.hpp`, `image_elf.cpp`, `image_none.cpp`; no `dlopen`/`dlclose` interposers), dictionary, ELF reader, file I/O (`file_io.cpp`), capture walker (the windowed readers and the process-wide `decode_limits`; format 1.7, `tools::info`, `--resync`), decoder, formatters (log rendering, `render_value`, `process_dictionary`); `libserreg_tools` sources: diff (`--direction`), cli, Wireshark dissector generation (`wireshark.cpp`), C++ generation (`cxxgen.cpp`); `internal.hpp` |
| `tools/srdump/main.cpp` | the one-line CLI |
| `python/` | the pure-Python reader — `serreg/` (`__init__.py`, `__main__.py`, `format.py`, `dictionary.py`, `reader.py`), `README.md`, `pyproject.toml` (`book/python-serreg.md`) |
| `ce-examples/` | the eight Compiler Explorer programs (FR-H4) with header symlinks into `include/`, `ce-run.py`, `ce_registry_stub.hpp`; compiled and run by every build (`tests/ce`) |
| `examples/` | `file_store`, `ipc_ring` (on the variable-size `es::ipc` ring), `ipc_log/` (two programs over a named `es::ipc` queue with the dictionary in band), `embedded_cli`, `user_example1/` (writer + three readers, standalone-buildable; `args.hpp`, reader1 is a `frame_router`), `dictionary_deltas/` (a host and two `dlopen`ed plugins: dictionary delta frames, the CHAIN, checkpoints with full dictionaries) |
| `tests/` | golden (incl. `corpus::DynMap`, the nine-frame `dynamic.ser`, `features.ser`), unit (incl. `local_in_main.cpp`, `test_limits.cpp`, `test_containers.cpp`, `test_format17.cpp`, `test_log.cpp`, `test_to_string.cpp`), python (`log_capture.cpp`, `parity.py`), ce (`build_and_run.cmake`), link, dlopen (unload/reload tests, `serreg_plugin_v2` from `plugin.cpp`), headers (`CMakeLists.txt`, one generated TU per header), negative (incl. `neg_print_nested_field.cpp`, `neg_router_generic_lambda.cpp`, `neg_zero_size_element.cpp`, `neg_pointer_argument.cpp`, `neg_multimap_member.cpp`), cxxgen (`check_*.cpp`, `roundtrip_*.cpp`, `compile_generated.cmake` — the `srdump --cxx` output compiled and run against the golden captures), canary, fuzz (the driver gates inputs on time and memory), bench (incl. the half-RTT benchmarks on the variable-size `es::ipc` rings, `hot_inline.cpp` and `check_inline.cmake` (the `-O2` inlining check), `registry_measure.cpp`), probe, support, fixtures, and `wireshark/` (`CMakeLists.txt`, `parity.py`: the generated dissector against tshark; `print_capture.cpp`, the print-format capture; `wslua_stub.lua`, Wireshark's Lua API for a plain Lua 5.2/5.4 interpreter) (`files/tests.md`) |
| `tests/fixtures/golden/` | current-format fixtures (format 1.7): `orders.*`, `dynamic.*`, `features.ser`; the 1.6 copies stay in `tests/fixtures/corpus/` for the compatibility tests |
| `share/serreg.magic` | `file(1)` magic for `.ser` captures and `.rri` images |
| `ports/serreg/`, `ports/triplets/`, `vcpkg.json` | vcpkg overlay port and a GCC 16 triplet |
| `scripts/` | `make-rebuild-prompt.py` (the single-file rebuild prompt, `book/make-rebuild-prompt.md`), `run-all-tests.sh`, `bench-hrtt-raw.sh`, `bench-hrtt-serreg.sh` (+ `bench-hrtt-lib.sh`), `make-release.sh` (`--public`; reproducible tarballs, format and book checks required), `bump-version.sh`, `check-book.py`, `check-includes.py` (the include rules), `amalgamate-golden.py` (one file for the Compiler Explorer cross-check), `format-all.sh`, `pre-commit` (POSIX `sh`), `install-hooks.sh` |
| `.clang-format`, `.gitignore`, `.gitattributes`, `.github/workflows/ci.yml` | style, ignores, line endings and binary fixtures, CI |
| `docs/` | the six flattened documents written from the current state: **`DECISIONS.md`** (the one decision ledger — R, P, A, C, G, T, D entries with their status), `ser-reg-requirements.md`, `ser-reg-architecture.md` (with the byte layouts and the registry cost model), `ser-reg-testing.md`, `ser-reg-stories.md` and `ser-reg-rebuild-prompt.md` (the core of the rebuild prompt); beside them `test-and-log-protocol.md`, `wireshark.md` (`srdump --wireshark`, installing and using the generated dissector), `deviations.md`, `ce-links.md`, `windows-porting.md`, `serreg-reflection.md`, `serreg-registry.md`, `ipc/` (`ipc-design.md`, `latency-analysis.md`, `testing-and-benchmarks.md`, `ipc-optimization-directions.md` — the variable-size rings' design, latency investigation and results) and `test-reports/`; the maintainer's working documents (`plans-for-v2.2.3.md`, `review-response-2.2.2.md`, `schema-evolution-specs.md`, `values-validation.md`, `talk/` — the Core C++ 2026 talk: content, generator, decks); and as history `ser-reg-requirements-v3.md` (the original spec), `plans-for-v2.0.md` (dynamic members) and `plans-for-containers.md` (containers as arguments and map/set members, format 1.6 — with the differences of what shipped). `public-files-list.txt` says which of them are public |
| `book/` | this book |

Not in the package: build outputs, `logs/`, Catch2 (fetched by CMake only when tests are built, or taken from
`ES_SERREG_CATCH2_DIR`), anything else third-party — the library and `srdump` have no dependencies beyond
the C++ standard library, glibc (`dlfcn`, `link.h`, `pthread`; the first two only in the default `elf` image
backend) and the compiler. tshark (with `text2pcap`) and Python 3 are optional test tools, used by
`wireshark_dissector_parity` and `python_reader_parity` (Python 3 only); the generated
dissector itself needs Wireshark or tshark on the user's side, the Python reader only a Python 3.10 interpreter.

**The public subset.** `scripts/make-release.sh --public` writes `ser-reg-public-v<version>.tar.gz`
(top-level directory `ser-reg-public-v<version>/`) with only the files `public-files-list.txt` selects: the build,
the library, the tools, the tests, the examples, the book, `README.md`, `LICENSE`, `docs/THIRD-PARTY-NOTICES.md`,
`CHANGELOG.md`, `AGENTS.md`, `CLAUDE.md`, and the design documents in `docs/`. It fails when a file of the tree is
not classified by the list, is both public and excluded, or when a public line matches nothing, and it prints the
public files that cite an excluded one. The public subset builds and passes its tests on its own: nothing in the
build, the tests or the scripts reads an excluded file.

## Versions

| number | where | meaning |
|---|---|---|
| library version `2.2.6` | `project(serreg VERSION …)`, `library_version` (`version.hpp`), `vcpkg.json` | API/ABI; the minor is the compatibility unit until 3.0 (DECISIONS G-2): SONAME `libserreg.so.2.2` (major.minor), and `find_package(serreg 2.2)` accepts 2.2.x only (`SameMinorVersion`) |
| wire format `1.7` | `fmt::format_major/format_minor`, FILE and RRI headers | the bytes; the library writes 1.7 and reads 1.6 and 1.7 (`fmt::format_minor_oldest`); older minors are refused (`bad_version`). Format 1.7 shipped in a patch release, the one exception G-2 allows in this early phase |
| `hash_algo 2` | headers | XXH3-64 (`hash_algo` 1, FNV-1a-64, is not read) |

`srdump --version` prints all three: `srdump 2.2.6 (format 1.7, hash_algo 2)`.

## What `cmake --install` puts where

```
<prefix>/include/es/serreg/*.hpp, <prefix>/include/es/serreg/detail/*.hpp, <prefix>/include/es/ipc/*.hpp
<prefix>/lib/libserreg.so.2.2.6, libserreg.so.2.2 → , libserreg.so →    (SONAME libserreg.so.2.2; or libserreg.a with
                                                                           -DES_SERREG_BUILD_SHARED=OFF; lib64/ where GNUInstallDirs says so)
<prefix>/lib/libserreg_tools.so.2.2.6, libserreg_tools.so.2.2 → , …   (libserreg_tools.a for a static build)
<prefix>/lib/cmake/serreg/serregConfig.cmake, serregConfigVersion.cmake, serregTargets*.cmake, serregIpcTargets.cmake
<prefix>/lib/pkgconfig/serreg.pc, serreg-tools.pc, es-ipc.pc              (relocatable: prefix computed from ${pcfiledir})
<prefix>/bin/srdump                                                        (RPATH $ORIGIN;$ORIGIN/../lib — works from any prefix)
<prefix>/share/serreg/serreg.magic
```

With `-DES_SERREG_IPC_ONLY=ON` only `include/es/ipc/*.hpp`, `serregConfig.cmake`,
`serregConfigVersion.cmake`, `serregIpcTargets.cmake` and `es-ipc.pc` are installed.

## Consuming it

```cmake
# installed prefix
find_package(serreg 2.2 CONFIG REQUIRED)          # -DCMAKE_PREFIX_PATH=<prefix>; 2.2.x only (SameMinorVersion)
target_link_libraries(app PRIVATE es::serreg)     # carries -std=c++26 -freflection -fno-gnu-unique, includes, libserreg
target_link_libraries(cli PRIVATE es::serreg_tools)  # cli::run, tools::diff, the Wireshark generator (+ es::serreg)
target_link_libraries(tool PRIVATE es::ipc)       # es::ipc only: header-only, C++23, no reflection flags
# es::serreg does not bring es::ipc — link both when both are used
find_package(serreg 2.2 CONFIG REQUIRED COMPONENTS ipc)   # es::ipc alone (also from an ES_SERREG_IPC_ONLY install)

# FetchContent
FetchContent_Declare(serreg URL /path/to/ser-reg-project-v2.2.6.tar.gz)
set(ES_SERREG_BUILD_TESTS OFF)  set(ES_SERREG_BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(serreg)
```

```sh
# pkg-config
c++ -std=c++26 $(pkg-config --cflags serreg) app.cpp $(pkg-config --libs serreg)
c++ $(pkg-config --cflags serreg-tools) cli.cpp $(pkg-config --libs serreg-tools)
c++ $(pkg-config --cflags es-ipc) ring.cpp $(pkg-config --libs es-ipc)               # -std=c++23, header-only

# vcpkg (overlay port + GCC 16 triplet shipped in the tree)
vcpkg install serreg --overlay-ports=<tree>/ports --overlay-triplets=<tree>/ports/triplets --triplet x64-linux-gcc16
```

A plugin that `dlopen`s into a program using ser-reg must link the **shared** `libserreg` (one registry per
process) and should use the exported target so it inherits `-fno-gnu-unique`, which keeps it unloadable and so
reloadable (chapter 9).

## Verifying a delivery

```sh
tar -xzf ser-reg-project-v2.2.6.tar.gz && cd ser-reg-project-v2.2.6
scripts/run-all-tests.sh            # all presets, fuzz, TSan repeats, install + consumer; logs/serreg-tests-<stamp>.tar.gz
```

Attach the log tarball to a review; `docs/test-and-log-protocol.md` explains how it is read and what must hold
before a release.

## The rebuild prompt

Next to the project tarball `scripts/make-release.sh` writes `ser-reg-rebuild-v<version>.md` (via
`scripts/make-rebuild-prompt.py`): one self-contained file — the prompt of `docs/ser-reg-rebuild-prompt.md`, the
six flattened documents, `format.hpp`, the golden values, `python/serreg/format.py`, the presets and warning set,
the public headers' leading comments, and the SHA-256 of every fixture and ce-example — from which the library can
be rebuilt and checked bit for bit (`book/make-rebuild-prompt.md`).
