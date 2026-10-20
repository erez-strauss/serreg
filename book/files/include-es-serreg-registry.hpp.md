# `include/es/serreg/registry.hpp`

The process-wide registry (spec §6.3, FR-R1..R12): the `registry` singleton's public API, the node and index
structures it publishes, and — the part that makes the design work — the registration-by-instantiation
machinery (`detail::registrar<T>`, `registrar_lit<S>`, `registrar_call<Ts...>`, `guard`, `touch<T>()`,
`touch_arg<T>()`). A type, literal or call signature enters the registry because `serialize()` *odr-uses* a
static guard whose constructor runs during the static initialisation of the image that contains the
instantiation — before `main()` for the executable, at `dlopen()` time for a DSO. No runtime call, no macro, no
registration list. The two public functions (A1) `register_types<Ts...>()` and
`register_call<Sig...>()` expose the same mechanism to a program that only *reads* a type. This is the **registry layer**; the header declares the API (the heap entries, the published
index, unload handling and the environment hooks are in `src/registry.cpp`, image identity in the image backend
`src/image_elf.cpp`, all in `libserreg`) and it is **reflection-dependent** (`touch_components`
splices member types).

`push()` makes every registration a heap entry (decision A; an entry of the main program *views* its ENTRY blob, name and descriptor in place — the executable is never unmapped — and only other
images' entries are copied), a `dlclose()` only drops the
references of the unloading image's guards, and an entry whose references are all gone is **retained**: it stays in
the index and in lookups. Loading the image again revives the same entries. The header comment
(`registry.hpp:11-17`) states this; `files/src-registry.cpp.md` describes the implementation.

## Depends on / used by

Includes: `es/serreg/blob.hpp` (`descriptor_v`, `call_desc_v`, `call_name_v`, `literal_blob_v`), `es/serreg/error.hpp`,
then `<array>` and `<flat_map>` (`registry_index::lookup`), `<atomic>`, `<cstddef>`, `<cstdint>`,
`<functional>` (`change_callback`), `<memory>`, `<span>`, `<string>`, `<string_view>`, `<vector>` — package headers
before system headers (include rules).

Included by: `serialize.hpp` (`touch_arg`, `registrar_call`), `deserialize.hpp` (same, plus `registry::instance().
find_type/find_call/find_literal` in the resolver and `dispatch`), `capture.hpp` (`export_image`, `version`,
`program_hash`), `serreg.hpp`, `src/registry.cpp` (definitions), `src/cli.cpp`. Every test that checks registration:
`tests/unit/test_registry.cpp`, `test_conflict.cpp`, `test_skip.cpp`, `test_anon_tu.cpp`, `premain_check.cpp`,
`tests/link/*`, `tests/dlopen/*`.

## What it defines

| name | kind | meaning |
|---|---|---|
| `ES_SERREG_API` | macro | `[[gnu::visibility("default")]]` when `ES_SERREG_SHARED`; the one exported class |
| `node_kind` | `enum class : uint8_t` | `type = 1, literal = 3, call = 4` (the ENTRY tag values; struct and enum share `type`), `scope = 5` (a (dictionary scope, call signature) pair; no ENTRY) |
| `scope_desc` | struct | the payload of a scope node: `scope_hash` (fingerprint64 of the scope tag's canonical name), `call_hash`, `scope_name` (`registry.hpp:64-69`) |
| `reg_node` | struct | one per (type \| literal \| call \| scope) per image, constant-initialised in `.data`: `payload, blob, hash, name, nk, entry, image_base, refs, linked`. `entry` points at the node's heap entry, `refs` is unused (kept for layout), and `linked` means "its entry is the registry's current entry of its key" |
| `registry_entry` | struct | one row of the published index: `nk, hash, name, blob, payload, image_base, refs, added` — views into the heap entry (which for the main program views the image itself), valid for the life of the process; `payload` is the main program's own descriptor or the entry's deep copy; `refs` counts loaded registrations, 0 = retained; `added` is the registry version at which the definition appeared, what delta exports select on |
| `conflict` | struct | `schema_hash, name, image_a, image_b` (FR-R4) |
| `registry_index` | struct | immutable, sorted by `(nk, hash)`: `entries, conflicts, version, retained, find(nk, hash), lookup` — `lookup` is one `std::flat_map<uint64_t, uint32_t>` per kind (four: type, literal, call, scope) |
| `registry` | class | the singleton; see below |
| `detail::guard` | struct | ctor `push(n)`, dtor `on_guard_destroyed(n)`; non-copyable |
| `detail::registrar<T>` | struct template (hidden) | `static reg_node node; static const guard g;` for a type |
| `detail::registrar_lit<S>` | struct template (hidden) | the same for a literal (name filled by `push()` from the blob) |
| `detail::registrar_call<Ts...>` | struct template (hidden) | the same for a call signature |
| `detail::scope_name_v<Scope>`, `scope_hash_v<Scope>` | variable templates | the scope tag's canonical name and its `fingerprint64` |
| `detail::registrar_scoped<Scope, Ts...>` | struct template (hidden) | the scope node of a (scope, call signature) pair; key = `fingerprint64_u64s({scope_hash, call_hash})`; odr-used by the serialiser for a scoped sink (`registry.hpp:310-330`) |
| `detail::touch<T>()` | `always_inline` function template | odr-use the guards of `T` and, recursively, of every component type |
| `detail::touch_member<U, I>()`, `touch_components<T>(…)` | helpers | per-member / per-base recursion, skipping `[[=skip]]` members |
| `detail::touch_arg<T>()` | function template | `touch<T>()` for a type, `registrar_lit` for a literal, nothing for form tags, strings and scalars; for a constant `touch_arg` of its value type, which registers the enum of an enum constant; for a container argument (`is_container_arg_v`) `registrar<T>::g` — the transparent type's own entry — then `touch<T>()` for its elements |
| `register_types<Ts...>()` | `always_inline` function template, `noexcept`, public (A1) | `touch_arg<Ts>()...`: registers every `Ts` — class types with their component types, enums, literals (`lit<"…">`), constants (their enum) — without a `serialize` or typed-reader call site; scalars and runtime strings have no entry and are ignored. Empty at run time |
| `register_call<Sig...>()` | `always_inline` function template, `noexcept`, public (A1) | `touch_arg<Sig>()...` plus `registrar_call<Sig...>::g`: registers a full call signature, argument types included, exactly as `deserialize<Sig...>` would |

`registry` public API:

| member | meaning |
|---|---|
| `static registry& instance() noexcept` | the one object (a `constinit` static in `registry.cpp`, no SIOF) |
| `push(reg_node*)`, `on_guard_destroyed(reg_node*)` | population; called from guards only. `push` is `noexcept`, so an allocation failure (the heap copy) terminates (documented, C-12); `on_guard_destroyed` drops the node's reference and never aborts, allocates or frees |
| `snapshot() const noexcept` | the current published `registry_index&` (owning; indexes are never freed before exit) |
| `size()`, `version()`, `conflicts()` | lock-free reads over the snapshot; `size()` includes retained entries; `version()` is an acquire load |
| `program_hash()`, `images()` | over loaded entries only (`refs > 0`): the FR-H3 hash of the loaded type schema hashes, the distinct images with loaded registrations |
| `retained() const noexcept` | the number of entries with `refs == 0` (`registry_index::retained`) |
| `generations() const noexcept` | diagnostic: how many indexes have been published. A program that registers *n* types before `main()` has published one when `main()` first reads, not *n* |
| `find_type(hash)`, `find_call(hash)`, `find_literal(id)`, `has_conflict(hash)` | O(log n) search in the snapshot's `lookup`, no lock. Retained entries are found too. The descriptor pointers are valid for the life of the process (G2-4): the main program's own descriptors, or deep copies held by the heap entry for any other image |
| `find_type_by_name(name)` | a walk over the entries; a loaded entry is preferred over a retained one of the same name (a plugin reloaded with a changed layout) |
| `export_image()` / `export_image(int fd)` / `export_image(string_view path)` | full RRI: header + every blob, retained entries included (FR-R9) |
| `export_image_since(since)` | the entries with `added > since` as an RRI with `rri_flag_delta` (`since` = 0: the full image, what `export_image()` returns); the header's program hash is the current one — a capture file's and a stream's dictionary deltas |
| `export_image_scope(scope_hash, since = 0)` | the dictionary of one scope — the calls registered under `scope_hash`, closed over their argument types (recursively through fields and element descriptors) and literals — as a full image or a delta |
| `scope_calls(scope_hash)` | how many (scope, call) pairs are registered under that scope |
| `to_json()`, `to_toml()` | via a `dyn::dictionary` built from the snapshot |
| `on_change(change_callback)` → id, `remove_on_change(id)` | FR-R10/R11: a callback runs once per published generation, after the publish, outside the lock, on the thread that published; a callback must not register types or publish — one that does is not re-notified (the re-entered publisher publishes and runs no callbacks; debug builds report once); `remove_on_change` returns `false` for an unknown id |
| `detail::validate_read_build<V>()`, `detail::validate_read_fuse` | the link-time fuse for the whole-program macro `ES_SERREG_VALIDATE_READ` (A-16, review B3): every TU holds a `[[gnu::used, gnu::retain]]` static pointer to the function for its own value, `libserreg` defines only its value, so a mismatch is an undefined reference — per TU because a data pointer, unlike the COMDAT guard constructor, is never folded across TUs |
| `refresh() const` | publish pending changes now and run the callbacks on this thread; call it right after a `dlopen()`/`dlclose()` |
| `dump_if_requested()` | the `ES_SERREG_DUMP_*` hooks, explicitly |
| `static unload_guard_active(const void* addr)` | diagnostic: can the image containing `addr` really be unloaded (no `DF_1_NODELETE`, not the main executable)? Answered by the image backend (`image::can_unload`; always `false` with `ES_SERREG_IMAGE_FORMAT=none`). The registry's behaviour does not depend on it |

## How it works

### Registration by odr-use (Option A)

```cpp
template<class T> struct ES_SERREG_HIDDEN registrar
{
    static reg_node node;
    static const guard g;
};
template<class T>
reg_node registrar<T>::node{&descriptor_v<T>, descriptor_v<T>.blob, descriptor_v<T>.schema_hash,
                            descriptor_v<T>.name, node_kind::type, nullptr, nullptr, 0, false};
template<class T> const guard registrar<T>::g{&registrar<T>::node};
```
(`registry.hpp:262`). `node` is constant-initialised (every initialiser is a `constexpr` object or a null), so it
lives in `.data` with no constructor. `g` has a constructor, so the compiler emits a dynamic initialiser *for the
image that instantiates it*; that initialiser runs `registry::instance().push(&node)` during the image's static
initialisation. `serialize_frame` odr-uses `g` (`static_cast<void>(&detail::registrar_call<Sig...>::g)` and, via
`touch_arg`, `&registrar<U>::g`), and odr-using a static data member of a class template instantiates its
definition ([temp.inst]). The expression itself is folded away — at run time nothing happens at the call site.

The storage the guard pushes into is a `constinit` object in `libserreg`, so there is exactly one registry per
process and no static-initialisation-order problem (FR-R3, FR-R8): `premain_registry_count`
(`tests/unit/premain_check.cpp`) reads `registry::instance().size()` in the first member initialiser of
`premain_check::app`, constructed by `main()` before anything else runs, and expects the full count;
`tests/link/main.cpp` (the same pattern in `link_test::app`) expects exactly 5 for three types and two calls across two TUs (dedup: both
TUs instantiate `registrar<Msg>`, but COMDAT folding leaves one `node`/`g` pair per image).

### `touch<T>()`: the recursive walk

```cpp
template<class T> [[gnu::always_inline]] inline void touch()
{
    check_supported<T>(); // FR-S7: the intended diagnostic fires before anything else
    using U = mapped_t<T>;
    if constexpr (is_scalar_v<U>) {}
    else if constexpr (array_traits<U>::is_array) touch<typename array_traits<U>::elem>();
    else if constexpr (std::is_enum_v<U>) static_cast<void>(&registrar<U>::g);
    else if constexpr (is_dyn_member_v<U>)
    { // v2: no entry of its own; the element types (vector/optional element, variant alternatives) register
        ...
    }
    else
    {
        static_cast<void>(&registrar<U>::g);
        touch_components<U>(std::make_index_sequence<bases<U>().size()>{}, std::make_index_sequence<members<U>().size()>{});
    }
}
```
(`registry.hpp:355`). A frame that carries `Order` registers `Order`, its enum members, its nested structs, the
element types of its arrays, the element types of its `vector`/`optional` members, every `variant`
alternative, and a map's key and mapped type or a set's key — each as an entry of its own, so a dictionary is
closed under "types referenced by a FIELD". The `vector_` branch covers every sequence of that shape: the element
of a `std::deque`/`list`/`forward_list`/`stack` member, and for a multimap its `std::pair<K,V>` element type,
which is a dynamic class of its own and registers with its parts (`first`, `second`) through `touch_components` —
`base_count`/`part_count`/`part_t` are the parts layer of `reflect.hpp`, so a `std::pair` or `std::tuple` member
is walked like a class and its part types register (`test_containers.cpp`: the dictionary names the pair type).
`touch_member<U, I>` skips `[[=skip]]` members entirely (cc F4): the skipped member's type is never instantiated,
never registered, never gets a section copy (`srdump_elf_no_skipped_type`). `check_supported<T>()` runs first so
an unsupported member produces the FR-S7 `static_assert` rather than a failure deep inside reflection. The calls
are empty at run time and `always_inline` so `-O1` removes them.

`touch_arg<T>()` (`registry.hpp:331`) is the argument-category dispatcher: a `lit<S>` odr-uses
`registrar_lit<T::str>::g`; form tags, runtime strings and scalars have no entry of their own (their identity is
in the call ENTRY's ARG); a container argument (format 1.6: `is_container_arg_v<T>`, every dyn member kind
but `std::string`) odr-uses `registrar<T>::g` — the entry of its transparent type, `std::vector<i32>` — and then
`touch<T>()` for its element types (`registry.hpp:337-341`), which is how `find_type_by_name("std::vector<i32>")`,
the `es_serreg_dict` section and `register_types<std::vector<int>>()` know it; anything else is a type and goes
through `touch<T>()`. A `constant<V>` forwards
to `touch_arg` of its value type (`registry.hpp:389-405`): a scalar constant registers nothing, an enum constant
registers its enum, so a reader with only the dictionary can name the enumerator.

### Registration without a call site (A1)

```cpp
template<class... Ts> [[gnu::always_inline]] inline void register_types() noexcept
{
    (detail::touch_arg<std::remove_cvref_t<Ts>>(), ...);
}
template<class... Sig> [[gnu::always_inline]] inline void register_call() noexcept
{
    (detail::touch_arg<std::remove_cvref_t<Sig>>(), ...);
    static_cast<void>(&detail::registrar_call<std::remove_cvref_t<Sig>...>::g);
}
```

(`registry.hpp:421-429`.) Everything `serialize()`, `deserialize()` and `dispatch()` register, they register
because they are instantiated. A program that only *reads* a type — through `srdump`, a dictionary-driven walk,
or a typed reader that lives in a plugin loaded later — has no such call site, so without these functions its own
binary does not know the type: `find_type()` returns nothing, the embedded `es_serreg_dict` section and
`export_image()` / `--elf` do not list it. `register_types<Ts...>()` is `touch_arg` over the pack: `Ts` may be class types (their
component types register with them, through the same recursive `touch`), enums, literals (`lit<"…">`) and
constants (their enum); scalars and runtime strings have no entry and are ignored. `register_call<Sig...>()`
registers a full signature, argument types included, exactly as `deserialize<Sig...>()` would — a typed reader
still needs the writer's full signature, and this is how a reader-only binary's dictionary can carry it. Both
are empty at run time: the registration happened during the static initialisation of the image that instantiated
them, so the call may sit in a function that is never called:

```cpp
void reader_only_code()
{ // never called: the instantiation registers, as serialize() would (2.1.16, A1)
    es::serreg::register_types<NeverSerialised, Flag, es::serreg::lit<"reader-only">>();
    es::serreg::register_call<es::serreg::lit<"ro">, NeverSerialised, es::serreg::constant<7>>();
}
```

(`test_registry.cpp:128-133`.) Dictionary-driven readers need no registration at all (they take the capture's
dictionary); the functions are for the cases where *this* binary's registry must know a type it never writes. The
test "2.1.16 A1 register_types / register_call register without a serialize call site" checks `find_type` for the
type, its component and the enum, `find_literal`, `find_call` with the constant's value, and that
`export_image()` lists them.

### Hidden visibility and per-image copies

`registrar*` and every constant they point at are `ES_SERREG_HIDDEN`. Consequences: (1) each image that
serialises `Order` has its own `node`, so the same logical entry arrives once per image — `push()` attaches every
such node to one heap entry, whose `refs` counts them; (2) no symbol is `STB_GNU_UNIQUE`, so glibc does not mark
the DSO `DF_1_NODELETE` and `dlclose` really unloads it. The library target also adds `-fno-gnu-unique`. This
matters for reloading: an image that glibc keeps mapped does not run its constructors again on the next
`dlopen`, so a rebuilt plugin would not register its new types. A plugin built without the flags
(`dlopen_I_plugin_without_flags_unloads`) still unloads because of the attributes alone.

### What `push()` does (in `src/registry.cpp`)

An entry first registered by the main program (DECISIONS G-5) views its ENTRY blob and name in place
(`make_entry`: `n->image_base == main_base`), as it already viewed the main program's descriptor; only entries of
other images get `blob_copy`/`name_copy` and a deep-copied descriptor. Viewing instead of copying halved `push` time and saved one
blob-size of RSS (`tests/bench/registry_measure.cpp`). Every new definition records `added`, the version its
publication will carry.

For a literal it points `name` at the text inside the blob (`literal_text_offset`); it resolves `image_base` with
`image::current()` on the payload/blob pointer (ELF: `dladdr`; outside the lock — it takes the loader lock); then
under the registry mutex it looks up the current entry of the same `(nk, hash)` in a hash map (`state::by_key`,
C-01):

| incoming node vs. current entry of its key | action | entry `refs` | node `linked` |
|---|---|---|---|
| no entry | a new heap entry (main program: views of its blob, name and descriptor; any other image: copies and a deep-copied descriptor) | 1 | true |
| same definition (same layout for a type; always for a literal or call) | attach to the entry (an alias, or a retained entry revived — its `image_base` becomes the new image) | +1 | true |
| same schema hash, different layout, current entry retained | a new entry replaces it in `by_key`; the retained one stays alive for pointers already handed out | 1 | true |
| same schema hash, different layout, current entry loaded (FR-R4) | a new entry for the rejected definition, recorded in `conflicts`, reported to `stderr` (`ES_SERREG_CONFLICT=abort` aborts) | 1 (its own) | false |

Every outcome bumps the version and marks the index stale; the next reader publishes it and runs the
change callbacks. Readers acquire-load an immutable `registry_index` of views into the heap entries —
that is why `find_*` never hand out a pointer into an image that could be unloaded (FR-R6/R11). Retired indexes are
kept until exit.

`push()` is declared `noexcept` although it allocates (the heap entry, the descriptor copy, the hash-map slot). The
declaration's comment says why (C-12): it runs during the static initialisation of
an image, where an allocation failure has no caller to report to, so a `std::bad_alloc` there terminates the
program.

### Unload, exit and reload (decision A)

`guard::~guard` runs when the image's static destructors run — at `dlclose` and at process exit. The
registry does not tell these apart: `on_guard_destroyed()` removes the node from its entry's `nodes`, decrements
`refs`, and marks the index stale. It neither aborts, allocates nor frees, because it runs inside `dlclose()` under
the loader lock. An entry with `refs == 0` is retained: it stays in `size()`, `find_*`, `export_image()` and the
JSON/TOML views, and frames written while the image was loaded still decode; `images()` and `program_hash()` no
longer count it. When the kept definition of a conflict unloads, a still-loaded conflicting definition takes its
place in `by_key` and the conflict ends (`dlopen_H_conflicting_plugins`). A reload with the same layout revives the
retained entry (`dlopen_B_reload_revives_entries`: 50 cycles, no growth); a rebuilt plugin whose type changed has a
new schema hash and adds a new entry next to the retained one (`dlopen_E_changed_layout_reload`).

An earlier design interposed `dlopen()`/`dlclose()` to tell an unload from exit, aborted on an unload (FR-R10), and
removed the entries under `ES_SERREG_ALLOW_DLCLOSE=1` (unlink mode); retaining entries needs none of that, and
`ES_SERREG_ALLOW_DLCLOSE` is ignored with one line on stderr at startup.
`registry::unload_guard_active()` remains as a diagnostic: it says whether the image can be unloaded at all.

### Versioning and change notification

`version_` is bumped on every mutation (at the mutation, before the index is published); `version()`
is one acquire load, which is what `dictionary_tracker::update` and a capture file's per-frame check poll (a
capture file writes a delta before the next frame when the version moved). Callbacks run when an index is published: the reader that publishes the next generation copies the callbacks out
of the lock and runs them once, on its own thread, after the publish. libserreg does not see `dlopen()`/`dlclose()`,
so a guard cannot know whether it runs under the loader lock; deferring to the next reader keeps callbacks out of
it unless that reader is itself inside a constructor. A program that wants to be told right after a `dlopen()` or
`dlclose()` calls `refresh()` on that thread (the dlopen host does). Every change notifies, including an alias-only
one (G2-10, `dlopen_K_alias_only_plugin_notifies`). `on_change()` returns an id; a callback whose code lives in a
plugin must be removed with `remove_on_change(id)` before that plugin unloads. No callback runs once the exit hook
has started. Callbacks receive `const registry&` and must not `dlopen`/`dlclose`.

### Descriptor lifetime (review G2-4)

The header comment (`registry.hpp:23-25`) states the contract. Heap entries, their descriptor copies and every
published index live until exit. `type_desc*` and `call_desc*` (`find_*`, `record_view::desc`,
`registry_entry::payload`) are the main program's own descriptors (`&descriptor_v<T>`, `&call_desc_v<Ts...>`) for
entries it registered first — the main program never unloads — and deep copies held by the heap entry for
entries registered first by any other image: names, fields, nested element descriptors, enumerators, gap and bool
tables, call arguments and the blob (`owned_desc` in `src/registry.cpp`). The copy is made once, in
`push()`, when the entry is created. A pointer
obtained before a `dlclose` stays readable after it and is what lookups still return
(`dlopen_A_dlclose_keeps_entries` checks the descriptor, its names, fields and blob). The copy describes the same
layout, but it is a different object from the plugin's `descriptor_v<T>`: compare descriptors by `schema_hash`,
not by address, across images.

### The published index

```cpp
struct registry_index
{
    std::vector<registry_entry> entries;
    std::vector<conflict> conflicts;
    std::uint64_t version = 0;
    std::size_t retained = 0; // entries with refs == 0 (2.1.13)
    const registry_entry* find(node_kind nk, std::uint64_t hash) const noexcept;
    // 2.1.13: per kind (type, literal, call), hash -> position in `entries`; the search touches only the keys
    std::array<std::flat_map<std::uint64_t, std::uint32_t>, 4> lookup; // type, literal, call, scope
};
```
(`registry.hpp:112-121`). `publish_locked()` (called by the first reader after a change) builds a fresh
index from the current entries — one `registry_entry` of views per key, no copy of blobs or names —
sorts it by `(nk, hash)`, fills `lookup` (each `flat_map` adopts the already sorted keys and positions), stores it
with release semantics and retires the previous one to a list that is freed at exit — never earlier, so a
`const registry_index&` obtained from `snapshot()` stays valid for the life of the process. `find(nk, hash)` searches
the flat map of that kind: the binary search touches the contiguous key array only. `find_type`/`find_call`/
`find_literal` are `snapshot().find(nk, hash)` plus a cast of `payload`; `find_literal` returns the entry's `name`.

### Dictionary scopes and deltas

```cpp
template<class Scope, class... Ts> struct ES_SERREG_HIDDEN registrar_scoped
{
    static constexpr scope_desc desc{scope_hash_v<Scope>, call_desc_v<Ts...>.hash,
                                     scope_name_v<Scope>};
    static constexpr std::uint64_t key = fingerprint64_u64s(
        std::array<std::uint64_t, 2>{scope_hash_v<Scope>, call_desc_v<Ts...>.hash});
    static reg_node node;
    static const guard g;
};
```
(`registry.hpp:310-318`.) A sink type that declares `using dictionary_scope = Tag;` (`scoped_sink`, `sink.hpp`) makes
`serialize_frame` odr-use `registrar_scoped<Tag, Sig...>::g` next to `registrar_call<Sig...>::g`. The node is
pushed like every other (one per image, COMDAT-folded, nothing per message), lands in the index with
`nk == node_kind::scope`, and has no ENTRY: it is not in the ELF section and not in `export_image()`.
`export_image_scope(scope_hash, since)` (`src/registry.cpp`) collects the calls of that scope, adds their literal
arguments and the type hashes of their type and enum-constant arguments, follows every FIELD's and element
descriptor's `type_hash` to a fixed point, and emits the selected entries' blobs (only those with `added > since`
for a delta). `scoped_capture_file<Tag>` and the logger's default `dictionary_policy::scoped` use it, so a log file's
dictionary holds the calls it logs, not the program's thousands of message types. The writer-info item
`dictionary = scoped:<scope_name_v<Tag>>` names the scope.

`export_image_since(since)` is the delta of a capture file or a stream after a `dlopen`: every entry whose
`added` is above the version of the last dictionary written, with `rri_flag_delta` set in the RRI header. Readers
merge deltas into what they already have.

### Environment hooks (compiled side, for orientation)

| variable | effect |
|---|---|
| `ES_SERREG_DUMP_REGISTRY=<path>` or `-` | write the registry at exit (or on `dump_if_requested()`) as a file / to stdout |
| `ES_SERREG_DUMP_REGISTRY_FD=<n>` | the same into an open descriptor (FR-R9; `tests/unit/test_capture.cpp` pipes it) |
| `ES_SERREG_DUMP_FORMAT=rri\|json\|toml` | dump format (default `json` for a path, `rri` for a descriptor; anything else renders the RRI) |
| `ES_SERREG_DUMP_ON=exit\|change\|both` | when (default `exit`; the spec's `init` is `exit` in practice) |
| `ES_SERREG_DUMP_EXIT=1` | `_Exit(0)` right after the dump |
| `ES_SERREG_DUMP_SIGNAL=USR1\|USR2\|RTMIN+n` | a helper thread `sigwait`s and dumps on the signal (no polling; `signal_dump_usr2` ctest) |
| `ES_SERREG_CONFLICT=abort\|silent` | abort on an FR-R4 conflict, or say nothing, instead of reporting it |
| `ES_SERREG_ALLOW_DLCLOSE` | ignored (one stderr line at startup); it once selected an unlink mode |

All are read once, by the priority-101 constructor `serreg_init` (the dump variables again at the point of use),
which is during static initialisation — they must be set before the process starts, not from `main`.

### Timeline of one process

1. Loader maps the executable and `libserreg.so`; `registry_state::inst` is `constinit` — no constructor runs.
   libserreg's constructor `serreg_init` reads the environment, starts the dump helper if configured and registers
   the exit hook (there, not on the first push).
2. Each image's dynamic initialisers run; every `registrar*<…>::g` constructor calls `push()`, which creates or
   attaches to a heap entry and marks the index stale (nothing is built yet).
3. `main()` starts; `registry::instance().size()` is already final for the executable
   (`premain_registry_count`); that first read publishes the one index of the startup
   (`registry_scale_one_index_for_startup`).
4. `dlopen("plugin.so")`: the plugin's guards push; new entries, aliases or revived retained entries; `version()`
   moves; the next read (or `refresh()`) publishes and notifies once.
5. `dlclose(plugin)`: the plugin's guard destructors drop their references; its entries are retained; the next
   read publishes and notifies once. Descriptor pointers obtained earlier stay readable and are still returned.
6. `exit()`: the exit hook sets `exiting` (no more callbacks), stops the helper, runs the dump if requested and
   something was registered, frees the retired indexes; guard destructors of the remaining images drop their
   references like at `dlclose`.

### Thread safety summary

| operation | synchronisation |
|---|---|
| `find_*`, `snapshot()`, `size()`, `version()`, `program_hash()`, `conflicts()`, `images()`, `retained()`, `export_image()` | lock-free: one acquire load of the `stale` flag and one of the index pointer; the first read after a mutation takes the mutex once, publishes and runs the callbacks outside it |
| `push()`, `on_guard_destroyed()` | the registry mutex (a `std::mutex`); the image backend's `current`/`main_image` (ELF: `dladdr`, `dl_iterate_phdr`) outside it (G2-5) |
| `on_change`, `remove_on_change` | the mutex; invocation outside it |
| `dump_if_requested()` | reads a snapshot; writes through `tools::write_all` |

Concurrent readers during `dlclose` churn are exercised by `dlopen_J_readers_vs_dlclose_churn`, two threads
loading and unloading by `dlopen_L_two_thread_load_unload`.

## Maintaining it

- **Never add a runtime registration path.** The whole design (no lock, no allocation, no registry access on the
  hot path; complete dictionary before `main`) rests on guards instantiated from `serialize`/`deserialize`. If a
  type must be registered without being serialised, name it in `register_types<T>()` from a TU of that
  image — that is still an instantiation, not a call.
- **`reg_node` is positionally initialised** in three places in this header; add members at the end and give them
  a trailing initialiser in all three. Keep it constant-initialisable (no constructors, no `std::string`). `refs`
  is unused but stays, so the layout of nodes compiled into older plugins does not change.
- **Keep `registrar*` hidden and the `-fno-gnu-unique` flag PUBLIC** on the target; removing either makes glibc
  keep DSOs mapped (`DF_1_NODELETE`), and a reloaded plugin then does not run its constructors again (cc F12).
- **`touch<T>()` must stay in sync with `reflect.hpp`'s notion of components**: `is_dynamic`/`dyn_member`
  (element types, map keys and values, and variant alternatives register), `member_skipped`, `array_traits`. A type that a FIELD
  references by `type_hash` but that `touch` does not register leaves a dangling hash in every dictionary
  (`tests/unit/test_dynamic.cpp` "2.0 registry: dynamic types and their element types are registered before
  main()", `test_skip.cpp` "cc F4 a skipped nested struct type is absent from the registry and the descriptor").
- **When touching `registry.*` run `tests/link` and `tests/dlopen`** (`dlopen_A`..`dlopen_L`) and, for
  `src/image_*.cpp`, the `link_image_none_*` tests, not just unit tests — the ELF/loader interactions (`dladdr`,
  `dl_iterate_phdr` for the main image, PIE vs `-no-pie` in `link_no_pie_main_image`, LTO in `link_lto`,
  `--gc-sections`, unload and reload) are not exercised by the unit binary.
- `program_hash()` is `fingerprint64_u64s` over the sorted schema hashes of loaded *type* entries; `tests/link`
  compares it across differently ordered link lines (`link_program_hash_order_independent`, `_reversed_equal`) and
  `golden_srdump_program_hash_elf_agrees` compares it with the ELF section's; `dlopen_A_dlclose_keeps_entries`
  checks that it returns to its value before the `dlopen` once the plugin is unloaded.
- `export_image()` reads one index generation (consistent snapshot); its byte output feeds `capture_file`,
  `write_dictionary_frame`, `ES_SERREG_DUMP_REGISTRY[_FD]` and the golden `orders.rri` fixture — the RRI header
  minor it writes is `fmt::format_minor`.
- The conflict diagnostic text is a §11 default; `srdump_elf_conflicting_plugins_message` and the dlopen host match
  on it.
- Tests: `tests/unit/test_registry.cpp` (FR-R1/R2 every entity once, FR-R5 lookups, FR-H3 program hash, FR-R9/D3
  export round trip, FR-D2 `to_json`, FR-R3/R10 version stable + `on_change` silent at run time),
  `test_conflict.cpp` (FR-R4), `test_anon_tu.cpp` (`corpus::Local`, an explicitly named anonymous-namespace type), `test_empty_member.cpp`, `premain_check.cpp`,
  `signal_dump_test.cpp` (`ES_SERREG_DUMP_SIGNAL`), `tests/link/*`, `tests/dlopen/*` (A–L, and
  `registry_second_copy_diagnostic`), the `srdump_elf_*` tests.
