# ce-example5: es::serreg over es::ipc, two threads, on Compiler Explorer

This example is one process with two threads. They share an **anonymous** shared-memory region
(`es::ipc::region::anonymous`) that holds a variable-size **SPSC** ring from `es::ipc`. It is the first
Compiler Explorer build of `es::ipc` and `es::ipc`, and the first build of them with clang.

**Producer thread.** It serializes 1000 messages directly into ring records through `es::ipc::sink`, with
no copy. The messages are:

- 799 `println` frames. Their arguments are scalars, a struct (`Fill`), or a runtime string of 2 to 1500
  bytes;
- 200 plain struct frames (`"tick"_`, `Tick`);
- **one large message** at #500: `"batch"_` with a `Batch`, which holds a `std::vector<std::int32_t>` of 30000
  samples. The frame is 120 KB.

After the messages it writes an end-of-stream record.

**Consumer thread.** It decodes each record in place with a `frame_router` of typed handlers (`on_println`,
`on`), formats one line of text per message, and prints it.

**Self-check.** The expected text is built separately from the same message generator. The program exits with 1
in any of these cases:

- the text differs;
- a message is lost or reordered (each message carries its sequence number);
- a frame has no handler or does not decode;
- the stream does not end within 20 s.

Otherwise it exits with 0 and prints `ok: 1000 messages, ...`.

**Typed path only.** Compiler Explorer cannot link libserreg. `ce_registry_stub.hpp` replaces the registry, and
everything used here is header-only: serialize, the print frames, `frame_router` and the typed reader. The
dictionary-driven renderer `tools::render_frame`, which renders messages the reader was not compiled with, lives
in `src/formatters.cpp`. That path is shown by **`examples/ipc_log`**, two programs that pass the dictionary in
band.

**Design.**

- Ring record types: 2 is one es::serreg frame, 3 is the end of stream (a `u64` message count).
- Sizing: the ring holds at least twice the largest frame, measured with `frame_size`, and
  `ring::for_capacity` rounds it up to a power-of-two number of units. Here that is a 256 KB ring for a
  120 KB frame. The queue wraps several times.
- Back-off: the producer uses `full_policy::fail` and retries a refused `serialize` (`sink_full`) after
  `std::this_thread::yield()`. The consumer yields when `poll()` finds nothing. Compiler Explorer may give the
  program one CPU. Locally the program also passes under `taskset -c 0`.

| File | What it is |
|---|---|
| `main.cpp` | The program |
| `ce_registry_stub.hpp` | Symlink to `../ce_registry_stub.hpp` |
| `es/serreg/*.hpp` (16), `es/serreg/detail/{blob_bytes,embed_elf,embed_none}.hpp`, `es/ipc/*.hpp` (8) | Relative symlinks into `include/`. The set is derived with `g++ -MM`, plus `detail/embed_none.hpp` |
| `ce-run.py` | Symlink to `../ce-run.py` |

## Run

```sh
g++ -std=c++26 -freflection -O1 -I. main.cpp -o ce5 && ./ce5
./ce-run.py --short-link --project "serreg ipc threads (ce-example5)"
```

## Results (2.1.13a headers, 2026-10-02)

| Lane | Result |
|---|---|
| local GCC 16.2.1 | **ok**: 1000 messages, 32031 bytes of text identical. Also passes with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` and under `taskset -c 0` |
| `gcc` (CE g162) | **ok**: 1000 messages, identical, exit 0 |
| `clang-libcxx` (CE clang_bb_p2996 + libc++) | **ok**: 1000 messages, identical, exit 0. `es::ipc` and `es::ipc` compile and run under clang + libc++ |
| `clang-libstdcxx` | unavailable, as expected (see `ce-run.py`) |

Short link (all lanes in one project): https://godbolt.org/z/YGGo1h4Mq
