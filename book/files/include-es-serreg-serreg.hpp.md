# `include/es/serreg/serreg.hpp`

The umbrella header: it includes twenty-two of the other public headers in alphabetical order and carries the
library's shortest complete example in its doc comment. It defines nothing itself. Including it gives every
layer at once — compile-time reflection (`reflect.hpp`, `blob.hpp`), wire format (`format.hpp`, `hash.hpp`,
`literal.hpp`, `arg.hpp`, `descriptor.hpp`, `error.hpp`), registry (`registry.hpp`), serialisation
(`serialize.hpp`, `sink.hpp`, `capture.hpp`, `print.hpp`), readers (`deserialize.hpp`, `dictionary.hpp`,
`projection.hpp` and `router.hpp`), the reader limits (`limits.hpp`), tools (`tools.hpp`, `walk.hpp`), the logger (`log.hpp`) and
the version string (`version.hpp`). `to_string.hpp` is deliberately *not* included: it links
`es::serreg_tools`, and the umbrella must stay usable with `es::serreg` alone. Because `reflect.hpp` is among them, a TU that includes `serreg.hpp` **must be compiled with
`-std=c++26 -freflection`** (GCC 16+) or `-freflection-latest -stdlib=libc++` (clang-p2996); the CMake target
`es::serreg` adds the flag PUBLIC, together with `-fno-gnu-unique`.

## Depends on / used by

Includes, in this order (`serreg.hpp:12-33`): `arg.hpp`, `blob.hpp`, `capture.hpp`, `descriptor.hpp`,
`deserialize.hpp`, `dictionary.hpp`, `error.hpp`, `format.hpp`, `hash.hpp`, `limits.hpp`
(`decode_limits`), `literal.hpp`, `log.hpp` (`logger`, `level`, dictionary scopes), `print.hpp`,
`projection.hpp` (`renamed_from`, `tools::project`, `projection_context`, `deserialize(dict, …)`), `reflect.hpp`, `registry.hpp`, `router.hpp` (`frame_router`), `serialize.hpp`, `sink.hpp`, `tools.hpp`,
`version.hpp` (`library_version`), `walk.hpp` (`tools::walk_values`). Every header is self-contained
(`#pragma once`, includes what it uses), so the alphabetical order carries no dependency meaning.

Included by: `examples/embedded_cli.cpp`, `examples/file_store.cpp`, `examples/ipc_ring.cpp`,
`examples/user_example1/*` (reader1 uses `frame_router` from it);
`tests/golden/corpus.hpp` (hence all golden tests); `tests/link/*.cpp`; `tests/dlopen/*.cpp`;
`tests/negative/*.cpp`; `tests/probe/depth4.cpp`; `tests/unit/signal_dump_test.cpp`. The library's own sources
(`src/*.cpp`) never include it — they include `tools.hpp`, `dictionary.hpp`, `format.hpp`, `error.hpp`,
`registry.hpp` individually, and compile without reflection except `registry.cpp`, which needs `registry.hpp`
(and so `blob.hpp`/`reflect.hpp`) for the `registry` class definition.

## What it defines

Nothing. The doc comment (`serreg.hpp:1-10`) is the canonical two-minute tour:

```cpp
#include <es/serreg/serreg.hpp>
using namespace es::serreg::literals;
struct Order { std::uint64_t id; std::int64_t px; std::uint32_t qty; char sym[8]; };

std::vector<std::byte> buf; es::serreg::vector_sink out{buf};
es::serreg::serialize(out, "new order"_lit, Order{42, 1050000, 100, "AAPL"});   // Order is now in the registry
Order o; es::serreg::deserialize(buf, "new order"_lit, o);
es::serreg::registry::instance().export_image("registry.rri");                  // or srdump --elf a.out
```

The comment "Order is now in the registry" is literally true at static-initialisation time of that TU's image,
before `main()` — see `registry.hpp`.

## How it works

### The dependency graph the umbrella flattens

Reading the headers bottom-up (each includes only those below it):

```
hash.hpp  error.hpp  descriptor.hpp  format.hpp  limits.hpp   no library includes; no reflection (limits: 2.1.16)
literal.hpp        (descriptor, format, hash)               no reflection
reflect.hpp        (descriptor, hash)                       <meta>: THE reflection header
arg.hpp            (format, hash, literal, reflect)
detail/blob_bytes.hpp (arg, descriptor, format, literal, reflect)   2.1.13: split out of blob.hpp
detail/embed_*.hpp (blob_bytes, literal)                    the executable-format part (2.1.8)
blob.hpp           (descriptor, blob_bytes, hash, literal, reflect, embed_*)
sink.hpp           (error)                                  no reflection; <unistd.h>
dictionary.hpp     (descriptor, error, format)              no reflection; implemented in libserreg
tools.hpp          (dictionary, error, limits)              no reflection; implemented in libserreg
registry.hpp       (blob, error)                            class registry implemented in libserreg
serialize.hpp      (arg, error, format, registry, sink)
deserialize.hpp    (dictionary, error, format, registry, serialize)
print.hpp          (serialize)
capture.hpp        (format, registry, serialize, sink)
walk.hpp           (deserialize, reflect, tools)             tools::walk_values (2.0.6)
projection.hpp     (arg, deserialize, dictionary, reflect)   schema evolution level 1: renamed_from, tools::project
router.hpp         (deserialize, print, tools)               frame_router (2.1.10)
serreg.hpp         (all of the above)
```

Two facts follow. First, the headers that `libserreg`'s readers and tools need (`format`, `descriptor`, `error`,
`dictionary`, `tools`, `hash`) form a reflection-free island: `srdump` and an embedder that only *reads* captures
from other programs can be built without `-freflection` by including those directly. Second, anything that
mentions a user type (`serialize`, `deserialize`, `dispatch`, `view`, `descriptor_v`, `schema_hash_v`, `lit<S>`
with registration) pulls in `reflect.hpp` and therefore the flag.

### What linking against `es::serreg` adds

The header-only part covers hashing, descriptors, blobs, the writer paths and the typed readers. The compiled
library provides: the one `registry` per process (`registry.cpp`: heap entries, published index, unload and reload
handling, `ES_SERREG_*` hooks), the image backend (`image_elf.cpp` — image identity — or `image_none.cpp`),
`dyn::dictionary` (`dictionary.cpp`), the ELF section reader and `extract_embedded_dictionary` (`elf_reader.cpp`),
the file helpers (`file_io.cpp`), the capture walker and verifier (`reader.cpp`), the descriptor-driven
decoder (`decoder.cpp`), and text/JSON/TOML/print rendering (`formatters.cpp`). `diff.cpp`, `cli.cpp`, the Wireshark dissector generator
(`wireshark.cpp`) and the C++ generator (`cxxgen.cpp`) are a second library (decision G),
`es::serreg_tools` (`libserreg_tools`), which links `es::serreg` PUBLIC; a program that calls `cli::run`,
`tools::diff` or `tools::wireshark_dissector` links `es::serreg_tools`, everything else only `es::serreg`. The umbrella
includes no `es/ipc` header; `es::ipc` is a separate library (`files/build-system.md`).
Even a program that never calls a
`tools::` function needs the library for `registry::instance()`, which `serialize` reaches through the guards.
`ES_SERREG_BUILD_SHARED=ON` (default) is required when `dlopen`ed DSOs serialise — a static `libserreg` linked into
both a host and a plugin yields two registries (`docs/deviations.md`).

### Namespaces and macros an includer sees

- `es::serreg` — everything public; `es::serreg::literals` (inline) — the two UDLs; `es::serreg::fmt` — wire
  constants, `cursor`, `writer`; `es::serreg::dyn` — the runtime dictionary model; `es::serreg::tools` — services;
  `es::serreg::cli` — the command line as a library; `es::serreg::detail` — internals (used by the golden tests,
  otherwise not API).
- Macros: `ES_SERREG_HIDDEN` (`descriptor.hpp`), `ES_SERREG_DICT_SECTION` (`detail/embed_elf.hpp`;
  absent with `ES_SERREG_EMBED_NONE`), `ES_SERREG_API`
  (`registry.hpp`; expands to default visibility under `ES_SERREG_SHARED`), `ES_SERREG_LIT(s)` (`literal.hpp`).
  Build-time defines: `ES_SERREG_SHARED` (PUBLIC on the shared target), `ES_SERREG_BUILDING` (private to the
  library), `ES_SERREG_EMBED_NONE` (PUBLIC with `ES_SERREG_IMAGE_FORMAT=none`; `blob.hpp` then includes
  `detail/embed_none.hpp` and no `es_serreg_dict` section is emitted), and no reflection shim: nothing in `include/` tests an `ES_SERREG_REFLECTION_SHIM` define.
- Environment variables honoured at run time by the compiled part: `ES_SERREG_DUMP_REGISTRY`,
  `ES_SERREG_DUMP_REGISTRY_FD`, `ES_SERREG_DUMP_FORMAT`, `ES_SERREG_DUMP_ON`, `ES_SERREG_DUMP_EXIT`,
  `ES_SERREG_DUMP_SIGNAL`, `ES_SERREG_CONFLICT` (`src/registry.cpp`). `ES_SERREG_ALLOW_DLCLOSE` is ignored
  (one stderr line at startup).

### Which header for which task

| task | minimal include | needs `-freflection`? | needs `libserreg`? |
|---|---|---|---|
| write frames of your own types | `serialize.hpp` (+ `sink.hpp`, `literal.hpp` via it) | yes | yes (registry) |
| write a `.ser` file / dictionary frames | `capture.hpp` | yes | yes |
| structured logging | `print.hpp` | yes | yes |
| read frames of your own types (`deserialize`, `dispatch`, `view`) | `deserialize.hpp` | yes | yes |
| route a capture's frames to one handler per signature (`frame_router`) | `router.hpp` | yes | yes |
| read an older or newer build's flat frames by member name (`deserialize(dict, …)`, `tools::project`) | `projection.hpp` | yes | yes |
| walk values typed where compiled, generic elsewhere (`walk_values`) | `walk.hpp` | yes | yes |
| read anyone's capture with a dictionary (`verify`, `dump`, `visit_record`) | `tools.hpp` | no | yes |
| parse an RRI / ELF section into a `dyn::dictionary` | `dictionary.hpp` + `tools.hpp` | no | yes |
| embed the `srdump` CLI | `tools.hpp` (`cli::run`) | no | yes, `libserreg_tools` |
| compute a schema hash / canonical name at compile time | `reflect.hpp` | yes | no |
| hash bytes / build canonical strings | `hash.hpp` | no | no |
| wire constants, `cursor`/`writer` | `format.hpp` | no | no |
| error codes | `error.hpp` | no | no |
| everything | `serreg.hpp` | yes | yes |

"needs `libserreg`" is about *linking*: the header-only writer paths still reach `registry::instance()` through the
guards, so a program that serialises anything links the library even if it never names `registry`.

### The two version numbers

Nothing in the umbrella states a version, and that is deliberate: the *library* version is
`library_version` (`version.hpp`, also reachable as `tools::library_version`; matched against the `project()`
version by a unit test) and the *wire* version is `fmt::format_major`.`fmt::format_minor` (`1.7`). They move
independently — many releases change no byte, and a wire-format change bumps the minor — and `srdump --version`
prints both. A maintainer bumping either runs `scripts/bump-version.sh` (`files/build-system.md`), or edits
`format.hpp` + fixtures + canary + `CHANGELOG.md`; the umbrella never changes for a version bump.

### Compile cost

Including the umbrella instantiates nothing by itself; cost accrues per type on first `serialize`/`deserialize`
(canonical strings, blobs built twice, gap tables, and for dynamic types the layout and the three walks). A TU
that only reads (`tools::`) pays the parse of `<meta>` and `<format>` for nothing — include `tools.hpp` directly
in such TUs when build time matters.

## Maintaining it

- **Add a public header → add its include here**, alphabetically, and mention it in `AGENTS.md` §5's repository
  map and `README.md`. The umbrella is what the examples, link tests and negative tests compile, so a header that
  does not compile standalone shows up there first; keep every header self-sufficient regardless.
- Keep the doc-comment example compiling as written: it is copied into `README.md` and is the smoke test a
  reviewer runs by eye. If `vector_sink`, `serialize`, `deserialize`, `export_image(path)` or the `_lit` UDL
  change signature, change the comment.
- The header must not gain definitions: things defined in an umbrella cannot be used by a TU that includes only
  one layer, and `src/*.cpp` deliberately do not include it.
- Do not add `using namespace es::serreg::literals;` (or any using-directive) to the header — the UDLs are opt-in
  by design, and `operator""_` in particular must not leak into every includer.
- Reflection gate: `CMakeLists.txt` refuses GCC < 16 and any compiler that does not accept the reflection flag; a
  Clang without P2996 cannot include this header even for reader-only code — point such users at the
  reflection-free island above instead of weakening the gate.
- Tests that exercise the umbrella as such: `negative_control_compiles` (`tests/negative/neg_control_compiles.cpp`
  must compile with everything included), `compile_probe_depth4` (`tests/probe/`), `link_two_tu_dedup` and
  friends (`tests/link/` — two TUs including `serreg.hpp` share one registry entry per entity), the examples
  (`examples/CMakeLists.txt`), and every golden test through `tests/golden/corpus.hpp`.
