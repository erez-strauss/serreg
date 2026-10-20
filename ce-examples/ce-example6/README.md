# ce-example6: the talk's "thirty seconds", on Compiler Explorer

Slide 8 of the Core C++ 2026 talk (`docs/talk/presentation-slides-content.md`) is a program of a dozen lines:
open a `capture_file("orders.ser")`, `serialize(file, "new order"_, Order{…})`, close. This example **is that
program** — `write_orders()` is the slide verbatim — followed by what the slide cannot show: the file is read
back and taken apart with nothing but the layouts of `format.hpp` and the dictionary the file carries.

What it prints, in order:

1. the registry size before `main()` (4 entries: the struct, the enum, the literal, the call signature);
2. the **FILE header** (48 B): magic, format 1.7, flags, program hash, `dict_offset`, `dict_len`, ABI tag;
3. the **RRI header** (40 B) and the **four ENTRY blobs** with their tag and length — the call signature (56 B),
   the literal (40 B), `corpus::Order` (264 B), `corpus::Side` (88 B) — then the CHAIN;
4. the **frames**: the writer-info frame, then the one call frame — 16-byte header annotated (`frame_len` 48,
   `n_records` 2: the literal is a record of 0 bytes, the call hash) and the 32 bytes of `Order`; and that the
   call hash equals `call_hash_v<lit<"new order">, Order>`;
5. a **decode that uses only the dictionary record** (`type_desc::fields`: name, kind, offset, size) — what
   `srdump`, the Python reader and the Wireshark dissector do, with no `Order` in that code path;
6. the **typed read back** with `deserialize(frame, "new order"_, back)`; the exit code is the comparison.

The file it writes is a real capture: run locally, `srdump orders.ser` / `srdump --verify orders.ser` read it
(the example deletes it after reading it back). If the executor's sandbox has no writable directory, the same
frames go through the **stream form** — `write_dictionary_frame(vector_sink)` then `serialize` — and the dump
starts at the dictionary frame instead of the FILE header; everything else is identical.

How this works in one translation unit: `ce_registry_stub.hpp` stands in for `libserreg` — the registrar guards
fill it before `main()` exactly as they fill the real registry, and since 2.2.5 it also implements
`registry::export_image_since()` and `program_hash()` (30 lines, the same bytes as `src/registry.cpp`), which is
all `capture_file`, `write_dictionary_frame` and `dictionary_tracker` need. Everything else of the writer is
header-only.

| File | What it is |
|---|---|
| `main.cpp` | Own file |
| `ce_registry_stub.hpp` | Symlink to `../ce_registry_stub.hpp` |
| `es/serreg/*.hpp` (17) | Relative symlinks into `include/` (`g++ -MM` set) |
| `ce-run.py` | Symlink to `../ce-run.py` |

Run on Compiler Explorer: `./ce-run.py -v . gcc` (lane `g162`, `-std=c++26 -freflection -O1`); a shareable
multi-file project: `./ce-run.py -v --short-link --project "serreg thirty seconds" . gcc` (short links are permanent
and public — record it in `docs/ce-links.md`). Locally, as the ctest `ce_example6_builds_and_runs` does:
`g++-16 -std=c++26 -freflection -O1 -W -Wall -Wextra -Wshadow -Werror -I. main.cpp && ./a.out`.
