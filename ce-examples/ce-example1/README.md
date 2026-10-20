# ce-example1: fingerprint layer on Compiler Explorer

The smallest part of es::serreg that carries the FR-H4 promise ("the same source gives the same hash on every
conforming compiler"): canonical type names and the XXH3-64 schema/layout hashes. It uses no `libserreg`, and
registers, serializes and links nothing.

| File | What it is |
|---|---|
| `main.cpp` | Own file: 28 corpus types copied verbatim from `tests/golden/corpus.hpp`, checked against `tests/golden/hashes.cpp`, plus the XXH3 known answer; "probe" lines for review findings C-03/C-04 |
| `es/serreg/{hash,error,format,descriptor,reflect}.hpp` | Relative symlinks to `../../../../include/es/serreg/` (identical to the project) |
| `ce-run.py` | Symlink to `../ce-run.py`, which sends `main.cpp` and the headers (symlinks resolved) to the godbolt.org API, builds and runs the example, and prints the output |

## Run

```sh
g++ -std=c++26 -freflection -O1 -I. main.cpp -o ce1 && ./ce1        # local GCC 16
./ce-run.py                                                          # Compiler Explorer: the three lanes (see ce-run.py)
./ce-run.py . g162 clang_bb_p2996 edg-trunk-reflection              # choose compilers
```

Exit status 0 means every golden value matches. The PROBE lines have no golden values; compare them across
compilers.

## Results (2.1.10, 2026-09-30; unchanged with 2.1.13a's headers, 2026-10-01)

| Compiler | Golden (28 types + XXH3) | Probes |
|---|---|---|
| local GCC 16.2.1 (Fedora) | **all ok** | `Outer<int>::Inner` = `Outer<long>::Inner` = `probe::{unnamed}::Inner` (C-03); std types as `std::__cxx11::basic_string<…>`, `std::vector<i32,std::allocator<i32>>` (C-04) |
| CE `g162` (x86-64 gcc 16.2) | **all ok** | identical to local |
| CE `clang_bb_p2996` (Bloomberg clang P2996, libc++) | **all ok**: FR-H4 verified for the golden corpus for the first time | the same `{unnamed}` collision; every std type spelled `std::__1::…`, so all five std-type probe hashes differ from GCC's (C-04 confirmed) |
| CE `edg-trunk-reflection` (`--c++26`) | build fails | EDG rejects `template for` at `reflect.hpp:401` (canonical names of templates); the spec §6.1 index-sequence fallback would avoid it |

## Lanes

`ce-run.py` runs three lanes (DECISIONS T-3):
- `gcc`: GCC 16.2 + libstdc++;
- `clang-libcxx`: clang-p2996 + libc++;
- `clang-libstdcxx`: clang-p2996 + libstdc++. This lane is **unavailable today**: libstdc++'s `<meta>` works only
  with GCC's front-end intrinsics. The script reports it as expected-unavailable, not as a failure.

EDG and Windows come later. Any other Compiler Explorer compiler id can still be passed by name.

Short link (all lanes in one project; built from 2.1.13a's headers, 2026-10-01): https://godbolt.org/z/ocPs7nMhK

## `ce-run.py` options

```sh
./ce-run.py [--short-link] [--project NAME] [example-dir] [lane | compiler-id ...]
```

- `--short-link` saves the example once as a multi-file Compiler Explorer project with one compiler pane and one
  executor pane per lane, and prints its `https://godbolt.org/z/…` link. Compiler Explorer keeps it permanently,
  and it is public to anyone who has the link.
- The upload names `main.cpp` as **`example.cpp`** (Compiler Explorer recognises the main source of a non-CMake
  project by that name) and opens it in an editor through a session linked to the file. Without the session the
  editor shows the file as missing.
- `--project NAME`: Compiler Explorer's saved state has no project-name field, so NAME becomes the first-line
  comment of `main.cpp` (`// Project: NAME`) and a `PROJECT.md` file in the project.
