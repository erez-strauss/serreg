# ipc_log: binary logging between two programs over es::ipc

Two separate programs share a named shared-memory region holding an `es::ipc` SPSC queue. Records in the
queue have variable sizes.

- `ipc_log_producer` logs with `println`. It uses about 100 distinct format literals, its own structs
  (flat ones, and ones with `std::string` and `std::vector` members) and enums. In each round it also sends one
  large message, interleaved with small ones.
- `ipc_log_consumer` is compiled with **one** of the producer's types: `ipc_log::Fill` in `messages.hpp`. It
  decodes that type with a typed handler. It renders everything else from the producer's **dictionary**, which
  the producer sends **in band** as the first record of the queue. The consumer never sees the producer's format
  literals or types.

| File | What it is |
|---|---|
| `messages.hpp` | The part both programs share: the record types, `Fill`, and `fill_text()` |
| `producer.cpp` | Builds the ring, sends the dictionary, the frames and the end record, and writes the reference text (`--expect`) |
| `consumer.cpp` | Attaches to the ring, routes `Fill` to its handler, renders the other frames from the dictionary, and checks the frame count |
| `run_ipc_log.cmake` | The ctest `example_ipc_log`: starts both programs at once and compares the two texts byte for byte |

## Run

```sh
cmake --build build/gcc16-debug --target ipc_log_producer ipc_log_consumer
ctest --test-dir build/gcc16-debug -R example_ipc_log --output-on-failure

# by hand, in either order (each waits for the other)
build/gcc16-debug/examples/ipc_log/ipc_log_producer --name mylog --expect expected.txt &
build/gcc16-debug/examples/ipc_log/ipc_log_consumer --name mylog > actual.txt
cmp expected.txt actual.txt
```

Producer options:

- `--expect FILE`: the reference text (required);
- `--name REGION`: the region in `/dev/shm`. Default `serreg_ipc_log`;
- `--rounds N`: default 3;
- `--linger-ms MS`: how long to wait for a consumer. Default 20000.

Consumer options:

- `--name REGION`;
- `--out FILE`: default stdout;
- `--timeout-ms MS`: how long to wait for the region. Default 20000.

Both programs print a status line on stderr, for example:

```
ipc_log_producer: dictionary 14856 bytes, largest frame 240056 bytes, ring 524288 bytes (8192 units)
ipc_log_producer: sent 316 frames
ipc_log_consumer: 316 frames (9 typed, 306 rendered, 1 unknown, 0 undecodable), dictionary 14856 bytes, corrupt records 0
```

## Design notes

**Record types.** These are `es::ipc` message types, the `type` field of each ring record. They are
defined in `messages.hpp`.

| Type | Payload | Consumer |
|---|---|---|
| 1 `record_dictionary` | The producer's `registry::instance().export_image()`: an RRI with every type, call signature and literal of the program | `tools::load_dictionary_rri` |
| 2 `record_frame` | One es::serreg frame, header included, serialized in place into the record by `es::ipc::sink` | `frame_router::route`, then `tools::render_frame` |
| 3 `record_end` | A `u64`: the number of type-2 records sent | Checked against the count received; then the consumer exits |

The dictionary is sent once, before any frame. The producer's registry is complete at static initialization, and
this program never calls `dlopen`. A producer that loads plugins would send a new type-1 record whenever
`registry::version()` changes, as `dictionary_tracker` does for streams.

**Sizing rule.** Both sizes are known only at run time. The ring capacity is at least
`max(2 × (dictionary + 64), 2 × (largest frame + 64))` bytes, rounded up to a power-of-two number of 64-byte units
by `es::ipc::ring::for_capacity(need, 64, 1)`. The largest frame is measured exactly with
`es::serreg::frame_size(options, values...)`. With 3 rounds the numbers are:

- dictionary: 14.5 KB;
- largest frame: 240 KB (a batch of 60000 samples);
- ring: 512 KB.

The queue wraps several times per run: one round is about 300 KB of records. A record that does not fit before
the end of the buffer is preceded by a skip record. A record larger than the free space waits until the consumer
has drained enough (`full_policy::spin`).

**Partial knowledge.** The consumer's `frame_router` has one handler: `on("fill"_, [](const Fill&) ...)`. A frame
with that call hash is decoded into `Fill` with no dictionary involved. `route()` returns `false` for every other
frame, and the consumer renders it with `tools::render_frame(frame, dictionary)`. Rendering is the reader-side
`std::format` of the format literal from the dictionary:

- scalars render as themselves;
- strings and literals render as their text;
- enums render as the enumerator name;
- a struct argument renders as its one-line text form, such as
  `app::Session{id: 7, user: "alice", roles: ["admin", "ops", "r0"]}`. In that form strings are quoted, an enum
  member is `Type::name (value)`, and a vector is `[a, b, ...]`.

The producer writes the same text with `std::format` and small `text_of()` helpers into the reference file. The
test therefore checks the renderer against an independent formatting of the same values.

**Unknown frames never crash the consumer.** The producer deliberately sends one *foreign* frame: a valid frame
whose call hash it flipped, so that it is in no dictionary. This stands for a newer producer's message.

- `render_frame` returns `unknown_call`, and the consumer prints `unknown call 0x<hash>`.
- Any other failure prints `undecodable frame: <error>`.
- The ring itself refuses records with an inconsistent header and counts them in `corrupt()`.

**Starting and stopping.**

1. The producer creates the region (`es::ipc::region::create`) and the ring.
2. The consumer retries `region::open` and `ring::attach` until both succeed. Before that they fail with
   `no_such_file_or_directory` or `resource_unavailable_try_again`.
3. The consumer joins the queue and then **unlinks the name**.
4. The producer exits only after the name is gone, or after `--linger-ms`, so a consumer that starts late still
   finds the queue.

An SPSC queue keeps every record until it is read, so nothing is lost whichever program starts first. The ctest
uses a random region name, so parallel runs do not collide.

**Message mix.** Each round sends:

- 22 hand-written formats, including the round markers and the batch lines: scalars of every kind, format specs
  (fill, align, `#x`, `+.1f`), escaped braces, positional arguments, a runtime string, literals, enums, flat and
  dynamic structs, and a struct with a padded spec;
- 80 generated formats, each with a distinct literal text `gen NN/x: ...` built at compile time, in four argument
  shapes;
- 3 `Fill` frames (typed);
- one batch: an `app::Batch` with a `std::vector<std::int32_t>` of 25000, 60000 or 35000 samples, between 100 KB
  and 240 KB.

The run ends with the foreign frame and the end record. In total there are 102 distinct println formats plus the
`Fill` call: 316 frames for 3 rounds, about 900 KB of text.

The Compiler Explorer version, `ce-examples/ce-example5`, is one process with two threads. It shows only the typed
path: the dictionary-driven renderer lives in libserreg (`src/formatters.cpp`), which Compiler Explorer cannot link.
