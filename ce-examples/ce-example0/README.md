# ce-example0: slide 17, "C++26 reflection in one slide"

The one P2996 program of the talk that needs no library: `^^T` (a reflection), `[: m :]` (a splice) and
`template for` (an expansion statement) over `nonstatic_data_members_of`, printing each member's name, offset and
value. It is here so the slide has a link, and so the three lanes of `ce-run.py` can show the same output from GCC 16
and clang-p2996. No `es/` headers, no `ce_registry_stub.hpp`.

Expected output:
```
Quote: 4 members, 32 bytes
id @0 = 42
px @8 = 105.25
qty @16 = 100
sym @20 = AAPL
```

Run on Compiler Explorer: `./ce-run.py -v . gcc` (or `--no-run --short-link --project "serreg slide 17" . gcc` for the
link). Locally, as the ctest `ce_example0_builds_and_runs` does:
`g++-16 -std=c++26 -freflection -O1 -W -Wall -Wextra -Wshadow -Werror -I. main.cpp && ./a.out`.
