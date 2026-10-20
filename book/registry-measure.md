# `tests/bench/registry_measure.cpp` — the registry at scale

*(DECISIONS G-5, item "K".)* A measurement program for what the registry costs a program with many
message types. A registration is one `registry::push()` run from an image's static initialisers; the first query
publishes one index for the whole burst; exporting the dictionary (a capture file's opening block, a dictionary
frame) and parsing it (srdump, a reader) cost in proportion to the entries. `registry_measure` synthesises N struct
entries whose ENTRY blobs add up to about B bytes, pushes them exactly as guards would, and times each phase; with
`--assert` it enforces the start-up budget of G-5. It is a test-tree program (`tests/bench/`), built with the
tests, never installed. (This page is kept at the top of `book/` because `files/` pages map to library sources —
see `scripts/check-book.py`; `files/tests.md` lists the program with the other benchmarks.)

## Usage

```
registry_measure [--entries N] [--bytes B] [--assert] [--plugin] [--elf <binary>] [--json]
```

| option | default | meaning |
|---|---|---|
| `--entries N` | 100 000 | synthetic struct entries (at most 131 072, the size of the static descriptor pool) |
| `--bytes B` | 100 MiB | total ENTRY bytes; each entry gets `max(64, B / N)` bytes through long field names |
| `--assert` | off | enforce the budget: push + publish ≤ N / 1000 ms (10 ms for 10 000, 100 ms for 100 000); only in an optimised, non-sanitizer build with a release library — otherwise the line says "not enforced" |
| `--plugin` | off | put the descriptors on the heap, so the registry treats the entries as a `dlopen`ed image's and deep-copies them |
| `--elf <binary>` | — | measure the reader side instead: `tools::extract_registry_from_elf` of a real program (section size, types, literals, calls, load time) |
| `--json` | off | one JSON object instead of the table |

Exit codes: 0 ok, 1 budget exceeded, 2 usage or a consistency failure (the registry does not hold N new entries,
a lookup misses, the export does not parse).

## How it works

`app::make_type` (`registry_measure.cpp:108-170`) builds one flat struct of eight `i64` members named
`measure::T<i>`: a `dyn::type` serialised with `dyn::to_rri` gives the ENTRY blob, a `type_desc` with its
`field_desc`s is the payload, and a `reg_node` ties them together. The descriptors of the default case live in
static pools (`g_desc_pool`, `g_field_pool`, `registry_measure.cpp:59-61`) — in the executable's `.bss` — because
the registry decides by the payload's address (`dladdr`) whether an entry belongs to the main program: those
entries are **viewed in place** (blob, name and descriptor; the executable is never unmapped), while
`--plugin` entries are copied and deep-copied as a `dlopen`ed image's are.

`run_synthetic` (`registry_measure.cpp:172-270`) then times:

| phase | what runs | the cost it stands for |
|---|---|---|
| push | `reg.push(&node)` × N | static initialisation of a program with N types (or a `dlopen`) |
| publish | the first `reg.size()` | the one index published for the burst: sorted entries, four `flat_map`s, program hash |
| lookup | `find_type(h)` × N in random order | per-message lookups of a reader (`deserialize` on a side dictionary, `srdump`) |
| export | `export_image()` | a capture file's opening dictionary, `ES_SERREG_DUMP_REGISTRY` |
| parse | `dyn::dictionary::add_rri(image)` | a reader loading that dictionary |
| rss | `/proc/self/statm` before and after | memory, including the measurement's own copies |

## The budgets and the numbers

DECISIONS G-5: a release build registers 10 000 entries of 10 MB in at most 10 ms and 100 000 entries of 100 MB in
at most 100 ms (push + publish). The ctests:

| ctest | command | label |
|---|---|---|
| `registry_measure_10k` | `--entries 10000 --bytes 10485760 --assert` | `bench` |
| `registry_measure_100k` | `--entries 100000 --bytes 104857600 --assert` | `bench` |
| `registry_measure_elf` | `--elf serreg_bench` (passes when the line reports types and literals) | `bench` |

Sandbox measurements (2 vCPU VM, GCC 16, `-O2`): 10 000 / 10 MB — push 7.7 ms + publish 1.9 ms, within
budget; lookup about 100 ns; export 14 ms; parse 12 ms. 100 000 / 100 MB — push 111 ms + publish 29 ms, **over the
100 ms budget on this VM** (the reference box is pending); lookup 130 ns; export 117 ms; parse 139 ms; RSS
+546 MB including the measurement's own blob copies. Viewing the main program's entries in place instead of
copying them halved the push time and removed one blob-size of RSS.

What a program with many types can do (chapter 5 and `docs/ser-reg-architecture.md` discuss each):
fewer, larger message types; scoped capture files (`scoped_capture_file<Tag>`: only the scope's entries are
exported per file); `capture_policy{.dictionary_updates = false}` when nothing is `dlopen`ed (no per-frame version
compare); `capture_options::checkpoint_dictionary = false` (the default) so a checkpoint frame stays 80 bytes; the ELF
dictionary section is read by tools only, never by the program itself.

## Maintaining it

- The program pushes nodes directly (`registry::push` is public for the guards), so it measures the registry
  alone, without the static-initialisation machinery around it; keep it that way — it must not depend on
  thousands of real instantiated types.
- The header comment still describes `push` as "heap copy of each ENTRY" (`registry_measure.cpp:9`); that is true
  only with `--plugin`.
- The budget check is compiled only for optimised, unsanitised builds with a release library
  (`ES_SERREG_BENCH_DEBUG_LIB` is defined for a Debug configuration by `tests/bench/CMakeLists.txt`); benchmark
  numbers come from the reference box, never from a sandbox.
