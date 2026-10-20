# dictionary_deltas: a capture whose dictionary grows while it is written

A capture file's opening dictionary holds what the process registry knows when the file is opened. When the
registry grows afterwards — here because the host `dlopen()`s plugins whose types and call signatures it was
never compiled with — the file must still be self-describing. Format 1.7 does that without rewriting anything:
the frame that first needs the new entries is preceded by a **dictionary delta frame**, and the previous
dictionary is linked to it with one aligned 8-byte `pwrite` (the opening block's CHAIN, then the tail link of the
previous delta frame). A reader collects the whole dictionary by following that chain with seeks, never with a
scan. With checkpoints on, every checkpoint can also carry a **full copy** of the dictionary as of that point,
so a reader that seeks into the middle of a large file needs neither the chain nor the opening block.

| File | What it is |
|---|---|
| `messages.hpp` | The host's types (`feed::Order`, `feed::Side`) and the `plugin_write` signature every plugin exports |
| `host.cpp` | Opens the capture, writes its own frames, loads each plugin and lets it write, closes, prints the layout (`tools::info`) |
| `plugin_tick.cpp` | The first plugin: `plugin_tick::Tick` and the call `("tick"_lit, Tick)`, unknown to the host |
| `plugin_quote.cpp` | The second plugin: `plugin_quote::Quote` and `("quote"_lit, Quote)` |
| `run_dictionary_deltas.cmake` | The ctest `example_dictionary_deltas`: runs the host, then `srdump --info` and `srdump --verify` on the file |

The plugins are `MODULE` libraries linked against the **shared** `libserreg` (one registry per process is what
makes a plugin's entries visible to the host); the example is skipped when `ES_SERREG_BUILD_SHARED` is off.
After each `dlopen` the host calls `registry::instance().refresh()` so the plugin's entries are published on the
calling thread; `capture_file` compares the registry version before every frame (one relaxed load) and writes the
delta when it moved.

## Run

```sh
cmake --build build/gcc16-debug --target dictionary_deltas_host dictionary_deltas_plugin_tick dictionary_deltas_plugin_quote srdump
ctest --test-dir build/gcc16-debug -R example_dictionary_deltas --output-on-failure

# by hand
B=build/gcc16-debug/examples/dictionary_deltas
$B/dictionary_deltas_host deltas.ser $B/dictionary_deltas_plugin_tick.so $B/dictionary_deltas_plugin_quote.so
build/gcc16-debug/srdump deltas.ser --info      # the chains, by seeks
build/gcc16-debug/srdump deltas.ser --verify    # ok: 9 frames, 18 records, 336 bytes
build/gcc16-debug/srdump deltas.ser             # the merged dictionary (4 types), then the frames
```

Options: `--frames N` (frames per phase, default 3). The file is reproducible (no host items in the writer-info
frame, a fixed clock): a run writes the same bytes as the checked-in copy
`tests/fixtures/corpus/capture/dictionary_deltas-1.7.ser`, which the ctests `example_dictionary_deltas_fixture_*`
read with `srdump` and `python_reader_parity` reads with the Python reader.

## The wire layout it produces

With the defaults (3 frames per phase, a checkpoint with a full dictionary every 96 bytes of frames) the file
is 5040 bytes. The host's own types go into the opening dictionary (4 entries: `feed::Side`, `feed::Order`, the
literal `"new order"` and their call); each plugin adds 3 entries (its type, its literal, its call) in a delta
frame. Offsets and sizes from a run:

```
@0     48 bytes  FILE header: format 1.7, flags LE|dict|checkpoints|info, dict_offset 48, dict_len 480
@48   480 bytes  opening dictionary (a full RRI, 4 entries)
@528   16 bytes  CHAIN: next_dictionary = 1408, first_checkpoint = 800     ← both 0 at open, patched later
@544  160 bytes  writer-info frame (library, format, program, abi, the options)
@704   48 bytes  frame 1  ("new order", feed::Order)
@752   48 bytes  frame 2
@800  560 bytes  checkpoint 1: prev 0, next 1752, 2 frames / 96 bytes before, dictionary @48, full RRI (480 bytes)
@1360  48 bytes  frame 3
                 ── dlopen(plugin_tick.so), refresh(): the registry moved ──
@1408 280 bytes  dictionary frame: delta RRI (3 entries: plugin_tick::Tick, "tick", call), next_dictionary = 2560
@1688  32 bytes  frame 4  ("tick", plugin_tick::Tick)
@1720  32 bytes  frame 5
@1752 776 bytes  checkpoint 2: prev 800, next 2944, 5 frames / 208 bytes before, dictionary @1408, full RRI (696 bytes)
@2528  32 bytes  frame 6
                 ── dlopen(plugin_quote.so), refresh() ──
@2560 320 bytes  dictionary frame: delta RRI (3 entries: plugin_quote::Quote, "quote", call), next_dictionary = 0
@2880  32 bytes  frame 7  ("quote", plugin_quote::Quote)
@2912  32 bytes  frame 8
@2944 1032 bytes checkpoint 3: prev 1752, next 4008, 8 frames / 304 bytes before, dictionary @2560, full RRI (952 bytes)
@3976  32 bytes  frame 9
@4008 1032 bytes checkpoint 4 (written by close()): prev 2944, next 0, 9 frames / 336 bytes before, dictionary @2560, full RRI
@5040            end
```

What was patched after it was first written, each with one 8-byte `pwrite`:

- the CHAIN's `next_dictionary` (0 → 1408) when the first delta frame was written, and its `first_checkpoint`
  (0 → 800) at the first checkpoint;
- the first delta frame's tail link (0 → 2560) when the second delta frame was written; the second one's stays 0;
- each checkpoint's `next_checkpoint` when the following checkpoint was written; the last one's stays 0.

`srdump --info` follows exactly these links (`tools::info`: O(links), never O(frames)):

```
format 1.7  program_hash 0xa2ec3e1caf34d6bd  size 5040  flags checkpoints info
dictionary  4 types, 3 literals, 3 calls; 3 block(s) at 48 1408 2560
checkpoints 4
  @800  frames 2  bytes 96  dictionary @48 (full copy)  wall 1700000000002000000
  @1752  frames 5  bytes 208  dictionary @1408 (full copy)  wall 1700000000004000000
  @2944  frames 8  bytes 304  dictionary @2560 (full copy)  wall 1700000000006000000
  @4008  frames 9  bytes 336  dictionary @2560 (full copy)  wall 1700000000008000000
writer      library = 2.2.2
writer      format = 1.7
writer      program = dictionary_deltas
...
```

A reader that only walks frames (`srdump` without `--info`, `--verify`, the Python reader) also sees every
dictionary frame inline, in order, before the first frame that needs it — so a pipe or a truncated copy of the
file is still decodable up to its last complete frame. The checkpoints' full dictionaries are what a seeking
reader uses: the dictionary at `@2560` is the merged result of the opening block and both deltas.

The 1.6 writers back-patched the opening dictionary at close instead (header flag bit 2); 1.7 writers never
rewrite a dictionary (`include/es/serreg/capture.hpp`, `docs/ser-reg-architecture.md`).
