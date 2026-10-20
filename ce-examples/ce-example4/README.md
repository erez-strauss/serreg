# ce-example4: dynamic members in frames, and dispatch, on Compiler Explorer

It covers:
- frames whose records are canonical images of dynamic types (`std::string`, `std::vector`, `std::optional`,
  `std::variant`, a flat base inside a dynamic class), in compact and full records and as a `println` argument,
  byte for byte against `tests/fixtures/golden/dynamic.ser`;
- typed reads of those frames;
- **reading GCC's golden frame with whatever compiler builds this file** (cross-compiler interchange);
- `dispatch<Ts...>` over six types, which takes the compile-time perfect-hash path.

| File | What it is |
|---|---|
| `main.cpp` | Own file. Corpus types copied verbatim from `tests/golden/corpus.hpp`; the 4 golden frames embedded as hex |
| `ce_registry_stub.hpp` | Symlink to `../ce_registry_stub.hpp` |
| `es/serreg/*.hpp`, `es/serreg/detail/*.hpp` | Relative symlinks into `include/` |
| `ce-run.py` | Symlink to `../ce-run.py` |

## Results (2.1.10, 2026-09-30; unchanged with 2.1.13a's headers, 2026-10-01) (10 checks)

| Lane | Result |
|---|---|
| local GCC 16.2.1 | **10/10 ok** |
| `gcc` (CE g162) | **10/10 ok** |
| `clang-libcxx` | **8/10**. `frame 1 (DynNested, DynArr)` differs **only in bytes 8–15, the call hash** (clang `0x6c2102e25a4471af`, GCC `0x594f3763afbcef17`); the other 240 bytes, all the canonical-image data, are identical. So `read GCC's golden frame 1 here` fails with `hash_mismatch` |
| `clang-libstdcxx` | unavailable (see `ce-run.py`) |

**Meaning.** The data is portable between the compilers. Only the identity is not: `DynArr` contains
`std::variant<std::monostate, …>`, and the canonical name of `std::monostate` is library-specific
(`std::__1::monostate` in libc++). That name feeds `DynArr`'s schema hash, and through it the call hash. Fixing
the names (DECISIONS P-3, the maintainer's "properties template" idea) fixes interchange with no change to
any byte of data.

Short link (all lanes in one project; built from 2.1.13a's headers, 2026-10-01): https://godbolt.org/z/e47xafc5K
