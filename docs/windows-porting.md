# Porting to Windows — plan, and which files depend on Linux

**Status:** plan only. **Policy (maintainer, 2026-10-02):** Windows is considered **after everything works and is
tested on Linux**. Until then this file is the map, and the rules in §6 keep new code portable.
**Background:** an earlier study ("study §9" below; kept for one release as `remove-me/docs/c-review-study.md`)
worked out the PE embedding (built and checked), a PE section reader, `image_win.cpp`, the DLL export macro, the
LLP64 ABI and the dump trigger. This file carries that study forward to decision A (no `dlopen`/`dlclose`
interposers, no unload abort), decision G (three libraries) and the variable-size `ipc`, and lists every file by how
it ports.

---

## 1. Prerequisites (in order)

1. **Linux complete:**
   - the canonical-name change (decision B), so names no longer contain library spellings;
   - done: the variable-size rings are `ipc` and the slot rings moved to `ipc_old` (`docs/ipc/ipc-design.md`
     Q1). Only the new `ipc` is ported; `ipc_old` is not.
2. **A P2996 toolchain for Windows.** MSVC has no reflection (do not write a shim, AGENTS.md). The candidates, from
   study §9.1:
   - **GCC 16 for MinGW-w64** (same compiler and libstdc++ as Linux: the most direct port);
   - **clang-p2996 targeting `x86_64-pc-windows-msvc`** with the fork's libc++ (interoperates with MSVC-built C APIs;
     a real second compiler);
   - clang-p2996 for MinGW (llvm-mingw).

   The choice decides the CI runner and the ABI (study Q13).
3. **A Windows CI runner** with that toolchain. Without one the port is unverifiable.

## 2. What decision A changed (simpler than the study's plan)

- **No interposers.** The Linux build no longer replaces `dlopen`/`dlclose`. Registration (guards, heap entries,
  retained entries after unload, reload) is plain C++ and ports unchanged. On Windows, `FreeLibrary` simply runs the
  guards' destructors, which drop references.
- **No unload abort, no exit detection.** The study's question about telling `FreeLibrary` from process exit
  (`RtlDllShutdownInProgress`, study Q14) **is moot**: nothing depends on it any more.
- **What remains of the image backend:**
  - which image an address belongs to: `GetModuleHandleExW(FROM_ADDRESS | UNCHANGED_REFCOUNT)`;
  - the image's path: `GetModuleFileNameW`;
  - the main image: `GetModuleHandleW(nullptr)`;
  - the second-copy marker: `GetProcAddress` over the loaded modules, or one named object.

  That is `src/image_win.cpp`, about 60 lines.

## 3. Every file, by how it ports

Legend:
- **P**: portable as is (standard C++ only);
- **S**: portable after a small, local change;
- **B**: a Linux backend; a Windows twin is needed;
- **L**: Linux by nature (tests, scripts); a Windows equivalent or exclusion is needed.

### 3.1 `include/es/serreg/` (the runtime headers)

| File | Class | What depends on Linux / what to do |
|---|---|---|
| `reflect.hpp`, `hash.hpp`, `format.hpp`, `error.hpp`, `literal.hpp`, `arg.hpp`, `serialize.hpp`, `deserialize.hpp`, `print.hpp`, `router.hpp`, `dictionary.hpp`, `walk.hpp`, `capture.hpp` (FILE*), `serreg.hpp` | P | none. Note `format.hpp`'s ABI tag already records `long` = 4 and `wchar_t` = 2 (LLP64) |
| `descriptor.hpp` | S | `ES_SERREG_HIDDEN` = `[[gnu::visibility("hidden")]]`: becomes empty on PE (symbols are module-local unless exported) |
| `registry.hpp` | S | `ES_SERREG_API`: `__declspec(dllexport/dllimport)` for a DLL build (study §9.6); comments about `-fno-gnu-unique`/`dladdr` |
| `sink.hpp` | S | `fd_sink` and `detail::report_line` use `::write(2)` (`<unistd.h>`): `_write` or `WriteFile` on a `HANDLE` |
| `blob.hpp`, `detail/blob_bytes.hpp` | P | they select the embed backend (`embed_elf` / `embed_none`); add `embed_pe` to the selection |
| `detail/embed_elf.hpp` | B | `[[gnu::section("es_serreg_dict"), gnu::used, gnu::retain]]` → **`detail/embed_pe.hpp`**: a `.esdict$…` section (MinGW/clang: `__attribute__((section))` + `used`; checked in study §9.3) |
| `detail/embed_none.hpp` | P | works on Windows today (no embedded dictionary; tools need an `.rri`) |
| `tools.hpp` | P | declarations only (`mapped_file` is implemented in `src/reader.cpp`) |

### 3.2 `src/` (libserreg and libserreg_tools)

| File | Class | What depends on Linux / what to do |
|---|---|---|
| `dictionary.cpp`, `decoder.cpp`, `formatters.cpp`, `diff.cpp`, `wireshark.cpp`, `cli.cpp` | P | none (`cli.cpp` uses stdio and the readers) |
| `elf_reader.cpp` | P | pure parsing of ELF *files*: a Windows `srdump` can still read Linux binaries. Add a **PE section reader** next to it (study §9.4, run on Linux), dispatched by `extract_embedded_dictionary` |
| `image.hpp` | P | the backend interface (comments mention `dladdr`/`dl_iterate_phdr`) |
| `image_elf.cpp` | B | `dladdr`, `dl_iterate_phdr`, `<dlfcn.h>`, `<elf.h>`, `<link.h>` → **`image_win.cpp`** (§2) |
| `image_none.cpp` | P | works anywhere |
| `registry.cpp` | S | portable core (heap entries, index, conflicts, export). Linux parts: the **signal-triggered dump** (`pthread_sigmask`, `sigwait` thread) → a named event plus a waiting thread (study §9.8); `::open(O_CLOEXEC)`/`::write` for dump files → stdio or Win32; `_Exit` after `ES_SERREG_DUMP_EXIT` (portable) |
| `file_io.cpp` | B | `::open/::read/::write`, `O_CLOEXEC`, `<sys/stat.h>` → Win32 (`CreateFileW`, `ReadFile`, `WriteFile`) or `std::fstream` |
| `reader.cpp` | B | `mapped_file`: `mmap`/`madvise` → `CreateFileMapping`/`MapViewOfFile`; the block reader's `::read` → `ReadFile` (pipes: `ReadFile` on a pipe `HANDLE`) |

### 3.3 `include/es/ipc/` (the variable-size rings) and `include/es/ipc_old/` (slot rings, not ported)

The **shared-memory layout and protocols are portable**:
- only integers accessed through `std::atomic_ref` (lock-free and address-free on x86-64 and AArch64 Windows);
- no pointers in shared memory;
- no OS objects in shared memory.

What is Linux:

| File | Class | What depends on Linux / what to do |
|---|---|---|
| `ipc/layout.hpp` | P | none (`es/ipc/common.hpp` vocabulary only) |
| `ipc/ring.hpp`, `producer.hpp`, `consumer.hpp` | S | `::getpid` → `GetCurrentProcessId`; `sched_yield` → `SwitchToThread`; fork-safety checks (owner pid ≠ current pid) are harmless (no fork on Windows) |
| `ipc/sink.hpp`, `ipc.hpp` | P | none |
| `es/ipc/common.hpp` (moves with the rename) | S | `process_gone`: `kill(pid, 0)` → `OpenProcess(SYNCHRONIZE)` + `WaitForSingleObject(…, 0)`; or, better, the planned **role locks** (Linux: OFD locks, review I-03; Windows: a named mutex per role, abandoned when the holder dies: `WAIT_ABANDONED`). `cpu_relax` is already per architecture |
| `es/ipc/region.hpp` (moves with the rename) | B | tmpfs/hugetlbfs files, `mmap`, `statfs`, `fallocate`, `mlock`, `/proc/mounts`, `O_CLOEXEC|O_NOFOLLOW` → **named file mappings** (`CreateFileMappingW` on the paging file, `Local\`/`Global\` names, `MapViewOfFile`); large pages via `SEC_LARGE_PAGES` (needs `SeLockMemoryPrivilege`); `VirtualLock` for `mlock`; ownership checks via the mapping's security descriptor instead of `st_uid`/mode bits |

### 3.4 Build, tools, tests, scripts

| Path | Class | What depends on Linux / what to do |
|---|---|---|
| `CMakeLists.txt`, `cmake/*` | S | `-fno-gnu-unique` (GCC/ELF only, already guarded), `-ldl`, `ES_SERREG_IMAGE_FORMAT=elf\|none` → add `pe`; DLL export macro definition; pkg-config files (keep for MinGW); presets for the chosen toolchain |
| `tools/srdump/main.cpp` | P | one line |
| `tests/unit/*` (most) | P | serialization, readers, tools, golden |
| `tests/unit/test_ipc.cpp` (`test_ipc_old.cpp`: not ported) | L | `fork()`-based multi-process tests → `CreateProcessW` children, or thread-only on Windows; region tests → the Windows region |
| `tests/unit/signal_dump_test.cpp` | L | signals → the named-event trigger |
| `tests/dlopen/*` | L | `dlopen/dlclose` → `LoadLibraryW/FreeLibrary` (same scenarios: keep, reload, changed layout, conflict take-over) |
| `tests/link/*` | L/S | `--gc-sections`, LTO, static-archive cases are GNU-ld specific; MinGW keeps most; MSVC-ABI clang needs `/OPT:REF` equivalents |
| `tests/bench/hrtt_*`, `ipc_compare_bench.cpp` | L | `fork`, CPU pinning (`sched_setaffinity` → `SetThreadAffinityMask`) |
| `tests/wireshark/*` | S | tshark exists on Windows; `parity.py` paths and process spawning |
| `tests/fuzz/*` | P | portable driver |
| `examples/*` | S | `examples/ipc_log`, `ipc_ring` use named regions and process spawning; the rest is portable |
| `ce-examples/*` | P | Compiler Explorer has no reflection-capable Windows compiler; nothing to port |
| `scripts/*.sh` (`run-all-tests.sh`, `make-release.sh`, `bench-*`, `pre-commit`) | L | bash: run under MSYS2, or move the test protocol to CMake/CTest scripts (also helps macOS) |
| `scripts/*.py` | P | Python |
| `share/serreg.magic` | — | `file(1)`; not applicable |

## 4. Data and ABI

- **LLP64:** `long` is 4 bytes and `wchar_t` 2 bytes, so the schema and layout hashes of types that use `long` or
  `wchar_t` differ from Linux by design (the canonical strings spell `long` by width). Fixed-width types
  (`std::int64_t`, …) hash identically. The ABI tag (format 1.3) records the writer's ABI. Typed reads across ABIs
  fail by hash; dictionary-driven tools read both.
- **A portable golden subset** (fixed-width scalars, enums with fixed types, dynamic kinds) must give identical
  hashes and frames on Linux and Windows. The `ce-examples` style (golden values in one self-checking program) is
  the model for that check.
- **Canonical names (decision B)** matter even more across platforms: a Windows standard library has its own
  inline namespaces too.

## 5. Phases (after §1)

| # | Phase | Can be done on Linux? | Estimate |
|---|---|---|---|
| 1 | `embed_pe.hpp`, the PE section reader, `extract_embedded_dictionary` dispatch, a `pe` fuzz target, committed PE fixtures | **yes** (cross-compiled fixtures, study §9.3/§9.4) | 1 session |
| 2 | Export macro, `image_win.cpp`, `file_io`/`mapped_file`/block reader/`fd_sink`/`report_line` Win32 twins, CMake `pe` backend | partly (compiles with a MinGW cross compiler, if one with P2996 exists) | 1–2 sessions |
| 3 | Registry dump trigger (named event) | no | ½ session |
| 4 | `ipc`: region and liveness backends, process tests | no | 1–2 sessions |
| 5 | Tests on the runner: `LoadLibrary` scenarios, ipc processes, the portable golden subset; CI bring-up; fixing what the new toolchain finds | no | 2–3 sessions |
| | **Total** | | **5–8 sessions**, after the toolchain exists (about one session less than study §9.11: no interposer, no exit detection) |

Mach-O, for completeness (not scheduled):
- `__attribute__((section("__DATA,__esdict"), used))`;
- `getsectiondata` for in-process reads;
- `_dyld_register_func_for_remove_image` is not needed after decision A.

## 6. Rules until then (keep new code portable)

1. **Operating-system calls only in backends:** `src/image_*.cpp`, `src/file_io.cpp`, `src/reader.cpp`
   (`mapped_file`, block reader), the dump trigger in `src/registry.cpp`, `region.hpp`, `common.hpp`'s liveness.
   Every other header and source is standard C++.
2. **No new `<unistd.h>`, `<sys/*.h>`, `<dlfcn.h>` or `<signal.h>`** outside the files marked B or S above. If one is
   unavoidable, add the file to this list in the same change.
3. **No new GNU attributes** outside `detail/embed_*.hpp` and the visibility/export macros.
4. **Shared-memory layouts stay pointer-free and OS-object-free** (integers through `std::atomic_ref`), as es::ipc
   is today.
5. **Tests that need `fork`, signals or `dlopen`** live in their own files (as today), so a Windows build can swap
   or skip them by file.
6. **Proposed check:** extend `scripts/check-includes.py` with an allow-list of the files that may include OS
   headers, so rule 2 is enforced by a ctest rather than by review.
