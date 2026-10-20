# The `es::serreg` book

Documentation of ser-reg **2.2.6** (wire format 1.7; reads 1.6 and 1.7): what the library does, how to use it from the simplest
program to plugins and dynamic members, what every byte on the wire means, and how every file and program in the
tree works — written so that someone new to the code can maintain it.

## Part I — Using the library

| chapter | contents |
|---|---|
| [1. Introduction and terminology](01-introduction-and-terminology.md) | what it is for, the design in one paragraph, the glossary (delta dictionary, CHAIN, CRC32C trailer, checkpoint, writer info, meta frame, scope / scoped sink, log envelope), what is unique about the registry |
| [2. Getting started](02-getting-started.md) | requirements, build/test/install, a first program, what can be serialized |
| [3. The writer API](03-serialization-api.md) | `serialize`, options (`crc`, `canonical_unordered`), sinks (scoped sinks), argument categories (a container — vector, optional, variant, map, set — is an argument of its own; what is still not one gets a named diagnostic), `capture_file` (format 1.7, `capture_options`, `capture_policy`, `scoped_capture_file`), streams, `print`, errors, `to_string`/`to_json`, the logger (§3.9) |
| [4. Reading back](04-reading-back.md) | the five reader styles, walking captures (in memory or by path; in windows, a capture of any size, the reader limits and the never-crash contract, the range check of enums without a fixed underlying type as a compile-time policy; containers as outputs — sequences, pairs and tuples, multimaps, duplicate keys, the map/set visitor events; reading an older or newer build's flat frames by member name, `deserialize(dict, …)` and `tools::project` with `renamed_from` (§4.1d); meta frames, CRC skipping, `tools::info`, `--resync`), `frame_router` (with `on_log` as one line handler or split by level into `tools::log_record`s), which reader when |
| [5. The registry](05-registry.md) | registration by instantiation (`register_types`/`register_call` without a call site, a whole namespace with `ES_SERREG_REGISTER_NAMESPACE`; the main program's entries viewed in place), lock-free reads, runtime export (deltas, `export_image_since`), dictionary scopes, the registry at scale and its budgets (`registry_measure`), `dlopen`/`dlclose` and reload (retained entries), literals |
| [6. Hashes and the dictionary](06-hashes-and-dictionary.md) | the four hashes, canonical strings (the kind of an enum without a fixed underlying type; the `map<K,V>`/`set<K>` kindspecs and the transparent type's name), annotations, `dyn::dictionary` (typed constant values in JSON/TOML; `transparent` and `fixed`), program hash and `--diff` |

## Part II — The bytes

| chapter | contents |
|---|---|
| [7. The wire format, byte by byte](07-wire-format-byte-by-byte.md) | FILE, RRI, every ENTRY kind and every frame kind, dumped from the golden fixtures and annotated byte by byte; format 1.6: the `map` and `set` shapes, the transparent record, the enum flags byte; format 1.7 (§7.2b and §7.9): the CHAIN, the writer-info frame, dictionary deltas and the forward chain, the CRC32C trailer, checkpoint frames, resynchronisation, reading 1.6 files |
| [8. Dynamic members](08-dynamic-members.md) | the canonical-image rule (maps and sets, the canonical order and its `canonical_unordered` opt-out), a worked example, why this design, limits |

## Part III — Maintaining it

| chapter | contents |
|---|---|
| [9. The shared library and the loader](09-shared-library-and-loader.md) | where every entity lives, static initialisation, the `es_serreg_dict` section, image identity, `dlopen`/`dlclose`/reload/exit and what the registry cannot protect, visibility, RPATH, threading, a pre-change checklist |
| [10. `srdump` and the tools layer](10-srdump-and-tools.md) | the CLI (`--window`, `--frame-limit`, `--diff --direction`, `--info`, `--resync`, `--level`, `--since`/`--until`, log rendering), `cli::app`, the `tools::` services, Wireshark dissector generation (format specs, heuristics, the embedded-CLI registry, map/set fields, 1.7 frames, log lines), C++ generation (`--cxx`, container spellings), `file(1)`, design rules, `to_string`, the Python reader, the CE examples as tests |
| [11. `es::ipc`: shared-memory rings and half-RTT benchmarks](11-ipc-and-latency.md) | `es::ipc` as its own library (`COMPONENTS ipc`, `ES_SERREG_IPC_ONLY`), regions (tmpfs/hugetlbfs, anonymous, explicit huge pages, `mlock`), the variable-size rings — SPSC queue and SPMC broadcast over one layout, `claim`/`commit`, `sink`, `peek`/`poll`, `message`, validation — the latency findings, the half-RTT and old-vs-new benchmarks, and the slot rings that preceded it (`es::ipc_old`, out of the build) and why they were replaced |
| [Executables](executables.md) | every program the tree builds: purpose, arguments, environment, exit codes, the ctests that run it (srdump's options, `registry_measure`, `python3 -m serreg`) |
| [Package contents](package-contents.md) | the release tarball (`docs/SECURITY.md`, `python/`, the six flattened documents, the rebuild prompt), versions, what `cmake --install` installs (three libraries in one package; SONAME `major.minor`), how to consume it |
| [The pure-Python reader](python-serreg.md) | `python/serreg` — layout, command line, how it walks and decodes, what it does not do, the parity test |
| [The registry at scale](registry-measure.md) | `tests/bench/registry_measure.cpp` — what it measures, the budgets of DECISIONS G-5, the numbers |
| [The CE examples as tests](ce-examples-tests.md) | `tests/ce/` — the eight Compiler Explorer programs compiled and run by every build |
| [The rebuild prompt](make-rebuild-prompt.md) | `scripts/make-rebuild-prompt.py` — the single file from which the library can be rebuilt |

## Part IV — One document per source file (`files/`)

Public headers — `include/es/serreg/`:
[arg.hpp](files/include-es-serreg-arg.hpp.md) ·
[blob.hpp](files/include-es-serreg-blob.hpp.md) ·
[capture.hpp](files/include-es-serreg-capture.hpp.md) ·
[descriptor.hpp](files/include-es-serreg-descriptor.hpp.md) ·
[deserialize.hpp](files/include-es-serreg-deserialize.hpp.md) ·
[dictionary.hpp](files/include-es-serreg-dictionary.hpp.md) ·
[error.hpp](files/include-es-serreg-error.hpp.md) ·
[format.hpp](files/include-es-serreg-format.hpp.md) ·
[hash.hpp](files/include-es-serreg-hash.hpp.md) ·
[limits.hpp](files/include-es-serreg-limits.hpp.md) ·
[literal.hpp](files/include-es-serreg-literal.hpp.md) ·
[log.hpp](files/include-es-serreg-log.hpp.md) ·
[print.hpp](files/include-es-serreg-print.hpp.md) ·
[projection.hpp](files/include-es-serreg-projection.hpp.md) ·
[reflect.hpp](files/include-es-serreg-reflect.hpp.md) ·
[register_namespace.hpp](files/include-es-serreg-register_namespace.hpp.md) ·
[registry.hpp](files/include-es-serreg-registry.hpp.md) ·
[router.hpp](files/include-es-serreg-router.hpp.md) ·
[serialize.hpp](files/include-es-serreg-serialize.hpp.md) ·
[serreg.hpp](files/include-es-serreg-serreg.hpp.md) ·
[sink.hpp](files/include-es-serreg-sink.hpp.md) ·
[to_string.hpp](files/include-es-serreg-to_string.hpp.md) ·
[tools.hpp](files/include-es-serreg-tools.hpp.md) ·
[version.hpp](files/include-es-serreg-version.hpp.md) ·
[walk.hpp](files/include-es-serreg-walk.hpp.md) ·
[detail/blob_bytes.hpp](files/include-es-serreg-detail-blob_bytes.hpp.md) ·
[detail/embed_elf.hpp](files/include-es-serreg-detail-embed_elf.hpp.md) ·
[detail/embed_none.hpp](files/include-es-serreg-detail-embed_none.hpp.md)

`es::ipc` headers — `include/es/ipc/` (the variable-size rings):
[common.hpp](files/include-es-ipc-common.hpp.md) ·
[region.hpp](files/include-es-ipc-region.hpp.md) ·
[layout.hpp](files/include-es-ipc-layout.hpp.md) ·
[ring.hpp](files/include-es-ipc-ring.hpp.md) ·
[producer.hpp](files/include-es-ipc-producer.hpp.md) ·
[consumer.hpp](files/include-es-ipc-consumer.hpp.md) ·
[sink.hpp](files/include-es-ipc-sink.hpp.md) ·
[ipc.hpp](files/include-es-ipc-ipc.hpp.md)

Library sources — `src/` (`libserreg`; `diff.cpp`, `cli.cpp`, `wireshark.cpp` and `cxxgen.cpp` form `libserreg_tools`):
[registry.cpp](files/src-registry.cpp.md) ·
[image.hpp](files/src-image.hpp.md) ·
[image_elf.cpp](files/src-image_elf.cpp.md) ·
[image_none.cpp](files/src-image_none.cpp.md) ·
[dictionary.cpp](files/src-dictionary.cpp.md) ·
[reader.cpp](files/src-reader.cpp.md) ·
[decoder.cpp](files/src-decoder.cpp.md) ·
[formatters.cpp](files/src-formatters.cpp.md) ·
[diff.cpp](files/src-diff.cpp.md) ·
[cli.cpp](files/src-cli.cpp.md) ·
[elf_reader.cpp](files/src-elf_reader.cpp.md) ·
[file_io.cpp](files/src-file_io.cpp.md) ·
[wireshark.cpp](files/src-wireshark.cpp.md) ·
[cxxgen.cpp](files/src-cxxgen.cpp.md) ·
[internal.hpp](files/src-internal.hpp.md)

Programs, tests and build:
[tools/srdump/main.cpp](files/tools-srdump-main.cpp.md) ·
[examples/](files/examples.md) ·
[tests/](files/tests.md) ·
[build system, packaging, scripts](files/build-system.md)

Outside `files/` (a `files/` page documents a C++ source or one of the three topic pages `scripts/check-book.py`
knows), the pages for parts of the tree that are not library sources:
[python/serreg](python-serreg.md) ·
[tests/bench/registry_measure.cpp](registry-measure.md) ·
[tests/ce](ce-examples-tests.md) ·
[scripts/make-rebuild-prompt.py](make-rebuild-prompt.md)

## Related documents in `docs/`

The project is described by six flattened documents, written from the current state:
`DECISIONS.md` (the one decision ledger: R requirements, P protocol, A API, C code, G general, T testing/tooling,
D documentation — each entry with its status), `ser-reg-requirements.md` (the FR-* requirements),
`ser-reg-architecture.md` (architecture, the byte layouts, the registry cost model), `ser-reg-testing.md` (the test
tree and the verification protocol), `ser-reg-stories.md` (use cases with complete code) and
`ser-reg-rebuild-prompt.md` (with `scripts/make-rebuild-prompt.py`: the prompt from which the library can be built
again). Beside them: `test-and-log-protocol.md` (how a full test run is read and what gates a release),
`wireshark.md` (`srdump --wireshark` and the generated dissector), `deviations.md` (where the code differs from the
v3 specification), `ipc/` (the design, latency analysis and test/benchmark results of the variable-size `es::ipc`
rings), `windows-porting.md`, `serreg-reflection.md` and `serreg-registry.md`, and `test-reports/`. Kept as history
— the reasoning behind a ledger entry: `ser-reg-requirements-v3.md` (the original specification and review log),
`plans-for-v2.0.md` (dynamic members) and `plans-for-containers.md` (maps and sets as members, containers as
arguments, with the differences of what shipped). In `docs/`, `SECURITY.md` states the never-crash contract
and how to report a crash, a hang or an amplification.

## Reading paths

- **New user:** 1 → 2 → 3 → 4, then the example programs.
- **Operating a system that uses it:** 5 (runtime export, unload and reload), 10, `executables.md` (environment).
- **Writing a reader in another language:** 7, 8, 6 — plus `tests/canary/rri_canary.cpp`, which decodes an RRI
  from byte offsets without any library header, and `python/serreg` (`python-serreg.md`), a complete reader in
  pure Python checked against `srdump`.
- **Binary logging:** 3 §3.5 and §3.9, then 10 §10.1e (`srdump --level`) and 4 §4.6d (`frame_router::on_log`).
- **Large, long-lived or crash-prone captures:** 3 §3.4 (`capture_options`), 7 §7.9, 4 §4.6e (`tools::info`,
  `--resync`), 5 §5.3b (the registry at scale).
- **Low-latency IPC:** 11 (`es::ipc`), then 3 (claiming sinks) and 4 (`dispatch` in place).
- **Maintainer:** 9 first, then the `files/` document of whatever you are about to change, then `files/tests.md`
  for the tests that lock it.
