# es::serreg — the registry, entry by entry

> **Companion:** `registry-services.md` explains the same material by topic (concepts first, with what the library does not use and why); this document is the per-function reference.

*Library 2.2.5. Line numbers refer to the tree as of 2026-10-19. The compile-time half of the registry is in the
headers `include/es/serreg/registry.hpp`, `blob.hpp`, `literal.hpp`, `detail/embed_elf.hpp` and `descriptor.hpp`.
The run-time half is in `src/registry.cpp` (inside `libserreg.so`) and in the image backend `src/image_elf.cpp`
(`src/image_none.cpp` for `ES_SERREG_IMAGE_FORMAT=none`). The reflection that produces each entry's content is
described in [`serreg-reflection.md`](serreg-reflection.md). The original request listed reflection items only,
so every entry below was chosen for this document: the registry has no "additional" marking.*

## How to read the "Context" line

| Context | Meaning |
|---|---|
| **consteval** | An immediate function: every call is evaluated by the compiler. |
| **compile-time constant** | A `constexpr` variable, a constant-initialised static object, a class template or a macro. Its value is fixed when the program is built. |
| **static initialisation** | Code that runs when an image (the executable or a shared object) is loaded: before `main()` for the executable, inside `dlopen()` for a plugin. Destructors of the same objects run at `dlclose()` or at exit. |
| **run time** | Ordinary code, run when the program calls it. |

## Summary

| Entry | Where | Context |
|---|---|---|
| `node_kind` | `registry.hpp:52` | compile-time constant (enum) |
| `reg_node` | `registry.hpp:75` | compile-time constant (constant-initialised) |
| `guard` | `registry.hpp:220` | static initialisation |
| `registrar<T>` | `registry.hpp:235–250` | static initialisation |
| `registrar_lit<S>` | `registry.hpp:252–261` | static initialisation |
| `registrar_call<Ts...>` | `registry.hpp:263–278` | static initialisation |
| `registrar_scoped<Scope, Ts...>`, `scope_desc`, `scope_name_v`, `scope_hash_v` | `registry.hpp:288–308, 65, 285, 287` | static initialisation / constants |
| `touch<T>()` | `registry.hpp:313` (decl), `328` (def) | run time (empty); consteval checks inside |
| `touch_member<U, I>()`, `touch_components<T>()` | `registry.hpp:315, 321` | run time (empty) |
| `touch_arg<T>()` | `registry.hpp:367` | run time (empty) |
| the odr-use in `serialize_frame()` | `serialize.hpp:386–389` | run time (empty) |
| `register_types<Ts...>()`, `register_call<Sig...>()` | `registry.hpp:399, 403` | run time (empty) |
| `descriptor_v<T>`, `make_type_desc<T>()` | `blob.hpp:52, 29` | consteval / compile-time constant |
| `call_desc_v`, `call_hash_v`, `call_name_v`, `make_call_desc` | `blob.hpp:61, 55, 58, 36` | consteval / compile-time constant |
| `literal_blob_v<S>`, `lit<S>::id` | `blob.hpp:64`, `literal.hpp:62` | compile-time constant |
| `ES_SERREG_DICT_SECTION`, `type_blob_copy_v`, `embed_type_blob<T>()` | `detail/embed_elf.hpp:14, 20, 28` | compile-time constant / consteval |
| `ES_SERREG_HIDDEN`, `ES_SERREG_API`, `-fno-gnu-unique` | `descriptor.hpp:13`, `registry.hpp:44` | build-time attributes |
| `class registry`, `registry::instance()` | `registry.hpp:127`, `registry.cpp:291–304` | run time; the object is `constinit` |
| `state`, `G()` | `registry.cpp:226–256` | run time (created on first use) |
| `entry`, `make_entry()` | `registry.cpp:163, 182` | run time |
| `registry::push(n)` | `registry.cpp:437` | static initialisation |
| `registry::on_guard_destroyed(n)` | `registry.cpp:488` | static de-initialisation / `dlclose` |
| `registry_entry`, `registry_index`, `registry_index::find` | `registry.hpp:91, 116`, `registry.cpp:307` | run time |
| `mark_stale_locked()`, `publish_locked()`, `publish_if_stale()`, `registry::snapshot()` | `registry.cpp:365, 322, 373, 540` | run time |
| `find_type`, `find_type_by_name`, `find_call`, `find_literal`, `has_conflict` | `registry.cpp:575–611` | run time |
| `registry::version()`, `generations()`, `size()` | `registry.hpp:148, 144`, `registry.cpp:827` | run time |
| `export_image()`, `export_image_since(v)` | `registry.cpp:614, 622` | run time |
| `export_image_scope(scope, since)`, `scope_calls(scope)` | `registry.cpp:665, 655` | run time |
| `program_hash()`, `images()`, `retained()` | `registry.cpp:566, 557, 552` | run time |
| `on_change`, `remove_on_change`, `refresh` | `registry.cpp:813, 821, 547` | run time |
| `conflict`, `conflict_policy`, `report_conflict()` | `registry.hpp:105`, `registry.cpp:67, 399` | run time |
| `image::current`, `main_image`, `name`, `can_unload`, `unload_guard_active` | `image_elf.cpp:34, 42, 53, 61`, `registry.cpp:832` | run time |
| `serreg_init()`, `exit_hook()`, `warn_if_second_copy()` | `registry.cpp` (end of file) | library load / exit |
| `ES_SERREG_DUMP_*`, `dump_if_requested()`, `to_json()`, `to_toml()` | `registry.cpp:841–1014, 804, 808` | run time |

---

## node_kind

**Where:** `enum class node_kind : std::uint8_t` at `registry.hpp:52`. **Context:** compile-time constant (enum),
used at run time.

`node_kind` says what a registration describes: `type` (1, a struct or enum with a `type_desc`), `literal` (3, a
compile-time string), `call` (4, a call signature with a `call_desc`) or `scope` (5: a (dictionary
scope, call signature) pair). The registry keys every entry by `(node_kind, hash)`, so a schema hash, a literal id
and a call hash with the same 64-bit value cannot collide. The published index keeps one lookup table per kind.

## reg_node

**Where:** `struct reg_node` at `registry.hpp:75–89`. **Context:** compile-time constant. Each node is a static
data member whose initialiser is a constant expression, so it is constant-initialised in `.data` before any code
runs. `push()` fills its last fields at load time.

A `reg_node` is the registration record one image contributes for one type, literal, call or scope. It holds:
- `payload`: a `const type_desc*`, `const call_desc*`, `scope_desc*` or null
- `blob`: the ENTRY bytes, in the image's own `.rodata`
- `hash`: schema hash, literal id or call hash
- `name`: canonical name, literal text or call string
- `nk`: the `node_kind`

Three fields are set by `push()`: `entry` (the heap entry this node references), `image_base` (from `dladdr`) and
`linked` (whether its entry is the key's current definition). There is one node per (entity, image), because the
registrar templates are hidden. The registry views the main program's nodes in place and copies a plugin's.

## guard

**Where:** `struct guard` at `registry.hpp:220–233`. **Context:** static initialisation. Its constructor runs
when the image is loaded and its destructor when the image is unloaded or the process exits.

`guard` is the object whose construction is the registration. Its constructor calls
`registry::instance().push(n)` and its destructor calls `registry::instance().on_guard_destroyed(n_)`. It holds
only the node pointer and is neither copyable nor assignable. Every registrar has a `static const guard g`, and
odr-using `g` is what makes a type register: that is the whole mechanism of "registration by instantiation".

## registrar\<T\>

**Where:** `template<class T> struct ES_SERREG_HIDDEN registrar` at `registry.hpp:235`; the definitions of `node`
at 240–249 and `g` at 250. **Context:** static initialisation.

`registrar<T>` pairs a constant-initialised `reg_node node` with `&descriptor_v<T>`, the ENTRY blob, the schema
hash and the canonical name, and a `const guard g` that pushes it. Both are static data members of a class
template. By [temp.inst], their definitions are instantiated only where something odr-uses them, and
`static_cast<void>(&registrar<U>::g)` inside `touch<T>()` does exactly that. A program therefore registers precisely
the types it can serialise or read. The dynamic initialisation of class-template statics is unordered, so the
registry is written not to depend on the order. `ES_SERREG_HIDDEN` gives each image its own copy, which is what
lets the registry count references per image.

## registrar_lit\<S\>

**Where:** `registry.hpp:252–261`. **Context:** static initialisation.

The same pattern for a literal `lit<S>`: the node has no payload, its blob is `literal_blob_v<S>` and its hash is
`lit<S>::id`. Its name is filled by `push()`, which views the text inside the ENTRY blob, so there is only one
static copy of the text per image. It is odr-used from `touch_arg` when an argument is a literal, which is why a
literal must be in the type (`"text"_`, `lit<"text">`): a run-time string can never reach static initialisation.

## registrar_call\<Ts...\>

**Where:** `registry.hpp:263–278`. **Context:** static initialisation.

One node per call signature: `call_desc_v<Ts...>` as payload, the call ENTRY blob, `call_desc_v<Ts...>.hash` and
`call_name_v<Ts...>`. `serialize_frame()` odr-uses `registrar_call<Sig...>::g` for the exact argument categories of
the call, so the dictionary holds every frame id the program can write. `deserialize` and `dispatch` do the same
for what they read (`deserialize.hpp:749–750`).

## registrar_scoped\<Scope, Ts...\>, scope_desc, scope_name_v, scope_hash_v

**Where:** `registry.hpp:288–308`; `struct scope_desc` at 65; `scope_name_v` at 285 and `scope_hash_v` at 287.
**Context:** static initialisation; `scope_name_v` and `scope_hash_v` are compile-time constants.

A sink type that declares `using dictionary_scope = Tag;` (the `scoped_sink` concept, for example
`scoped_capture_file<Tag>`) makes `serialize()` also odr-use `registrar_scoped<Tag, Sig...>::g`. That registers a
`scope` node whose payload `scope_desc { scope_hash, call_hash, scope_name }` records that this call can be
written into that kind of sink. The node has no ENTRY, so it is not in the ELF section. `export_image_scope()`
later builds a file's dictionary from exactly those calls and the types they reach: a log file carries its forty
format strings, not the program's four thousand types. The cost is one node per (scope, signature) per image and
nothing per message.

## touch\<T\>()

**Where:** declared at `registry.hpp:313`, defined at 328–365. Called from `touch_arg` (367), `register_types`,
`serialize_raw` (`serialize.hpp:516`) and `deserialize_raw` (`deserialize.hpp:828`). **Context:** run time, but
the body compiles to nothing. Its only effects are compile-time ones: it calls the consteval `check_supported<T>()`
and odr-uses guards.

`touch<T>()` registers `T` and every type reachable from it. It first runs `check_supported<T>()`, so an
unsupported type is reported with the library's message before anything else. Then, on `mapped_t<T>`:
- scalars stop
- arrays recurse into the element
- enums odr-use `registrar<U>::g`
- dyn members recurse into their element (every `vector`-shaped container included: `std::deque`, `std::list`,
  `std::forward_list`, `std::stack`, and the `std::pair<K, V>` entry of a multimap), variant alternatives, or key
  and mapped type, but get no entry of their own
- classes, tuple-likes included, odr-use `registrar<U>::g` and recurse into bases and parts through
  `touch_components`

The resulting dictionary is complete by construction and only as large as what the program uses.
`[[gnu::always_inline]]` and the empty bodies make every call fold away at `-O1`.

## touch_member\<U, I\>(), touch_components\<T\>()

**Where:** `registry.hpp:315–319` and `320–326`; the index sequences are built in `touch<T>()` at 362–363.
**Context:** run time (empty bodies; compile-time effect only).

`touch_components<T>(B..., M...)` receives two index sequences, `std::make_index_sequence<base_count<U>()>` and
`std::make_index_sequence<part_count<U>()>` from the parts layer of `reflect.hpp`, and calls `touch` on each base
type (`base_t<T, B>`) and `touch_member` on each part. `touch_member<U, I>` skips a `[[=skip]]` member
(`member_skipped<U, I>()`), so a skipped member's type never enters the registry or the ELF section, and calls
`touch<part_t<U, I>>()` for the others. Because the parts layer presents a `std::pair` or `std::tuple` as a class
with positional parts, their element types register through the same two functions.

## touch_arg\<T\>()

**Where:** `registry.hpp:367–383`. Called from `serialize_frame` (`serialize.hpp:386`), `deserialize`
(`deserialize.hpp:749`), `dispatch` (952) and `register_types`/`register_call`. **Context:** run time (empty).

It handles one argument category of a call:
- a literal odr-uses `registrar_lit<T::str>::g`
- a compile-time constant registers the enum it stands for, if any
- a container argument (format 1.6) odr-uses its own transparent `registrar<T>::g` and then `touch<T>()`
- form tags, run-time strings and scalars register nothing, because the call ENTRY names them by kind
- anything else is `touch<T>()`

## the odr-use in serialize_frame()

**Where:** `serialize.hpp:383–389`. **Context:** run time (these lines compile to nothing).

```cpp
(detail::touch_arg<Sig>(), ...);                         // every argument type, recursively
static_cast<void>(&detail::registrar_call<Sig...>::g);   // the call signature itself
if constexpr (scoped_sink<S>)                            // the (scope, signature) node
    static_cast<void>(&detail::registrar_scoped<typename S::dictionary_scope, Sig...>::g);
```

These three statements connect `serialize()` to the registry. Because they are in the template body, any
instantiation of `serialize(out, …)` registers its types, literals and call signature, even if that line of code
never runs. That is how a capture file's header can hold the full dictionary before the first frame.

## register_types\<Ts...\>(), register_call\<Sig...\>()

**Where:** `registry.hpp:399–407`. **Context:** run time (empty).

These register types, or a full call signature, where nothing serialises them: a program that
only reads a type through a dictionary-driven tool, or a typed reader that lives in a plugin loaded later. They
apply `touch_arg` to each argument and, for `register_call`, odr-use `registrar_call<Sig...>::g`. As with every
registration, the work happens at image load; the call itself does nothing.

## descriptor_v\<T\>, make_type_desc\<T\>()

**Where:** `make_type_desc` at `blob.hpp:29–35` (consteval); `descriptor_v` at `blob.hpp:52–53`
(`ES_SERREG_HIDDEN inline constexpr type_desc`). **Context:** consteval / compile-time constant.

`descriptor_v<T>` is the complete compile-time description of a type: `make_type_desc_base<T>()` from
`reflect.hpp` plus `blob = type_blob_bytes_v<T>`, the type's serialised ENTRY. It lives in `.rodata`, is what
`registrar<T>::node.payload` points at, and is what `registry::find_type()` returns for the main program.
`make_type_desc` also calls `embed_type_blob<T>()`, which instantiates this translation unit's copy of the blob in
the `es_serreg_dict` section.

## call_desc_v, call_hash_v, call_name_v, make_call_desc

**Where:** `blob.hpp:36–41` (`make_call_desc`, consteval), 55 (`call_hash_v`), 58 (`call_name_v`), 61
(`call_desc_v`). **Context:** consteval / compile-time constants.

`call_hash_v<Ts...>` is `fingerprint64(call_string<Ts...>())`, the 8 bytes in every frame header of that call.
`call_name_v` is the call string itself (`call(2){lit:0x…,0x…}`) as a static string. `call_desc_v` holds the hash,
the argument list, the call ENTRY blob and the flags (print, newline, log). These constants are the payload, blob,
hash and name of the call's `reg_node`.

## literal_blob_v\<S\>, lit\<S\>::id

**Where:** `blob.hpp:64` (`literal_blob_v`, via `literal_blob_span<S>()` at 42); `lit<S>::id` at
`literal.hpp:62`. **Context:** compile-time constants.

`lit<S>::id` is the XXH3-64 of the literal's text, the identity of the literal. `literal_blob_v<S>` is the literal
ENTRY (`u64 id, u32 len, u32 rsv, text`), built by consteval code in `literal.hpp`. The blob is the only static
copy of the text in the image: `lit<S>::text()` and the registry both view it. Making the span also instantiates
the section copy.

## ES_SERREG_DICT_SECTION, type_blob_copy_v, embed_type_blob\<T\>()

**Where:** `detail/embed_elf.hpp:14` (macro), 20, 22 and 25 (the type, literal and call copy templates), 28–39 (the `embed_*`
hooks). **Context:** compile-time constants placed in a named section; the hooks are consteval.

`ES_SERREG_DICT_SECTION` is `[[gnu::section("es_serreg_dict"), gnu::used, gnu::retain]]`. Each
translation unit that describes a type, literal or call gets a `static constexpr` copy of the ENTRY blob in that
section. `gnu::used` keeps it although nothing references it, and `gnu::retain` keeps it under `--gc-sections`
(binutils ≥ 2.36). The copies are TU-local on purpose: GCC puts every COMDAT variable naming a custom section into
a single section group, which breaks multi-TU links. Duplicates are harmless because readers key by hash. The
consteval `embed_type_blob<T>()` only takes the address of `type_blob_copy_v<T>`, which instantiates it. The
result is that `srdump --elf ./app` reads the dictionary of a binary that has never run.
`ES_SERREG_IMAGE_FORMAT=none` swaps in `embed_none.hpp`, where the hooks are empty.

## ES_SERREG_HIDDEN, ES_SERREG_API, -fno-gnu-unique

**Where:** `ES_SERREG_HIDDEN` at `descriptor.hpp:13`, `ES_SERREG_API` at `registry.hpp:44`; `-fno-gnu-unique` is
a PUBLIC compile option of the CMake target. **Context:** build-time attributes.

The registrars, descriptors and blobs are `[[gnu::visibility("hidden")]]`, so every image keeps its own copies.
That is the registry's per-image reference-counting model, and it avoids `STB_GNU_UNIQUE` symbols: glibc marks a
DSO holding one `DF_1_NODELETE`, which makes `dlclose` a no-op and breaks reloading. `-fno-gnu-unique` is the
first line of defence and hidden visibility the second. The `registry` class itself is `ES_SERREG_API`
(`visibility("default")` in the shared build), so every image calls the one instance in `libserreg.so`.

## class registry, registry::instance()

**Where:** declaration at `registry.hpp:127–215`; `registry_state::inst` and `instance()` at
`registry.cpp:291–304`. **Context:** run time. The object is `constinit` (constant-initialised, with a
`constexpr` private constructor), so it exists before any guard runs.

There is exactly one registry per process. It lives in `libserreg.so`, and a static `libserreg` linked into a
plugin would be a second registry with a warning. The object holds only `std::atomic<std::uint64_t> version_`;
the mutable state is behind `G()`. `instance()` returns a reference to the `constinit` object, so a guard in
another image can call it during static initialisation without the static-initialisation-order fiasco.

## state, G()

**Where:** `struct state` at `registry.cpp:226–249`; `state& G() noexcept` at 252–256. **Context:** run time.
`G()` creates the state on first use and never destroys it.

`state` holds:
- the mutex
- every heap entry ever made (`all`)
- the current entry per key (`by_key`)
- the conflict list
- the change callbacks
- the atomic pointer to the published index, and the retired indexes
- the `stale` flag and the generation counter
- the conflict policy, the exit flag and the signal-dump helper

`G()` is a function-local `static state* const s = new state{}` that is deliberately leaked. Guards of other
images may run before `libserreg`'s own globals are initialised, or after they are destroyed at exit; a leaked
object is valid in both cases. An earlier design with a destructor crashed at exit.

## entry, make_entry()

**Where:** `struct entry` at `registry.cpp:163–180`; `make_entry()` at 182–224. **Context:** run time (called from
`push()`, so during static initialisation).

An `entry` is the registry's record of one logical definition: `(kind, hash)`, layout hash, name, blob, payload,
image base, reference count, the registry version at which it appeared (`added`, for delta exports) and the list
of nodes holding a reference. Entries are never freed before exit. For a node of the main program, `make_entry`
views the blob, name and descriptor in place (G-5: the executable is never unmapped). For any other image
it copies the blob and name and deep-copies the descriptor (`owned_desc`), so everything the registry hands out
stays valid after `dlclose`.

## registry::push(n)

**Where:** `registry.cpp:437–486`. **Context:** static initialisation (called from `guard`'s constructor, before
`main()` or inside `dlopen`). It is `noexcept` although it allocates: there is no caller to report a failure to.

`push` sets up a literal's name view and finds the node's image with `image_of()`, both outside the lock, because
`dladdr` may take the loader lock. Then, under the mutex, it does one of four things:
- creates a new entry
- attaches an alias, when the same definition comes from another TU or image
- revives a retained entry, when a plugin is loaded again
- replaces a retained entry whose definition changed

When a loaded definition has the same schema hash but a different layout, it records a conflict (FR-R4) and
reports it after the lock is released. Every push calls `mark_stale_locked()`. No index is built here: registering
ten thousand types before `main()` costs one index, built later.

## registry::on_guard_destroyed(n)

**Where:** `registry.cpp:488–537`. **Context:** static de-initialisation: called from `guard`'s destructor at
`dlclose()` or at process exit. It never aborts, allocates or frees.

It removes the node from its entry and decrements the reference count. When the count reaches 0, the entry is
retained, not removed: lookups still find it, its descriptor stays readable, and frames written while the plugin
was loaded still decode. A rejected conflicting definition that unloads leaves no conflict behind. If the kept
definition unloads, a still-loaded rival takes over its key. The registry version moves, and the next reader
publishes.

## registry_entry, registry_index, registry_index::find

**Where:** `struct registry_entry` at `registry.hpp:91–103`, `struct registry_index` at 116–125;
`registry_index::find` at `registry.cpp:307–316`. **Context:** run time.

A `registry_index` is an immutable snapshot: a vector of `registry_entry` views sorted by `(kind, hash)`, the
conflicts, the registry version, the number of retained entries, and one
`std::flat_map<std::uint64_t, std::uint32_t>` per kind mapping a hash to its position. `find(nk, hash)` searches
only that kind's keys. Indexes are never freed before exit, so a snapshot, and every pointer taken from it, stays
valid for the life of the process.

## mark_stale_locked(), publish_locked(), publish_if_stale(), registry::snapshot()

**Where:** `registry.cpp:365, 322, 373, 540`. **Context:** run time.

These implement lazy, lock-free publishing:
1. A change calls `mark_stale_locked()`, which bumps `version` and sets `stale`.
2. Any read calls `snapshot()`, which calls `publish_if_stale()`.
3. If the index is stale, `publish_if_stale()` takes the mutex and `publish_locked()` builds a new index from the
   heap entries. It is swapped in with an atomic exchange, and the old one is retired.
4. The change callbacks then run once, on the reading thread and outside the lock.

Readers that find nothing stale only acquire-load the index pointer and binary-search, with no lock.

## find_type, find_type_by_name, find_call, find_literal, has_conflict

**Where:** `registry.cpp:575–611`. **Context:** run time, lock-free (through `snapshot()`).

`find_type(schema_hash)` returns the `type_desc*`, `find_call(call_hash)` the `call_desc*`, and
`find_literal(id)` the text. Each is an `O(log n)` search in the published index. `find_type_by_name` scans for
the canonical name and prefers a loaded entry to a retained one, which is the right answer after a plugin was
rebuilt with a changed type. `has_conflict` checks the conflict list. All returned pointers stay valid until exit.

## registry::version(), generations(), size()

**Where:** `version()` at `registry.hpp:148`, `size()` at 144; `generations()` at `registry.cpp:827`. **Context:** run time.

`version()` is one acquire load of the counter that every registration and unload bumps. It is the cheap
"did anything change?" test that `capture_file::before_frame()` and `dictionary_tracker::update()` perform per
frame, and the `since` value of delta exports. `generations()` counts the indexes published so far (a diagnostic).
`size()` is the number of entries in the current snapshot, retained ones included.

## export_image(), export_image_since(v)

**Where:** `registry.cpp:614–653` (plus the `fd` and path overloads at 779 and 784). **Context:** run time.

`export_image_since(v)` writes an RRI: the 40-byte header (`"ESRG"`, format, the delta flag when `v` is not 0,
hash algorithm, entry count, program hash, ABI tag) followed by the ENTRY blobs of every entry whose `added` is
after version `v`. Scope nodes are skipped. `export_image()` is `since = 0`, the full dictionary. This is the
opening dictionary of a capture file, the delta in a dictionary frame, the payload of `ES_SERREG_DUMP_REGISTRY`,
and the `.rri` file `srdump --rri` reads.

## export_image_scope(scope, since), scope_calls(scope)

**Where:** `registry.cpp:665` and 655. **Context:** run time.

The dictionary of one scope. It parses the full image, starts from the calls registered under
`scope_hash` and closes over their argument types (recursively through fields) and literals. It emits only those
entries, either as a full image or as the delta of entries that entered the scope after `since`. The delta rule
uses the version at which an entry entered the scope, so an old type that a newly loaded plugin's call brings into
the scope is included. `scope_calls` counts the (scope, call) nodes.

## program_hash(), images(), retained()

**Where:** `registry.cpp:566, 557, 552`. **Context:** run time.

`program_hash()` is an order-independent hash over the schema hashes of the loaded types (FR-H3), written into the
FILE and RRI headers to identify the writer's set of types. `images()` lists the distinct images with loaded
registrations. `retained()` counts the entries whose images have all been unloaded.

## on_change, remove_on_change, refresh

**Where:** `registry.cpp:813, 821, 547`. **Context:** run time.

These provide change notification (FR-R10/R11). Callbacks never run inside a guard, because that may be under the
loader lock. They run on the thread that publishes the next index. `refresh()` publishes and notifies on the
calling thread, for example right after a `dlopen()`. A callback whose code lives in a plugin must be removed
before that plugin is unloaded: the registry keeps data alive, not code.

## conflict, conflict_policy, report_conflict()

**Where:** `struct conflict` at `registry.hpp:105`; `enum class conflict_policy` at `registry.cpp:67`;
`report_conflict()` at `registry.cpp:399`. **Context:** run time.

A conflict is two loaded definitions with the same schema hash but different layout hashes, for example two
plugins built from different versions of a header. `push()` keeps the first, records the pair, and reports it
outside the lock with one `report_line` on stderr. `ES_SERREG_CONFLICT=warn|abort|silent` selects the policy,
read once at load time.

## image::current, main_image, name, can_unload, unload_guard_active

**Where:** `src/image_elf.cpp:34, 42, 53, 61`; `registry::unload_guard_active()` at `registry.cpp:832`.
**Context:** run time.

The image backend answers the executable-format questions:
- `current(addr)`: which image contains this address, via `dladdr`'s `dli_fbase`
- `main_image()`: the executable's base, via `dl_iterate_phdr`
- `name(base)`: the image's path, for messages
- `can_unload(addr)`: whether the image is unloadable, i.e. not `DF_1_NODELETE` and not the main program

`push()` stores the node's image so that references are counted per image. `unload_guard_active()` exposes the
unload check for tests and diagnostics. `src/image_none.cpp` implements the same interface for builds without ELF.

## serreg_init(), exit_hook(), warn_if_second_copy()

**Where:** at the end of `src/registry.cpp`: `serreg_init()` is `[[gnu::constructor(101)]]`; `exit_hook()` and
`warn_if_second_copy()` are next to it. **Context:** library load (before any guard of a program linked against
`libserreg`) and process exit.

`serreg_init` runs when `libserreg.so` loads, ahead of every guard. It:
- warns if a second copy of `libserreg` is in the process
- reads `ES_SERREG_CONFLICT` and the `ES_SERREG_DUMP_*` variables
- blocks the dump signal and starts the helper thread if configured
- registers `exit_hook` with `atexit`

Because it is registered first, `exit_hook` runs after the guards' destructors. It stops callbacks, stops the
helper, performs the exit dump (which therefore sees retained entries too) and frees the retired indexes.

## ES_SERREG_DUMP_*, dump_if_requested(), to_json(), to_toml()

**Where:** `registry.cpp:841–1014` (`dump_configured`, `do_dump`, the signal helper),
`registry::dump_if_requested()` at 1009, `to_json()` at 804 and `to_toml()` at 808. **Context:** run time.

With no code change, a process can write its registry at exit (`ES_SERREG_DUMP_REGISTRY=<path|->`), to an open
descriptor (`ES_SERREG_DUMP_REGISTRY_FD=<n>`), after every change (`ES_SERREG_DUMP_ON=change|both`), or on a signal
(`ES_SERREG_DUMP_SIGNAL=USR1`). The format is RRI, JSON or TOML (`ES_SERREG_DUMP_FORMAT`). This is the third
carrier of the dictionary, next to the capture file and the ELF section: the dictionary of a running process,
plugins included.
