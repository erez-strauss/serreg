# ce-example3: the frame layer on Compiler Explorer

It tests the parts that examples 1 and 2 do not:
- the **frame writer**: the 16-byte header, compact and full records, the padding scrub (an `Order` built with
  `0xAB` padding bytes), 0-byte literals, string and scalar records, and `print`/`println` frames with their
  compile-time `std::format` check;
- **`frame_size`**;
- the **typed readers**: `deserialize`, `deserialize_print/println`, refusal of a wrong signature by the call
  hash, and `inspect` plus the zero-copy `view<T>`.

Every frame written must equal, byte for byte, the corresponding frame of the checked-in golden capture
`tests/fixtures/golden/orders.ser` (written by `tests/golden/make_fixtures.cpp` with GCC 16.2).

| File | What it is |
|---|---|
| `main.cpp` | Own file. Corpus types copied verbatim from `tests/golden/corpus.hpp`; the 8 golden frames embedded as hex |
| `ce_registry_stub.hpp` | Symlink to `../ce_registry_stub.hpp`: a minimal registry (a list with linear lookups) replacing libserreg so the example is one translation unit. Not the project's registry |
| `es/serreg/*.hpp` (14), `es/serreg/detail/embed_{elf,none}.hpp` | Relative symlinks into `include/` |
| `ce-run.py` | Symlink to `../ce-run.py` |

## Run

```sh
g++ -std=c++26 -freflection -O1 -I. main.cpp -o ce3 && ./ce3
./ce-run.py --short-link --project "serreg frames"        # three lanes, plus one shareable project link
```

## Results (2.1.10, 2026-09-30; unchanged with 2.1.13a's headers, 2026-10-01) (19 checks)

| Lane | Result |
|---|---|
| local GCC 16.2.1 | **19/19 ok** |
| `gcc` (CE g162) | **19/19 ok** |
| `clang-libcxx` (CE clang_bb_p2996 + libc++) | **19/19 ok**: every flat and print frame is byte-identical to GCC's |
| `clang-libstdcxx` | unavailable (see `ce-run.py`) |

Short link (all lanes in one project; built from 2.1.13a's headers, 2026-10-01): https://godbolt.org/z/va8d4jG1K
