# ce-examples: es::serreg on Compiler Explorer, three lanes

Four single-translation-unit programs (plus `ce-example0`, `ce-example5`, `ce-example6` and `ce-example7`, demonstrations) that check, on any compiler Compiler
Explorer hosts, that the parts of es::serreg that depend on the compiler and the standard library give the **same names, hashes and bytes** as the
golden values frozen in `tests/golden/` and `tests/fixtures/golden/` (GCC 16.2). This is the FR-H4 cross-check
(`docs/ce-links.md`; DECISIONS T-3). None of them needs `libserreg`.

| Directory | What it checks | Library headers | Checks |
|---|---|---|---|
| `ce-example0/` | slide 19 of the talk, "C++26 reflection in one slide": `^^T`, `[: m :]`, `template for` over `nonstatic_data_members_of` — no library, no stub; the same output on every lane | 0 | 1 (the printed members) |
| `ce-example1/` | canonical names and schema/layout hashes of 28 corpus types, the XXH3 known answer; probes for C-03/C-04 | 5 | 29 + probes |
| `ce-example2/` | ENTRY blobs (dictionary bytes), call/print ids, the byte-exact canonical image, decode round trips | 11 | 20 |
| `ce-example3/` | the 8 frames of `orders.ser` byte for byte (padding scrub, full records, literals, print/println), typed reads, `inspect` + `view` | 16 | 19 |
| `ce-example4/` | the 4 frames of `dynamic.ser` (dynamic members), reading GCC's golden frame, `dispatch` over 6 types | 16 | 10 |
| `ce-example5/` | demonstration: 1000 frames of mixed sizes (one of 120 KB) through an `es::ipc` SPSC ring between two threads, typed `frame_router` decoding, self-checked text | 27 (incl. `es/ipc`) | 1 (text identical, no loss or reorder) |
| `ce-example6/` | demonstration (the talk's "thirty seconds", slide 9): the slide's program as written — a real `capture_file("orders.ser")`, one `serialize`, close — then the file read back and taken apart: FILE header, RRI header, the four ENTRY blobs, CHAIN, writer-info frame, the call frame annotated, a decode driven by the dictionary record alone, the typed read back (stream form if the sandbox has no writable directory) | 17 | 1 (typed read identical) |
| `ce-example7/` | demonstration (binary logging, slide 12): three `ES_SERREG_LOG` lines (info/warn/error; scalar, string and struct arguments) into a `capture_file("app.log.ser")`; the warn frame byte by byte (16-byte header, timestamp, two argument records — level, file, line, mask and format are constants of the call ENTRY, 0 bytes per line); a trace line under `ES_SERREG_LOG_MIN_LEVEL = info` whose argument is never evaluated; every frame read back typed (the log signature) and from the dictionary record alone, rendered as `srdump --level` prints it; `srdump`'s real output as a comment at the end | 20 | 1 (3 written, 3 read back, 0 evaluations) |

The library headers in each `ce-exampleN/es/serreg/` are **relative symlinks into `../../../../include/`**, so the
examples always test this tree's headers. Shared files:
- **`ce-run.py`**: builds and runs an example on the godbolt.org API; `--short-link` and `--project`; uploads
  `es/**.hpp` and the `*.hpp` next to `main.cpp`;
- **`ce_registry_stub.hpp`**: a minimal registry standing in for libserreg, used by examples 3 to 7; since
  2.2.5 it also exports the RRI image (`export_image_since`, `program_hash`), so `capture_file`,
  `write_dictionary_frame` and `dictionary_tracker` work in a single translation unit on Compiler Explorer.

Each example directory has symlinks to both.

## Lanes (maintainer, 2026-09-30)

| Lane | Compiler Explorer | Status |
|---|---|---|
| `gcc` | `g162`: GCC 16.2 + libstdc++ | the reference |
| `clang-libcxx` | `clang_bb_p2996`: Bloomberg clang P2996 + libc++ | runs |
| `clang-libstdcxx` | `clang_bb_p2996` + GCC 16.2's libstdc++ | **unavailable today**: libstdc++'s `<meta>` is GCC-intrinsic-only; `ce-run.py` reports it, not as a failure |

EDG (`edg-trunk-reflection`, rejects `template for` at `reflect.hpp`) and Windows come later; any CE compiler id can
be passed by name.

## In the test suite (2.2.1)

The examples are **not** CMake targets and have no CMakeLists of their own: each is one `main.cpp` meant for a
plain compiler command line (that is what Compiler Explorer runs). They are integrated with the project's build
through `tests/ce/CMakeLists.txt`, which adds one ctest per example (label `ce`, part of every `ctest` of every
preset and so of every `scripts/run-all-tests.sh` run, `--quick` included): `ce_exampleN_builds_and_runs` invokes
`tests/ce/build_and_run.cmake`, which compiles `main.cpp` with the project's compiler and
`-std=c++26 -freflection -O1 -W -Wall -Wextra -Wshadow -Werror -I. -lpthread` — the maintainer's command line,
against the example's own header symlinks only, no library linked — into the build directory
(`build/<preset>/tests/ce/ceN`), runs it there, and checks the output: `0 golden mismatches` / `0 mismatches` /
`ok: 1000 messages` / `typed read: … identical` / `log frames: 3 written, 3 read back, trace argument evaluated 0
times`. Run them alone with `ctest --preset gcc16-debug -L ce --output-on-failure`. A header added to the library must get a symlink in every example that includes it (derive the set with
`g++ -std=c++26 -freflection -I. -I../../include -MM main.cpp`); the ctest fails first. The golden constants inside
the examples follow the frozen values of `tests/golden/` (ce-example2's enum blobs moved with the 1.6 `fixed` flag
and were refrozen in 2.2.1).

## Run

```sh
cd ce-examples/ce-example3                                               # any ce-exampleN
g++ -std=c++26 -freflection -O1 -W -Wall -Wextra -Wshadow -Werror -I. main.cpp -o /tmp/ce3 && /tmp/ce3   # local, GCC 16
ctest --preset gcc16-debug -L ce --output-on-failure                     # all eight, as the test suite runs them
./ce-run.py                                                              # the three lanes on Compiler Explorer
./ce-run.py . gcc                                                        # one lane (the arguments are [dir] [lane…])
./ce-run.py -v --short-link --project "serreg frames" . gcc              # + one shareable project with that lane's panes
```

`--short-link` uploads `main.cpp` as `example.cpp` with an editor session; Compiler Explorer needs both. Short links
are permanent and public to anyone who has them. The free arguments are an optional example directory (default:
the directory the script is run from) followed by lane names or raw compiler ids; **with no lane the script runs
all three lanes**, one request each, and a cold clang-p2996 compile can take minutes on the shared server, so what
looks like a hang is usually that — `-v`/`--verbose` prints every step with a timestamp (bytes uploaded, HTTP
status, server time), `--timeout S` bounds one request (default 600 s — uploads have taken more than five minutes).
`--layout tabs` (the default) saves the project as an explicit GoldenLayout config in the arrangement of the
talk's example-6 link (`godbolt.org/z/EM4aY3qWP`): one column, 60/40 — the file tree, `example.cpp` and every
`--open` file as tabs of the upper stack (`example.cpp` in front), the compiler (flags view open) and executor
(arguments and stdin panels, no compiler output) panes of every lane as tabs of the lower stack — so the link
opens ready for a demo; `--layout side` puts tree + editor left and compiler/executor panes right;
`--layout default` posts the normalized sessions/trees state and lets Compiler Explorer arrange the panes.
Sizes are percentages in `golden_layout()` of `ce-run.py`; change them there if a screen wants another split.
`--open FILE` (repeatable) opens a tree file in an editor tab beside `example.cpp`; `--asm-flags F` sets the
compiler (assembly) panes' flags apart from the executors' (default `-O2 -Wall -Wextra -Wshadow -I.` after the
lane's language flags). The talk's example-6 link, https://godbolt.org/z/PzGTv95xv, was made with:

```sh
./ce-run.py -v --short-link --project Example6 . gcc \
  --open ce_registry_stub.hpp --open es/serreg/reflect.hpp --open es/serreg/registry.hpp \
  --open es/serreg/sink.hpp --open es/serreg/version.hpp
```

## Results with 2.1.13a's headers (2026-10-01)

| Example | `gcc` | `clang-libcxx` | `clang-libstdcxx` | Short link |
|---|---|---|---|---|
| 1 (2.1.14) | 0 mismatches | 0 mismatches; the C-03/C-04 probes print the same names as GCC | unavailable | https://godbolt.org/z/GcPdaq7x9 |
| 2 (2.1.14) | 0 | 0 (the `DynArr` blob now matches) | unavailable | https://godbolt.org/z/jW7jqPz9c |
| 3 (2.1.14) | 0 | 0: flat and print frames byte-identical | unavailable | https://godbolt.org/z/nadWfoPT4 |
| 4 (2.1.14) | 0 | 0: every frame identical, GCC's frames read on clang | unavailable | https://godbolt.org/z/zWjz9Webr |
| 5 (2026-10-02) | ok | ok: `es::ipc` builds and runs under clang + libc++ | unavailable | https://godbolt.org/z/354a6K4h6 |
| 0 (2026-10-19, slide 19 "reflection in one slide"; no library) | ok locally (ctest) | not run | unavailable | **to mint**: `cd ce-examples/ce-example0 && ./ce-run.py --no-run --short-link --project "serreg slide 17" . gcc` |
| 6 (2026-10-19, the "thirty seconds" of slide 9 and the file taken apart) | ok (maintainer's box, 2026-10-19) | not run | unavailable | https://godbolt.org/z/PzGTv95xv (layout `tabs`; earlier arrangement https://godbolt.org/z/EM4aY3qWP) |
| 7 (2026-10-19, binary logging of slide 12: frames, floor, typed and dictionary read back) | ok locally (ctest `ce_example7_builds_and_runs`; the sandbox cannot reach godbolt) | not run | unavailable | **to mint**: `cd ce-examples/ce-example7 && ./ce-run.py --no-run --short-link --project "serreg binary logging" . gcc` |

**No interchange break since 2.1.14** (canonical names, decision B). Until 2.1.13 `std::variant<std::monostate, …>`
named its alternative `std::__1::monostate` under libc++, so example 2's `DynArr` blob and example 4's frame 1 call
hash differed between the lanes, while every data byte was identical. The std vocabulary (`std::monostate`,
`std::string`, `std::vector<E>`…) is now the same on every library. The pre-2.1.14 links: 1
https://godbolt.org/z/ocPs7nMhK, 2 https://godbolt.org/z/6G9hY5Tox, 3 https://godbolt.org/z/va8d4jG1K, 4
https://godbolt.org/z/e47xafc5K.

## Not covered here, by design

The registry proper, `dlopen`, ELF reading, `srdump`, the dictionary-driven decoder, Wireshark and `es::ipc` are
runtime or OS code, not reflection. They belong in a **full project build with clang + libc++** (a CI lane), not on
Compiler Explorer. (`ce-example5` does build `es::ipc`'s region and ring with clang + libc++
and runs them between two threads, but it is a demonstration, not a test of `es::ipc`.)

## Demos: Compiler Explorer or local

What the talk (`docs/talk/presentation-slides-content.md`, numbering of 2026-10-19) can run live, and where. **CE**
= one translation unit plus `ce_registry_stub.hpp` on Compiler Explorer (lane `gcc`, `g162`); **local** = needs
`libserreg` / `libserreg_tools` (`srdump`, `tools::`), the Python package, plugins, tshark or files. Every local
command below was run in this tree on 2026-10-19 (`LD_LIBRARY_PATH=/opt/gcc-16/lib64`, `S=build/gcc16-debug/srdump`,
`F=tests/fixtures/golden/orders.ser`); the first output line is quoted. `build/gcc16-release/srdump` works the
same where it exists (it was 2.2.4 here; the debug one 2.2.5).

| Slide | Demo | Where | Command / example | First line of the output |
|---|---|---|---|---|
| 9 | the "thirty seconds" writer | **CE** `ce-example6` | https://godbolt.org/z/PzGTv95xv — the program verbatim, then the file taken apart (local: `build/gcc16-debug/tests/ce/ce6`) | `registry before main(): 4 entries …` |
| 10 | `srdump orders.ser` | local | `$S $F` | `dictionary  program_hash 0xd6bdb39e65bcecb7  (3 types, 8 literals, 9 calls)` |
| 10 | `--json \| jq` | local | `$S --json $F \| jq -c 'select(.type == "corpus::Order") \| .value \| {id, px, side}'` | `{"id":"42","px":"1050000","side":{"name":"buy","value":1}}` |
| 10, 44 | `--verify` on a damaged copy | local | `python3 ce-examples/flip-a-byte.py $F /tmp/orders-bad.ser && $S --verify /tmp/orders-bad.ser; echo $?` | `flipped bit 3 of the frame_len at offset 1720: 48 -> 56` then `problem: frame 2 @1776: bad frame_len …`, `FAILED (2 problems)…`, exit 1 (the intact file: `ok: 11 frames, 34 records, 576 bytes`, exit 0) |
| 10 | `--info` | local | `$S --info $F` | `format 1.7  program_hash 0xd6bdb39e65bcecb7  size 2296  flags info` |
| 10 | `--dict-only --verbose` | local | `$S --dict-only --verbose $F` (with deltas: `$S --dict-only --verbose build/gcc16-debug/examples/dictionary_deltas/dictionary_deltas.ser`, 3 blocks) | `dictionary  program_hash 0xd6bdb39e65bcecb7 …`; deltas: `… opening block  @48  +2 types, +1 literals, +1 calls` |
| 10, 17, 38 | `--elf ./app`, never executed | local | `$S --elf build/gcc16-debug/tests/golden/make_fixtures` (the binary that wrote `orders.ser`) | `dictionary  program_hash 0x0000000000000000  (3 types, 8 literals, 9 calls)` |
| 10, 43 | `--cxx`: C++ back from a dictionary | local | `$S --elf build/gcc16-debug/examples/file_store --cxx -` | `// generated by srdump --cxx: 3 types, 4 literals, 3 calls; …` |
| 10, 38, 50 | `--diff old.rri ./new-app` | local | `$S --diff tests/fixtures/golden/orders.rri build/gcc16-debug/tests/golden/make_fixtures` (identical, exit 0); `$S --diff tests/fixtures/golden/orders.rri build/gcc16-debug/tests/ce/ce6` (`Cancel` and 8 calls removed: breaking, exit 1) | `identical       corpus::Cancel  0x35af6843b770c519 -> 0x35af6843b770c519` |
| 10, 16, 43 | Wireshark | local (needs a non-root user: Wireshark refuses Lua as root) | `mkdir -p /tmp/ws && cp $F /tmp/ws && $S --wireshark /tmp/ws/serreg.lua /tmp/ws/orders.ser && chmod -R a+rX /tmp/ws && runuser -u nobody -- tshark -n -X lua_script:/tmp/ws/serreg.lua -r /tmp/ws/orders.ser` (filters: `-Y 'serreg.corpus_Order.qty >= 100' -T fields -e serreg.corpus_Order.id`) | `1 … SERREG 1568 file header, format 1.7; dictionary: 20 entries known, 0 unknown` |
| 11 | literals: `"…"_` costs 0 bytes | **CE** `ce-example3` (frames of `orders.ser` with literals, print/println, byte for byte) and `ce-example6` (the 0-byte record in the annotated frame) | https://godbolt.org/z/nadWfoPT4 | `ok   frame 0 ("new order", Order), dirty padding 48 bytes identical` |
| 12 | binary logging: frames, floor, read back | **CE** `ce-example7` (to mint: the results table above) | the warn frame's 40 bytes annotated; `trace argument evaluated 0 times`; the rendered lines | `app.log.ser written by capture_file and read back:` |
| 12 | `srdump --level warn` | local | `build/gcc16-debug/tests/python/python_log_capture /tmp/demo.log.ser && $S --level warn /tmp/demo.log.ser` (a scoped dictionary: `$S --info /tmp/demo.log.ser` shows `dictionary = scoped:py_log::capture_scope`); or run ce-example7 with its `std::remove(path)` commented out and `$S --level warn app.log.ser` (its output is the comment block at the end of `ce-example7/main.cpp`) | `1700000000000003000 WARN  log_capture.cpp:71 book AAPL depth    7` |
| 13 | dynamic members: the bytes of `Msg{7, "ab", {1, -2, 3}}` | **CE** `ce-example4` (frame 0 of `dynamic.ser`, byte for byte, and the typed reads) | https://godbolt.org/z/zWjz9Webr (local: `$S --hex tests/fixtures/golden/dynamic.ser`) | `ok   frame 0 (DynMsg)                         56 bytes identical` |
| 14 | typed reads, `dispatch`, `frame_router` | **CE** `ce-example4` (`deserialize`, `dispatch<6 types>`) and `ce-example5` (a typed `frame_router` over 1000 frames) | https://godbolt.org/z/354a6K4h6 | `ring: 262144 bytes (4096 units of 64), largest frame 120056 bytes` … `ok: 1000 messages` |
| 14, 15 | `tools::for_each_frame`, `dyn::dictionary`, `visit_record` | local (`libserreg_tools`); the dictionary-only idea is on **CE** in `ce-example6` (`decode_with_dictionary`: `type_desc::fields` over the record bytes) | `$S $F` is this API; the C++ is in `docs/ser-reg-stories.md` | — |
| 16 | not even C++: the Python reader | local | `cd python && python3 -m serreg ../tests/fixtures/golden/orders.ser --json \| jq -c 'select(.type == "corpus::Order") \| .value \| {id, px, side}'` | `{"id":"42","px":"1050000","side":{"name":"buy","value":1}}` |
| 17 | the dictionary from a running process | local | `ES_SERREG_DUMP_REGISTRY_FD=3 build/gcc16-debug/examples/file_store /tmp/fs.ser 3>/tmp/reg.rri && $S --rri /tmp/reg.rri` | `dictionary  program_hash 0x1d68ea3fe04a8360  (3 types, 4 literals, 3 calls)` |
| 19 | C++26 reflection in one slide | **CE** `ce-example0` (no library; to mint: the results table above) | the same output on `gcc` and `clang-libcxx` | `Quote: 4 members, 32 bytes` |
| 29 | ENTRY blobs as `consteval` bytes | **CE** `ce-example2` (blob bytes, call ids, the canonical image) | https://godbolt.org/z/jW7jqPz9c | `ok   blob Scalars … size  192` … `0 golden mismatches` |
| 21–28 | canonical names, schema/layout hashes | **CE** `ce-example1` | https://godbolt.org/z/GcPdaq7x9 | `ok   fingerprint64("string") = 0xd8ad194648e774f1` … `0 golden mismatches` |
| 35, 39 | one registry per process, 4 entries before `main()` | **CE** `ce-example6` prints the stub's count; the real `registry::instance().size()` and the published index are local (`libserreg`) | `registry before main(): 4 entries (struct, enum, literal, call signature)` | — |
| 36 | `dlopen` / `dlclose`: retain, revive | local | `build/gcc16-debug/tests/dlopen/dlopen_host build/gcc16-debug/tests/dlopen/serreg_plugin.so keep` (`reload` for the 50 cycles; the ctests `dlopen_A…M`) | `size 2 -> 6 (+4 expected), version 2 -> 6, images 1 -> 2, on_change fired 1` |
| 37 | `-fno-gnu-unique`, hidden registrars | local | `ctest --preset gcc16-debug -R dlopen_I_plugin_without_flags_unloads` | `Passed` (0.02 s) |
| 41 | frame 1 byte by byte | **CE** `ce-example6` (the call frame annotated) | local: `$S --hex --frames 1..1 $F` | `dictionary …` then `frame 1  call 0xbcf8e25bde66fdb2  (2 records)` |
| 42 | FILE / RRI / CHAIN / frames; deltas and checkpoints | **CE** `ce-example6` (the opening block); deltas and checkpoints local | `$S --info build/gcc16-debug/examples/dictionary_deltas/dictionary_deltas.ser` (built by `ctest -R example_dictionary_deltas`) | `format 1.7  program_hash 0xa2ec3e1caf34d6bd  size 5040  flags checkpoints info` |
| B1 | `es::ipc`: frames in shared memory | **CE** `ce-example5` (two threads); two processes local | `ctest --preset gcc16-debug -R example_ipc_log` (then `build/gcc16-debug/examples/ipc_log/ipc_log.actual.txt`) | `round 0 begins` |
| B2 | scoped dictionaries | local | the `python_log_capture` file above: `$S --info /tmp/demo.log.ser \| grep dictionary` | `dictionary  2 types, 11 literals, 7 calls; 1 block(s) at 48` / `writer      dictionary = scoped:py_log::capture_scope` |

Everything that is "CE" above also runs locally as `build/gcc16-debug/tests/ce/ceN` (what the ctests built). Slide
numbers follow the current deck (`docs/talk/presentation-slides-content.md`); the example headers say the same.
