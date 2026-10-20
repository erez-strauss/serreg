# Compiler Explorer cross-check (FR-H4) — status as of 2.1.14 (canonical names, 2026-10-02)

**Run (2026-10-02, after decision B; wire format 1.4).** The amalgamated golden check (`scripts/amalgamate-golden.py`
→ `-fsyntax-only`) passes on **both** lanes with no exception:
- gcc (`g162`): 0 errors;
- clang-p2996 + libc++ (`clang_bb_p2996`): 0 errors.

That covers every frozen hash, canonical name, descriptor table, gap table and call hash, including
`std::monostate` and `corpus::DynArr` and the new 2.1.14 name rules (`Outer<X>::Inner`, the std vocabulary, user
inline namespaces, explicit names). ce-example1–4 are all-ok on both lanes (`ce-examples/README.md`, new links).
The section below is the 2.1.13a state, kept for the record.

## 2.1.13a state (2026-10-01)

**Run (2026-10-01, through the godbolt.org API; `remove-me/cc-project-update-2.1.13a.md` §2).** The golden corpus passes on
clang-p2996 + libc++ **except two frozen values**: `schema_hash_v<std::monostate>` and
`schema_hash_v<corpus::DynArr>`, which contains `std::variant<std::monostate, …>`. libc++ names the type
`std::__1::monostate`, libstdc++ `std::monostate`, and the name enters the schema string. This is review finding
C-04 hitting a corpus type, and it waits for decision B (now DECISIONS P-3). Everything else
passes: every other frozen hash, canonical name, descriptor table, gap table and call hash in `hashes.cpp`. The
byte-level checks are in `ce-examples/` (three lanes; all flat and print frames are byte-identical across
compilers). Earlier text said the build sandbox cannot reach godbolt.org; the review sandbox of 2026-09-30 could.

## Procedure

1. `python3 scripts/amalgamate-golden.py > golden-ce.cpp` — one self-contained file (about 260 KB): the library
   headers, `tests/golden/corpus.hpp` and `tests/golden/hashes.cpp` in its standalone mode
   (`ES_SERREG_GOLDEN_STANDALONE`: every `STATIC_REQUIRE` is a `static_assert`, no Catch2). The same mode is a ctest
   (`golden_hashes_standalone_compiles`) and the CI `clang-p2996` job.
2. On https://godbolt.org/ paste it twice, with:
   - **x86-64 gcc 16**: `-std=c++26 -freflection -fsyntax-only`
   - **x86-64 clang (experimental P2996)**: `-std=c++26 -freflection-latest -stdlib=libc++ -fsyntax-only`
3. Both must compile with no error. A failing `static_assert` names the value that differs between the compilers:
   that is a canonical-string or reflection difference to fix before anyone relies on cross-compiler hashes.
4. Share both (the "Share" button, short link) and record them below; do the same for the three examples of
   `remove-me/docs/c-review-study.md` §12, which are dependency-free single files.

| Snippet | x86-64 gcc 16 | clang (experimental P2996) | Result |
|---|---|---|---|
| golden hashes (`scripts/amalgamate-golden.py`, 2.1.13a), one editor with both compilers: https://godbolt.org/z/64715o34q | ok | **2 failures**: `schema_hash_v<std::monostate>`, `schema_hash_v<corpus::DynArr>` | C-04 (decision B) |
| study §12 example 1: compile-time schema string and fingerprints | _todo_ | _todo_ | _todo_ |
| study §12 example 2: registration by odr-use, before `main` | _todo_ | _todo_ | _todo_ |
| study §12 example 3: annotations drive evolution | _todo_ | _todo_ | _todo_ |
| `-O2` assembly of `serialize_raw<Order>` into a `span_sink` (zero-overhead check, spec §8) | _todo_ | — | _todo_ |

For the last row, compile the amalgamated file with `-O2 -S` plus a function
`std::size_t f(std::span<std::byte> b, const corpus::Order& o) { es::serreg::span_sink s{b}; return *es::serreg::serialize_raw(s, o); }`
and check that it is a bounds check and a `memcpy` (no call into `libserreg`).

## `ce-examples/` (2.1.13a, three lanes)

Runnable on every lane with `ce-examples/ce-run.py` (`--short-link` makes one multi-file project with all lanes):

| Example | gcc | clang + libc++ | Short link |
|---|---|---|---|
| 1 names and hashes | ok | ok | https://godbolt.org/z/ocPs7nMhK |
| 2 blobs, call ids, image bytes | ok | 1 (`DynArr` blob) | https://godbolt.org/z/6G9hY5Tox |
| 3 frames of `orders.ser`, typed reads | ok | ok (byte-identical) | https://godbolt.org/z/va8d4jG1K |
| 4 frames of `dynamic.ser`, dispatch | ok | 2 (call hash of frame 1 only) | https://godbolt.org/z/e47xafc5K |
| 5 two threads, es::ipc SPSC (2026-10-02, after the switch; pre-switch `es::ipc_dynamic` link: https://godbolt.org/z/6K7TsbEsr) | ok | ok (es::ipc under clang + libc++) | https://godbolt.org/z/354a6K4h6 |
| 0 slide 19, "C++26 reflection in one slide" — `^^T`, `[: m :]`, `template for`; no library (2026-10-19) | ok locally | not run | **to mint**: `cd ce-examples/ce-example0 && ./ce-run.py --no-run --short-link --project "serreg slide 17" . gcc` |
| 6 the talk's "thirty seconds" (slide 9): the slide's program verbatim (`write_orders()`: a real `capture_file("orders.ser")`, one `serialize`, close), then the file read back and taken apart with `format.hpp`'s layouts alone — the 48-byte FILE header, the 40-byte RRI header, the four ENTRY blobs (call signature, literal, `Order`, `Side`), the CHAIN, the writer-info frame, the call frame annotated (`n_records` 2, the literal a 0-byte record, the call hash equal to `call_hash_v<lit<"new order">, Order>`), a decode driven by the dictionary record alone (`type_desc::fields`, no `Order` in that code path), the typed read back; stream form (`write_dictionary_frame` + `vector_sink`) when the sandbox has no writable directory (main.cpp of 2026-10-19; the stub gained `export_image_since`/`program_hash` for it) | ok (2026-10-19, maintainer's box; the sandbox cannot reach godbolt) | not run | https://godbolt.org/z/PzGTv95xv (layout `tabs`, editors for the stub, reflect/registry/sink/version.hpp; an earlier arrangement by hand: https://godbolt.org/z/EM4aY3qWP) |
| 7 binary logging (slide 12): three `ES_SERREG_LOG` lines (info/warn/error; scalar, string, `Order`) into a `capture_file("app.log.ser")`; the warn frame's 40 bytes annotated (16-byte header, timestamp, two argument records — level, file, line, mask and format are constants of the call ENTRY: 0 bytes per line); a trace line under `ES_SERREG_LOG_MIN_LEVEL = info` whose argument is never evaluated (counter 0); every frame read back typed (`deserialize_frame<log_tag, …>`) and from the dictionary record alone, rendered as `srdump --level` prints it; `srdump`'s real output pasted at the end of main.cpp (2026-10-19) | ok locally (ctest `ce_example7_builds_and_runs`, GCC 16.2; the sandbox cannot reach godbolt) | not run | **to mint**: `cd ce-examples/ce-example7 && ./ce-run.py --no-run --short-link --project "serreg binary logging" . gcc` |

The third lane, clang + libstdc++, is not possible today: libstdc++'s `<meta>` only works with GCC's intrinsics.
EDG trunk rejects `template for` (`reflect.hpp`, canonical names of templates).

## Known differences to expect

- `-freflection-latest` is needed on clang for annotations (`since`, `deprecated`, `skip`).
- Upstream Clang 22/23 do not implement P2996; only Bloomberg's clang-p2996 fork (the "experimental P2996" compiler
  on Compiler Explorer) does.
