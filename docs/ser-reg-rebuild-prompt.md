# ser-reg rebuild prompt (core)

**For:** library 2.2.1, wire format 1.7 · **Generated companion:** `scripts/make-rebuild-prompt.py` assembles this
file together with the flattened documents and the normative sources into one self-contained file,
`ser-reg-rebuild-v<version>.md`, which is the prompt to hand to an agent (or a person) who has to build the library
again from an empty directory. This core file is the part written by hand: the instructions. The generated file adds
the content — `DECISIONS.md`, `ser-reg-requirements.md`, `ser-reg-architecture.md`, `ser-reg-testing.md`,
`ser-reg-stories.md`, the byte layouts (`include/es/serreg/format.hpp`), the golden values (`tests/golden/hashes.cpp`,
`tests/golden/blobs.cpp`, `tests/golden/corpus.hpp`), the Python format module (`python/serreg/format.py`), the CMake
presets and warning set, the public API headers' leading comments, and the SHA-256 of every checked-in fixture — so
the rebuilt library can be checked bit for bit against the original without the original's source.

`genesis-prompt.md` and `ground-level-prompt.md` are retired by this file (DECISIONS D-2).

---

## The prompt

```text
You are rebuilding es::serreg — a C++26 reflection-driven binary serialization library with a compile-time /
load-time type registry — from an empty directory, to the state described by the documents attached below
(library <version>, wire format <format>). The attached documents are the complete specification: the decisions
(DECISIONS.md), the requirements (ser-reg-requirements.md), the architecture and the byte layouts
(ser-reg-architecture.md, format.hpp), the tests and their procedures (ser-reg-testing.md), the use cases
(ser-reg-stories.md), the golden values that your build must reproduce exactly (hashes.cpp, blobs.cpp,
corpus.hpp) and the checksums of the fixture files. Nothing outside these documents is required.

HOW TO WORK
1. Read DECISIONS.md first; every decision there is final. Do not reopen one. If you must deviate, write the
   deviation into docs/deviations.md with the reason and continue.
2. Read ser-reg-requirements.md and ser-reg-architecture.md next; keep the requirement IDs (FR-*) in test names
   and commit messages exactly as the documents use them.
3. Work in milestones; a milestone is done only when its gate passes. The gates are the tests described in
   ser-reg-testing.md; the golden values are the arbiter. Write the golden/STATIC_REQUIRE test before the code it
   locks, for anything that affects names, hashes, sizes or bytes.
4. Keep the layout, the file names, the target names, the options and the scripts of the architecture document,
   so that the attached tests, CMake presets and scripts apply unchanged.
5. Follow the code conventions of DECISIONS C-*: std::print only, const parameters, std::expected errors, no
   exceptions or RTTI, nothing on the hot path allocates or locks, constinit globals, include rules, the warning set
   with -Werror on by default, clang-format clean, version bumps only through scripts/bump-version.sh.
6. End every session with: verified (and how) / not verified / deviations / next step.

MILESTONES AND GATES
M1 Compile time: reflect.hpp, descriptor.hpp, hash.hpp, blob.hpp, literal.hpp, arg.hpp — canonical names,
   schema/layout/call/literal hashes, ENTRY blobs. Gate: tests/golden/hashes.cpp and blobs.cpp pass unchanged
   (every STATIC_REQUIRE), the ce-examples compile and report 0 mismatches.
M2 Registry: registry.hpp, src/registry.cpp, src/image_elf.cpp, the es_serreg_dict section, scope nodes, delta and
   scoped exports. Gate: test_registry, tests/link, tests/dlopen (keep, reload, changed layout, conflict take-over,
   churn, under TSan too), srdump --elf on the test binary, registry_measure budgets (10k/10 ms; 100k/100 ms on
   the reference box).
M3 Writing: sink.hpp, serialize.hpp, print.hpp, capture.hpp (format 1.7: CHAIN, deltas, CRC, checkpoints, writer
   info, sync policy, no rewrite), log.hpp. Gate: the byte-exact fixtures orders.ser, dynamic.ser, features.ser
   (their SHA-256 are attached) regenerate identically from make_fixtures / make_dynamic_fixtures; test_format17,
   test_log, test_print, test_capture, test_sinks pass; serialize_inlines_at_O2 passes.
M4 Reading: deserialize.hpp, walk.hpp, router.hpp, src/decoder.cpp, src/reader.cpp (span and windowed walkers,
   meta frames, CRC, resync, tools::info), src/dictionary.cpp, src/formatters.cpp, to_string.hpp. Gate: the golden
   text/JSON/TOML fixtures match byte for byte, test_readers, test_dynamic, test_containers, test_limits,
   test_to_string, runtime_selftest with the negative fixtures, the fuzz targets run clean under ASan/UBSan.
M5 Tools: src/cli.cpp (srdump and cli::app), src/wireshark.cpp, src/diff.cpp, src/cxxgen.cpp, the Python reader
   (python/serreg). Gate: srdump_* tests, wireshark parity (tshark and Lua 5.2/5.4 stubs), cxxgen tests,
   python_reader_parity.
M6 Package: three libraries, install/find_package (SameMinorVersion, SONAME major.minor), pkg-config, vcpkg port,
   make-release.sh, bump-version.sh, run-all-tests.sh, the book checks. Gate: scripts/run-all-tests.sh is green on
   every preset; the install + consumer stage passes.

WHAT TO DELIVER
- The tree as described, with CHANGELOG.md starting at the version you were asked to reproduce.
- The full-run report (docs/test-reports/) and the verified / not verified / deviations / next-step summary.
- Any golden value, fixture byte or SHA-256 that you could not reproduce, listed explicitly with the difference —
  never a refrozen golden.
```

---

## How the generated file is laid out

`scripts/make-rebuild-prompt.py [--out DIR]` writes `DIR/ser-reg-rebuild-v<version>.md` (default: the parent of the
repository, next to the release tarballs; `scripts/make-release.sh` calls it) with these parts, each under its own
heading and inside a fenced block where it is source:

1. this core prompt, with `<version>` and `<format>` filled in;
2. `docs/DECISIONS.md`;
3. `docs/ser-reg-requirements.md`;
4. `docs/ser-reg-architecture.md`;
5. `docs/ser-reg-testing.md`;
6. `docs/ser-reg-stories.md`;
7. `include/es/serreg/format.hpp` (the normative byte layouts and constants);
8. `tests/golden/corpus.hpp`, `tests/golden/hashes.cpp`, `tests/golden/blobs.cpp` (the golden values);
9. `python/serreg/format.py` (the format as the second reader states it);
10. `CMakePresets.json`, `cmake/warnings.cmake`, `public-files-list.txt`;
11. the leading comment of every public header (`include/es/serreg/*.hpp`, `include/es/ipc/*.hpp`) — the API
    contract as the headers state it;
12. SHA-256 and size of every file under `tests/fixtures/` and of every `ce-examples/*/main.cpp`.

The script refuses to run when any of the documents is missing or when the version it finds in `CMakeLists.txt`
differs from the version `docs/DECISIONS.md` refers to.
