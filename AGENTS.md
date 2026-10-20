# AGENTS.md — ser-reg (`es::serreg`)

Instructions for any coding agent (Claude Code, Codex, Cursor, …) working in this repository. `CLAUDE.md` adds Claude-Code-specific notes and otherwise defers to this file.

## 1. What this project is

A C++26 library: reflection-driven (P2996) binary serialization of flat C++ types plus a process-wide **type registry** populated at static-initialization time (and on `dlopen`) purely by template instantiation of `es::serreg::serialize(...)`. It ships headers + three libraries in one CMake package (decision G): `es::serreg` (`libserreg`, static and shared: the registry, readers, decoder, formatters), `es::serreg_tools` (`libserreg_tools`: the command line `es::serreg::cli`, the Wireshark generator, the dictionary diff) and `es::ipc` (header-only, C++23, no reflection; `es::serreg` does not link it); the `srdump` CLI (a one-line `main` over `es::serreg::cli::run`), tests, fuzzers and benchmarks.

The authoritative specification is **`docs/ser-reg-requirements.md`** (the flattened requirements, FR-* IDs) together
with **`docs/DECISIONS.md`** (the decision ledger: R/P/A/C/G/T/D entries with status and provenance),
`docs/ser-reg-architecture.md`, `docs/ser-reg-testing.md` and `docs/ser-reg-stories.md`. The v3 spec
(`docs/ser-reg-requirements-v3.md`) and the remaining plan documents are history. Requirement IDs (`FR-S1`,
`FR-R10`, `NFR-1`, …) are referenced in tests and commits. **Do not re-open decided items** (`DECISIONS.md`). The
non-blocking defaults are `ser-reg-requirements.md` §14 — use them unless the user changes one. If a compiler
limitation forces a change, make the smallest deviation and record it in `docs/deviations.md`.

## 2. Toolchain (assume; do not downgrade)

- GCC 16.x, `-std=c++26 -freflection` (hard error below C++26; CMake gates it). Clang 22+ may build the non-reflection parts; P2996 cross-checks use Bloomberg's clang-p2996 on Compiler Explorer (`-std=c++26 -freflection-latest -stdlib=libc++`).
- CMake ≥ 4 with presets: `gcc16-debug`, `gcc16-release`, `gcc16-static`, `gcc16-static-none` (`ES_SERREG_IMAGE_FORMAT=none`, no ELF dependency), `gcc16-ubsan-trap` (UBSan in trap mode, no runtime needed), `gcc16-asan-ubsan`, `gcc16-tsan`, `clang-p2996`, `opt-gcc16-debug`.
- Target: Linux x86-64 (ELF64, little-endian). Toolchains are available in the ai-box Docker images (Fedora / Ubuntu).
- **No external dependencies** for the library or tools. Catch2 v3 is test-only (CMake `FetchContent`; `ES_SERREG_CATCH2_DIR` points at a vendored copy for offline builds).

## 3. Build / test / run

```sh
scripts/run-all-tests.sh                 # the whole protocol of §6, every log captured (docs/test-and-log-protocol.md)
scripts/run-all-tests.sh --quick         # gcc16-debug + format + srdump checks: the smoke run to start a session with
cmake --preset gcc16-debug && cmake --build --preset gcc16-debug -j
ctest --preset gcc16-debug --output-on-failure
cmake --preset gcc16-asan-ubsan && cmake --build --preset gcc16-asan-ubsan -j && ctest --preset gcc16-asan-ubsan -LE bench
./build/gcc16-release/srdump tests/fixtures/golden/orders.ser --verify
./build/gcc16-release/srdump --elf ./build/gcc16-release/tests/unit/serreg_tests --json
ES_SERREG_DUMP_REGISTRY=- ES_SERREG_DUMP_FORMAT=toml ./build/gcc16-release/examples/file_store
```

Options: `ES_SERREG_BUILD_SHARED`, `ES_SERREG_BUILD_TOOLS`, `ES_SERREG_BUILD_TESTS`, `ES_SERREG_BUILD_EXAMPLES`, `ES_SERREG_WERROR`, `ES_SERREG_CATCH2_DIR`, `ES_SERREG_IMAGE_FORMAT` (`elf`|`none`), `ES_SERREG_IPC_ONLY` (configure and install es::ipc alone, no reflection compiler). Consumers: `find_package(serreg CONFIG REQUIRED [COMPONENTS ipc serreg tools])`; pkg-config `serreg`, `serreg-tools`, `es-ipc`. Select the compiler with `CXX=/opt/gcc-16/bin/g++-16` (or `-DCMAKE_CXX_COMPILER=…`, or the `opt-gcc16-debug` preset); at runtime set `LD_LIBRARY_PATH` to that GCC's `lib64` if it is not the system compiler. Installed targets (`serreg`, `serreg_tools`, `srdump`) are built with their final `$ORIGIN`-relative RPATH so `cmake --install` never relinks.

## 4. Code conventions (enforced in review)

- Namespace `es::serreg`; headers under `include/es/serreg/`; Beman-style layout.
- **All function parameters are `const` unless the function modifies them.** Same for locals where practical.
- Errors via `std::expected<T, es::serreg::error>`; no exceptions in library code; `noexcept` where true.
- Every mutable namespace-scope object is `constinit`. No static-initialization-order dependencies.
- Prefer `consteval` for compile-time-only helpers; descriptors/blobs/hashes must be constant expressions.
- Reflection code lives in `reflect.hpp` (descriptors, canonical strings, and the dynamic-image
  measure/encode/decode walks, which splice members), with two known exceptions: `registry.hpp` (`touch_components`
  splices member types) and `blob.hpp` (includes `reflect.hpp`) — `docs/deviations.md`. Do not add more; the
  `es::ipc` headers and `format.hpp`, `error.hpp`, `descriptor.hpp`, `dictionary.hpp`, `tools.hpp` must compile
  without `-freflection`.
- No RTTI dependence. Nothing allocates or locks on the serialization hot path. Registry mutex/`dladdr` only at load/unload time.
- All wire IDs are 64-bit. Frame headers are 16 bytes (compact) — do not add fields without a format version bump.
- Warnings (`cmake/warnings.cmake`, DECISIONS C-4): `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation` (+ `-Wduplicated-cond -Wduplicated-branches -Wlogical-op` on GCC), `-Werror` on by default (`ES_SERREG_WERROR`); clean under ASan/UBSan. Catch2 assertions must not nest (an assertion inside a lambda inside an assertion shadows `catchAssertionHandler`): hoist the outer expression into a `const auto` first.
- Every public header starts with a doc comment (purpose, minimal example). Non-obvious tricks (odr-use registration, heap entries and retained entries, padding gap table, two-pass consteval blob, dictionary back-patch) get a comment citing the spec section.
- Hash canonicalization strings are frozen by golden tests; never change them without a format/major bump and a `CHANGELOG.md` note. `since`/`deprecated` go into the schema string only, never the layout string.
- Signal handlers (when `ES_SERREG_DUMP_SIGNAL` is set) may only store to an atomic flag; all I/O happens off-signal.
- **Output (maintainer rule):** no `printf`/`fprintf`/`puts`/`fputs` family anywhere — library, tools, tests,
  examples. Use `std::print`/`std::println`. Library diagnostics that can run in a destructor, in libserreg's
  load-time constructor or inside `dlclose()` use `detail::report_line(fmt, args...)` (`sink.hpp`: `std::format`
  syntax, no allocation, no exception).
- **`main()` is minimal (maintainer rule):** in every executable (tools, examples, tests) `main` at most parses
  the command line and hands it to an application class that does the work (`return app{opts}.run();`).
- **Includes (maintainer rule):** package headers (`"..."`, then `<es/...>`) always come before third-party
  (`<catch2/...>`) and system headers (`<*.h>`, then the standard library) — `.clang-format` regroups them
  (`IncludeBlocks: Regroup`), so `scripts/format-all.sh` puts them right. Every header includes what it needs to compile
  on its own and starts with `#pragma once`. The build checks the first (`tests/headers`: one generated TU per
  header, the header included twice); `scripts/check-includes.py` (ctest `headers_include_rules`) checks the order and
  the `#pragma once` line. The one exception is marked in place (`// include-order: after …`, `blob.hpp`).

## 4b. Formatting

`.clang-format` (Microsoft base, 100 columns, `T* p`) is authoritative. Run `scripts/install-hooks.sh` once; the
pre-commit hook rejects unformatted staged C++ files, CI runs `clang-format --dry-run --Werror`. Format with
`scripts/format-all.sh` before committing. The reference formatter is **clang-format 22** (the maintainer's box;
CI pins 22.1.8; `format-all.sh` warns on an older major), and the tree must stay clean under **every clang-format
from 18 to 23** (checked with 18.1.3, 22.1.8 and 23.1.1; `pip install "clang-format==22.*"` in a venv gives any
version). Avoid spellings that versions parse differently: write `struct stat st = {};` (18 reads
`struct stat st{};` as a struct definition), and put a comment above a declaration rather than after a
wrapped initializer.

## 5. Repository map

```
include/es/serreg/   reflect (only reflection user) descriptor blob hash literal arg (argument categories) registry sink serialize
                     print (print/println) deserialize router (frame_router) capture (format 1.7 writer) log (logger)
                     to_string (links serreg_tools) version format error dictionary tools limits walk serreg(umbrella)
                     detail/embed_{elf,none} (the es_serreg_dict section copy or nothing; A4) detail/blob_bytes
include/es/ipc/      common region layout ring producer consumer sink ipc(umbrella) — es::ipc, its own library (C++23, no reflection):
                     variable-size SPSC/SPMC rings (docs/ipc/)
src/                 es::serreg: registry (heap entries, retained entries, env/signal hooks) image_{elf,none} (A4 backend) dictionary
                     elf_reader file_io reader decoder formatters — es::serreg_tools: cli wireshark diff
tools/srdump/        main.cpp (one line)
tests/               golden/ unit/ headers/ link/ dlopen/ negative/ canary/ fuzz/ bench/ (incl. registry_measure) probe/ support/ wireshark/ cxxgen/ python/ ce/ (the five ce-examples)
                     fixtures/{golden (current format 1.7), corpus (fuzz seeds + the 1.6 compatibility files), negative}
python/serreg/       the stand-alone pure-Python reader (stdlib only; python3 -m serreg)
examples/            file_store ipc_ring embedded_cli (the srdump command line inside an application) ipc_log/ user_example1/
                     dictionary_deltas/ (dlopen-ed plugins: dictionary delta frames, the CHAIN, checkpoints)
ports/               vcpkg overlay port + gcc16 triplet
share/               serreg.magic (file(1) magic for .ser/.rri)
scripts/             run-all-tests.sh (full verification run → logs/<stamp>/), make-release.sh, make-rebuild-prompt.py, bump-version.sh, check-book.py (book ↔ code sync), check-includes.py (include rules), pre-commit, install-hooks.sh, format-all.sh
book/                the maintainer's book: chapters 01–11, executables.md, package-contents.md, files/ (one doc per source file)
docs/                DECISIONS.md ser-reg-{requirements,architecture,testing,stories,rebuild-prompt}.md (current) test-and-log-protocol.md deviations.md ce-links.md
                     ser-reg-requirements-v3.md plans-for-*.md (history) wireshark.md windows-porting.md serreg-{reflection,registry}.md ipc/
                     test-reports/ (one record per green reference run; maintainer tree)
remove-me/           superseded documents kept one release (#private)
logs/                git-ignored output of run-all-tests.sh
cmake/               warnings.cmake serregConfig.cmake.in serreg.pc.in
public-files-list.txt  the public repository's files (make-release.sh --public)
```

## 5b. The book

`book/` documents the library for users and maintainers (index: `book/README.md`). A change to a public API, a
file's mechanism, a byte of the wire format, a program's options or the package contents updates the matching
chapter or `book/files/*.md` document in the same change. `scripts/check-book.py` (a gating stage of
`run-all-tests.sh`) checks that every source file has a page, that every `file:N` citation is in range and points
at the code block it follows, that no page names a removed symbol, and that every test name the book
quotes still exists; `--fix` moves drifted citations.

## 6. Verification protocol (required before claiming anything is done)

1. Build every preset; run all tests — `scripts/run-all-tests.sh` does exactly this and keeps the logs; quote its `00-summary.txt` table (and every SKIP with its reason) in your report. `docs/test-and-log-protocol.md` is the triage and record-keeping procedure, and the gate before a release.
2. Golden `STATIC_REQUIRE` file (`tests/golden/hashes.cpp`) compiles on GCC 16 **and** clang-p2996; keep Compiler Explorer share links current in `docs/ce-links.md`.
3. Registry tests: counts at `main()` entry and after `dlopen`; after `dlclose` the entries are retained and their descriptors and frames stay valid; reload cycles revive the same entries; a plugin rebuilt with a changed layout coexists (`tests/dlopen`, decision A); static lib, shared lib, `-Wl,--gc-sections`, `-flto`.
4. `srdump --verify` exits 0 on all golden captures; `--json` / `--toml` output byte-identical to checked-in expected files; `--elf <test binary>` equals `registry::export_image()` of that binary (modulo RRI header); dictionary-completeness tests (file back-patch, pipe delta frame) pass.
5. ASan/UBSan preset clean; fuzz targets (reader, RRI, ELF, JSON/TOML parsers) run at least briefly after changes to them.
6. If your sandbox lacks a reflection-capable compiler: say so at the top of the report. There is no reflection shim (every header that matters instantiates reflection), so no partial compile counts as verification; list precisely what remains unverified. Never claim reflection code compiled if it did not.

## 7. Deliverables and reporting

- Versioned filenames only: `ser-reg-project-vX.Y.Z.tar.gz` containing a top-level directory `ser-reg-project-vX.Y.Z/`, produced by `scripts/make-release.sh` (reads the version from `CMakeLists.txt`, refuses to overwrite, requires the CHANGELOG entry, excludes build/log/stray capture files). Never reuse a filename; never ship two files with the same name.
- **The public subset (maintainer tree):** `public-files-list.txt` (not itself published) says which files go into the public repository. A new file must be classified there (a plain line, `#private:` or `#decide:`) in the change that adds it; `scripts/make-release.sh --public` fails on an unclassified file and warns when a public file cites an excluded one. Never delete a file to keep it out of the public tree.
- `CHANGELOG.md` entry per version; requirement IDs in test names and commit messages (e.g. `test(FR-R11): a reload revives retained entries`).
- End every work session with: (a) what was verified and how, (b) what is not verified, (c) deviations from the spec, (d) proposed next step. Short prose; command output is the evidence.

## 8. Things not to do

- Do not add dependencies. In-house minimal JSON/TOML parsers only (scope in spec FR-D9); external parsers are a later-major decision.
- Do not make `srdump` do anything the library cannot do through `es::serreg::tools` — the CLI stays a thin wrapper.
- Do not register types lazily (function-local statics) — registration happens at image load.
- Do not make the registry point into an image that can be unloaded, and do not bring back an abort on `dlclose` or a
  `dlopen`/`dlclose` interposer: registrations are copied to the heap at `push()` and retained after an unload
  (decision A). Never free a heap entry or an index before process exit — callers hold pointers into them.
- Do not make `es::serreg` depend on `es::ipc` (or the reverse), or put tool code (CLI, Wireshark, diff) into
  `es::serreg` (decision G).
- Do not use compiler type spellings (`display_string_of`, `__PRETTY_FUNCTION__`) as hash input.
- Do not introduce narrower-than-64-bit IDs on the wire.
- Do not silently reinterpret unsupported members; rejections are `static_assert`s naming type and member and mentioning `as_flat<T>`.
