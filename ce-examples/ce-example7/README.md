# ce-example7: binary logging, formatted by the reader, on Compiler Explorer

Slide 12 of the Core C++ 2026 talk (`docs/talk/presentation-slides-content.md`): the writer formats nothing. This
example is the logger of `log.hpp` in one translation unit — three `ES_SERREG_LOG` lines (`info`, `warn`,
`error`; a scalar, a string and a `corpus::Order` as arguments) into a real `capture_file("app.log.ser")` — and then
what the slide can only claim:

1. **the bytes of one log frame** (the `warn` line, 40 bytes): the 16-byte frame header, the 8-byte timestamp, the
   string record (`u32` length + `"AAPL"`), the `int` record — and that the level, the source file, the line, the
   envelope mask and the format string are **constants of the call ENTRY** in the dictionary: 0 bytes per line
   (`n_records 8`, three of them with bytes);
2. **the compile-time floor**: the file sets `ES_SERREG_LOG_MIN_LEVEL` to `info`, so a `trace` line with a
   side-effecting argument (`costly()`) compiles to nothing — no call ENTRY, no code — and the macro's `enabled() &&`
   never evaluates the argument: the counter stays 0;
3. **the typed read back** of each frame through the compiled log signature (`detail::deserialize_frame<log_tag,
   constant<level>, lit<file>, constant<line>, constant<mask>, u64, lit<format>, args…>` — what
   `frame_router::on_log` does with the dictionary instead): a wrong level, file, line or format is a call-hash
   mismatch, never a wrong line;
4. **the dictionary-driven read back**: `inspect()` resolves the call hash to its ENTRY and hands out one record per
   argument, the constants and literals included; the example renders the line the way `srdump --level` prints it
   (`<ns> warn  main.cpp:174 book AAPL depth 7`), with nothing of the writer's types in that code path.

The last line, `log frames: 3 written, 3 read back, trace argument evaluated 0 times`, is what the ctest
`ce_example7_builds_and_runs` matches. The real `srdump --level warn` output of the same frames — the dictionary
rendering lives in `libserreg_tools`, which Compiler Explorer does not link — is pasted as a comment block at the
end of `main.cpp`, produced with `build/gcc16-debug/srdump` on the file of a local run.

The logger uses `dictionary_policy::full` (the whole registry is the file's dictionary): the scoped dictionary of
a `scoped_capture_file<Tag>` is computed by `registry::export_image_scope()` in `src/registry.cpp`, which
`ce_registry_stub.hpp` does not carry. The frames are the same bytes either way. If the executor's sandbox has no
writable directory the same frames go through the stream form (`write_dictionary_frame` into a `vector_sink`,
then the logger over that sink) and the dump says so.

| File | What it is |
|---|---|
| `main.cpp` | Own file |
| `ce_registry_stub.hpp` | Symlink to `../ce_registry_stub.hpp` |
| `es/serreg/*.hpp` (20) | Relative symlinks into `include/` (`g++ -MM` set; `log.hpp`, `print.hpp` beyond ce-example6's) |
| `ce-run.py` | Symlink to `../ce-run.py` |

Run on Compiler Explorer: `./ce-run.py -v . gcc` (lane `g162`, `-std=c++26 -freflection -O1`); the shareable
multi-file project (to mint, from this directory):
`./ce-run.py --no-run --short-link --project "serreg binary logging" . gcc` (short links are permanent and public —
record it in `docs/ce-links.md`). Locally, as the ctest does:
`g++-16 -std=c++26 -freflection -O1 -W -Wall -Wextra -Wshadow -Werror -I. main.cpp && ./a.out`; keep the file for
`srdump` by commenting out the `std::remove(path)` line.
