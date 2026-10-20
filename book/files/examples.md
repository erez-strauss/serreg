# `examples/` — complete programs

Built with `ES_SERREG_BUILD_EXAMPLES=ON` (the default for a top-level build); each doubles as a smoke test.
Each links only the libraries it uses (decision G, `examples/CMakeLists.txt`): `file_store` and
`user_example1` link `es::serreg`, `print_sinks` links `es::serreg` and `es::serreg_tools` (it reads its two
dictionaries back with `tools::info`), `ipc_ring` and the two `ipc_log` programs link `es::serreg` and
`es::ipc` (which `es::serreg` does not bring), `embedded_cli` links `es::serreg_tools` (the CLI), and the
`dictionary_deltas` host and its two plugins link the shared `es::serreg` (the example needs
`ES_SERREG_BUILD_SHARED`).

Every example has a minimal `main()`: it at most parses the command line and calls an application
class (`file_store::app{path}.run()`, `app::program::run(dir)` in `print_sinks`, `ipc_ring::app{}.run()`, `ipc_log::producer_app`/`ipc_log::consumer_app`
after `ipc_log::parse()`, `embedded_cli::application` with `parse()` and `run()`,
`dictionary_deltas::app{dictionary_deltas::parse(argc, argv)}.run()`, `ue1::writer`,
`ue1::typed_reader`, `ue1::generic_reader`, `ue1::hybrid_reader`), and all output uses
`std::print`/`std::println` — no `printf`/`puts` family.

## `examples/file_store.cpp` — write, verify and dump a capture

```sh
build/gcc16-release/examples/file_store /tmp/orders.ser
# /tmp/orders.ser: 5 frames, 10 records, 192 bytes, ok
# dictionary  program_hash 0x…  (3 types, 4 literals, 3 calls)
# …
```

Walk-through:

`main()` is `return file_store::app{argc > 1 ? argv[1] : "file_store.ser"}.run();`; `app::run()` does the work:

1. `capture_file out(path)` — opening writes the FILE header, the dictionary snapshot, the CHAIN and the
   writer-info frame (format 1.7); failure is sticky (`is_open()`), never an exception.
2. Three `serialize(out, "new order"_lit, ns::Order{…})` calls, then `serialize(out, "cancel"_lit,
   ns::Cancel{2, 7}, "user"_lit)` and `serialize(out, "eod"_lit)` — five frames, ten records (four of them
   literals that cost no bytes).
3. `out.close()` is `[[nodiscard]]`: it flushes and syncs; nothing is rewritten — had a `dlopen` registered new
   types in between, a dictionary delta would already precede the first frame that used them. An error means the last frames may not
   have reached the disk.
4. `tools::read_file` → `tools::verify` (frames, records, bytes, problems) → `tools::dump` to text.

ctest `example_file_store` checks the summary line (`5 frames, 10 records, … ok`). Run it with
`ES_SERREG_DUMP_REGISTRY=- ES_SERREG_DUMP_FORMAT=toml` to see the registry printed at exit as well.

## `examples/register_namespace.cpp` — a namespace registered before `main()`

`ES_SERREG_REGISTER_NAMESPACE(app::messages);` once, at namespace scope, after the message headers: every
serialisable class and enum of `app::messages` and `app::messages::v2` is in the registry before `main()` with no
`serialize()` anywhere in the program — the reader-only process, the schema publisher. The example prints the
selection three ways (`selected_type_names<^^app::messages>()`, `name_prefix<"Msg">`, `annotated<message_t>`),
checks `registry::instance().find_type()` for the never-serialised types, and opens and closes an empty
`capture_file` whose dictionary nevertheless lists the five types (`tools::info`). `static_assert(registered_type_count<…> == 5)`
guards the count at compile time. Links `es::serreg` and `es::serreg_tools`.

## `examples/print_sinks.cpp` — `print` / `println` into two sinks, and how the names resolve

`print_sinks [dir]` writes `print_sinks.ser` (a `capture_file`: scope `void`, its opening dictionary is the whole
registry) and `print_sinks.log.ser` (a `scoped_capture_file<log_scope>` opened with `capture_options{.crc =
true}`: its dictionary is the closure of the calls made into sinks of that type, every frame carries a CRC32C
trailer). Three typed messages go to the archive (`Order`, `Fill`, a dynamic `Book`);
four print frames go to the log, each spelled differently on purpose:

- `sr::println(log, "started with {} workers"_, 4)` — qualified through `namespace sr = es::serreg;`, the
  reading most code should use;
- `println(log, "book {} depth {}"_, "AAPL", 3)` — unqualified with **no using-directive**: argument-dependent
  lookup finds `es::serreg::println` through the sink argument; `std::println` is in scope and is not viable (a
  sink is neither a `FILE*` nor a format string), so there is no ambiguity;
- `sr::println<"order {} rejected: {}">(log, …)` — the format string as a template argument;
- `sr::print(log, "px={:.2f} "_, 105.0)` — a print frame (no newline), beside a `std::println` call in the same
  function.

A fifth call, `println(mine, "user sink {}"_, 1)` into a user-defined sink type derived from `vector_sink`,
shows that ADL still finds the library's overload: the associated namespaces of a class include those of its
bases, and the `lit<F>` argument is an `es::serreg` type as well. The program ends by printing both dictionaries'
sizes: the archive's holds 4 types, 7 literals and 8 calls (everything the program serializes anywhere, the log
formats included); the log's holds **0 types, 4 literals, 4 calls** — the four format strings and nothing else.
ctest: `example_print_sinks` (asserts the log dictionary's counts). `srdump print_sinks.log.ser` renders
`started with 4 workers`, `book AAPL depth 3`, … from that dictionary alone.

## `examples/ipc_ring.cpp` — serreg frames through an `es::ipc` SPSC queue between two processes

The example uses the library's SPSC queue, the variable-size `es::ipc::ring` (chapter 11).
`main()` is `return ipc_ring::app{}.run();`. `ipc_ring::app::run()` makes an anonymous shared region of
`ring::bytes_for(g)` bytes for `g = ring::for_capacity(128 * 1024, 64, 1)` — 128 KiB of records in 64-byte units,
what the 1024 slots of 128 bytes were — and creates an SPSC ring in it (failure: one `std::println(stderr, …)`
line, exit 1). It then `fork()`s: the child runs `consume()` and the parent runs the private `app::produce()`. The
mapping is inherited across `fork()`, so both processes see the same queue.

```cpp
const auto g = es::ipc::ring::for_capacity(128 * 1024, 64, 1);
auto mem = es::ipc::region::anonymous(es::ipc::ring::bytes_for(g));
auto q   = es::ipc::ring::create(*mem, g, es::ipc::ring_kind::spsc);
// child:  auto c = es::ipc::consumer::join(*q);
//         c->poll([&](const es::ipc::message& m) { dispatch<ns::Order, ns::Cancel>(m.data, on_frame); });
// parent: auto p = es::ipc::producer::attach(*q); es::ipc::sink out{*p}; serialize(out, "order"_lit, ns::Order{…});
```

The producer serializes 100 000 frames straight into claimed ring records. Three times out of four it writes
`("order"_lit, Order)`, otherwise `("cancel"_lit, Cancel)`. `es::ipc::sink` is a `claiming_sink`, so each frame is
built once, in place, with no intermediate buffer, allocation or lock. When the queue is full, `claim()` spins until
the consumer frees room. The consumer `poll()`s, yielding when nothing is there: each record's payload (`m.data`)
is handed over in place and passed to `dispatch<ns::Order, ns::Cancel>(m.data, overloaded{…})`, which reads it
without copying because the payload is 16-aligned. Literals are counted through the `literal_view` overload. The
child prints `orders=75000 cancels=25000 literals=100000 total_qty=…` (flushed before `_Exit`), and the parent's exit
status is the child's. ctest `example_ipc_ring` (timeout 120 s). For two unrelated programs, replace the anonymous
region with `region::create("orders.q", ring::bytes_for(g))` + `ring::create(region, g, ring_kind::spsc)` in one
program and `region::open("orders.q")` + `ring::attach(region, ring_kind::spsc)` in the other; the rest of the code
stays the same.

## `examples/ipc_log/` — binary logging between two programs, the dictionary in band

Two separate programs share a named region holding an `es::ipc` SPSC queue of variable-size records
(`examples/ipc_log/README.md` has the full design):

- **`ipc_log_producer --expect FILE [--name REGION] [--rounds N] [--linger-ms MS]`** logs with `println`:
  102 distinct format strings over scalars, its own flat and dynamic structs (`std::string`, `std::vector` members)
  and enums, one large batch message per round (100–240 KB) between small ones. It sends its registry image
  (`registry::instance().export_image()`) **in band** as the first record, then the frames through `es::ipc::sink`,
  then an end record with the frame count; it writes the reference text to `--expect`. The ring capacity is computed
  at run time as at least twice the larger of the dictionary and the largest frame (`frame_size`), rounded up by
  `ring::for_capacity(need, 64, 1)` (512 KiB for 3 rounds), so the queue wraps several times per run and large
  records are preceded by skip records.
- **`ipc_log_consumer [--name REGION] [--out FILE] [--timeout-ms MS]`** is compiled with **one** of the producer's
  types, `ipc_log::Fill` (`messages.hpp`). A `frame_router` decodes `Fill` frames with a typed handler; every other
  frame is rendered from the in-band dictionary with `tools::render_frame`. It never sees the producer's format
  literals or other types. A deliberately foreign frame (a flipped call hash) prints `unknown call 0x…`; the ring
  refuses inconsistent records and counts them in `corrupt()`.

Record types (the `type` field of each ring record): 1 the dictionary (RRI), 2 one es::serreg frame, 3 the end (a
`u64` frame count). Either program may start first: the consumer retries `region::open` and `ring::attach`, joins
and unlinks the name; the producer lingers until the name is gone. Each `main()` parses its options
(`ipc_log::parse`; usage error: exit 2) and runs `ipc_log::producer_app` or `ipc_log::consumer_app`. The ctest
`example_ipc_log` (`run_ipc_log.cmake`, timeout 120 s) starts both at once under a random region name and passes on
`ipc_log: identical` when the consumer's text equals the producer's reference byte for byte (316 frames for 3
rounds).

The Compiler Explorer version is `ce-examples/ce-example5/`: one process, two threads over an anonymous region,
the typed path only (Compiler Explorer cannot link libserreg's renderer); see `ce-examples/README.md`.

## `examples/dictionary_deltas/` — a capture whose dictionary grows while it is written

A host and two plugins (`examples/dictionary_deltas/README.md` has the byte map of the file a run produces):

- **`dictionary_deltas_host <out.ser> <plugin.so>... [--frames N]`** opens a `capture_file` (checkpoints every 96
  bytes of frames, each with a full dictionary, `info_host = false` and a fixed clock, so the file is reproducible),
  writes `N` frames of its own `feed::Order` (`messages.hpp`), then `dlopen()`s each plugin, calls
  `registry::instance().refresh()` and the plugin's `plugin_write(capture_file*, n)`. A plugin's type, literal and
  call enter the registry at load time, after the opening dictionary, so `capture_file` writes a dictionary delta
  frame before the plugin's first frame and patches the previous dictionary's forward link (the opening block's CHAIN,
  then the previous delta's tail link). After `close()` the host prints `tools::to_text(tools::info(path))` — the
  chains by seeks — and a summary line (`3 dictionary blocks (the opening one + 2 delta frames), 4 checkpoints`).
- **`plugin_tick.cpp`** (`plugin_tick::Tick`, `"tick"_lit`) and **`plugin_quote.cpp`** (`plugin_quote::Quote`,
  `"quote"_lit`) are `MODULE` libraries with hidden visibility; `plugin_write` is exported explicitly.

The ctest `example_dictionary_deltas` (`run_dictionary_deltas.cmake`, timeout 120 s) runs the host with both
plugins, requires `srdump --info` to report `3 block(s)` in the dictionary chain and a checkpoint chain, and
`srdump --verify` to accept the file (`ok: 9 frames, 18 records, 336 bytes`). The bytes a run writes are checked
in as `tests/fixtures/corpus/capture/dictionary_deltas-1.7.ser` (a fuzz seed and a `python_reader_parity` input);
`example_dictionary_deltas_fixture_verify`, `_fixture_json` (the merged dictionary names `plugin_quote::Quote` and
`plugin_tick::Tick`) and `_fixture_info` read that copy with `srdump`. Skipped without `ES_SERREG_BUILD_SHARED`.

## `examples/embedded_cli.cpp` — the srdump command line inside an application

```cpp
int main(const int argc, char** const argv)
{
    embedded_cli::application program;
    if (const int rc = program.parse(argc, argv))
        return rc;
    return program.run();
}
```

(`embedded_cli.cpp:110-116`.) The srdump surface lives in the class `embedded_cli::application`. Its
members are `cli::options options_` and `const cli::app me_{.name = "embedded_cli", .version = "0.1",
.extra_usage = …, .on_extra = [this](…) { return on_extra(a, rest); }}`; because `me_.on_extra` points back at the
object, the class is not copyable. `parse(argc, argv)` is `cli::parse(argc, argv, options_, me_)` (0 to go on,
else the exit code). `run()` prints a `--verbose` line with `std::println(stderr, …)`, then, when nothing for the
srdump surface was requested (`nothing_for_srdump()`: no capture, `--stdin`, `--elf`, `--rri`, `--diff`, `--help`
or `--version`), does the application's own default action (`dump_own_registry()`), and otherwise returns
`cli::execute(options_, me_)`.

`--verbose` is a flag, `--selftest <file>` consumes a value (`rest = rest.subspan(1)`) and writes three
`serialize<"tick">(f, app::Tick{…})` frames; every srdump option works under the application's name;
with no arguments the program prints its own registry. `embedded_cli --wireshark app.lua` with no
capture, `--dict`, `--elf` or `--rri` writes a Wireshark dissector for the program's own registry
(`files/src-cli.cpp.md`). ctests: `example_embedded_cli_version` (`embedded_cli 0.1 (format 1.7, hash_algo 2)`),
`example_embedded_cli_selftest` (`--trace --selftest x.ser --verify x.ser` → `ok: 3 frames, 6 records`),
`example_embedded_cli_default` (prints `app::Tick`), `example_embedded_cli_wireshark` (`--wireshark -`
must print `the registry of embedded_cli` and a `serreg.app_Tick.*` `ProtoField`).

## `examples/user_example1/` — writer, and three ways to read

A self-contained user-style example: `types.hpp`, `args.hpp`, `writer.cpp`, `reader1.cpp`, `reader2.cpp`,
`reader3.cpp`, `tree_printer.hpp`, `CMakeLists.txt`. It builds inside the tree (added by `examples/CMakeLists.txt`),
or standalone against an installed ser-reg:

```sh
cmake -S examples/user_example1 -B build-ue1 -G Ninja -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build-ue1 && cd build-ue1 && ./writer && ./reader1 && ./reader2 && ./reader3
```

The readers use the library's reader API: no chain of trial `deserialize` calls, no hand-written comparison, no
mapped-or-blocks branch in any reader, one argument parser, and no private copy of srdump's scalar rendering.

Each program's `main()` constructs its class and calls `run()`: `ue1::writer{path}`, and
`ue1::typed_reader`, `ue1::generic_reader`, `ue1::hybrid_reader`, each from
`ue1::reader_options::parse(argc, argv)`; the default path is `user_example1.ser`.

- **`types.hpp`**: the data model. It contains a `Side` enum; the flat structs `Fill`, `Limit` and `Market`;
  `Order`, with `std::string symbol`, `std::variant<Limit, Market> type`, `std::vector<Fill> fills` and
  `std::optional<std::string> note`; and `Book`, with `std::string venue`, a `double fee_bps[3]` and
  `std::vector<Order> orders`. There are no macros and no registration. `Fill`, `Limit`, `Market`,
  `Order` and `Book` have a defaulted `operator==` (for reader1's check, not for the library). `sample_book()`
  provides data both sides agree on.
- **`args.hpp`**: `ue1::reader_options { std::string path = "user_example1.ser"; tools::file_options file;
  static reader_options parse(argc, argv); }` — the command line of all three readers, `readerN [--blocks|--mapped]
  [file]`. The library picks the reading strategy from `file.blocks`: `true` by default (every reader over a path
  works in windows of 10 MiB), so `--blocks` changes nothing — the `_blocks` ctests still run the readers with it —
  and `--mapped` sets it `false` to map the whole file.
- **`writer.cpp`** writes, through a `capture_file`: one log line
  (`println("session {} {} opened with {} orders"_, venue, date, count)`), the whole book as one frame
  (`serialize(out, book)`), each order again tagged `"order"_` with a log line per fill
  (`"  fill #{} of order {}: {} @ {:.2f}"_`), and a closing line. Nine data frames in total.
- **`reader1.cpp`** is typed, with the same structs, built on a `frame_router` (`files/include-es-serreg-router.hpp.md`)
  set up in the `typed_reader` constructor: `on([this](const demo::Book& b) {…})` for `serialize(out, book)`,
  `on("order"_, [this](const demo::Order& o) {…})`, `on_println("  fill #{} of order {}: {} @ {:.2f}"_,
  [this](std::uint64_t exec_id, std::uint64_t order_id, std::int32_t qty, double px) {…})`, and `otherwise(…)`,
  which renders the other log lines with `tools::render_frame`. `run()` is one call,
  `tools::for_each_frame(opts_.path, router_.callback(), opts_.file)` — in windows of 10 MiB (mapped with
  `--mapped`).
  The book is compared with `book == demo::sample_book()`; the summary requires no walk problem, `router_.failed()
  == 0`, one book, three orders and three fills.
- **`reader2.cpp`** is generic. It includes no application header and calls `tools::walk_values(opts_.path, out_,
  on_frame, opts_.file)` — the path overload without compiled types — with the `tree_printer` visitor.
  Every value is printed from the file's dictionary: nested structs, the C array, vectors, optionals (`null`),
  variants (`variant<0:demo::Limit>`), enum names; `on_frame` prints each frame header and renders log lines.
- **`reader3.cpp`** is hybrid: `tools::walk_values<demo::Order>(opts_.path, on_order, rest_, on_frame,
  opts_.file)`. The `Book` record is walked generically, and each `Order` inside it, as well as each `"order"_`
  record, is delivered as a `demo::Order` object. It reports 6 typed orders. It accepts `--blocks` and `--mapped`
  too.
- **`tree_printer.hpp`**: the `decoded_value_visitor` of reader2 and reader3, one indented line per leaf value. Its
  `scalar` callback prints `tools::value_text(k, bytes)` and the kind name.

ctests: `user_example1_writer` (fixture setup), `user_example1_reader1`, `_reader1_blocks`, `_reader2`,
`_reader2_blocks`, `_reader3`, `_reader3_blocks`, each checking its summary line (`reader1: 1 book, 3
orders, 3 fills, 2 other log lines, matches the writer`, `reader2: 9 frames, 28 records, …, 0 errors`, `reader3: 6
typed orders (6 fills inside them)`). `scripts/run-all-tests.sh` also builds the directory standalone against the
fresh install and runs all four programs.
