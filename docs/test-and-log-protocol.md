# Test-and-log protocol — what to do with the full test run before a release

Audience: Claude Code (or any coding agent) and the maintainer. This is the gate before a release: no release
ships until the steps below have been done on the reference machine and the result is recorded in the repository.

## 1. The script

`scripts/run-all-tests.sh` is the whole verification protocol of `AGENTS.md` §6 in one command. It runs, in
order, and captures every byte of output:

| stage                      | what                                                                                     | gating |
|----------------------------|------------------------------------------------------------------------------------------|--------|
| `format/…`                 | `clang-format --dry-run --Werror` over `include src tools tests examples`                | yes    |
| `book/check-book.py`       | book ↔ code sync: a page per source file, `file:N` citations in range and on their code block, no removed names, quoted test names exist | yes |
| `<preset>/configure`       | fresh `cmake --preset <p>` (build dir deleted first; `--keep-build` reconfigures instead) | yes    |
| `<preset>/build`           | `cmake --build --preset <p> -j`                                                          | yes    |
| `<preset>/ctest`           | `ctest --preset <p> -j --output-on-failure` (`-LE bench` under release and sanitizers)   | yes    |
| `gcc16-debug/golden-print` | `serreg_tests "[golden-print]"` → `checks/golden-print.txt` (the hash oracle)            | yes    |
| `gcc16-debug/srdump …`     | `--version`, `--verify` on the golden capture, `--elf … --program-hash`, `--elf … --json` | yes    |
| `gcc16-debug/file(1) magic`| `file -m share/serreg.magic` identifies `.ser` and `.rri`                                | yes    |
| `gcc16-release/bench`      | `ctest -L bench` (hot-path ceilings)                                                     | WARN   |
| `fuzz/<target>`            | `serreg_fuzz rri|capture|elf|config` under ASan/UBSan (else under the UBSan trap preset, else in the debug build with `_GLIBCXX_ASSERTIONS`), `--fuzz-iters` runs each; `capture` also aborts if the typed decoder and the walker disagree on an image | yes |
| `gcc16-tsan/concurrency`   | `dlopen_[ABDEFJL]|signal_dump|concurrent|hrtt_.*_thread` repeated `--tsan-repeat` times (`until-fail`)     | yes    |
| `install/…`                | `cmake --install`, a `find_package(serreg)` consumer configured/built/run, `pkg-config`  | yes    |

Presets: `gcc16-debug gcc16-release gcc16-static gcc16-static-none gcc16-ubsan-trap gcc16-asan-ubsan gcc16-tsan` by default
(`clang-p2996` only when `CXX` is a P2996 clang and it is named in `--presets`). A preset whose runtime is missing
(no `libasan` for the chosen `CXX`) is **SKIP**, with the reason in the summary — SKIP is not PASS, see §3.
`gcc16-ubsan-trap` is UBSan with `-fsanitize-trap=undefined`: undefined behaviour executes a trap
instruction (SIGILL, the test fails), so it needs no runtime library and runs on any box. It is **SKIP** by default
where `gcc16-asan-ubsan` runs (the same UBSan checks plus vptr and ASan, with diagnostics); name it in `--presets` to
run both. `gcc16-static-none` is the static library built with `ES_SERREG_IMAGE_FORMAT=none` (no dladdr,
no `es_serreg_dict` section): the whole suite runs, with the ELF-only checks replaced
by their no-section counterparts (`srdump_elf_none_has_no_section`, the null-backend `link_image_none_backend`).
It is not a substitute for ASan: out-of-bounds reads that do not fault are invisible to it.

Run it on the reference box (ai-box Fedora/Ubuntu image, GCC 16 as `g++`):

```sh
scripts/run-all-tests.sh                       # everything, ~10–25 min on 8 cores
scripts/run-all-tests.sh --quick               # gcc16-debug only, short fuzz, no install — a smoke run
scripts/run-all-tests.sh --presets gcc16-asan-ubsan,gcc16-tsan --fuzz-iters 1000000 --tsan-repeat 20
CXX=/opt/gcc-16/bin/g++-16 scripts/run-all-tests.sh   # a non-system GCC 16 (LD_LIBRARY_PATH is set for you)
```

Output: `logs/serreg-tests-YYYYMMDD-HHMMSS/` and the same as `logs/serreg-tests-YYYYMMDD-HHMMSS.tar.gz`.
`logs/` is git-ignored. Exit status 0 means every gating stage passed or was skipped with a reason.

## 2. Reading a run (agent instructions)

Read in this order; stop as soon as the question "is this run green?" is answered.

1. `00-summary.txt`, top to bottom. It has one row per stage (`PASS|FAIL|WARN|SKIP`, seconds, log path,
   reason), the ctest totals per preset, the list of failed tests per preset, then the **last 40 lines of every
   FAIL/WARN log** and **every sanitizer report** verbatim. For most failures this file is enough to start.
2. `00-environment.txt`: compiler, cmake, ninja, binutils, glibc, host, `git describe`, the script options
   and the sanitizer option strings. Quote it in the session report; differences from the last green run
   (compiler patch level above all) are the first suspect.
3. Only then the stage logs the summary points at. `<preset>/LastTest.log` has the full output of every test
   including passing ones; `<preset>/LastTestsFailed.log` is the machine-readable failed list.

When the user attaches a tarball instead of running the script in the agent's sandbox:

```sh
mkdir -p "$SCRATCH/logs" && tar -xzf serreg-tests-*.tar.gz -C "$SCRATCH/logs"
cat "$SCRATCH"/logs/serreg-tests-*/00-summary.txt
grep -rn 'ERROR: \(Address\|Leak\|Thread\)Sanitizer\|runtime error:' "$SCRATCH"/logs/serreg-tests-*/ | head
```

Never claim a stage is green that is not in the summary as PASS. A sandbox without GCC 16 or without the
sanitizer runtimes cannot produce those rows; say so and ask for the tarball.

## 3. Triage by stage

| stage failed            | first questions                                                                                     | what to do                                                                                                                                       |
|-------------------------|-----------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| configure               | compiler < 16? `-freflection` rejected? Catch2 fetch blocked?                                       | toolchain problem, not a code problem: fix the environment (`CXX`, `ES_SERREG_CATCH2_DIR`), rerun. Do not patch `CMakeLists.txt` around it.       |
| build                   | new GCC patch level? reflection spelling changed? `-Werror` on a new warning?                       | check `<meta>` of that toolchain first (`CLAUDE.md`); a spelling change is a deviation → `docs/deviations.md`; a new warning is fixed at the source, never silenced with a flag. |
| ctest: `golden_*`, `blobs` | hash or blob size changed                                                                        | **never refreeze to make it pass.** Diff `checks/golden-print.txt` against the last green run; find the canonical-string or blob change; either revert it or bump `format_minor` with the fixtures/canary update `CLAUDE.md` demands. |
| ctest: `link_*`, `dlopen_*` | binutils/glibc changed? `[[gnu::retain]]` support? `DF_1_NODELETE` back?                        | `readelf -d` the plugin (`FLAGS_1`), `readelf -S` for `es_serreg_dict`; these tests encode loader facts, see `AGENTS.md` §6.3 and `CLAUDE.md` "ELF/loader facts". |
| ctest: `negative_*`     | a diagnostic text changed with the compiler                                                          | update the expected diagnostic substring in `tests/negative/` only after confirming the rejection still happens for the right reason.            |
| ctest: `canary_*`       | a checked-in fixture no longer decodes                                                               | wire-format regression until proven otherwise — the fixtures are the compatibility contract for the readers. Bisect the reader change.           |
| `book/check-book.py`    | `checks/book.log`: `pages:`, `range:`, `code:`, `stale:` or `tests:` lines                                   | the book lags the code: run `scripts/check-book.py --fix` (moves drifted citations), then fix the rest by hand — a missing page is written, a stale name is rewritten, a `tests:` line gets the test's current name. Never delete a citation to silence it. |
| ctest under ASan/UBSan  | the report in the summary names file:line                                                            | fix the code. No suppression files, no `-fno-sanitize=` additions beyond the three the preset already carries (documented in `CMakePresets.json`). |
| `G2-12 a bool byte other than 0/1 reads as true …` under ASan/UBSan | `-fsanitize=undefined` includes `-fsanitize=bool` (GCC); this test decodes frames whose bool bytes are 0x02/0x07/0xff into live objects and loads the bools | the row is the B8 check that `copy_flat`'s in-place repair leaves no trap representation behind: "load of value 2, which is not a valid value for type 'bool'" here means the repair was skipped or reordered — fix the code; do not move the test to a copy buffer to silence it (review B8). |
| ctest under TSan        | which two stacks race? registry push/guard destructor/publish, signal thread?                                               | real unless proven a TSan false positive on `dl_iterate_phdr`/loader locks; a proven false positive gets a one-line `__attribute__((no_sanitize("thread")))` with a comment, never a blanket suppression. |
| `serreg_ipc_tests` green under TSan, torn-record question open | the TSan lane compiles the ring's fences out (`ipc-design.md` §5.7) | a green TSan run says nothing about the overwrite path (torn record headers, lapped readers); that path is judged by the lapped-reader tests and the `ipc_dynamic` kill harnesses under Debug and ASan/UBSan, never by the TSan row (review B9). |
| `fuzz/<target>`         | the log's first line has the seed; the last lines the crashing input file/iteration                 | reproduce with `serreg_fuzz <target> <corpus> <iters> <seed>`, minimise, add the input to `tests/fixtures/corpus/<target>/`, fix, rerun the target for ≥ 200 000 iterations. |
| `gcc16-tsan/concurrency`| flaky or deterministic? (`--tsan-repeat 20` decides)                                                | a test that fails 1/20 is a real ordering bug in the test or the code, not "flaky"; do not raise timeouts to hide it.                               |
| `gcc16-release/bench` (WARN) | machine loaded? `nproc` < 4? other presets running?                                             | informational on a shared machine; gating only on a quiet reference box. Record the numbers in the report either way.                            |
| `install/*`             | `find_package` config, RPATH, `pkg-config` prefix                                                   | packaging regression — treat as a build failure. The consumer program is in the script; run its steps by hand from `install/*.log`.              |
| SKIP rows               | which runtime is missing                                                                             | a SKIP for ASan/TSan on the **reference** box is a failed gate: install the runtime (`libasan`, `libubsan`, `libtsan` for that GCC) and rerun.  |
| `ipc region: explicit 2 MiB huge pages …` SKIP or FAIL | `00-environment.txt` lines `hugepages:` / `hugetlbfs:` | SKIP is environment: no pool (`HugePages_Free 0`), no 2M hugetlbfs mount, or a mount NOT writable by the test user (the skip message says which). The test needs 2 free pages. Fix: `echo 64 > /proc/sys/vm/nr_hugepages`; `chmod 1777 /dev/hugepages` or mount with `pagesize=2M,uid=,gid=,mode=0770`. A FAIL prints the `region::create` error; that one is real. |
| `FATAL: ThreadSanitizer: encountered an incompatible memory layout but was unable to disable ASLR` | environment, not a race: high `vm.mmap_rnd_bits` + a seccomp profile blocking `personality(ADDR_NO_RANDOMIZE)` | the script detects it (40-run probe), retries under `setarch -R`, else SKIP `ENV: TSan vs ASLR`. Fix on the host: `sudo sysctl vm.mmap_rnd_bits=28` (or run the container with `--security-opt seccomp=unconfined`), rerun `--presets gcc16-tsan`. Never count it as PASS. |

Two rules that override everything above: no test is deleted, disabled, or given a looser expectation to make
a run green; and no hash, golden or fixture is refrozen without the `CHANGELOG.md` line and `format_minor`
bump that `CLAUDE.md` requires.

## 4. What to keep from a run

- `logs/` stays out of git. The tarball is what gets attached to a review or a session report.
- After a **green** run on the reference box for a release, commit a short record:
  `docs/test-reports/<version>-<YYYYMMDD>.md` containing, verbatim, `00-environment.txt`, the stage table
  and ctest totals from `00-summary.txt`, and `checks/golden-print.txt`. That file is the frozen hash oracle
  the next version is compared against (the golden `STATIC_REQUIRE`s are the compile-time copy of it).
- The session report (`AGENTS.md` §7) quotes the summary table, names the tarball, and lists every SKIP with
  the reason. "All tests pass" without the table is not a report.

## 5. The release gate

A release ships only when both of the following hold:

1. A green run of `scripts/run-all-tests.sh` with the default presets on the reference box, **no SKIP rows**
   (ASan/UBSan and TSan actually ran), fuzz targets at ≥ 200 000 iterations each, and its
   `docs/test-reports/<version>-<date>.md` committed.
2. `tests/fixtures/golden/*`, `tests/fixtures/corpus/*` and `tests/canary/` are the **compatibility corpus**:
   the checked-in captures of earlier format minors stay unchanged, every reader must decode them, and the run
   must show the canary tests green against them.
