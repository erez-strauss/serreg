# Contributing to es::serreg

Thank you for looking under the hood. This library exists to make binary serialization *disappear* from the
programmer's day: write `serialize(file, "new order"_, order)` and everything a reader will ever need — the
schema, the names, the call signature — travels with the bytes, produced by C++26 static reflection and
collected by pre-main registration. It was written for one environment (Linux, x86-64, GCC 16) by one person, and
it will only become a general tool through people who take it to places it has not been: another compiler,
another CPU, another container library, another wire protocol, another language reading the files. **That is the
contribution we want most.** Shape it to your needs, and when a change would help the next person, send it back.

Everything below is here to make that easy, not to put a fence around the code.

## 1. Ways to contribute

- **Use it and report.** A type that does not serialize, a reflection spelling your compiler rejects, a reader that
  misreads a file, a benchmark number that surprises you — open an issue with the smallest program that shows it.
  A `.ser` file that `srdump --verify` rejects is a bug report by itself.
- **Port it.** Each port is a clear, bounded piece of work and the design already has the seams:
  - *another compiler* — clang-p2996 is a test lane today (`ce-examples/`, `docs/ce-links.md`); a second
    toolchain that builds the whole library would be the single most valuable addition;
  - *another CPU or ABI* — aarch64, a different `long double`, big-endian: the ABI tag in every FILE/RRI header
    (`format.hpp`) and the canonical image are the hooks; `docs/roadmap.md` §3 (G-6) describes the order of work;
  - *another image format* — `src/image_elf.cpp` / `image_none.cpp` is the whole platform-specific surface of the
    registry (Mach-O or PE would be a third file behind `ES_SERREG_IMAGE_FORMAT`);
  - *another standard library or container* — the container vocabulary (`docs/features.md`, `reflect.hpp`) is a
    set of concepts; a new sequence, map or string type is a few overloads plus a golden test.
- **Extend it.** New sinks and sources (sockets, io_uring, a message bus), new renderers over the existing walks
  (XML, CBOR, Arrow columns), schema export to other serializers, readers in other languages from the dictionary
  alone, compression or integrity per block, an RPC layer over the call signatures. `docs/roadmap.md` §5 lists
  what the framework makes possible, with the assets each item builds on and a size estimate; nothing there is
  reserved. Pick one, open an issue saying so, and go.
- **Measure it.** Numbers from a machine that is not the maintainer's laptop are valuable; `docs/benchmarks.md`
  §5 says how to reproduce every figure and what to record.
- **Teach it.** The `book/` is generated from the code's own comments and the docs; corrections, a missing
  example in `docs/ser-reg-stories.md`, a clearer paragraph — all welcome, and the easiest first pull request.

## 2. What we keep, whatever changes

These are the properties users rely on. A pull request that keeps them is easy to merge; one that trades one
away needs a conversation first (an issue, or a note in `docs/DECISIONS.md`).

- **C++26, and more of it over time.** The library is written against the current standard and the current
  compilers, not a lowest common denominator: static reflection, `std::expected`, `std::print`, `constinit`,
  `consteval`, `std::flat_map`, concepts everywhere. When a newer language or library feature expresses something
  better than what is there (`define_aggregate`, `std::simd`, `std::execution`, `std::inplace_vector`, …), using it
  is encouraged, with the compiler fact recorded in `CLAUDE.md`'s "Compiler facts" and, if a toolchain lacks it, a
  fallback in `docs/deviations.md`.
- **No warnings, ever.** Every target builds with the full warning set and `-Werror` (`cmake/warnings.cmake`,
  DECISIONS C-4), under ASan/UBSan and TSan too. A warning is a bug report from the compiler; fix the code, do
  not silence the flag.
- **No dependencies.** The library and the tools depend on the C++ standard library and the platform's loader;
  Catch2 is test-only. A feature that needs another library (zstd, OpenSSL, Arrow) goes behind a CMake option that
  is *off* by default and must leave the default build untouched.
- **The wire format is a contract.** Byte layouts, hashes and canonical names are frozen by golden tests
  (`tests/golden/`, `tests/fixtures/golden/`, `tests/canary/`). A change there is a format-minor (additive) or a
  major, never silent; `docs/ser-reg-testing.md` says what to update together.
- **Nothing on the hot path allocates, locks or throws.** `serialize`/`deserialize` are a header write and a
  `memcpy`; the registry is read lock-free. Keep it that way, and measure (`tests/bench`) when you touch it.
- **The reader never needs the writer's headers.** Every reader — `srdump`, the Python package, the Wireshark
  dissector, `walk_values` — works from the dictionary alone. A new feature that only a typed reader can use is
  half a feature.
- **Errors are values.** `std::expected<T, error>`; the code never crashes on wrong data, with overridable limits
  (DECISIONS R-2). Fuzz targets exist for every parser; a new parser gets one.
- **Style.** `const` parameters by default, `constinit` globals, `std::print` (never the `printf` family), one
  `main()` that parses arguments and calls an application class, package headers before system headers,
  `#pragma once`, `.clang-format` enforced (`scripts/format-all.sh`). `AGENTS.md` §4 is the full list; it is short.

## 3. How a change goes in

1. **Start from green.** `scripts/run-all-tests.sh --quick` (a few minutes) before you change anything; read
   `logs/serreg-tests-<stamp>/00-summary.txt`, not just the exit code.
2. **Small steps.** One header, one service, one renderer per commit, each with its test. For anything that touches
   hashes, sizes or bytes on the wire, write the golden or `STATIC_REQUIRE` test first.
3. **Name the requirement.** Tests and commit messages carry the requirement IDs (`FR-S3`, `FR-R10`, …) from
   `docs/ser-reg-requirements.md`; a new capability gets a new ID there and, if it is a design choice, an entry in
   `docs/DECISIONS.md` (new entries only; a decided item is not reopened, a deviation is recorded in
   `docs/deviations.md`).
4. **Docs with the code.** The public headers' doc comments, `CHANGELOG.md` (the `Unreleased` section) and, when it
   applies, `docs/features.md` / `docs/ser-reg-stories.md`. `scripts/check-book.py` and `scripts/check-includes.py`
   must report 0 problems; every new file is classified in `public-files-list.txt`.
5. **Run the full protocol** before asking for review when you touched the registry, the decoder, the parsers or
   `es::ipc`: `scripts/run-all-tests.sh` (all presets, fuzzing, sanitizers, install check). Attach
   `00-summary.txt` to the pull request; a SKIP row is reported as unverified, not hidden.
6. **Compiler Explorer.** For reflection code, a `ce-examples` run on the clang-p2996 lane is the second opinion we
   have today (`ce-examples/ce-run.py -v . clang-libcxx`); paste the short link in the pull request.

Pull requests are reviewed for the properties in §2 and for whether the next reader of the code will understand it
without you in the room — a non-obvious trick gets a comment that says *why*, not what.

## 4. The IPC component

`include/es/ipc/` (`es::ipc`, header-only C++23, no dependency on es::serreg) holds the shared-memory rings —
variable-size records, SPSC/SPMC, gates by CAS — that the talk and the benchmarks use to show the serializer at
work between processes (`docs/ipc/ipc-design.md`, `scripts/bench-hrtt-*.sh`). It is **a small library that ships
with the package**: a real component with its own design document, tested under TSan with corrupt-record and
kill tests, installable on its own (`ES_SERREG_IPC_ONLY`, `find_package(serreg COMPONENTS ipc)`), and not part of
what would be submitted with the serializer (DECISIONS A-4, G-4).

It may move to a repository of its own — if people use the rings without the serializer, or want them on
platforms where the serializer is not yet ported — keeping its namespace, its `ring_version` and its tests; the
CMake component boundary already exists for exactly that move. Contributions to `es::ipc` are welcome under the
same rules (§2, with the additional rule that a ring layout change bumps `detail::ring_version`), and so is an
opinion on whether and when it should leave.

## 5. Talking to us

- Issues and pull requests on the repository; for a design question, an issue titled with the component
  (`registry:`, `reflect:`, `ipc:`, `srdump:`, `python:`) gets the right eyes fastest.
- Security problems: `docs/SECURITY.md`.
- License: MIT (`LICENSE`); it may change if the library is submitted to a target that requires another
  (DECISIONS G-7). By contributing you agree that your contribution is licensed under the project's license.

If you are unsure whether something is wanted: it probably is. Open the issue. And if the library simply works for
you and you have nothing to add, a star on the repository is a contribution too: it is how the next person finds it.
