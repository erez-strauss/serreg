# 5. The registry

`registry::instance()` is the process-wide table of every type, literal and call signature the program can
serialize. It is populated during static initialisation of each image — before `main()` for the executable, at
`dlopen()` for a plugin — and never lazily. This chapter is the user's and operator's view; chapter 9 explains
the loader-level machinery underneath.

## 5.1 How a type gets in (FR-R1)

```cpp
template<class T> struct registrar { static reg_node node; static const guard g; };   // registry.hpp
serialize(sink, order)  →  detail::touch<Order>()  →  static_cast<void>(&registrar<Order>::g);
```

Odr-using `registrar<Order>::g` instantiates its definition ([temp.inst]); `g` is a `guard` whose constructor
pushes `node` (a constant-initialised `reg_node` pointing at `descriptor_v<Order>` and its ENTRY blob) into the
registry during the image's dynamic initialisation ([basic.start.dynamic]). `push()` makes the registration a heap
entry of the registry. For the **main program** the entry *views* its ENTRY blob, name and descriptor in place — the
executable is never unmapped before exit, so no copy is needed (DECISIONS G-5). For any **other image** (a `dlopen`ed plugin) the entry holds a copy of the
blob and the name and a deep copy of the descriptor, so nothing the registry hands out points into an image that
can be unloaded. Each entry also records `added`, the registry version at which the definition appeared — what a
dictionary delta selects on (§5.3). `touch<T>()` recurses through
bases, members, array elements, vector/optional elements and variant alternatives, so every reachable type has
its own node; literals get `registrar_lit<S>::g`, call signatures `registrar_call<Ts...>::g`, and a call
serialised into a *scoped sink* also `registrar_scoped<Scope, Sig...>::g` (§5.3a). There is no
static-initialisation-order problem: the registry's storage is a leaked function-local static inside
`libserreg` (`G()`), so it exists before any guard runs and outlives every `atexit` handler.

Consequences:

- **Counts are exact at `main()`.** `tests/unit/premain_check.cpp` reads `registry::instance().size()` in the
  first member initialiser of `premain_check::app`, which `main()` constructs before anything else runs. Types used only by a `dlopen`'ed plugin appear when it loads.
- **Aliases collapse.** The same type instantiated in ten TUs or three DSOs is one logical entry with a
  reference count per image; readers never see duplicates.
- **Conflicts are detected.** Two images defining the same schema hash with different layout hashes (an ODR
  violation or a stale plugin) are recorded; `ES_SERREG_CONFLICT=warn` (default: one stderr line) | `abort` |
  `silent`. `srdump --verify` reports the same conflict from a capture or from `--elf a --elf b`.
  `serialize()` never checks for conflicts (no registry access on the hot path). A host that `dlopen`s plugins it
  does not build itself should call `registry::instance().has_conflict(schema_hash_v<T>)` (or check `conflicts()`)
  after each load for the types it exchanges with them: a conflicting plugin keeps writing its own layout under the
  shared schema hash, and readers take the first definition. When the first definition's image unloads, a
  still-loaded conflicting definition takes its place and the conflict ends.

### 5.1a Registering without a call site: `register_types`, `register_call` (A1)

```cpp
es::serreg::register_types<ex::Order, ex::Side, es::serreg::lit<"evt">>();   // the types, an enum, a literal
es::serreg::register_call<es::serreg::lit<"evt">, ex::Order>();              // the signature of serialize(out, "evt"_, o)
```

Everything `serialize()`, `deserialize()` and `dispatch()` register, they register because they are
instantiated. A program that only *reads* a type — through `srdump`, a dictionary-driven walk, or a typed reader
in a plugin loaded later — has no such call site, so without help its own registry does not know the type:
`find_type()` returns nothing, and the embedded `es_serreg_dict` section, `export_image()` and `--elf` do not
list it. A container is a type too — `register_types<std::vector<int>>()` registers the transparent
type `std::vector<i32>` and its element types. `register_types<Ts...>()` names the types once: class types (their component types register with
them), enums, literals and constants; scalars and runtime strings have no entry and are ignored.
`register_call<Sig...>()` registers a full call signature, argument types included, exactly as
`deserialize<Sig...>` would. Both are empty at run time: the registration is a side effect of the
instantiation, during the static initialisation of the image that contains it, so the call can sit in a function
that is never executed. Two things do not change: a typed reader still needs the writer's full signature (that
is what `register_call` records), and a dictionary-driven reader needs no registration at all — it takes the
capture's dictionary (chapter 4 §4.4). Test: "2.1.16 A1 register_types / register_call register without a
serialize call site" (`tests/unit/test_registry.cpp`).

### 5.1b Registering a whole namespace: `ES_SERREG_REGISTER_NAMESPACE` (A-22)

```cpp
#include <es/serreg/register_namespace.hpp>
ES_SERREG_REGISTER_NAMESPACE(app::messages);                                   // every serialisable type, nested namespaces too
ES_SERREG_REGISTER_NAMESPACE(app::messages, sr::name_prefix<"Msg">{});        // a sub-group by name (name_suffix, name_glob, qualified_glob)
ES_SERREG_REGISTER_NAMESPACE(app::messages, sr::annotated<sr::message_t>{});  // struct [[=sr::message]] Order {…};
static_assert(sr::registered_type_count<^^app::messages> == 5);
for (const char* n : sr::selected_type_names<^^app::messages>()) std::println("{}", n);   // dump, then narrow
```

`register_types<Ts...>()` names the types one by one; `register_namespace.hpp` lets reflection list them:
`std::meta::members_of(^^ns)` enumerates the namespace (what is declared before that point, in this translation
unit), the complete class and enum types pass through a selector — the default takes all of them — and every one
the library can serialise (`detail::supported_type<T>`, `check_supported()` as a predicate) is registered through
the same `detail::touch<T>()` a `serialize()` would instantiate. A helper struct with a pointer member is listed
by the selection and skipped by the registration; `[[=skip]]` on a type keeps it out of both. Selectors compose
(`all_of`, `any_of`, `not_`); the annotation is the exact form, a name pattern the retrofit for a codebase that
already names its messages consistently. The registration is an instantiation side effect, so the line can sit
in any translation unit, once per program. Header page:
[register_namespace.hpp](files/include-es-serreg-register_namespace.hpp.md); example
`examples/register_namespace.cpp`; tests `tests/unit/test_register_namespace.cpp` ("A-22 register_namespace: …").

## 5.2 Reading the registry (lock-free)

```cpp
const registry& reg = registry::instance();
reg.size();                                   // logical entries, retained ones included
reg.retained();                               // entries whose image(s) unloaded (refs == 0)
reg.find_type(schema_hash) / find_type_by_name("ex::Order") / find_literal(id) / find_call(call_hash);
const registry_index& ix = reg.snapshot();    // immutable, sorted by (kind, hash); valid for the process lifetime
reg.version();                                // bumps on every mutation (dictionary_tracker and capture files use it)
reg.program_hash();                           // FR-H3, over loaded types; images(): images with loaded entries
std::vector<std::byte> rri = reg.export_image();   // the RRI; export_image(path) / export_image(fd)
reg.export_image_since(v);                    // a delta — the entries added after registry version v
reg.export_image_scope(scope_hash_v<Tag>);    // one scope's dictionary (scope_hash_v is in detail::)
reg.scope_calls(scope_hash);                  // how many (scope, call) pairs are registered under a scope
auto id = reg.on_change(callback);            // once per published generation; returns an id
reg.remove_on_change(id);                     // before unloading the plugin the callback lives in
reg.refresh();                                // publish pending changes and notify on this thread
reg.generations();                            // diagnostic: indexes published so far
```

Readers acquire-load an immutable `registry_index` of views into the registry's heap entries and search it: one
`std::flat_map` from hash to position per kind. Nothing on the read side points into an image that could
be unloaded; a `type_desc*` or `call_desc*` from `find_*` is valid for the life of the process (review G2-4). Mutations (load/unload time) take a mutex, bump `version()` and mark the index stale;
the first read after them builds and publishes the next index, once for all of them. So a program with
hundreds of types publishes one index at startup, not one per type — one index per type would cost O(n²) time and
memory, because an old index is never freed while a reader may hold it (400 types: 70 MiB that way, 3 MiB with one
publish). A read when nothing changed takes no lock and allocates nothing.

Change callbacks run on the thread that publishes the next index, once for that generation, after the publish and
outside the mutex. libserreg does not see `dlopen()`/`dlclose()`, so a registration cannot know
whether it runs under the loader lock and never calls back itself. After a `dlopen()` or `dlclose()`, call
`registry::refresh()` to publish and be notified right there; otherwise the next read does it. No callback runs once
process exit has begun.

## 5.3 Exporting the dictionary at run time

Without touching the program:

| variable | effect |
|---|---|
| `ES_SERREG_DUMP_REGISTRY=<path\|->` | write the registry (JSON by default) at exit; `-` = stdout |
| `ES_SERREG_DUMP_REGISTRY_FD=<n>` | write it (RRI by default) to an already-open descriptor |
| `ES_SERREG_DUMP_FORMAT=rri\|json\|toml` | format for the two above |
| `ES_SERREG_DUMP_ON=exit\|change\|both` | when: at exit (default), at every published change (each `dlopen`/`dlclose`, on the thread that reads next), or both |
| `ES_SERREG_DUMP_EXIT=1` | exit right after the first dump (schema extraction from a program you cannot run to completion) |
| `ES_SERREG_DUMP_SIGNAL=USR1\|USR2\|RTMIN+n` | a dedicated `sigwait` thread dumps on that signal (nothing is installed otherwise; the handler path does no I/O in signal context) |

From code: `registry::instance().export_image(...)`, `tools::load_dictionary_rri(...)`; also
`export_image_since(version)` — an RRI with `rri_flag_delta` holding only the entries whose `registry_entry::added`
is above `version`, which is what a capture file and `dictionary_tracker` write after a `dlopen` (chapter 7 §7.9) —
and `export_image_scope(scope_hash, since = 0)` (§5.3a). Offline:
`srdump --elf ./binary` reads the `es_serreg_dict` section (`tools::extract_embedded_dictionary`) — the same blobs,
no execution required. A program built with `ES_SERREG_IMAGE_FORMAT=none` has no such section; its `.rri`
is the offline source.

## 5.3a Dictionary scopes

A process may register thousands of message types while one of its files — a log, an audit trail — carries a
dozen calls. A **dictionary scope** gives such a file a dictionary of its own, computed at compile time:

```cpp
struct audit_scope {};                                         // any type: its canonical name names the scope
scoped_capture_file<audit_scope> f{"audit.ser"};               // a scoped sink: using dictionary_scope = audit_scope;
serialize(f, "login"_, user);                                  // also registers (audit_scope, call("login", User))
```

Every `serialize`/`print` into a sink that declares `using dictionary_scope = Tag;` (`scoped_sink`,
`sink.hpp`) odr-uses `detail::registrar_scoped<Tag, Sig...>::g`, one node per (scope, signature) per image, COMDAT
folded like every registrar: nothing happens per message. Scope nodes (`node_kind::scope`, payload `scope_desc{
scope_hash, call_hash, scope_name}`) have no ENTRY — they are not in the ELF section or in `export_image()`.
`export_image_scope(scope_hash, since)` collects the scope's calls, their literal arguments and argument types,
and every type those types reach through fields and element descriptors, and emits exactly those entries (a delta
when `since` > 0). A scoped capture file writes that as its opening dictionary and its deltas, and names the scope
in its writer-info frame (`dictionary = scoped:audit_scope`). The logger's default `dictionary_policy::scoped`
requires a scoped sink (chapter 3 §3.9).

## 5.3b The registry at scale (DECISIONS G-5)

What a large registry costs is measured by `tests/bench/registry_measure` (`book/registry-measure.md`): N synthetic
struct entries of about B / N bytes each, pushed as guards push them, then the first publish, N lookups in random
order, the export and its parse. The budget, enforced in release builds by the ctests `registry_measure_10k` and
`registry_measure_100k` (label `bench`): **10 000 entries / 10 MB registered and published within 10 ms, 100 000 /
100 MB within 100 ms**. Measured in the sandbox (2 vCPU VM, GCC 16, `-O2`): 10k — push 7.7 ms + publish
1.9 ms, lookup about 100 ns, export 14 ms, parse 12 ms; 100k — push 111 ms + publish 29 ms (over the budget on this
VM; the reference box is pending), lookup 130 ns, export 117 ms, parse 139 ms. Viewing the main program's entries
in place instead of copying them halved the push time and saved one blob-size of memory.

A program with many types can keep these costs down: fewer, larger message types; scoped capture files (only the
scope's entries are exported per file); `capture_policy{.dictionary_updates = false}` when nothing is `dlopen`ed
(no per-frame version compare); `checkpoint_dictionary = false` (the default; checkpoints stay 80 bytes). The ELF
dictionary section is read only by tools (`srdump --elf`), never by the program itself.

## 5.4 `dlopen`, `dlclose` and reload

A plugin that calls `serialize()` registers its types when it loads (its own static initialisation), with its
image base recorded per node (`dladdr`, through the image backend — chapter 9.4). An open capture file notices the
new registry version at its next frame and writes a dictionary delta first (chapter 3 §3.4). Unloading follows
decision A:

- **`dlclose` never aborts.** The plugin's guards drop their references; an entry no loaded image references any
  more is **retained** (`refs == 0` in its `registry_entry`, counted by `registry::retained()`). It stays in
  `size()`, in every lookup, in `export_image()` and in the JSON/TOML views; descriptor pointers obtained before
  the unload stay valid and are still returned; frames the plugin wrote still decode. `images()` and
  `program_hash()` count loaded entries only.
- **Reloading revives.** Loading the same plugin again attaches its registrations to the retained entries: the
  same descriptors, no new memory. A plugin rebuilt with a changed type gives that type a new schema hash, so the
  new definition is a new entry next to the retained one; `find_type_by_name()` prefers the loaded one, and
  `find_type(old_hash)` still decodes old frames.
- **Process exit** drops references the same way; the exit dump (`ES_SERREG_DUMP_*`) lists retained entries too.

An earlier design interposed `dlclose()` and aborted when the image owned registered entries, unless
`ES_SERREG_ALLOW_DLCLOSE=1` removed them; retaining entries is simpler and keeps every descriptor valid. The
variable is ignored with one line on stderr at startup.

Requirements: `libserreg` **shared** (one registry per process — a static `libserreg` linked into both host and
plugin gives per-image registries, and the plugin's are lost when it unloads), plugins built with
`-fno-gnu-unique` and hidden visibility for the registrar symbols (the exported target carries both flags) —
otherwise glibc marks the DSO `DF_1_NODELETE`, `dlclose` silently becomes a no-op, and a rebuilt plugin cannot be
reloaded. The registry protects what it hands out, not the plugin's own code: remove a plugin's `on_change`
callback (`remove_on_change(id)`), handlers and objects before unloading it (chapter 9.5). `tests/dlopen` exercises
load, unload, reload, a changed layout, nested and concurrent `dlclose`, conflicts between plugins and readers under
churn.

## 5.5 The literal table

Literals are entries like any other: `find_literal(id)` returns the text (empty if unknown). The text exists once
per image (the ENTRY blob; `lit<S>::text()` and the registry view it), and a compact frame carries no bytes for it. `"a"_` in one TU and `"a"_` in another are one entry (same id, alias).

## 5.6 What the registry is not

It is not a serialization cache, not a factory, not a place to look up things on the hot path (`serialize` never
touches it), and not a type-erasure mechanism: it holds descriptions (`type_desc`), not behaviour. Decoding a
foreign type goes through the dictionary (`dyn::dictionary`), which is the registry's portable, parsed form.
