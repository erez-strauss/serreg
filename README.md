# ser-reg — `es::serreg`

THIS PROJECT IS wORK iN pROGRESS, BE AWARE, TEST IT BEFORE USE.
      
You Help and Contribution to the project are welcome and appreciated.


[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://gcc.gnu.org/projects/cxx-status.html)
[![GitHub stars](https://img.shields.io/github/stars/erez-strauss/serreg?style=flat)](https://github.com/erez-strauss/serreg/stargazers)

Reflection-driven binary serialization for C++26 with a **load-time type registry**.

A self-describing capture and structured-log format: the specification (`format.hpp`) and the dictionary carried
in every file let any reader on any host decode it — `srdump`, the pure-Python reader and the generated Wireshark
dissector do, with no headers; the writer's native layout makes the typed read a `memcpy` on the same ABI (Linux
x86-64 today). One trust domain: readers never crash on wrong data within documented limits, but hashes and CRCs
are integrity checks, not authentication.

| | Who writes the schema | What a flat struct costs | A reader without your headers |
|---|---|---|---|
| IDL serializers (Protobuf, FlatBuffers, Cap'n Proto, SBE) | you, in the IDL; a generator emits the C++ | a field-by-field encode, or a generated accessor layout you must adopt | the IDL file, and a decoder built from it |
| Macro/intrusive serializers (cereal, Boost.Serialization) | you, once per type, in the type | a member-by-member visit | nothing: the archive is opaque without the code |
| `memcpy` of a POD | nobody; the struct is the schema | one copy | nothing: bytes without a description |
| **this library** | the C++ type itself, through P2996 reflection | one copy after a padding scrub | the dictionary in the file |

- `serialize(sink, "new order"_lit, order)` — one call, one frame, `memcpy`-speed for flat types; the
  same call takes classes with `std::string` (any allocator, e.g. `std::pmr::string`) / `std::vector` / `std::optional` / `std::variant` members — and
  `std::map` / `std::unordered_map` / `std::flat_map` / `std::set` / `std::unordered_set` / `std::flat_set` members and any of
  these containers as an argument of its own — encoded as a canonical image (identical values ⇒ identical bytes),
  and `print("px={:.2f}"_, px)` is binary structured logging formatted by the reader.
- Every type (and every string literal) you ever pass to `serialize()` is described by **C++26 reflection
  (P2996)** at compile time and **registered before `main()`** — no macros, no manual registration, no code
  generation. `dlopen`'ed libraries add their types when they load.
- Stable **64-bit hashes** per type (schema & layout), per call signature, per program.
- The dictionary travels with the data (file header / dictionary frames), can be **exported** from a running
  process (`ES_SERREG_DUMP_REGISTRY_FD=…`, signal, API), and a copy is **embedded in every binary** in the ELF
  section `es_serreg_dict`, so `srdump --elf ./your-app` prints the schema of any program built with it.
- `srdump` decodes any capture with only the dictionary — it knows nothing about your types.
- Zero dependencies (Catch2 for tests only). Headers + `libserreg` (static or shared).

**Documentation: [`book/README.md`](book/README.md)** — from the first program to the loader internals, every
byte of the wire format, and one document per source file and executable.

Design documents: [`docs/ser-reg-requirements.md`](docs/ser-reg-requirements.md) (requirements),
[`docs/ser-reg-architecture.md`](docs/ser-reg-architecture.md) (architecture and byte layouts),
[`docs/DECISIONS.md`](docs/DECISIONS.md) (the decision ledger).
Deviations of this release from them: [`docs/deviations.md`](docs/deviations.md). Captures in Wireshark:
[`docs/wireshark.md`](docs/wireshark.md).

## Requirements

| | |
|---|---|
| Compiler | **GCC 16.x** (`-std=c++26 -freflection`). Bloomberg [clang-p2996](https://github.com/bloomberg/clang-p2996) is the intended cross-check compiler (`-freflection-latest -stdlib=libc++`); upstream Clang 22/23 do not implement P2996 yet. |
| Build | CMake ≥ 4.0, Ninja recommended |
| Platform | Linux x86-64, ELF64, little-endian |
| binutils | ≥ 2.36 for `[[gnu::retain]]` (the embedded dictionary section survives `--gc-sections`) |

## 30-second example

```cpp
#include <es/serreg/serreg.hpp>
using namespace es::serreg;
using namespace es::serreg::literals;

namespace ex {
enum class Side : std::uint8_t { buy = 1, sell = 2 };
struct Hdr   { std::uint32_t seq; std::uint16_t len; };
struct Order : Hdr {
  std::uint64_t id; std::int64_t px; Side side; char sym[8];
  [[=since(2)]] std::uint32_t venue;          // schema metadata, carried in the dictionary
  [[=skip]]     std::uint32_t cache;          // never serialised (scrubbed to zero on the wire)
};
}

int main() {
  // The registry already knows ex::Order, ex::Hdr, ex::Side, the literal and the call signature here —
  // registration happened during static initialisation, as a side effect of instantiating serialize() below.
  std::println("{} entries", registry::instance().size());

  capture_file file("orders.ser");                              // header + dictionary written now
  if (!file.is_open()) return 1;                                // file.state().error() says why
  ex::Order o{{7, 40}, 42, 1050000, ex::Side::sell, "AAPL", 3, 0};
  if (!serialize(file, "new order"_lit, o)) return 1;           // one compact frame: 16-byte header + id + 48 bytes
  if (!file.close()) return 1;                                  // final checkpoint + fdatasync; nothing is rewritten

  auto bytes = tools::read_file("orders.ser").value();
  std::string text; tools::dump(bytes, nullptr, {}, text).value();
  std::print("{}", text);                                       // what `srdump orders.ser` prints
}
```

Literals — the identity must be in the type (or a template argument), which is what lets the registry see it
before `main()`; four spellings, one literal (`FR-L1`), and only the 8-byte id crosses the wire:

```cpp
using namespace es::serreg::literals;
serialize(out, "order rejected"_lit, o);              // UDL
serialize(out, "order rejected"_, o);                 // short UDL
serialize<"order rejected">(out, o);                  // literal as the leading template argument
constexpr es::serreg::lit<"order rejected"> k_rejected;  serialize(out, k_rejected, o);   // named constant
```

The text is stored once per image in the ENTRY blob (plus the TU-local copy inside the `es_serreg_dict` section
that offline readers need); `lit<S>::text()` and the registry view it there. A
literal costs **no bytes** in a compact frame: the call hash already names it (FR-L6).

Arguments are not only structs: scalars and runtime strings are records too, and
`print` is binary structured logging — the format string is a literal, the arguments are records, and
the *reader* runs `std::format`:

```cpp
serialize(out, "abc", 42, 1.5, "evt"_);                        // string (u32 len + bytes), i32, f64, literal (0 bytes)
std::string s; int i; double d;  deserialize(frame, s, i, d, "evt"_);   // or std::string_view for a zero-copy view

print  (out, "hello {} {} {}"_, "new", 100, "worlds"_);     // 32-byte frame; nothing formatted here
println(out, "px={:.2f} qty={} side={}"_, px, qty, side);  // checked at compile time by std::format_string
// `namespace sr = es::serreg; sr::println(out, …)` reads like std::println; unqualified `println(out, …)` is
// found by ADL through the sink and never collides with std::println (a sink is not a FILE* or a format string).
// (examples/print_sinks.cpp shows the spellings side by side.)
tools::render_frame(frame, dictionary);   // → "px=2.50 qty=7 side=sell\n"   (srdump prints this by default; --raw for records)
```

`"new"` (no suffix) is a runtime string — its bytes travel; `"worlds"_` is a literal — only its id does. Enums
render as their enumerator name, structs as a one-line text form; a placeholder/argument mismatch is a compile
error at the call site.

`_` does for numbers what it does for strings: a **compile-time constant** is part of the
call signature and occupies no bytes in the frame.

```cpp
serialize(out, "fill"_, 42_, order, -7_, 1.5_, k<Side::sell>);   // 16-byte header + the Order: four constants, 0 bytes
println(out, "limit={} on={}"_, 5'000'000'000_, k<true>);   // a 16-byte frame; srdump: limit=5000000000 on=true
Order o;  deserialize(frame, "fill"_, 42_, o, -7_, 1.5_, k<Side::sell>);   // the reader names the same constants
```

`42_` is an `int` (else `long long`, else `unsigned long long`), `1.5_` a `double`; `k<V>` spells the rest (`k<true>`,
`k<'x'>`, `k<42u>`, `k<Side::buy>`). The type and the value are in the call hash (`const:i32=42`), the value is
stored once in the call's dictionary entry, and every reader shows it as the scalar or enum it stands for.

Dynamic members — no macros, no registration, the same `serialize()`:

```cpp
struct Msg { std::uint32_t id; std::string name; std::vector<std::int16_t> xs; };
struct Nested { std::vector<Msg> msgs; std::optional<Order> maybe; std::variant<int, Order, std::string> v; };
serialize(out, Msg{7, "ab", {1, -2, 3}});      // 16-byte header + u32 28 + u32 0 + a 28-byte canonical image
Nested back;  deserialize(frame, back);        // vectors resized, optionals engaged/reset, variants emplaced
```

The image is a fixed part (members at canonical offsets, every `string`/`vector`/`optional`/`variant` an 8-byte
`{u32 off, u32 n}` slot) plus a tail the slots point into — `srdump` decodes it with only the dictionary, and
`Msg{7, "ab", {1,-2,3}}` is always exactly
`07 00 00 00 | 14 00 00 00 02 00 00 00 | 16 00 00 00 03 00 00 00 | 61 62 | 01 00 fe ff 03 00`.

Maps, sets and containers as arguments:

```cpp
struct Book { std::map<std::string, double> px; std::unordered_map<int, Order> live; std::set<std::uint16_t> ids; };
serialize(out, book);                          // every map/set: an 8-byte slot → n entries (key, then value), sorted by key
std::vector<int> ids{3, 1, 2};
serialize(out, "ids"_, ids);                   // a container is an argument too: the record of a transparent type
std::vector<int> back;  deserialize(frame, "ids"_, back);     // typed read; srdump prints `ids: [3, 1, 2]`
```

A `std::map`, `std::unordered_map` and `std::flat_map` of the same `K, V` are the one wire shape `map<K,V>` (a
member's kindspec, like `string` for every `std::basic_string<char>`), so a reader may decode the shape into any of
the three; the entries are written in key order whatever the container (an `unordered_map` is sorted by the key's
`operator<` — `std::strong_order` for a floating-point key — or by its bytes when it has none), so identical values
are identical bytes; `serialize_options{.canonical_unordered = false}` writes the iteration order instead. The
dictionary tells a map from a vector of pairs (`{a: 1, b: 2}` in `srdump`, `[{"key": …, "value": …}]` in JSON).
The `std` vocabulary is `string`, `vector`, `deque`, `list`, `forward_list`, `array`, `optional`, `variant`,
`pair`, `tuple`, `map`, `unordered_map`, `flat_map`, `multimap`, `unordered_multimap`, `set`, `unordered_set`,
`flat_set`, `stack`; everything else is refused with a diagnostic that names the rule and the alternative —
`std::bitset`, `std::expected`, `std::priority_queue`, `std::chrono::*`, `std::complex`, `std::vector<bool>`,
`std::string_view`, smart pointers, raw pointers, references, unions, polymorphic classes, virtual bases,
bit-fields, recursive types (`as_flat<T>` or `type_properties<T>` is the escape hatch).

Reading back, five ways (spec §6.4):

```cpp
ex::Order o2;  deserialize(frame, "new order"_lit, o2);          // 1. typed: validates hash + lengths first; on failure a
                                                                 //    decode_error{code, expected, actual, failed}:
                                                                 //    items expected (frame + arguments), decoded, first failed
auto fv = inspect(frame);                                        // 2. what is in this buffer? (lazy range, zero copies)
for (record_view r : fv->records()) …                            //    inspect(frame, &dictionary) for foreign captures
dispatch<ex::Order, ex::Cancel>(frame, overloaded{               // 3. compile-time dispatch per record
    [](const ex::Order& x) { … }, [](literal_view l) { … } });
tools::visit_record(payload, type, dictionary, visitor);         // 4. dictionary-driven visitor (srdump)
const ex::Order* p = view<ex::Order>(rec).value();               // 5. zero-copy typed view (aligned only)
```

A whole capture, one handler per message kind — the frame's call hash picks the handler, the handler's
parameters are the decoded values:

```cpp
es::serreg::frame_router router;
router.on([&](const ex::Book& b) { … })                               // serialize(out, book)
      .on("new order"_lit, [&](const ex::Order& o) { … })             // serialize(out, "new order"_lit, order)
      .on_println("fill {} @ {:.2f}"_, [&](std::uint64_t id, double px) { … });
tools::for_each_frame("capture.ser", router.callback());             // mapped; {.blocks = true} for pipes/stdin
```

## `srdump`

```
srdump capture.ser                      # dictionary, then every frame decoded
srdump capture.ser --verify             # exit 0 iff all hashes/lengths/frame bounds check out
srdump capture.ser --json | jq …        # dictionary + one JSON object per record
srdump --elf ./your-app                 # the embedded dictionary of any binary (also works on .so files)
srdump --elf ./your-app --rri-out app.rri
srdump --rri app.rri --toml             # an exported registry image
srdump --diff old.rri ./new-app         # identical | names-only | metadata-only | breaking | added | removed
srdump stream.bin --dict app.rri        # bare IPC stream + side-loaded dictionary
srdump --config srdump.toml capture.ser # flags from a TOML file (subset: strings, ints, bools, string arrays)
srdump --version                        # srdump 2.2.6 (format 1.7, hash_algo 2)
srdump capture.ser --program-hash       # 0x-hex program hash (capture header, or recomputed from the dictionary)
srdump --wireshark serreg.lua capture.ser   # a Wireshark dissector for these types (docs/wireshark.md)
srdump --cxx types.hpp --elf ./app      # C++ definitions of the dictionary's types, self-checking (--cxx-router adds a frame_router skeleton)
```

`--verify` decodes every record (not just the frame structure), checks literal ids against the call signature,
and reports schema conflicts between merged dictionaries; any problem is a non-zero exit.

### `file(1)` support

`share/serreg.magic` teaches the `file` command about captures and registry images:

```
$ file -m share/serreg.magic orders.ser registry.rri
orders.ser:   es::serreg capture (FILE container), format 1.7, XXH3-64 hashes, abi ptr8/long8/wchar4, program hash 0xd6bdb39e65bcecb7
registry.rri: es::serreg registry image (RRI), format 1.0, 5 entries, program hash 0x0f563c6a3ce5b324
```

Install it system-wide by appending it to `/etc/magic` (or per user: `cat share/serreg.magic >> ~/.magic`); it is
also installed to `share/serreg/serreg.magic`.

`srdump`'s `main` is one line; everything is in the library (`es::serreg::tools`, `es::serreg::cli`).

### The `srdump` command line inside your own application

`es::serreg::cli` is a library: `parse()` fills a public `cli::options` (after any `--config` file),
`execute()` runs them exactly as `srdump` would, and a `cli::app` gives the whole surface your program's name,
version, extra usage text and extra flags. `examples/embedded_cli.cpp` is the full version of this:

```cpp
int main(int argc, char** argv)
{
    bool verbose = false;
    const es::serreg::cli::app me{
        .name = "myapp", .version = "2.3", .extra_usage = "  --verbose   more output\n",
        .on_extra = [&](std::string_view a, std::span<char* const>& rest) {   // flags cli does not own
            return a == "--verbose" ? (verbose = true) : false;               // consume values via rest = rest.subspan(1)
        }};
    return es::serreg::cli::run(argc, argv, me);          // myapp --elf ./myapp --toml, myapp --verify x.ser, myapp --version …
}
```

### Runtime hooks (library, no code changes)

| Variable | Effect |
|---|---|
| `ES_SERREG_DUMP_REGISTRY=<path\|->` | write the registry (JSON by default) at exit |
| `ES_SERREG_DUMP_REGISTRY_FD=<n>` | write the registry (RRI by default) to an already-open descriptor |
| `ES_SERREG_DUMP_FORMAT=rri\|json\|toml` | output format for the two above |
| `ES_SERREG_DUMP_ON=exit\|change\|both` | when to dump (`change` = after every registration, e.g. each `dlopen`) |
| `ES_SERREG_DUMP_EXIT=1` | exit right after the first dump |
| `ES_SERREG_DUMP_SIGNAL=USR1\|USR2\|RTMIN+n` | install a handler that requests a dump (nothing is installed otherwise) |
| `ES_SERREG_ALLOW_DLCLOSE` | ignored: `dlclose` never aborts, an unloaded plugin's entries are retained (decision A) |
| `ES_SERREG_CONFLICT=warn\|abort\|silent` | what to do when two images define the same type with different layouts (default `warn`: one stderr line) |

## `es::ipc`: shared-memory rings and half-RTT benchmarks

`#include <es/ipc/ipc.hpp>` (CMake target `es::ipc`: header-only C++23, no reflection flags; a library
of its own that `es::serreg` does not link — link `es::ipc` where you use it, or install it alone with
`-DES_SERREG_IPC_ONLY=ON` and `find_package(serreg CONFIG REQUIRED COMPONENTS ipc)`) gives one **variable-size
ring** in two kinds (`ring_kind`): an SPMC **broadcast** (every joined consumer sees every record published after
it joined; the slowest gates the producer; up to 256 readers) and an SPSC **queue** (one always-active gate: records
are kept without a consumer and a new consumer resumes where the last stopped), for threads and processes. A record
is a 16-byte header `{size, seq, type, check}` plus a payload of any size, laid out in units of 16, 32 or 64 bytes;
the geometry (`unit_bytes`, `units`, `max_consumers`, or `ring::for_capacity(bytes)`) is set at run time and read
from the ring header by whoever attaches. Regions are named files on tmpfs or hugetlbfs, or anonymous mappings,
with normal or explicit 2 MiB/1 GiB huge pages and optional `mlock`. Both sides are zero-copy: the producer
`claim()`s a record and `commit()`s it (or `publish`/`emplace` a plain struct), and `es::ipc::sink` is an
`es::serreg` claiming sink, so `serialize()` builds the frame in the record; consumers `peek()`/`poll()` the
record in place (`message{data, type, position}`) and `deserialize`/`dispatch` it there. Wrong data never crashes a
reader (design: `docs/ipc/ipc-design.md`).

```cpp
namespace ipc = es::ipc;
const auto g = ipc::ring::for_capacity(64 << 20, 64, 8);     // 64 MiB of 64-byte units, up to 8 consumers
auto mem  = ipc::region::create("fills", ipc::ring::bytes_for(g), {.pages = ipc::pages::huge_2m});
auto ring = ipc::ring::create(*mem, g, ipc::ring_kind::spmc); // other process: ring::attach(*ipc::region::open("fills"))
auto prod = ipc::producer::attach(*ring);
auto cons = ipc::consumer::join(*ring);
ipc::sink out{*prod};
es::serreg::serialize(out, Fill{...});                        // built in place in the claimed record
cons->poll([](const ipc::message& m) { es::serreg::dispatch<Fill>(m.data, on_fill); }); // read in place
```

Two benchmarks measure the half round-trip time between **two processes over two rings** (ping → pong →
ping, one message in flight). One uses 64-byte raw packets with timestamps, built and read in place. The other
uses the same content as an es::serreg message. Each has a script that builds the benchmark in release, pins two
cores, optionally uses huge pages, and writes a dated report:

```sh
scripts/bench-hrtt-raw.sh                      # → logs/bench-hrtt-raw-YYYYMMDD-HHMMSS.txt
scripts/bench-hrtt-serreg.sh --huge 2m --lock  # → logs/bench-hrtt-serreg-YYYYMMDD-HHMMSS.txt   (--help for options)
```

Chapter 11 of the book covers regions, the rings' protocol and guarantees, and how to read the reports.

## Testing

`scripts/run-all-tests.sh` runs the complete verification protocol — format check; fresh configure/build/ctest
for `gcc16-debug`, `gcc16-release`, `gcc16-static`, `gcc16-static-none` (no ELF dependency),
`gcc16-ubsan-trap` (UBSan without a runtime; run when named), `gcc16-asan-ubsan`, `gcc16-tsan`; `srdump`/`file(1)` checks; the four fuzz targets under ASan/UBSan; repeated concurrency tests under TSan; the release bench;
`cmake --install` plus a `find_package` consumer — and captures every log under
`logs/serreg-tests-YYYYMMDD-HHMMSS/` (`00-summary.txt`, `00-environment.txt`, one log per stage) plus a
`.tar.gz` of the same. `--quick` is the smoke run; `--help` lists the knobs. Sanitizer presets are skipped
(with the reason in the summary) when the chosen compiler has no sanitizer runtime.
`docs/test-and-log-protocol.md` says how to read a run, how to triage each stage, and what must hold before
a release.

## Contributing: formatting and hooks

The code style is `.clang-format` (Microsoft base, 100 columns, `T* p`). Install the git hooks once with
`scripts/install-hooks.sh`; the pre-commit hook refuses commits with staged C++ files that are not
clang-format clean (`scripts/format-all.sh` formats the tree). CI runs the same check.

## Building this repository

```sh
export CXX=/opt/gcc-16/bin/g++-16                  # or -DCMAKE_CXX_COMPILER=…; must be GCC 16+
cmake --preset gcc16-release && cmake --build --preset gcc16-release -j
ctest --preset gcc16-release
# other presets: gcc16-debug, gcc16-asan-ubsan, gcc16-static, gcc16-static-none, clang-p2996, opt-gcc16-debug
# if g++-16 is not the system compiler, run binaries with LD_LIBRARY_PATH=<gcc16>/lib64 (or set -Wl,-rpath)
cmake --install build/gcc16-release --prefix /opt/serreg
```

Options: `ES_SERREG_BUILD_SHARED` (default ON — required when `dlopen`'ed libraries serialise, so that there is
one registry per process), `ES_SERREG_BUILD_TOOLS`, `ES_SERREG_BUILD_TESTS`, `ES_SERREG_BUILD_EXAMPLES`,
`ES_SERREG_WERROR` (off by default; the presets turn it on), `ES_SERREG_CATCH2_DIR` (offline Catch2 checkout), `ES_SERREG_IMAGE_FORMAT` (`elf`, the
default, or `none` — static library only: one image, no unload guard, no embedded `es_serreg_dict` section, so
offline tools need an `.rri`, e.g. from `registry::instance().export_image()`). `none` also drops the
ELF-only build flags (`-fno-gnu-unique`, `-ldl`) from the target and from `serreg.pc`; the preset `gcc16-static-none`
builds and tests it.

## Using it from your project

### CMake `find_package` (after `cmake --install`)

```cmake
find_package(serreg 2.2 CONFIG REQUIRED)     # -DCMAKE_PREFIX_PATH=/opt/serreg (SameMinorVersion: 2.2.x; the
                                             # minor is the compatibility unit until 3.0 — SONAME libserreg.so.2.2)
add_executable(app app.cpp)
target_link_libraries(app PRIVATE es::serreg)   # brings -std=c++26 -freflection -fno-gnu-unique, include dirs, libserreg
```

The exported target carries everything a consumer needs: C++26, the reflection flag, `-fno-gnu-unique` (ELF
builds, so your DSOs stay unloadable and can be reloaded), include directories and the link to `libserreg`.
`srdump` is installed to `bin/`.

The package holds three libraries: `es::serreg` (the runtime: registry, readers, decoder, formatters),
`es::serreg_tools` (srdump's command line `es::serreg::cli`, the Wireshark generator and the dictionary diff — link
it to embed the CLI) and `es::ipc` (header-only, C++23, not linked by the others). `find_package(serreg CONFIG
REQUIRED COMPONENTS ipc serreg tools)` names what you need; without components you get what the installation has.
pkg-config: `serreg`, `serreg-tools`, `es-ipc`.

### CMake `FetchContent` (no install step)

```cmake
include(FetchContent)
FetchContent_Declare(serreg GIT_REPOSITORY https://github.com/erez-strauss/serreg.git GIT_TAG v2.2.6)
set(ES_SERREG_BUILD_TESTS OFF)
FetchContent_MakeAvailable(serreg)
target_link_libraries(app PRIVATE es::serreg)
```

### vcpkg

The repository ships a ready-to-use **overlay port** in [`ports/serreg`](ports/serreg) and is itself a vcpkg
manifest project (`vcpkg.json`). Consume it from another project like this:

1. In your project's `vcpkg.json`:
   ```json
   { "name": "app", "version": "0.1.0", "dependencies": [ "serreg" ] }
   ```
2. Point vcpkg at the overlay (until the port is upstreamed to the vcpkg registry) — either
   `vcpkg-configuration.json`:
   ```json
   { "overlay-ports": [ "path/to/ser-reg/ports" ] }
   ```
   or on the command line / in the preset: `-DVCPKG_OVERLAY_PORTS=path/to/ser-reg/ports`.
3. Configure with the vcpkg toolchain and a GCC 16 triplet (a sample is in [`ports/triplets/x64-linux-gcc16.cmake`](ports/triplets/x64-linux-gcc16.cmake)):
   ```sh
   cmake -B build -S . \
     -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
     -DVCPKG_OVERLAY_PORTS=path/to/ser-reg/ports \
     -DVCPKG_OVERLAY_TRIPLETS=path/to/ser-reg/ports/triplets -DVCPKG_TARGET_TRIPLET=x64-linux-gcc16
   ```
4. `find_package(serreg CONFIG REQUIRED)` + `target_link_libraries(app PRIVATE es::serreg)` as above.

The port builds the shared library by default (`ES_SERREG_BUILD_SHARED` follows `VCPKG_LIBRARY_LINKAGE`),
installs headers, the CMake config and `srdump` (feature `tools`, on by default). To publish a release for the
port, tag the repository and update `REF`/`SHA512` in `ports/serreg/portfile.cmake` (`vcpkg hash <tarball>`),
or keep using the local source directory as the port does now.

### Packaging a release tarball

```sh
scripts/make-release.sh --out ..            # ../ser-reg-project-v<version>.tar.gz (checks CHANGELOG and format)
```

## Wire format in one screen

```
FILE    "ESSR" | u16 major,minor (1.7) | u32 flags | u32 hash_algo (2) | u64 program_hash | u64 dict_offset | u64 dict_len | u64 abi_tag  (48 B)
        RRI (dictionary) | pad8 | CHAIN (u64 next_dictionary, u64 first_checkpoint; 1.7) | FRAME* — types registered
        after the file was opened go into dictionary frames (deltas, forward-linked); nothing is rewritten at close
        (1.6 files may carry a rewritten dictionary at the end, flag bit 2: read, never written)
FRAME   u32 frame_len | u16 flags | u16 n_records | u64 call_hash   (16 B)   one ID per frame, records 8-aligned
        flags: bit0 full records, bit1 dictionary frame, 1.7 opt-in: bit2 CRC32C trailer (last 8 B), bit3 checkpoint
        frame (seek/resync points, "ESSRCKPT"), bit4 writer-info frame ("ESSRINFO": the options the file was written with)
        compact (default): raw object bytes (padding zeroed) / literal: nothing / scalar bytes / u32 len + string bytes
                           / dynamic type: u32 len | u32 0 | canonical image (a container argument: the slot + its tail)
        full   (opt-in):   u64 hash | u32 len | u16 layout | u16 rsv | payload   per record (a literal: u64 id)
RRI     "ESRG" | u16,u16 | u32 flags | u32 hash_algo | u32 n_entries | u64 program_hash | u32 rsv | u64 abi_tag (40 B) | ENTRY*
ENTRY   u32 total_len | u8 tag(struct|enum|literal|call) | u8 ver | u16 rsv | body          (see include/es/serreg/format.hpp)
```

Hashes are XXH3-64 (`hash_algo` 2) over canonical description strings (they are
**not** collision-resistant against an adversary), e.g.
`struct<ex::Order>[48,8]{<base:ex::Hdr>:struct<0x…>@0[8,4];id:u64@8[8,8];…;venue:u32@36[4,4;s2;d0];}` (schema, with
names and `since`/`deprecated`) and `struct[48,8]{struct<L:0x…>@0[8,4];u64@8[8,8];…}` (layout, names stripped). The
strings contain no compiler spellings (template-argument values render as `3`, `true`, `120`, `enum(7)`), so the
same source yields the same hash on every conforming compiler **for the same ABI**: offsets, padding and the width
of `long` are part of the layout, so hashes are ABI-portable, not source-portable across x86-64/aarch64/ILP32. The
ABI tag in the FILE and RRI headers records the writer's ABI. A class nested in a class template and user templates
over standard-library types get implementation-independent names too (decision B).

## Layout

```
include/es/serreg/   reflect (the only reflection user) descriptor blob hash literal arg print registry sink serialize
                     deserialize router capture walk format error dictionary tools serreg (umbrella) detail/embed_{elf,none}
include/es/ipc/      common layout region ring producer consumer sink ipc (umbrella) — header-only, no reflection
src/                 registry image_{elf,none} dictionary elf_reader file_io reader decoder formatters diff cli wireshark
tools/srdump/        main.cpp (one line)
tests/               golden (frozen hashes + frozen blob bytes + fixtures) unit link dlopen negative canary fuzz bench probe
                     wireshark support
examples/            file_store ipc_ring (es::ipc SPSC queue across fork) embedded_cli (cli::app)
                     user_example1/ (writer + typed / generic / hybrid readers; builds standalone with find_package)
                     ipc_log/ (two programs, the dictionary in band) dictionary_deltas/ (dlopen'ed plugins: dictionary
                     delta frames, the CHAIN, checkpoints with full dictionaries)
ports/               vcpkg overlay port + GCC 16 triplet
share/               serreg.magic (file(1))
scripts/             run-all-tests.sh (full verification run, dated logs) bench-hrtt-raw.sh bench-hrtt-serreg.sh
                     (half-RTT benchmarks, dated reports) make-release.sh bump-version.sh check-book.py pre-commit install-hooks.sh format-all.sh
book/                the book: user guide, wire format byte by byte, loader internals, one document per source file
docs/                DECISIONS.md ser-reg-{requirements,architecture,testing,stories}.md deviations.md
                     test-and-log-protocol.md ce-links.md wireshark.md ipc/ (history: ser-reg-requirements-v3.md plans-for-*.md)
```

## Status

2.2.5 — wire format 1.7 (readers accept 1.6 and 1.7), identities XXH3-64 (`hash_algo` 2). Linux x86-64, GCC 16 with `-freflection`;
clang-p2996 + libc++ has reproduced the golden corpus on Compiler Explorer (a rerun against the current tree is on
the maintainer's list).

- **Types:** flat types (arithmetic, enums, arrays, nested classes including private members and bases) and dynamic
  members (`std::string`, `std::vector`, `std::optional`, `std::variant`, `std::map` /
  `std::unordered_map` / `std::flat_map` / `std::set` / `std::unordered_set` / `std::flat_set`, at any depth) as
  canonical images; any of those containers as an argument of its own (a transparent type); literals,
  compile-time constants (`42_`, `k<V>`), scalar and string arguments, `print` / `println`.
  Canonical names are injective within a program and the same on every compiler (decision B); enums without a
  fixed underlying type take their kind from their enumerators (decision D).
- **Registry:** every serialisable type and call registered before `main()` and at `dlopen` (or with
  `register_types<Ts...>()` or `ES_SERREG_REGISTER_NAMESPACE(ns[, selector])` where nothing serialises them); the main program's entries are viewed in place, other
  images' are copied to the heap at load, so `dlclose` never aborts and plugins can be reloaded; conflict reports
  and change notification; one published index per registration burst; scoped dictionaries per sink type;
  `tests/bench/registry_measure` for the budgets (10k entries in 10 ms, 100k in 100 ms).
- **Logging:** `logger<log_options, Sink>` writes log lines as print frames with a zero-byte envelope
  (level, file:line, policies as constants of the call); `ES_SERREG_LOG` evaluates nothing when the level is off;
  `srdump --level/--since/--until`, `frame_router::on_log`. `to_string`/`to_json`/`formattable<T>` render any
  serialisable value from the dictionary.
- **Readers:** `deserialize` (with `decode_error`: expected, actual, first failed item), `dispatch`,
  `frame_router`, typed and generic capture walks over memory, mapped files or 10 MiB windows of a file of any
  size; the never-crash contract (FR-D5) with `decode_limits`; a stand-alone pure-Python reader (`python/serreg`,
  parity-tested against `srdump --json`).
- **Containers and dictionaries:** compact and full frames, file and stream captures, dictionaries embedded in a
  capture (format 1.7: a forward-linked chain of deltas, never rewritten; opt-in CRC32C per frame, checkpoint
  frames, `--resync`, a writer-info frame read by `srdump --info`), in the `es_serreg_dict` ELF section, or in RRI
  files; an image backend without ELF (`ES_SERREG_IMAGE_FORMAT=none`).
- **Tools:** `srdump` (dump, verify, JSON, ELF, RRI, `--diff --direction`, `--cxx` C++ generation, config, Wireshark
  dissector generation) on `es::serreg_tools`; `es::ipc` variable-size shared-memory rings with half-RTT
  benchmarks, a library of its own.

Next (`docs/DECISIONS.md`, G-15): 2.3.0 is schema evolution level 1 in both directions (projection by member name,
widening, `renamed_from`; R-9), 2.2.x patch releases stay on format 1.7, then the public contract of 3.0 (portable
wire mode, additive-only format, a second compiler lane in CI, Windows). The current documents are
`docs/DECISIONS.md`, `docs/ser-reg-requirements.md`, `docs/ser-reg-architecture.md`, `docs/ser-reg-testing.md`,
`docs/ser-reg-stories.md` and the generated rebuild prompt (`scripts/make-rebuild-prompt.py`).
Other targets than x86-64 are not supported yet (the ABI tag in the FILE and RRI headers records the writer's
target). `CHANGELOG.md` has the history of every release.


## Contributing

`CONTRIBUTING.md`: ways to help (ports to other compilers, CPUs and libraries, new sinks and renderers,
readers in other languages, measurements, the book), the properties every change keeps (C++26, no warnings, no
dependencies, the wire format as a contract, nothing on the hot path allocates), how a change goes in, and the
standing of the IPC component.

If the library is useful to you, a star on GitHub helps others find it; an issue saying what you used it for
helps decide what comes next.

## License

MIT (`LICENSE`), with the xxHash notice in `docs/THIRD-PARTY-NOTICES.md`. The license may change if the library
is submitted to a target that requires another (Boost: BSL-1.0; Beman: Apache-2.0 WITH LLVM-exception) while
there is one copyright holder — DECISIONS G-7.
