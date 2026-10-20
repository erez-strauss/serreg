# ce-example2: dictionary blobs, call ids and canonical images on Compiler Explorer

The wire-level half of the FR-H4 cross-check. It covers:
- the ENTRY blobs that go into the ELF section, the RRI and every capture dictionary;
- call-signature hashes and literal ids;
- the byte-exact canonical image of a dynamic type, with encode → decode round trips.

It uses no `libserreg`. The one library function the decoder can call, `detail::report_warning`, is stubbed in
`main.cpp`.

| File | What it is |
|---|---|
| `main.cpp` | Own file. Corpus types copied verbatim from `tests/golden/corpus.hpp`. Checked against `tests/golden/blobs.cpp`, `tests/golden/hashes.cpp` (call ids) and the README/plan v2.0 §2 image bytes |
| `es/serreg/{hash,error,format,descriptor,reflect,literal,arg,blob}.hpp`, `es/serreg/detail/embed_{elf,none}.hpp` | Relative symlinks into `include/` (identical to the project) |
| `ce-run.py` | Symlink to `../ce-run.py` |

## Run

```sh
g++ -std=c++26 -freflection -O1 -I. main.cpp -o ce2 && ./ce2        # local GCC 16 (add -DES_SERREG_EMBED_NONE for the "none" backend)
./ce-run.py                                                          # Compiler Explorer: the three lanes (see ce-run.py)
```

## Results (2.1.10, 2026-09-30; unchanged with 2.1.13a's headers, 2026-10-01)

| Compiler | Result |
|---|---|
| local GCC 16.2.1, with and without `ES_SERREG_EMBED_NONE` | **20/20 ok** (12 blobs, 4 call ids, image bytes, 3 round trips) |
| CE `g162` | **20/20 ok** |
| CE `clang_bb_p2996` (libc++) | **19/20**: **`blob DynArr` differs** (`0x762f28d78391cf5f` vs golden `0xc3060b35d803ae72`) |
| CE `edg-trunk-reflection` | build fails (`template for`, see ce-example1) |

**Why DynArr differs.** `DynArr` has a `std::variant<std::monostate, DynMsg>`. The canonical name of `std::monostate`
is `std::monostate` under libstdc++ but `std::__1::monostate` under libc++, and that name enters the variant's
alternative list and the schema string. So the **schema hash** of `DynArr` differs too (GCC `0x9032db9ee55d1318`,
clang `0x92f1a7c2e838c75e`), while the layout hash agrees (`0xab9b51e9c98b131d`).

A GCC writer and a clang reader of any type that contains `std::variant<std::monostate, …>` therefore reject each
other's frames with `hash_mismatch`. This is review finding C-04 in a shipped corpus type, not just in user
templates over std types. It is evidence for DECISIONS P-3: std types need fixed canonical names
(`monostate`, `string`, `vector<E>`, …) independent of the standard library.

## Lanes

`ce-run.py` runs three lanes (DECISIONS T-3):
- `gcc`: GCC 16.2 + libstdc++;
- `clang-libcxx`: clang-p2996 + libc++;
- `clang-libstdcxx`: clang-p2996 + libstdc++. This lane is **unavailable today**: libstdc++'s `<meta>` works only
  with GCC's front-end intrinsics. The script reports it as expected-unavailable, not as a failure.

EDG and Windows come later. Any other Compiler Explorer compiler id can still be passed by name.

Short link (all lanes in one project; built from 2.1.13a's headers, 2026-10-01): https://godbolt.org/z/6G9hY5Tox
