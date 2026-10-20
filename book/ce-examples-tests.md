# `tests/ce/` — the Compiler Explorer examples as tests

*(Maintainer decision, DECISIONS T-3.)* The eight programs of `ce-examples/` are the FR-H4
cross-check: single translation units that reproduce, on any compiler Compiler Explorer hosts, the names, hashes and
bytes frozen in `tests/golden/` and `tests/fixtures/golden/`. Besides the manual cross-check on Compiler Explorer
(`ce-run.py` against godbolt.org), every build compiles and runs all eight against this tree's headers, so a header
change that breaks an example — most often a new `#include` without the matching symlink — fails the ordinary test
run instead of the next manual cross-check. (This page is kept at the top of `book/` because `files/` pages map to
library sources — see `scripts/check-book.py`; `files/tests.md` lists the directory with the other test trees.)

## Files

| file | contents |
|---|---|
| `tests/ce/CMakeLists.txt` | one ctest per example, `ce_example1_builds_and_runs` … `ce_example5_builds_and_runs`, label `ce`, timeout 600 s, each with the output the example must print |
| `tests/ce/build_and_run.cmake` | a `cmake -P` script: compile one `main.cpp`, run it, match its output |

The examples themselves (`ce-examples/ce-exampleN/main.cpp`, their `es/` header symlinks, `ce-run.py`,
`ce_registry_stub.hpp`) are described in `ce-examples/README.md`.

## How it works

```cmake
execute_process(COMMAND ${CXX} ${flag_list} -O1 -W -Wall -Wextra -Wshadow -Werror -I. main.cpp -o ${OUT} -lpthread
                WORKING_DIRECTORY ${DIR} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
```
(`build_and_run.cmake:5-6`.) The example is compiled exactly as the maintainer compiles it on a plain command line
— the project's C++ compiler, `-std=c++26` plus the reflection flag of the lane (`ES_SERREG_REFLECTION_FLAG`),
`-O1 -W -Wall -Wextra -Wshadow -Werror`, and **only `-I.`**: the headers compiled are the example's own relative
symlinks into `include/`, never the build tree's include path, so a header missing from the example's `es/`
directory fails here. No library is linked (examples 3–5 use `ce_registry_stub.hpp` in place of `libserreg`). The
program is then run (120 s timeout) and its output must match:

| ctest | the example checks | expected output |
|---|---|---|
| `ce_example1_builds_and_runs` | canonical names and schema/layout hashes of the corpus types, the XXH3 known answer | `0 golden mismatches` |
| `ce_example2_builds_and_runs` | ENTRY blobs, call and print ids, the canonical image, decode round trips | `0 golden mismatches` |
| `ce_example3_builds_and_runs` | the frames of `orders.ser` byte for byte, typed reads, `inspect` and `view` | `0 mismatches` |
| `ce_example4_builds_and_runs` | the frames of `dynamic.ser`, `dispatch` over six types | `0 mismatches` |
| `ce_example5_builds_and_runs` | 1000 frames through an `es::ipc` SPSC ring between two threads, decoded by a `frame_router` | `ok: 1000 messages` |

## Maintaining it

- **A header-set change needs new links.** When a library header gains an `#include`, derive what each example
  needs with `g++ -MM -I. -I../../include main.cpp` in its directory and add the missing relative symlinks
  (`ce-examples/README.md`); never copy headers into an example.
- **A golden change reaches the examples.** The examples embed the golden values; a `format_minor` bump or a
  refreeze of hashes or fixtures updates them in the same change (the examples are also checksummed into the
  rebuild prompt, `scripts/make-rebuild-prompt.py`).
- The examples are compiled with `-Werror` at `-O1` with the reduced set `-W -Wall -Wextra -Wshadow`; the
  library's own targets use the full set of `cmake/warnings.cmake` (which includes `-Wshadow`), so a
  warning a header produces only at `-O1` or only in a single-TU program shows up here first.
- These ctests prove the GCC lane of this machine only. The clang-p2996 lane and the short links are still run
  with `ce-run.py` on Compiler Explorer and recorded in `docs/ce-links.md` and `ce-examples/README.md`.
