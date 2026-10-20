# `src/registry.cpp`

The process-wide registry behind `es::serreg::registry` (spec §6.3, §6.3b, §6.7, §6.11; FR-R2..R12). It
implements every member of `registry` declared in `include/es/serreg/registry.hpp` — `instance()`, `push()`,
`on_guard_destroyed()`, `snapshot()`, `refresh()`, `retained()`, `images()`, `program_hash()`, `find_type()`,
`find_type_by_name()`, `find_call()`, `find_literal()`, `has_conflict()`, the three `export_image()` overloads,
`export_image_since()`, `export_image_scope()` and `scope_calls()`, `to_json()`, `to_toml()`, `on_change()`, `remove_on_change()`, `generations()`, `unload_guard_active()`,
`dump_if_requested()` — plus `registry_index::find()`, the `ES_SERREG_*` environment hooks, the `sigwait` dump
thread, the exit hook and the second-copy warning.

The storage is a set of **heap entries** (decision A, `docs/DECISIONS.md`): `push()` makes every registration an
`entry` — for the main program views of its ENTRY blob, name and descriptor (the executable is never unmapped), for
any other image copies of the blob and the name and a deep copy of the descriptor. There is one entry per `(kind, hash)`; every guard
that registered it holds a reference. A guard destructor — at `dlclose()` or at process exit — drops its reference
and nothing else. An entry whose references are gone is **retained**: it stays in the index and in lookups, and
the registry never reads the unloaded image's memory again. A reload of the image revives the same entries.

Mutations take one mutex and mark the published index stale; the next reader publishes one fresh immutable index
of views into the heap entries for every change since the last and runs the change callbacks once for that
generation, outside the mutex. Otherwise a read is one acquire load and no allocation. The file never runs
a user callback under the lock, never asks the image backend a question that may take the loader lock under the
lock, and never frees a heap entry or a published index before process exit.

An earlier design held an abort mode (FR-R10: a guard destroyed inside a real `dlclose()` aborted unless
`ES_SERREG_ALLOW_DLCLOSE=1` selected unlink mode), `loader_events` called by `dlopen()`/`dlclose()` interposers,
`thread_local` depth counters and a re-armed `atexit` marker; retaining entries needs none of these.

The file is portable C++ (review addendum A4, "the seam"): it includes no `<dlfcn.h>`, `<elf.h>` or
`<link.h>`. Image identity, the `DF_1_NODELETE` probe and the Q10 marker lookup are in the image backend
(`src/image.hpp`, `src/image_elf.cpp` by default, `src/image_none.cpp` for `ES_SERREG_IMAGE_FORMAT=none`).

## Depends on / used by

Includes, package headers first (include rules): `image.hpp` (the backend), `es/serreg/dictionary.hpp`
(`dyn::dictionary` for `to_json`/`to_toml`), `es/serreg/format.hpp` (`fmt::writer`, header constants,
`literal_len_offset`/`literal_text_offset`), `es/serreg/registry.hpp`, `es/serreg/sink.hpp` (`detail::report_line`), `es/serreg/tools.hpp` (`tools::write_all`); then the POSIX headers `<fcntl.h>`, `<pthread.h>`,
`<signal.h>`, `<unistd.h>`; then the standard headers, among them `<deque>`, `<memory>`, `<unordered_map>` for the
entries and the descriptor copies.

Used by: every `detail::guard` in every image (`registrar<T>::g`, `registrar_lit<S>::g`,
`registrar_call<Ts...>::g` construct → `push`, destruct → `on_guard_destroyed`); `deserialize.hpp` and
`serreg.hpp` lookups through `registry::instance()`; `capture.hpp` (the opening dictionary and the deltas via
`export_image_since()` / `export_image_scope()`); `src/formatters.cpp` (`process_dictionary`);
`tests/unit/test_registry.cpp`, `test_conflict.cpp`, `test_capture.cpp` (`ES_SERREG_DUMP_REGISTRY_FD`),
`signal_dump_test.cpp`, `premain_check.cpp`, `tests/link/*`, `tests/dlopen/*`.

## Structure

- **The Q10 marker declaration** (`registry.cpp:53-58`): `extern "C" const void* es_serreg_registry_marker_v3()
  noexcept` (a function, not a data object), defined at the end of the file.
- **Anonymous namespace, first block** (`registry.cpp:74-275`): `conflict_policy {warn, abort_, silent}`;
  `node_key` / `node_key_hash` (C-01); `owned_desc` and `deep_copy()` (`registry.cpp:99-161`); `struct entry`
  and `make_entry()` (`registry.cpp:163-220`; views for the main program, `added`); `struct state` — `mu`, `all` (every entry ever made),
  `by_key` (the current entry of each key), `conflicts` (pairs of entries), `next_callback_id`, `callbacks`, the
  atomic `index`, `retired` generations, `stale`, `generations`, `on_conflict`, `exiting`, `pushed`, and the
  dump-signal fields (`registry.cpp:214-237`); `G()` returning the leaked singleton; `g_empty_index`; the forwarding
  functions `image_of`, `image_name`, `main_image`; `kind_slot()` (type 0, literal 1, call 2,
  scope 3).
- **`registry_state`** (`registry.cpp:302-309`): holds the `constinit registry inst` and `bump()` for the version
  counter; `registry::instance()` returns it.
- **Index** (`registry.cpp:305-314`): `registry_index::find()` — a lookup in the flat map of the kind.
- **Anonymous namespace, second block** (`registry.cpp:304-453`): `publish_locked()`, `mark_stale_locked()`,
  `publish_if_stale()`, `free_at_exit()`, `report_conflict()`, `add_entry()`, `attach()`, `same_definition()`.
- **Population** (`registry.cpp:455-550`): `registry::push()` and `registry::on_guard_destroyed()`.
- **Queries** (`registry.cpp:552-624`): `snapshot()`, `refresh()`, `retained()`, `images()`, `program_hash()` and
  the lookups.
- **Export** (`registry.cpp:643-820`): `export_image()` ×3 (the span form is `export_image_since(0)`),
  `export_image_since()` (`registry.cpp:653-683`), `scope_calls()` and `export_image_scope()`
  (`registry.cpp:686-786`); the `to_dictionary()` helper, `to_json()`, `to_toml()`.
- **Callbacks and diagnostics** (`registry.cpp:684-706`): `on_change()`, `remove_on_change()`, `generations()`,
  `unload_guard_active()` (`image::can_unload(addr)`).
- **Environment + signal hooks** (`registry.cpp:708-878`): `dump_configured()`, `do_dump()`, `parse_signal()`,
  `helper_main()`, `start_helper_if_configured()`, `exit_hook()`, `own_marker` and
  `warn_if_second_copy()` (Q10), `[[gnu::constructor(101)]] serreg_init()`; then
  `registry::dump_if_requested()` (`registry.cpp:990-995`).
- **Marker** (`registry.cpp:889-894`): the `es_serreg_registry_marker_v3()` definition, outside the namespace:
  `extern "C"`, default visibility, `gnu::used, gnu::noinline`, returns `&own_marker`.

## How it works

### The leaked `G()` state

```cpp
// Never destroyed: guards of other TUs/images may run before or after this TU's static (de)initialisation,
// and the exit hook must be able to use it at the very end (gk 1.13).
state& G() noexcept
{
    static state* const s = new state{};
    return *s;
}
constinit registry_index g_empty_index{};
```

(`registry.cpp:238-245`.) Guards in other images run their destructors at arbitrary points relative to
libserreg's own static destruction, and the exit hook runs after most of them. A function-local `static state*`
that is never `delete`d is constant-initialised (a pointer) and immortal, so there is no order in which it can be
used after destruction. `registry` itself is `constinit` inside `registry_state` for the same reason (no SIOF,
FR-R3/R8).

### Heap entries

```cpp
struct entry
{
    node_kind nk{};
    std::uint64_t hash = 0;
    std::uint64_t layout = 0; // types: layout hash (FR-R4); 0 otherwise
    std::vector<std::byte> blob_copy;
    std::string name_copy;
    std::unique_ptr<owned_desc> desc_copy; // null for entries first registered by the main program
    std::string_view name;
    std::span<const std::byte> blob;
    const void* payload = nullptr; // the main program's descriptor, or &desc_copy->type / ->call
    const void* image_base = nullptr;
    std::uint32_t refs = 0;
    std::uint64_t added = 0; // 2.2.1: the registry version at which this definition appeared (delta exports)
    std::vector<reg_node*>
        nodes; // the guards holding a reference (one per image/TU that registered it)
};
```

(`registry.cpp:165-181`.) `make_entry()` (`registry.cpp:183-220`) fills one from a node:

```cpp
if (n->image_base == main_base)
{ // 2.2.1 (G-5): the main program is never unmapped before exit, so its ENTRY blob and name are viewed in
    // place — no heap copy per registration (100k entries of 1 KB: 100 MB and ~50 ms less at start-up)
    e->blob = n->blob;
    e->name = n->name;
}
```
(`registry.cpp:198-203`.) For the main program the entry views the node's blob and name (and, as before, its
descriptor); for any other image it copies the ENTRY blob into `blob_copy`, lets a literal's `name` view its text
inside that copy (offset `fmt::literal_text_offset`), copies any other name into `name_copy`, and `deep_copy()`
makes an `owned_desc` whose `type` or `call` becomes `payload`. Copying the blob and the name for the main program
too would double the start-up cost of 100 000 entries (`registry_measure`). For a
type, `layout` records the `layout_hash` that `same_definition()` compares. Entries are owned by `G().all` and never
freed before exit, so `name`, `blob` and `payload` — and every index row that views them — stay valid after the
image that registered them unloads.

`deep_copy()` (`registry.cpp:133-161`) fills an `owned_desc` (`registry.cpp:100-131`) with the blob, every name (in a
`deque<string>`, whose elements never move), the field arrays in a `deque<vector<field_desc>>` — the top-level
fields and, recursively, every nested `elem` array (1 element for `array_`/`vector_`/`optional_`, `extent`
alternatives for `variant_`, the same rule as `put_field`) — the enumerators, padding gaps, bool offsets and, for a
call, the argument table; the copy's spans are re-pointed at that storage. The copy is made once, in `push()`, while the image is certainly
mapped.

### `push()` — new, alias, revival, replacement or conflict

`push()` (`registry.cpp:456-499`) runs from a guard constructor during static initialisation of the image. Before
taking the lock it fixes up a literal node's `name` to view the text inside its own ENTRY blob, resolves
`image_base` with `image_of()` (ELF: `dladdr`) and makes sure `main_image()` is known — deliberately outside
`G().mu`, because the ELF backend takes a loader lock for both and a guard destructor inside `dlclose` (loader lock
held) may be waiting for `G().mu` (review G2-5). Then, under the lock:

```cpp
entry*& slot = G().by_key[node_key{n->nk, n->hash}];
// 2.2.1: a new definition appears at the version the bump below publishes (delta exports)
const std::uint64_t appears = registry_state::inst.version() + 1;
if (!slot)
    attach(slot = add_entry(make_entry(n, main_base)), n), slot->added = appears;
else if (same_definition(slot, n))
{
    if (slot->refs == 0)
        slot->image_base =
            n->image_base; // a retained entry revived (the image was loaded again)
    attach(slot, n);       // alias: another image/TU brought the same entry
}
else if (slot->refs == 0)
{
    attach(slot = add_entry(make_entry(n, main_base)), n); // replaces a retained definition
    slot->added = appears;
}
else
{ // FR-R4: same schema hash, different layout, both loaded
    entry* const rejected = add_entry(make_entry(n, main_base));
    rejected->added = appears;
    attach(rejected, n, false);
    G().conflicts.emplace_back(slot, rejected);
    conflicted = true;
    conflict_name = slot->name;
    kept_image = slot->image_base;
}
mark_stale_locked(); // the version moves for aliases too (G2-10); published by the next reader
```

(`registry.cpp:486-514`.) Every new definition records `added`, the version its publication will carry; an alias
or a revival keeps the entry's original `added`, so a delta export never repeats an entry a reader already has.
`attach()` (`registry.cpp:464-470`) appends the node to the entry's `nodes`, increments
`refs`, stores the entry in `reg_node::entry` and sets `linked` (false only for a rejected definition).
`same_definition()` is true for a literal or a call, and for a type with the same `layout_hash`. The outcomes:

| case | result |
|---|---|
| key unseen | a new entry, the node linked |
| same definition, entry loaded | an **alias** (another TU or image instantiated the same `registrar`): one more reference |
| same definition, entry retained | the entry is **revived**: one reference again, `image_base` is the new image; no memory is allocated except the `nodes` slot |
| different layout, entry retained | a new entry **replaces** the retained one in `by_key`; the old entry stays alive, so pointers handed out earlier stay valid |
| different layout, entry loaded | an FR-R4 **conflict**: the rejected definition gets an entry of its own with one reference but stays unlinked; the pair is recorded, and `report_conflict()` prints the §11 diagnostic outside the lock (or aborts under `ES_SERREG_CONFLICT=abort`, or says nothing under `silent`) |

A changed type normally has a new schema hash and so is simply a new key: a plugin rebuilt with one more member
adds a new entry next to the retained old one (`dlopen_E_changed_layout_reload`). Every outcome bumps the version
and marks the index stale, and an alias notifies the `on_change` observers too
(review G2-10; `dlopen_K_alias_only_plugin_notifies`). The `by_key` slot of a key is never erased.

### `on_guard_destroyed()` — drop the reference

```cpp
std::lock_guard lock(G().mu);
auto* const e = static_cast<entry*>(n->entry);
if (!e)
    return;
if (const auto it = std::find(e->nodes.begin(), e->nodes.end(), n); it != e->nodes.end())
    e->nodes.erase(it);
n->entry = nullptr;
n->linked = false;
if (e->refs > 0)
    --e->refs;
if (e->image_base == n->image_base && !e->nodes.empty())
    e->image_base = e->nodes.front()->image_base; // still registered by another loaded image
```

(`registry.cpp:522-533`.) The destructor runs inside `dlclose()` (loader lock held) or during process exit; it
does the same in both cases and needs no way to tell them apart. It erases in place from vectors, so it neither
allocates nor frees, and it never aborts. When the entry still has references from another image, `image_base`
moves to that image. When `refs` reaches 0 the entry is retained, and two conflict rules apply
(`registry.cpp:516-548`): a rejected definition that unloads leaves no conflict behind; when the *kept* definition
unloads, the first still-loaded rejected definition takes its place — its entry becomes the `by_key` slot, its nodes
become linked, and any further rejected definition now conflicts with it (`dlopen_H_conflicting_plugins`). The
replaced entry stays alive. The destructor ends with `mark_stale_locked()`; the next reader publishes.

### Deferred publication and notification

```cpp
void publish_if_stale()
{
    if (!G().stale.load(std::memory_order_acquire))
        return;
    std::vector<registry::change_callback> cbs;
    {
        std::lock_guard lock(G().mu);
        if (!G().stale.load(std::memory_order_relaxed))
            return;
        publish_locked();
        if (!G().exiting.load(std::memory_order_acquire))
            for (const auto& [id, cb] : G().callbacks)
                cbs.push_back(cb);
    }
    for (const auto& cb : cbs)
        cb(registry::instance());
}
```

(`registry.cpp:405-433`.) A mutation does not build an index. `mark_stale_locked()` (`registry.cpp:376-380`) bumps
the version (so `version()`-based observers such as `dictionary_tracker` see every change at once) and sets
`stale`; the first `snapshot()` after it — and every query goes through `snapshot()` — publishes one index for all
the changes since the last one. Publishing at every registration would make *n* types registered before `main()`,
or inside one `dlopen`, build *n* indexes — O(n²) time and memory at startup (400 types: 70 MiB RSS against 3 MiB).
`registry::generations()` counts published indexes; `registry_scale_one_index_for_startup` (128 types),
`dlopen_A_dlclose_keeps_entries` (one generation per `dlopen`) and `dlopen_B_reload_revives_entries` (one per load
and one per unload) check the counts.

This is also where every notification comes from: the thread that publishes copies the callbacks
under the lock and runs them after releasing it, once per generation. A guard cannot know whether it runs under
the loader lock, so it never calls back. `registry::refresh()` (`registry.cpp:591-594`) is `publish_if_stale()`: a program calls it right after a
`dlopen()` or `dlclose()` to publish and be notified on that thread (`tests/dlopen/host.cpp` does). After the exit
hook has set `exiting`, publishing continues but no callback runs. `dlopen_L_two_thread_load_unload` checks that
every published generation notifies exactly once.

### `publish_locked()` — the immutable index

```cpp
auto* const ix = new registry_index;
ix->entries.reserve(G().by_key.size());
for (const auto& [key, e] : G().by_key)
{
    ix->entries.push_back(
        registry_entry{e->nk, e->hash, e->name, e->blob, e->payload, e->image_base, e->refs});
    if (e->refs == 0)
        ++ix->retained;
}
```

(`registry.cpp:335-343`.) One row per key, from the current entry of that key: views into the heap entry, no copy
of blobs or names. The rows are sorted by `(nk, hash)`; then the keys and positions of each kind are
collected into two vectors, already sorted and unique, which `std::flat_map(std::sorted_unique, …)` adopts as they
are (`registry.cpp:323-333`). `registry_index::find()` (`registry.cpp:318-327`) looks the hash up in the flat map of
its kind, so the binary search touches only the contiguous key array. `conflicts` are re-derived from
`G().conflicts` (kept entry's hash, name and image, rejected entry's image). `ix->version` records the version, the
`generations` counter grows, and the pointer is swapped in with `acq_rel`; the previous generation is pushed onto
`retired`, never deleted while the process runs (a reader may still hold it), and freed only by `free_at_exit()`.
`stale` is cleared with release *after* the exchange, so a reader that sees `false` also sees that index; a reader
that still sees `true` takes the mutex, waits for the publish in progress and finds nothing left to do.

### Queries read one generation

```cpp
const registry_index& registry::snapshot() const noexcept
{
    publish_if_stale(); // 2.1.11: the first reader after a change publishes (and notifies, 2.1.13)
    const registry_index* const ix = G().index.load(std::memory_order_acquire);
    return ix ? *ix : g_empty_index;
}
```

(`registry.cpp:584-589`.) Every query is `snapshot()` + a walk or `find()`; nothing else is read. Retained entries
are part of the index: `size()`, `find_type()`, `find_call()`, `find_literal()`, `export_image()`, `to_json()` and
`to_toml()` include them, so a frame written by an unloaded plugin still decodes. `images()` and `program_hash()`
(`registry.cpp:570-586`) count only rows with `refs > 0`, so both return to their earlier values once a plugin is
unloaded. `find_type_by_name()` (`registry.cpp:612-624`) walks the type rows and returns the first loaded match,
else the first retained one: after a plugin is rebuilt with a changed layout, the old and the new `plugin::Tick`
are both in the index, and the loaded one wins. `retained()` is `snapshot().retained`. `export_image()` is
`export_image_since(0)` (`registry.cpp:653-683`), which binds `const registry_index& ix = snapshot()` once and builds
both the RRI header (`n_entries`, `fingerprint64_u64s` over the type hashes) and the body from that same `ix`, so
header and body can never describe different generations. A `since` above 0 selects the entries with
`added > since` and sets `rri_flag_delta`; the header's program hash is always the current one, and scope nodes
(`node_kind::scope`) never appear (they have no ENTRY).

### Dictionary scopes: `export_image_scope()`

`scope_calls(scope_hash)` counts the scope rows whose `scope_desc::scope_hash` matches. `export_image_scope(scope_hash,
since)` (`registry.cpp:696-786`) parses the full image once into a `dyn::dictionary`, collects the calls of the
scope from its scope rows, adds each call's literal arguments and the hashes of its type arguments (and of enum
constants), then follows every FIELD's and element descriptor's `type_hash` to a fixed point; it emits the selected
type, literal and call entries in index order (only those with `added > since` for a delta) under a header whose
program hash is the fingerprint of the *selected* types. It runs when a scoped capture file opens and at each
registry change, never per frame.

### `unload_guard_active()`

A diagnostic helper (`registry.cpp:703-706`) that returns `image::can_unload(addr)`. With the ELF backend that is
the `DT_FLAGS_1 & DF_1_NODELETE` probe (`files/src-image_elf.cpp.md`); with the null backend it is always `false`.
`dlopen_C_rtld_nodelete`, `dlopen_I_plugin_without_flags_unloads` and `link_image_none_backend` use it.

### `serreg_init` — constructor priority 101

```cpp
// Runs when libserreg is loaded, before any guard of the image (priority 101 sorts first in .init_array).
// Environment is read once here; the signal is blocked here so every later thread inherits the mask.
[[gnu::constructor(101)]] void serreg_init()
{
    warn_if_second_copy();
    if (std::getenv(
            "ES_SERREG_ALLOW_DLCLOSE")) // until 2.1.12: unlink mode instead of the FR-R10 abort
        detail::report_line(
            "es::serreg: ES_SERREG_ALLOW_DLCLOSE is ignored since 2.1.13: dlclose() never aborts, "
            "and the unloaded image's entries are retained");
```

(`registry.cpp:837-846`.) Calls `warn_if_second_copy()` (review Q10: one stderr line when another copy of
libserreg is already in the process — chapter 9.1, `registry_second_copy_diagnostic`; the marker lookup is
`image::first_registry_marker()`, which calls the first `es_serreg_registry_marker_v3()` in the global
scope and compares its result with `&own_marker`). Prints one line when `ES_SERREG_ALLOW_DLCLOSE` is set (the
variable is ignored). Reads `ES_SERREG_CONFLICT` (`abort`/`silent`, default warn), `ES_SERREG_DUMP_ON`
(`change`/`both` install a dump callback when a dump target is configured), and `ES_SERREG_DUMP_SIGNAL` (`USR1`,
`USR2`, `RTMIN`, `RTMIN+n` via `parse_signal`). The chosen signal is *blocked* here with `pthread_sigmask`, before
any thread exists, so every later thread inherits the mask and only the helper receives it. Threads that already
existed — libserreg `dlopen`ed into a running multi-threaded program — keep the signal unblocked (the mask is per
thread); chapter 9.8 says to load libserreg before creating threads (review G2-8). The constructor
also starts the dump helper thread (`start_helper_if_configured()`) and registers `exit_hook` with `std::atexit`.
Priority 101 puts it ahead of the
priority-default guards in the same image; when libserreg is a separate DSO it is initialised before its
dependents anyway.

### The exit hook

`exit_hook()` (`registry.cpp:979-992`) sets `exiting` (no callback runs after this), stops the helper thread
(`helper_stop`, `pthread_kill` with the dump signal to wake it, `pthread_join`), runs `dump_if_requested()` if
something was ever registered (`pushed`), and frees the retired indexes. It is registered before any guard of a
program linked against libserreg, so it runs after their destructors: the exit dump sees every entry, retained
ones included. Guard destructors that run after it (images unloaded by `_dl_fini`) drop their references like at
`dlclose`; nothing depends on telling exit from `dlclose`.

### Dump variables and the `sigwait` thread

`do_dump()` (`registry.cpp:855-891`) writes to `ES_SERREG_DUMP_REGISTRY` (a path, or `-` for stdout; default
format `json`) and/or `ES_SERREG_DUMP_REGISTRY_FD` (an inherited descriptor; default format `rri`), format
overridable by `ES_SERREG_DUMP_FORMAT` (`json`, `toml`, anything else = RRI), then `_Exit(0)` if
`ES_SERREG_DUMP_EXIT=1`. `dump_if_requested()` (`registry.cpp:990-995`) runs it at exit unless
`ES_SERREG_DUMP_ON=change`. The helper thread:

```cpp
void* helper_main(void*)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, G().dump_signo);
    for (;;)
    {
        int sig = 0;
        if (sigwait(&set, &sig) != 0)
            continue;
        if (G().helper_stop.load())
            return nullptr;
        do_dump(registry::instance());
    }
}
```

(`registry.cpp:944-958`.) `sigwait` instead of a handler: the dump allocates and does I/O, which is not
async-signal-safe. The exit hook wakes and joins it, so the process never exits with the thread mid-dump
(`signal_dump_usr2`).

### Diagnostics without `printf`

```cpp
detail::report_line("es::serreg: two copies of libserreg in one process ({} and {}): each has "
                    "its own registry; link every image against libserreg.so",
                    image_name(image_of(first)), image_name(image_of(&own_marker)));
```

(`registry.cpp:1005-1007`.) Every stderr line of this file — the schema conflict (`report_conflict`), the
second-copy warning, the `ES_SERREG_ALLOW_DLCLOSE` notice, and the three `ES_SERREG_DUMP_SIGNAL` lines (bad value,
`pthread_sigmask` failure, no dump thread) — goes through `detail::report_line` (`include/es/serreg/sink.hpp`).
These lines run from `push()` inside `dlopen`, from `serreg_init` before `main`, and possibly with stderr closed.
`report_line` formats with `std::format_to_n` into a 1024-byte stack buffer and writes the line with
`detail::write_fd(2, …)`, so it neither allocates nor throws; `std::println` would format into a `std::string` and
throw `std::system_error` on a failed write. The texts are the §11 defaults.

The marker itself:

```cpp
extern "C" [[gnu::visibility("default"), gnu::used, gnu::noinline]] const void*
es_serreg_registry_marker_v3() noexcept
{
    return &es::serreg::own_marker;
}
```

(`registry.cpp:1063-1067`.) One definition per copy of libserreg, always in the dynamic symbol table. It is a
function because the earlier data object `es_serreg_registry_marker_v2`, defined by two copies, is an ODR
violation under AddressSanitizer (chapter 9.1).

## Maintaining it

- **Run the loader tests, not just unit tests.** Any change here must pass `tests/link` (`link_two_tu_dedup`,
  `link_alt_registration_order`, `link_gc_sections`, `link_no_pie_main_image`, `link_lto`, and in Debug builds
  `link_image_none_backend`/`link_image_none_program_hash_equal`) and all of `tests/dlopen` (`dlopen_A`..`dlopen_L`,
  among them `dlopen_B_reload_revives_entries`, `dlopen_E_changed_layout_reload`, `dlopen_H_conflicting_plugins`, and
  `registry_second_copy_diagnostic`), plus `premain_registry_count`, `premain_env_dump`, `signal_dump_usr2`,
  `test_registry.cpp`, `test_conflict.cpp` and the `FR-R9 ES_SERREG_DUMP_REGISTRY_FD` case in `test_capture.cpp`.
- **Keep this file format-neutral**: no `<dlfcn.h>`, `<elf.h>`, `<link.h>`; an executable-format question
  is a new function in `src/image.hpp` implemented by every backend.
- **Never read a node after its guard was destroyed.** Everything a published row points at belongs to a heap
  entry. A new `type_desc`/`call_desc` member that is a pointer or span must be deep-copied in
  `deep_copy()`/`owned_desc::own_fields()`, or `dlopen_A_dlclose_keeps_entries` (and every user of a retained
  descriptor) reads unmapped memory after an unload.
- **Never free an entry or a published index during the run.** Entries live in `G().all`, old indexes in
  `retired`. Memory grows by one entry per distinct `(kind, hash)` definition ever registered — a reload with the
  same layout adds nothing — plus one index generation per change burst that is followed by a read. Reclaiming
  retired indexes (hazard pointers, epochs) is not done.
- **`on_guard_destroyed()` must not allocate, free, abort or call back**: it runs inside `dlclose()` under the
  loader lock. It may only erase from vectors, adjust counters and slots, and call `mark_stale_locked()`.
- **Never lock in a reader, except to publish.** `snapshot()` and everything built on it are the hot path for
  registry-backed lookups; when nothing changed they are one acquire load, no lock, no allocation. Keep
  `publish_if_stale()` the only place a reader takes `G().mu`, and keep the callbacks outside it.
- **Callbacks do not re-enter.** `publish_if_stale()` keeps a `thread_local` depth (`notifying`, `notifying_scope`):
  a callback that pushes a node and reads the registry re-enters the publisher on the notifying thread, which
  publishes and runs no callbacks (debug builds report once via `report_line`). The explicit instantiation
  `validate_read_build<ES_SERREG_VALIDATE_READ>` at the top of the file is the one definition behind the link-time
  fuse of `registry.hpp`.
- **A mutation calls `mark_stale_locked()`, never `publish_locked()`**: publishing per registration is
  O(n²) at startup. `registry_scale_one_index_for_startup` fails if startup publishes more than one index.
- **Never call `image::current`, `image::main_image` (their ELF versions take the loader lock), `dlopen`,
  `dlclose` or a user callback under `G().mu`** — deadlock with the loader lock or with a callback that mutates the
  registry. `main_image()` is primed by `push()` before the lock.
- **Environment is read once, in `serreg_init`** (plus `dump_if_requested`/`do_dump` at the point of use).
  Changing the process environment later has no effect on the policies; tests set variables through ctest
  `ENVIRONMENT` properties.
- **Diagnostic texts are §11 defaults** (`schema conflict: …`); the dlopen host and
  `srdump_elf_conflicting_plugins_message` match them. Changing a word is a test change.
- **Diagnostics use `detail::report_line`**, never `printf`/`puts`/`snprintf` or `std::println`: they run
  in the library constructor and inside `dlopen()`. A line longer than 1023 bytes is truncated.
- **The marker stays a function** (`es_serreg_registry_marker_v3`). An exported data symbol defined by every
  copy is an ODR violation under ASan; run `registry_second_copy_diagnostic` under the ASan/UBSan preset after
  touching it.
- **Scopes in plugins.** A scope node registered by a `dlopen`ed image goes through `make_entry()`'s non-main path,
  and `deep_copy()` knows only type and call payloads: it would read the `scope_desc` as a `call_desc`. Until
  `deep_copy()` handles `node_kind::scope`, scoped sinks are safe only in the main program; add a `tests/dlopen`
  case with a scoped sink when fixing it.
- **Deltas and scopes.** `export_image_scope(h, since)` selects by `added > since`, so a scope call added by a later
  image that names an *older* type (or an older call newly placed in the scope) does not carry that type in the
  delta; keep this in mind when changing the selection, and test a plugin that adds a scoped call over main-program
  types.
- **Wire format:** `export_image()` writes the 40-byte RRI header (`rri_header_size`, `format_major`/`format_minor`,
  `hash_algo_xxh3_64`, `fingerprint64_u64s` of the type hashes, the host ABI tag `fmt::host_abi_tag`) and
  concatenates the compile-time blobs. Any change here is a wire-format change and
  needs the `format_minor` bump, `tests/golden/blobs.cpp`, the `tests/fixtures/golden/orders.rri` and
  `tests/canary/rri_canary.cpp` (`format_canary_rri`) updated together.
- **Threading summary:** `push`/`on_guard_destroyed`/`on_change`/`remove_on_change` — mutex; queries — lock-free
  acquire load of the index pointer (rows, including `payload`, are immutable once published); callbacks run on the
  thread that published; the helper thread only calls `do_dump` (which reads a snapshot); `serreg_init` runs
  single-threaded at load.
