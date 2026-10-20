# CLAUDE.md — ser-reg (`es::serreg`)

Read `AGENTS.md` first; it holds the project rules, conventions, build commands and verification protocol. This file only adds what is specific to working here with Claude Code.

## Where the project stands

**Library 2.2.5, wire format 1.7.** The project is described by six flattened documents, written from the current
state; the v3 spec stays in the tree as history only:
- **`docs/DECISIONS.md`** — the one decision ledger: sections R (requirements), P (protocol), A (API), C (code
  specification), G (general), T (testing/tooling), D (documentation); each entry with its status (implemented /
  to be implemented / open) and where it was first defined. New decisions go here, nowhere else. Do not reopen an
  entry; a deviation is recorded in `docs/deviations.md`.
- **`docs/ser-reg-requirements.md`** — the full requirements (FR-* IDs, kept in test names and commit messages).
- **`docs/ser-reg-architecture.md`** — architecture, design, the code's structure, the byte layouts, the registry
  cost model and its optimisation options.
- **`docs/ser-reg-testing.md`** — the test tree, the golden/fixture rules, the verification protocol, how to write a
  test (with examples).
- **`docs/ser-reg-stories.md`** — use cases with complete code.
- **`docs/ser-reg-rebuild-prompt.md`** + `scripts/make-rebuild-prompt.py` — the prompt from which the library can
  be built again; the generated `ser-reg-rebuild-vX.Y.Z.md` is written next to the release tarballs by
  `scripts/make-release.sh`.

The superseded hand-overs, plans and reviews are kept for one release under `remove-me/` (see its `README.md`);
`DECISIONS.md` replaces them, so read them only for the reasoning behind an entry.

Binding rules that are easy to miss (all in `DECISIONS.md`): the users are the public (R-1); the same trust domain
but the code never crashes on wrong data, with overridable limits (R-2); the minor is the compatibility unit until
3.0 — SONAME `major.minor`, `SameMinorVersion` (G-2); every 1.7 feature is opt-in and the defaults write what 1.6
wrote (A-11); lanes gcc + libstdc++ and clang-p2996 + libc++ (G-1); `-Werror` with the full warning set on every
target by default (C-4); the eight `ce-examples` are part of every test run (T-3).

## Start of every session

1. Read `docs/DECISIONS.md` (the status table and the "to be implemented" / "open" entries), then the top of
   `CHANGELOG.md`, `docs/deviations.md`, and the part of `docs/ser-reg-requirements.md` the session touches.
2. State which DECISIONS entries and FR-IDs the session targets. Do not start work that needs an open maintainer
   decision; list the decision instead.
3. Run `scripts/run-all-tests.sh --quick` (gcc16-debug, format check, srdump checks; `CXX` defaults to `g++` if it
   is GCC ≥ 16, else `/opt/gcc-16/bin/g++-16`) to confirm a green baseline before changing
   anything. Read `logs/serreg-tests-<stamp>/00-summary.txt` — never just the exit code.
4. Before a release: the full run (`scripts/run-all-tests.sh`, all presets, on the reference box or from the
   tarball the user attaches), triaged by `docs/test-and-log-protocol.md` §3, with the report in
   `docs/test-reports/`.

## Files with their own rules

| Path | What | Rule |
|---|---|---|
| `ce-examples/` | Eight Compiler Explorer programs (slide 19's reflection, names/hashes, blobs and images, frames of `orders.ser`, dynamic frames + dispatch, an es::ipc ring, the talk's `capture_file`, the logger), shared `ce-run.py` and `ce_registry_stub.hpp`, a README with results and short links | Library headers are **relative symlinks into `include/`**: never copy headers in. A header-set change (e.g. a new include in `blob.hpp`) needs new links in every example: derive them with `g++ -MM -I. -I../../include main.cpp` and link what is missing (the `ce_exampleN_builds_and_runs` ctests compile with `-I.` only, so a missing link fails there first). Public. |
| `remove-me/` | superseded documents (old ledgers, plans, reviews, rebuild prompts, session notes), kept one release | `#private`; do not edit or append — new decisions go to `docs/DECISIONS.md` |
| `review-programs-for-2.1.11/` | the 2.1.10 review's repro programs and crafted inputs (`*.ser.input`); has its own `CLAUDE.md` | `#private`; turn a repro into a real test before deleting it |

**Every new file must be classified** in `public-files-list.txt` (a plain, `#private:` or `#decide:` line), or
`scripts/make-release.sh --public` fails. That script keeps symbolic links in the public tree and fails on a
public link to an unpublished file. No `*.ser`/`*.rri` outside `tests/fixtures` (the release refuses
them).

## Compiler facts to rely on

- GCC 16 reflection flag: `-freflection` (only with `-std=c++26`). Header `<meta>`, namespace `std::meta`.
- Spellings used in this codebase: `^^T`, `[: r :]`, `std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())`, `bases_of`, `parent_of`, `identifier_of`, `has_identifier`, `type_of`, `offset_of(m).bytes`, `size_of`, `alignment_of`, `is_bit_field`, `enumerators_of`, `annotations_of`, `std::define_static_string`, `std::define_static_array`, `std::define_static_object`, `template for`.
- If a spelling fails: check `/usr/include/c++/16/meta` (or the toolchain's `<meta>`) before guessing; if the feature is missing, use the index-sequence fallback from spec §6.1 and add a line to `docs/deviations.md`.
- Compiler Explorer cross-check (FR-H4). The godbolt.org REST API is reachable from the sandbox (2026-10-01), so run
  the checks yourself rather than asking the maintainer:
  - **Lanes:**
    - `gcc` = id `g162`, `-std=c++26 -freflection`;
    - `clang-libcxx` = id `clang_bb_p2996`, `-std=c++26 -freflection-latest -stdlib=libc++`;
    - `clang-libstdcxx` = the same compiler with GCC 16.2's libstdc++. **This lane is unavailable today:**
      libstdc++'s `<meta>` only declares the metafunctions (GCC implements them as intrinsics) and needs
      `__cpp_impl_reflection`, which clang-p2996 does not define.
    - EDG (`edg-trunk-reflection`, flag `--c++26`) rejects the library's `template for`.
  - **Run:** `ce-examples/ce-exampleN/ce-run.py` (all lanes; `--short-link --project NAME` saves one multi-file
    project) and `docs/ce-links.md`'s amalgamated golden check. Record the results and links in `docs/ce-links.md`
    and `ce-examples/README.md`.
  - **Expected (canonical names):** everything matches on `gcc` and `clang-libcxx`, including `std::monostate` and
    `corpus::DynArr`. A mismatch is a real regression.
  - **Short links:** Compiler Explorer finds a multi-file project's main source by the name `example.cpp` and shows
    a file in an editor only through a linked session; `ce-run.py` does both. Short links are permanent and public.
  - Upstream Clang has no P2996. Without network access, the Docker image `vsavkov/clang-p2996` (libc++) is a local
    option.
- ELF/loader facts the design depends on: `[[gnu::retain]]` needs binutils ≥ 2.36; `dladdr` gives the image base for FR-R12; an `STB_GNU_UNIQUE` symbol makes glibc mark a DSO `NODELETE` (why `-fno-gnu-unique` stays: plugin reloading needs real unloads). Nothing depends on telling `dlclose` from process exit (decision A).

## Working style

- Small, reviewable steps: one header or one service per commit, each with its test.
- Write the golden/`STATIC_REQUIRE` test **before** the implementation for anything that affects hashes, sizes or the wire format.
- When touching `registry.*` or `src/image_*.cpp` (heap entries, retained entries and notifications in `src/registry.cpp`; image identity in `src/image_elf.cpp`), always run `tests/link` and `tests/dlopen` (keep, reload, changed-layout, conflict take-over, churn) — under TSan too — not just unit tests.
- When touching the decoder, RRI/ELF readers or the JSON/TOML parsers, run the corresponding fuzz target under the ASan/UBSan preset for at least a few minutes.
- When touching `include/es/ipc/` (the variable-size rings; design `docs/ipc/ipc-design.md`), run `serreg_ipc_tests` under Debug, TSan and ASan/UBSan and the `hrtt_*_smoke_*` ctests under TSan (`--presets gcc16-tsan --tsan-repeat 20`); a ring layout change bumps `detail::ring_version`. The huge-page test needs a hugetlbfs mount and free pages, otherwise it reports SKIP (as root in a sandbox: `echo 16 > /proc/sys/vm/nr_hugepages; mount -t hugetlbfs -o pagesize=2M none /dev/hugepages`). Benchmark numbers come only from `scripts/bench-hrtt-*.sh` on the reference box, never from this sandbox.
  - Also rerun the many-core stress and kill harnesses in `review-programs-for-2.1.11/ipc_dynamic/` (README
    there).
  - The rules for record headers, gates (layout 2: an ownership word changed only by CAS, a position word written
    with plain stores) and the never-crash validation are in `docs/ipc/ipc-design.md` §4–§5.
  - Latency work: measure with `review-programs-for-2.1.11/ipc_dynamic/latency/` (HT off, pinned, interleaved
    rounds; < 20 ns is noise) and follow `docs/ipc/ipc-optimization-directions.md`.
  - `es::ipc_old` (the old slot rings) is out of the build (G-17); its sources wait under `remove-me/` until 2.3.0:
    do not build on it, do not bring it back.
- Output with `std::print`/`std::println` (library diagnostics: `detail::report_line`); never the `printf`/`puts` family. Every `main()` only parses the command line and calls an application class (AGENTS.md §4).
- Includes (AGENTS.md §4): package headers before system headers, every header self-contained, `#pragma once`. `scripts/format-all.sh` orders them; a new header is checked by the build (`tests/headers`) and by `scripts/check-includes.py`.
- Three libraries (decision G): new tool code goes into `es::serreg_tools`, never into `es::serreg`; `es::ipc` stays header-only C++23 with no dependency on es::serreg (check with `-DES_SERREG_IPC_ONLY=ON`).
- `const` parameters by default; `std::expected` errors; `constinit` globals; no exceptions, no RTTI, nothing on the hot path allocates or locks; no new dependencies.
- Prefer editing existing files over adding new ones; keep `srdump/main.cpp` at one line.
- Bump the version only with `scripts/bump-version.sh X.Y.Z` (never a blanket `sed`: it rewrites historical "since X" references).
- Run `scripts/format-all.sh` before finishing; the pre-commit hook and CI enforce `.clang-format`.
- Byte-level format changes must update `tests/golden/blobs.cpp`, the checked-in `tests/fixtures/golden/*` and `tests/canary/` together with a `format_minor` bump — never silently.
- Spec §11 defaults (sentinel value, `uint16_t` since/deprecated, TOML subset, back-patch strategy, 100 ms signal poll, abort diagnostic text) are to be used as written; if one proves wrong, propose the change in the session report rather than silently diverging.

## Tests and logs

- `scripts/run-all-tests.sh` is the verification protocol (`AGENTS.md` §6) as one command; every stage's output lands in `logs/serreg-tests-YYYYMMDD-HHMMSS/` plus a `.tar.gz`. `docs/test-and-log-protocol.md` says how to read a run, how to triage each stage, what to keep (`docs/test-reports/`), and the release gate.
- A SKIP row (no sanitizer runtime here) is not a PASS. Report it as unverified and list the exact rerun command for the user's box.
- Never make a run green by deleting, disabling or loosening a test, or by refreezing a hash/golden/fixture without the `format_minor` bump and `CHANGELOG.md` line.

## Sandbox limitations

If this environment has no GCC 16 with `-freflection`, say so at the top of your report. There is no reflection shim (every header that matters instantiates reflection); do not present any partial compile as verification of reflection code. List exactly what the user must verify on a real GCC 16 box or on Compiler Explorer.

Things this sandbox *can* do, and so should be done here rather than handed to the maintainer:
- reach Compiler Explorer;
- run many-core stress tests: `review-programs-for-2.1.11/ipc/r_join.cpp` reproduces I-01 on 14 cores without the
  fix;
- run tshark 4.6 (Lua 5.4).
- run `perf` (verified 2026-10-02; `perf_event_paranoid` is -1, no root needed):
  - **`perf stat`** works on the P-cores (pin with `taskset -c 4,6`). Cross-core line transfers show in
    `cpu_core/mem_load_l3_hit_retired.xsnp_fwd/` (and `xsnp_no_fwd`).
  - **`perf record`** and **`perf mem`** work.
  - **`perf c2c`** samples only with `-e ldlat-loads`, and on this hybrid i9-13900H it files the cross-core spin
    loads as "fill buffer hit" with 0 HITM. For cache-line traffic, use the snoop counters with `perf stat`.

A headless browser for checking short links can be installed in the session scratchpad (Playwright). It downloads
about 650 MB into `.cache-fedora/`; remove it afterwards.

## Deliverables

Versioned names only (`ser-reg-project-vX.Y.Z.tar.gz`, top-level dir `ser-reg-project-vX.Y.Z/`, built by `scripts/make-release.sh`), never reuse a filename, `CHANGELOG.md` entry per version, requirement IDs in commit messages and test names. End each session with: verified (and how) / not verified / deviations / next step.
