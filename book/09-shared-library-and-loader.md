# 9. The shared library and the loader — what a maintainer must understand

The registry's guarantees ("registered before `main()`", "one registry per process", "an unloaded plugin's types
stay readable, and a reloaded plugin registers again") rest on specific ELF, linker and glibc behaviours. This chapter lists each one, why the code depends on it, and
which test would fail if it changed. Read it before touching `registry.hpp`, `registry.cpp`, the image backend
(`src/image.hpp`, `src/image_elf.cpp`, `src/image_none.cpp`), `blob.hpp` and `detail/embed_*.hpp`, the build flags,
or the packaging.

## 9.1 Where things live

| entity | linkage / placement | why |
|---|---|---|
| `libserreg.so.2.2` (default) | one shared object; the registry state is a leaked function-local static `G()` inside `registry.cpp` | one registry per process across executable and plugins; exists before any guard runs; outlives `atexit` |
| heap entries (`entry` in `registry.cpp`) | heap memory owned by `G().all`, never freed before exit | `push()` makes every registration one: for the main program views of its ENTRY blob, name and descriptor (the executable is never unmapped, so nothing is copied), for every other image copies of the blob and the name and a deep copy of the descriptor; they outlive the image that registered them (§9.5) |
| `registrar<T>::g`, `registrar<T>::node`, `registrar_lit<S>::…`, `registrar_call<Ts...>::…` | static data members of class templates: COMDAT (vague linkage), **hidden visibility** (`ES_SERREG_HIDDEN`) | one per *image*, never exported → the dynamic linker never merges them across DSOs; each image registers and counts itself (FR-R12) |
| `descriptor_v<T>`, `type_blob_bytes_v<T>`, `literal_blob_bytes_v<S>`, `call_blob_bytes_v<…>` | `inline constexpr`, hidden, COMDAT `.rodata` | ODR-clean single copy per image; referenced by nodes |
| `type_blob_copy_v<T>` etc. (`detail/embed_elf.hpp`) | **TU-local** `static constexpr`, `[[gnu::section("es_serreg_dict"), gnu::used, gnu::retain]]` | the offline-readable section (§9.3); not emitted with `ES_SERREG_EMBED_NONE` |
| the reader limits (`g_limits`, `reader.cpp`) | a `constinit decode_limits` behind a `constinit std::mutex` in `libserreg`, read once per walk by `get_decode_limits()` | process-wide and constant-initialised, so no initialisation order; one set per copy of libserreg (a static library in a host and in a plugin gives two, as it gives two registries) |
| `serreg_init` | `[[gnu::constructor(101)]]` in `registry.cpp` | before user static initialisers of priority ≥ 101 and before `main()`: warns about a second copy of libserreg, reads the `ES_SERREG_*` variables (and prints one line if the ignored `ES_SERREG_ALLOW_DLCLOSE` is set), **blocks** the dump signal in the loading thread (so threads created later inherit the mask), starts the dump thread if configured and registers the exit hook |
| `es_serreg_registry_marker_v3()` | `extern "C"` function, default visibility, `gnu::used, gnu::noinline`, in every copy of libserreg (end of `registry.cpp`) | review Q10: it returns the address of that copy's private `own_marker`; `serreg_init` calls the first one in the global scope and prints one line when the result is not its own. A function rather than a data symbol: see below |

libserreg does not define `dlopen` or `dlclose`: in a process that links it they are libc's (an earlier design
interposed them to tell an unload from process exit).

Static `libserreg.a` (`-DES_SERREG_BUILD_SHARED=OFF`) is supported for monolithic programs; linking it into
both a host and a `dlopen`'ed DSO yields *two* registries: the DSO's types never reach the host's registry, and
when the DSO is unloaded its registry copy goes with it, so nothing of it is retained (`gcc16-static` skips the
dlopen tests for this reason). The second copy says so when it starts:

```
es::serreg: two copies of libserreg in one process (<first image> and <second image>): each has its own registry; link every image against libserreg.so
```

It is found through `es_serreg_registry_marker_v3()`. The symbol can be interposed — every image binds to the
first definition in the global scope — so the check calls the function and compares what it *returns*, the address
of the defining copy's own internal marker, not the symbol's address (`registry_second_copy_diagnostic`). A copy
whose marker is not in the global scope (a static executable linked without `-rdynamic`) cannot be seen.

An earlier marker was an exported data object, `es_serreg_registry_marker_v2`, whose value was that address. Two
copies of libserreg each defined that object, which AddressSanitizer reports as an ODR violation and aborts on — in
exactly the processes the diagnostic is for (`registry_second_copy_diagnostic` aborted under ASan). Two definitions
of an exported *function* are not instrumented globals, so the marker is
`extern "C" const void* es_serreg_registry_marker_v3() noexcept`. `image::first_registry_marker()`
(`src/image_elf.cpp`) looks it up with `dlsym(RTLD_DEFAULT, …)` and calls it; if it is absent it still reads an
older copy's `_v2` object (reading defines nothing), so an older copy is recognised too.
`tests/dlopen/second_copy.cpp` defines its own `_v3` function to play the second copy.

## 9.2 Registration order and static initialisation

- Guards run during the dynamic initialisation of the image that instantiated them. Within one image the order
  is unspecified across TUs; the registry does not care (aliases collapse by hash, `program_hash` is
  order-independent — `link_alt_registration_order`, `link_program_hash_order_independent`).
- `G()` is a function-local static that is *constructed on first use and intentionally never destroyed* (a
  leaked `new`). This avoids both the static-initialisation-order fiasco (a guard in another image can run
  before `libserreg`'s own globals) and the exit-time inverse (`atexit`/`dlclose` paths run after globals with
  destructors are gone; an earlier design crashed with `free(): invalid pointer` here).
- `-fno-gnu-unique` is a **PUBLIC** compile option of the `es::serreg` target: without it GCC emits vague-linkage
  variables as `STB_GNU_UNIQUE`, glibc then marks the DSO `DF_1_NODELETE`, and `dlclose` becomes a silent no-op.
  That does not endanger the registry (an image that stays mapped keeps its references), but it breaks reloading: the next `dlopen` of the same path returns the old image, its constructors do not run again,
  and a rebuilt plugin's new types never register. Hidden visibility on the registrar symbols is the second line
  of defence (`dlopen_I_plugin_without_flags_unloads` builds a plugin *without* the public flag and checks that
  `ES_SERREG_HIDDEN` alone keeps the symbols local and the image unloadable).

## 9.3 The `es_serreg_dict` section

Each TU that instantiates `descriptor_v<T>` (or a literal/call) gets a `static constexpr` *copy* of the blob in
section `es_serreg_dict` (defined in `detail/embed_elf.hpp`, instantiated by `blob.hpp`'s `embed_*`
hooks), 8-aligned, `[[gnu::used, gnu::retain]]` so `--gc-sections` keeps it
(`link_gc_sections`; binutils ≥ 2.36 for `retain`). Why copies and not one COMDAT object: GCC places every COMDAT
variable that names a custom section into *one* section carrying the group of whichever came first, which breaks
multi-TU links. Duplicates across TUs cost ~250 B per type per TU and are harmless — readers key by hash. The
section is read offline by `tools::extract_embedded_dictionary` (`extract_registry_from_elf` is a wrapper
for it: ELF magic → section headers → `.shstrtab` → bounds-checked copy; a non-ELF image is `unsupported`) and is
what `srdump --elf` decodes; it is *not* used at run time. The `link_no_pie_*` tests check the main-image case,
`link_lto` that LTO keeps the copies. A build with `ES_SERREG_IMAGE_FORMAT=none` defines `ES_SERREG_EMBED_NONE`,
the hooks are empty (`detail/embed_none.hpp`) and there is no section: `extract_embedded_dictionary` returns
`not_found` (`link_image_none_backend`); offline tools then use an `.rri` or the capture's own dictionary.

## 9.4 Image identity and the image backend

`registry::push()` records the node's image base — `image::current(payload)` (or the blob address for literals),
which the ELF backend answers with `dladdr(…).dli_fbase` — outside the registry mutex (`dladdr` takes the loader
lock). Entries are counted per image: a type registered by the executable and by a plugin has `refs == 2`; the
plugin's `dlclose` drops it to 1 and the entry stays loaded. The image base also decides what `push()` copies: a
registration of the main program keeps pointers to the program's own descriptor, ENTRY blob and name; any other
image's blob and name are copied and its descriptor deep-copied (§9.5). `image::main_image()` identifies the main
image once — the ELF backend walks `dl_iterate_phdr` to the executable's lowest `PT_LOAD` — before the mutex is
taken, because it takes a loader lock while a guard destructor inside `dlclose` may be waiting for the mutex
(review G2-5: lock-order inversion).

**The backend (review addendum A4).** All of this — `dladdr`, `dl_iterate_phdr`, the `DF_1_NODELETE` probe, the
marker lookup — is behind `src/image.hpp`, and `registry.cpp` includes no loader header. The registry needs these operations from the
executable format:

| operation | what the registry asks | ELF backend (`image_elf.cpp`, default) | null backend (`image_none.cpp`) |
|---|---|---|---|
| **identity** | `current(addr)`, `main_image()`, `name(base)`, `first_registry_marker()` | `dladdr`, `dl_iterate_phdr`, `dli_fname`, `dlsym(RTLD_DEFAULT, "es_serreg_registry_marker_v3")` and a call (fallback: an older copy's `_v2` object) | one image id for every address, `"<main program>"`, no marker lookup |
| **diagnostics** | `can_unload(addr)` (`registry::unload_guard_active`) | the `DT_FLAGS_1` probe | `false` (nothing unloads) |
| **embedding** | (header side) the `embed_*` hooks; (offline) `extract_embedded_dictionary` | `es_serreg_dict` section copies, read by the section reader | `ES_SERREG_EMBED_NONE`: no section |

The backend is chosen when `libserreg` is built: CMake `ES_SERREG_IMAGE_FORMAT=elf` (default) or `none`
(`files/build-system.md`). `none` is refused (`FATAL_ERROR`) together with `ES_SERREG_BUILD_SHARED=ON`: a shared
`libserreg` is there to be used by images that can be `dlopen`ed and `dlclose`d, and with one image id for every
address it would take a plugin's descriptors for the main program's and keep pointers into the plugin instead of
deep copies.
With the static library and `none`, the whole program is one image: registration, dedup, the published index,
captures, `export_image()` and the program hash are those of the ELF build (`link_image_none_program_hash_equal`
compares with `link_plain`), and every entry points at the program's own descriptor.
A program built this way must not `dlopen` images that serialise. The two `link_image_none_*` tests build a private
static library with the null backend in Debug builds only (`tests/link/CMakeLists.txt`).

`none` also drops what only ELF needs from the build: the `es::serreg` target adds `-fno-gnu-unique`
(§9.2) and links `-ldl` only for `elf`, and the installed `serreg.pc` follows (`-DES_SERREG_EMBED_NONE` instead).
The preset `gcc16-static-none` builds the whole tree that way and runs the whole suite: tests that read a binary's
`es_serreg_dict` section read the registry's own RRI instead (`test::embedded_dictionary_of_self()`,
`tests/support/test_support.hpp`), `srdump_elf_none_has_no_section` checks that `srdump --elf` finds no section,
and `tests/link` runs `link_image_none_backend` against the real library. `run-all-tests.sh` runs the preset by
default.

## 9.5 `dlopen`, `dlclose`, reload and exit (decision A)

The registry never reads an image's memory after that image's guards were destroyed, so it does not
need to know *why* they are destroyed. What happens at each event:

```
dlopen(path)   → the image's static initialisers run inside dlopen (loader lock held):
                   each guard → push(): new heap entry (blob, name, descriptor copied) | alias | revival of a
                   retained entry | replacement of a retained definition | FR-R4 conflict
                   each push marks the index stale; no index is built, no callback runs
                 → return; the next read (or registry::refresh()) publishes one index and runs on_change once
dlclose(h)     → the image's guard destructors run inside dlclose (loader lock held):
                   each → on_guard_destroyed(): remove the node from its entry, --refs; refs 0 = retained;
                   a kept conflicting definition that unloads hands its key to a still-loaded rival
                   no abort, no allocation, no free, no callback
                 → return; the next read (or refresh()) publishes and notifies once
exit()         → exit hook: exiting = true (no more callbacks), stop the dump thread, dump if requested, free
                   retired indexes; guard destructors that run later drop their references like at dlclose
```

**dlopen.** A plugin's types are visible to any read once `dlopen` has returned (FR-R6): the first read publishes
one index for all of the plugin's registrations (`dlopen_A_dlclose_keeps_entries` checks that a `dlopen`
publishes exactly one index). `push()` allocates inside `dlopen` (the heap entry and the descriptor copy), as
registration always did; it takes no lock other than the registry mutex and asks the image backend nothing while
holding it (review G2-5).

**dlclose.** The unloaded image's entries are **retained** (`refs == 0`): `size()`, `find_type()`,
`find_type_by_name()`, `find_call()`, `find_literal()`, `export_image()` and the JSON/TOML views still include
them, the descriptor pointers obtained before the unload are the ones lookups keep returning, and frames written
while the image was loaded still decode — `dlopen_A_dlclose_keeps_entries` checks the descriptor, its names,
fields and blob, an old snapshot, `inspect()` and `tools::dump()` of a plugin frame after the plugin is unmapped.
`images()` and `program_hash()` count loaded entries only, so both return to their values before the `dlopen`;
`registry::retained()` counts the retained entries. Nested `dlclose` (a plugin's destructor closes another,
`dlopen_D_nested_dlclose`), two threads unloading at once (`dlopen_F_concurrent_dlclose`), readers during load and
unload churn (`dlopen_J_readers_vs_dlclose_churn`) and two threads loading and unloading (`dlopen_L_two_thread_load_unload`,
which ends itself with `alarm` if a lock-order bug hangs it) need nothing special: each destructor drops one
reference under the mutex. `RTLD_NODELETE` keeps the image mapped; its `dlclose` runs no destructor and changes
nothing (`dlopen_C_rtld_nodelete`).

**Reload.** Loading the same plugin again revives its retained entries: same key, same layout, so `push()`
attaches the new node to the old entry and no memory is allocated (`dlopen_B_reload_revives_entries`: 50 cycles,
the same descriptor every time, `size()` unchanged, one generation per load and one per unload). A plugin rebuilt
with a changed type normally has a new schema hash for it: the new definition becomes a new entry next to the
retained one, without a conflict, and `find_type_by_name()` prefers the loaded one; loading the old build again
revives the old entry (`dlopen_E_changed_layout_reload`, with `serreg_plugin_v2`: `plugin.cpp` built with
`ES_PLUGIN_V2`, one more member in `plugin::Tick`). A definition with the same schema hash but another layout
replaces a retained entry, and conflicts (FR-R4) with a loaded one; when the kept definition of a conflict
unloads, the still-loaded rival takes its place (`dlopen_H_conflicting_plugins`). Reloading needs the image to be
really unloaded, which is why `-fno-gnu-unique` and the hidden registrar symbols stay (§9.2).

**Exit.** The exit hook is registered by `serreg_init`, before any guard of a program linked against libserreg,
so it runs after their destructors and the exit dump sees every entry, retained ones included. It still dumps only
when something was registered.

**Notifications.** libserreg does not see `dlopen`/`dlclose`, so a guard cannot know whether it runs under the
loader lock and never calls back. The thread that publishes the next index runs the `on_change` callbacks once for
that generation, after the publish, outside the mutex. That is normally the program's first read after the
`dlopen`/`dlclose`, outside the loader lock — unless that read is itself made from a static initialiser or
destructor. A program that wants its callbacks right after a `dlopen()` or `dlclose()` calls
`registry::refresh()` on that thread; `tests/dlopen/host.cpp` does after every call. Every change notifies,
including a load or unload that only adds or drops aliases (`dlopen_K_alias_only_plugin_notifies`).

**Diagnostics inside the loader.** The schema-conflict line (written from `push()` inside `dlopen`), the
second-copy line, the `ES_SERREG_ALLOW_DLCLOSE` notice and the dump-signal lines are written by
`detail::report_line` (`include/es/serreg/sink.hpp`), not by `printf` or `std::println`: it formats with
`std::format_to_n` into a 1024-byte stack buffer and writes it with `detail::write_fd(2, …)` — no allocation, and no
`std::system_error` from a failed write in a `noexcept` path.

**Why it is this simple.** An earlier design interposed `dlopen()`/`dlclose()` (with `thread_local` depth counters
and an `atexit` marker re-armed after every `dlopen`) to tell an unload from process exit; a `dlclose` of an image
with registered entries aborted (FR-R10) unless `ES_SERREG_ALLOW_DLCLOSE=1` removed its entries (unlink mode,
FR-R11). Making the entry at load and retaining it at unload needs none of that; `ES_SERREG_ALLOW_DLCLOSE` is
ignored with one stderr line at startup.

### What the registry cannot protect

The registry protects what it hands out: entries, descriptors, names, blobs and indexes. It cannot keep alive
anything else that belongs to an unloaded image:

- **Plugin code that is called later.** An `on_change` callback whose code lives in the plugin must be removed
  with `remove_on_change(id)` before the plugin is unloaded; so must any handler, visitor, sink or `std::function`
  the plugin gave the host (a `frame_router` handler, a `tools::decoded_value_visitor`). Objects of the plugin's classes
  with virtual functions, and threads running plugin code, must be gone before `dlclose`.
- **Pointers the plugin hands out directly.** `&descriptor_v<T>`, `call_name_v<Ts...>`, `lit<S>::text()` or a
  blob span taken inside the plugin point into the plugin image; after the unload, use the registry's copy
  (`find_type(schema_hash)`, `find_literal(id)`). Frames stored in plugin-owned buffers go with the plugin.
- **A static libserreg inside a plugin.** The plugin then has a registry of its own (the second-copy warning in
  §9.1): nothing it registers reaches the process registry, and nothing is retained when it unloads. Link every
  image against `libserreg.so`.
- **A plugin that cannot unload.** If glibc keeps the image mapped (`DF_1_NODELETE`: `-z nodelete`, an
  `STB_GNU_UNIQUE` symbol), its `dlclose` changes nothing and a later `dlopen` of a rebuilt file at the same path
  does not run its constructors; `registry::unload_guard_active()` reports such an image.
- **Memory.** Entries are never freed before exit. Reloading the same build adds nothing, but every rebuilt plugin
  whose types changed adds its changed entries, and every published index stays until exit.

## 9.6 Symbol visibility and the exported target

The installed export set `serregTargets` gives `es::serreg` the interface options `-freflection -fno-gnu-unique`
and `cxx_std_26`; consumers get the flags automatically. `cmake/serregConfig.cmake.in` (decision G)
offers the components `ipc`, `serreg` and `tools` (`files/build-system.md`); `es::serreg_tools` links `es::serreg`
PUBLIC and inherits the same flags. The library's API classes carry
`ES_SERREG_API` (`[[gnu::visibility("default")]]` in the shared build) and the per-image entities
`ES_SERREG_HIDDEN` (`[[gnu::visibility("hidden")]]`). Do not
export the registrar templates or the blobs: exporting them would re-introduce cross-image merging and the
`DF_1_NODELETE` trap.

## 9.7 RPATH and installation

`serreg`, `serreg_tools` and `srdump` are built with `BUILD_WITH_INSTALL_RPATH ON` and
`INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"` (`lib64` on such distributions): an installed `srdump` finds its
`libserreg_tools.so.2.2` and `libserreg.so.2.2` next to it or in the sibling `lib`, and `cmake --install` never
relinks (the Ninja generator refuses to change RPATHs on install otherwise). Both libraries carry
`VERSION ${PROJECT_VERSION}` and `SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}` (file
`libserreg.so.2.2.6`, SONAME `libserreg.so.2.2`): the SONAME follows the minor, the compatibility unit until 3.0
(DECISIONS G-2), which is also what `find_package(serreg 2.2)` checks (`SameMinorVersion`).

## 9.8 Threading rules

- Hot path (`serialize`, `deserialize`, `dispatch`, `inspect`): no lock, no allocation (claiming sinks), no
  registry mutation; registry *reads* are acquire loads of the published index.
- Mutations (`push`, `on_guard_destroyed`, conflict recording) take `G().mu` and mark the index stale; the first
  reader after them takes `G().mu` once and `publish_locked()` builds a new index of views into the heap entries
  and stores it with release semantics; old indexes are never freed (a snapshot stays valid for the
  process), and neither are the heap entries they point at: a descriptor pointer (`find_type`, `payload`) is valid
  for the life of the process (§9.5, G2-4).
- The signal path: `ES_SERREG_DUMP_SIGNAL` is **blocked** by `serreg_init` in the thread that loads libserreg, and
  a helper thread started by `serreg_init` waits for it with
  `sigwait`; the dump runs on that thread,
  never in a handler. `pthread_sigmask` is per thread: threads that already existed when libserreg was loaded
  (libserreg `dlopen`ed late, into a running multi-threaded program) do not have the signal blocked, and if the
  signal is delivered to one of them its default action — for `SIGUSR1`, terminate — applies. **Load libserreg
  before creating threads** (link it into the executable), or block the signal in those threads yourself
  (review G2-8).
- `on_change` callbacks run outside the mutex, once per published generation, on the thread that published it
  (`registry::refresh()` publishes on demand). Every change notifies, including a registration that only
  aliases an existing entry (G2-10; `dlopen_K`). No callback runs once the exit hook has started; a callback whose
  code is in a plugin is removed with `remove_on_change(id)` before that plugin unloads.
- `capture_file` is single-writer; `fd_sink` is as thread-safe as the descriptor.

## 9.9 Checklist before changing any of this

1. Run `tests/link` and all of `tests/dlopen` (`dlopen_A`…`dlopen_L`: unload, reload, changed layout, nested and
   concurrent `dlclose`, conflicts, churn, and `registry_second_copy_diagnostic` — also under the ASan/UBSan preset,
   where a data marker would abort) — unit tests do not cover the loader. Diagnostics written inside
   `dlopen`/`dlclose` or from `serreg_init` use `detail::report_line`. A guard destructor may not allocate,
   free, abort or call back; no registration may run a callback; nothing may take the loader lock
   (`image::current`/`main_image` — `dladdr`, `dl_iterate_phdr` — or `dlopen`) while holding `G().mu`. A change to the image backend also runs `link_image_none_backend` and
   `link_image_none_program_hash_equal` (Debug builds), and a new question for the backend is a function in
   `src/image.hpp` implemented by every `src/image_*.cpp`.
2. `readelf -d plugin.so | grep FLAGS_1` must not show `NODELETE`; `readelf -s | grep UNIQUE` must be empty.
3. `readelf -S binary | grep es_serreg_dict` must exist after `--gc-sections` and `-flto` (and must not exist for a
   program built with `ES_SERREG_IMAGE_FORMAT=none`).
4. Under TSan (`gcc16-tsan`), repeat `dlopen_[EFGJL]|signal_dump|concurrent|hrtt_.*_thread` (`scripts/run-all-tests.sh
   --tsan-repeat 20`).
5. Any change to the section name, the blob shapes, or the header sizes is a wire-format change: golden
   `blobs.cpp`, fixtures, canary, `format_minor`, CHANGELOG.
