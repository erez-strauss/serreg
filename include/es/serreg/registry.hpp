// es/serreg/registry.hpp — the process-wide registry (spec §6.3, FR-R1..R12).
//
// How a type gets registered without any runtime call (Option A):
//   serialize<Ts...>() odr-uses detail::registrar<T>::g for every T and every component type of T (touch<T>),
//   plus registrar_call<Ts...>::g and registrar_lit<S>::g. Odr-using a static data member of a class template
//   instantiates its definition ([temp.inst]); `g` is a guard whose constructor pushes the node, so the push
//   happens during the static initialisation of the image that contains the instantiation — before main()
//   for the executable, at dlopen() time for a DSO. The registry storage itself lives in libserreg
//   (registry.cpp) as a constinit object, so there is exactly one per process and no SIOF (FR-R3, FR-R8).
//
// Storage (2.1.13, decision A): each registration becomes a heap entry when it is pushed — for the main program a
// view of its ENTRY blob, name and descriptor (the executable is never unmapped; 2.2.1, G-5: no copy at all), for
// every other image a copy of the blob and the name and a deep copy of the descriptor.
// Guards hold references to their entry; a dlclose() drops the references of that image's guards and never aborts.
// An entry whose references are all gone stays in the registry (refs == 0, "retained"): lookups still find it,
// its descriptor stays readable, and frames written while the image was loaded still decode. Loading the image
// again revives the same entries (same hash, same layout) instead of adding new ones; a type whose layout changed
// has a new schema hash and becomes a new entry next to the retained one (FR-R10/FR-R11, 2.1.13).
//
// Reads (v1.0.2): every change marks the registry stale; the next reader publishes an immutable, sorted index of
// views into the heap entries, with one std::flat_map per kind for the lookup; readers acquire-load it and
// binary-search over the hashes only — lock-free, O(log n) (FR-R6). Mutations take a mutex (load/unload time only).
//
// Lifetime: heap entries, their descriptors and every published index live until process exit, so every pointer
// the registry hands out — find_type(), find_type_by_name(), find_call(), record_view::desc,
// registry_entry::payload/name/blob — stays valid after the image that registered it is unloaded.
#pragma once
#include <es/serreg/blob.hpp>
#include <es/serreg/error.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(ES_SERREG_SHARED)
#define ES_SERREG_API [[gnu::visibility("default")]]
#else
#define ES_SERREG_API
#endif

namespace es::serreg
{

enum class node_kind : std::uint8_t
{
    type = 1,
    literal = 3,
    call = 4,
    scope =
        5 // 2.2.1: (dictionary scope, call signature) — registered by serialize() into a scoped sink
};

// 2.2.1: the payload of a scope node. A sink type that declares `using dictionary_scope = Tag;` makes every
// call signature serialised into it register one of these, so registry::export_image_scope(scope_hash) can list
// exactly the calls (and their types and literals) that sink type ever carries — a log file's dictionary is the
// forty format strings it uses, not the program's four thousand archive types. Compile-time, zero per message.
struct scope_desc
{
    std::uint64_t scope_hash; // fingerprint64 of the scope tag's canonical name
    std::uint64_t call_hash;
    std::string_view scope_name;
};

// One per (type | literal | call signature | scope) per image; constant-initialised in .data. Read by push() while
// the image is loaded; the registry views the main program's nodes in place and copies every other image's (2.1.13,
// C-11), and never reads a node after its guard was destroyed.
struct reg_node
{
    const void* payload;             // const type_desc* | const call_desc* | nullptr for literals
    std::span<const std::byte> blob; // the ENTRY bytes (COMDAT .rodata of the image)
    std::uint64_t hash;              // schema hash | literal id | call hash
    std::string_view name; // canonical type name | literal text (set by push()) | call(n){…}
    node_kind nk;
    void* entry; // 2.1.13: the heap entry this node references (registry-private), set by push()
    const void* image_base; // FR-R12: the image of the node (dladdr), set by push()
    std::uint32_t refs;     // unused since 2.1.13 (the entry counts references); kept for layout
    bool
        linked; // true while its entry is the registry's entry of its key (not a rejected conflict)
};

// One entry of the published index. `blob` and `name` are owned copies; `payload` is the main program's descriptor
// or an index-owned deep copy (2.1.8, G2-4). All are valid for the life of the process.
struct registry_entry
{
    node_kind nk;
    std::uint64_t hash;
    std::string_view
        name; // valid for the life of the process (the image's own for the main program, a
    std::span<const std::byte> blob; // heap copy for every other image — 2.2.1, C-11)
    const void* payload;             // const type_desc* | const call_desc* | nullptr
    const void* image_base; // the image that registered it (the last one, for a retained entry)
    std::uint32_t refs; // loaded registrations; 0 = retained after its image(s) unloaded (2.1.13)
    std::uint64_t
        added; // 2.2.1: the registry version at which this definition appeared (delta exports)
};

struct conflict
{
    std::uint64_t schema_hash;
    std::string_view name;
    const void* image_a;
    const void* image_b;
};

// Immutable, sorted by (nk, hash). A snapshot is a pointer to one of these; indexes are never freed before
// process exit, so a snapshot stays valid for the life of the process. Entries are views into the registry's heap
// entries (2.1.13: no per-generation copy of blobs and names).
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

class ES_SERREG_API registry
{
public:
    static registry& instance() noexcept;
    registry(const registry&) = delete;
    registry& operator=(const registry&) = delete;

    // ---- population (called from guards; never from user code) ----
    // noexcept although it allocates (the heap copy of the entry): it runs during static initialisation of an image,
    // where an allocation failure has no caller to report to, so it terminates (review C-12, 2.1.12).
    void push(reg_node* n) noexcept;
    // Drops the node's reference (dlclose() or process exit). Never aborts, allocates or frees (2.1.13).
    void on_guard_destroyed(reg_node* n) noexcept;

    // ---- lock-free queries (FR-R5, FR-R6) ----
    const registry_index& snapshot()
        const noexcept; // the current published index (owning; never freed)
    std::size_t size() const noexcept
    {
        return snapshot().entries.size();
    }
    std::uint64_t version() const noexcept
    {
        return version_.load(std::memory_order_acquire);
    }
    std::uint64_t program_hash()
        const; // FR-H3: order-independent over loaded type schema hashes (refs > 0)
    // 2.1.11 (diagnostic): indexes published so far. Registrations only mark the registry changed; the next
    // snapshot() (or find_*, size(), export_image(), …) publishes one index for the whole burst, so a program that
    // registers n types before main() has published one index when main() first looks, not n.
    std::uint64_t generations() const noexcept;
    std::vector<conflict> conflicts() const
    {
        return snapshot().conflicts;
    }
    std::vector<const void*> images() const; // distinct images with loaded registrations (refs > 0)
    // 2.1.13: entries whose image(s) unloaded and that are kept (refs == 0); included in size() and lookups
    std::size_t retained() const noexcept;

    const type_desc* find_type(std::uint64_t schema_hash) const noexcept;
    // A loaded entry is preferred over a retained one of the same name (a plugin reloaded with a changed layout).
    const type_desc* find_type_by_name(std::string_view canonical_name) const noexcept;
    const call_desc* find_call(std::uint64_t call_hash) const noexcept;
    std::string_view find_literal(std::uint64_t id) const noexcept; // empty if unknown
    bool has_conflict(std::uint64_t schema_hash) const noexcept;

    // ---- export (§6.7) — reads one consistent index generation ----
    std::vector<std::byte> export_image() const; // full RRI: header + all blobs
    // 2.2.1: the entries that appeared after registry version `since` as an RRI delta (0 = the full image)
    std::vector<std::byte> export_image_since(std::uint64_t since) const;
    // 2.2.1: the dictionary of one scope — the calls registered under `scope_hash`, closed over their argument
    // types (recursively) and literals — as a full image (`since` 0) or a delta; empty when the scope has no calls
    std::vector<std::byte> export_image_scope(std::uint64_t scope_hash,
                                              std::uint64_t since = 0) const;
    std::size_t scope_calls(
        std::uint64_t scope_hash) const noexcept; // registered (scope, call) pairs
    result<void> export_image(int fd) const;      // write RRI to an already-open descriptor (FR-R9)
    result<void> export_image(std::string_view path) const;
    std::string to_json() const;
    std::string to_toml() const;

    // ---- change notification (FR-R10/R11) — callbacks run outside the lock; they must not dlopen/dlclose ----
    // 2.1.13: libserreg no longer interposes dlopen()/dlclose(), so a registration cannot tell whether it runs under
    // the loader lock. Changes only mark the registry stale; the reader that publishes the next index runs the
    // callbacks once for that generation, on its own thread, after the publish (never under the loader lock unless
    // that reader is itself inside a constructor). Call refresh() right after a dlopen()/dlclose() to publish and
    // notify on that thread. Every change notifies, including one that only adds or drops an alias. Callbacks do
    // not run once process exit has begun. A callback whose code lives in a plugin must be removed (the id) before
    // that plugin is unloaded.
    // Re-entry (2.2.6, review Q10): callbacks must not register types or publish. A callback that does so (a push
    // from a dlopen() it triggers, or a snapshot()/refresh() after a push) re-enters the publisher on its own
    // thread: the change is published, the callbacks are NOT run again for it — neither the one running nor the
    // others — and a debug build (!NDEBUG) reports the re-entry once on the diagnostic sink. Callbacks that only
    // read the registry (snapshot(), find_*, export_image()) are fine; a change that another thread made in the
    // meantime is published by that read, silently, and notified by nobody (the contract is "not re-notified",
    // not "serialised").
    using change_callback = std::function<void(const registry&)>;
    std::uint64_t on_change(change_callback cb); // returns an id for remove_on_change()
    bool remove_on_change(std::uint64_t id);     // false if the id is unknown
    // Publishes the pending changes (if any) and runs the callbacks on this thread (2.1.13).
    void refresh() const;

    // Explicit trigger for the ES_SERREG_DUMP_* environment hooks (they also run at exit); harmless when unset.
    void dump_if_requested() const;

    // True if the image containing `addr` can actually be unloaded by dlclose (no DF_1_NODELETE, not the main
    // executable). A DSO built without -fno-gnu-unique / hidden visibility may come back false: glibc then keeps it
    // mapped, and reloading it does not run its code again (cc F12). Its entries are retained either way (2.1.13).
    // Diagnostic/test helper; not for the hot path.
    static bool unload_guard_active(const void* addr) noexcept;

private:
    constexpr registry() = default;
    friend struct registry_state;
    std::atomic<std::uint64_t> version_{0};
};

namespace detail
{

// ---- link-time fuse for the whole-program switches (2.2.6, review B3; DECISIONS A-16) -----------------------
// ES_SERREG_VALIDATE_READ and ES_SERREG_LOG_MIN_LEVEL are whole-program switches (one value for every TU and for
// libserreg); a mismatch fails to link here. reflect.hpp's `validate_read_enums` is an inline constexpr that
// copy_flat<U>, bools_canonical<U> and the decode walks branch on, so two TUs that instantiate them for the same
// U with different -D values are an ODR violation the linker would otherwise resolve silently (one copy wins).
// The fuse: every TU that includes this header holds one TU-local, retained pointer to
// validate_read_build<its value>; libserreg (src/registry.cpp) defines that function for the value it was built
// with, and nothing else. A TU built with the other value links against nothing and fails with
// "undefined reference to validate_read_build<N>" — for a shared or a static libserreg, under --gc-sections
// ([[gnu::retain]], binutils >= 2.36) and under LTO. Cost: 8 bytes of .data.rel.ro and one load-time relocation
// per TU, no code. ES_SERREG_LOG_MIN_LEVEL has no fuse yet: it sets logger<Opts>::floor, which the inline
// enabled() gate compares against, so two TUs sharing one logger<Opts> with different values are the same
// hazard; ce-example7 still uses it per TU, so the second fuse waits for the maintainer (2.2.6 report). A
// program that defines the missing instantiation itself (tests/unit/test_registry.cpp does, for serreg_tests,
// which is built with =1 on purpose against the default libserreg) takes the ODR responsibility over.
#ifndef ES_SERREG_VALIDATE_READ
#define ES_SERREG_VALIDATE_READ                                                                    \
    0 // the same default as reflect.hpp (which blob.hpp brings in before this point)
#endif
template<int V> ES_SERREG_API void validate_read_build() noexcept;
[[gnu::used, gnu::retain, maybe_unused]] static void (*const validate_read_fuse)() noexcept =
    &validate_read_build<ES_SERREG_VALIDATE_READ>;

struct guard
{
    explicit guard(reg_node* const n) noexcept : n_(n)
    {
        registry::instance().push(n);
    }
    ~guard()
    {
        registry::instance().on_guard_destroyed(n_);
    }
    guard(const guard&) = delete;
    guard& operator=(const guard&) = delete;
    reg_node* n_;
};

template<class T> struct ES_SERREG_HIDDEN registrar
{
    static reg_node node;
    static const guard g;
};
template<class T>
reg_node registrar<T>::node{&descriptor_v<T>,
                            descriptor_v<T>.blob,
                            descriptor_v<T>.schema_hash,
                            descriptor_v<T>.name,
                            node_kind::type,
                            nullptr,
                            nullptr,
                            0,
                            false};
template<class T> const guard registrar<T>::g{&registrar<T>::node};

template<fixed_string S> struct ES_SERREG_HIDDEN registrar_lit
{
    static reg_node node;
    static const guard g;
};
template<fixed_string S>
reg_node registrar_lit<S>::node{
    nullptr, literal_blob_v<S>, lit<S>::id, {},   node_kind::literal,
    nullptr, nullptr,           0,          false}; // name: push() views the text inside the blob
template<fixed_string S> const guard registrar_lit<S>::g{&registrar_lit<S>::node};

template<class... Ts> struct ES_SERREG_HIDDEN registrar_call
{
    static reg_node node;
    static const guard g;
};
template<class... Ts>
reg_node registrar_call<Ts...>::node{&call_desc_v<Ts...>,
                                     call_desc_v<Ts...>.blob,
                                     call_desc_v<Ts...>.hash,
                                     call_name_v<Ts...>,
                                     node_kind::call,
                                     nullptr,
                                     nullptr,
                                     0,
                                     false};
template<class... Ts> const guard registrar_call<Ts...>::g{&registrar_call<Ts...>::node};

// 2.2.1: dictionary scopes. A sink type with `using dictionary_scope = Tag;` is a scoped sink: serialize() into it
// also instantiates registrar_scoped<Tag, Sig...>, one node per (scope, signature) per image, COMDAT-folded like
// every registrar — nothing per message. scope_name_v<Tag> names the scope in a file's writer-info frame
// (`dictionary = scoped:<name>`); scope nodes have no ENTRY, so they are not in the ELF section (a scoped
// dictionary is computed in the process that writes it, from export_image_scope).
template<class Scope> inline constexpr std::string_view scope_name_v = canonical_name_v<Scope>;
template<class Scope>
inline constexpr std::uint64_t scope_hash_v = fingerprint64(scope_name_v<Scope>);
template<class Scope, class... Ts> struct ES_SERREG_HIDDEN registrar_scoped
{
    static constexpr scope_desc desc{scope_hash_v<Scope>, call_desc_v<Ts...>.hash,
                                     scope_name_v<Scope>};
    static constexpr std::uint64_t key = fingerprint64_u64s(
        std::array<std::uint64_t, 2>{scope_hash_v<Scope>, call_desc_v<Ts...>.hash});
    static reg_node node;
    static const guard g;
};
template<class Scope, class... Ts>
reg_node registrar_scoped<Scope, Ts...>::node{&registrar_scoped<Scope, Ts...>::desc,
                                              {},
                                              registrar_scoped<Scope, Ts...>::key,
                                              scope_name_v<Scope>,
                                              node_kind::scope,
                                              nullptr,
                                              nullptr,
                                              0,
                                              false};
template<class Scope, class... Ts>
const guard registrar_scoped<Scope, Ts...>::g{&registrar_scoped<Scope, Ts...>::node};

// FR-R1: odr-use the guards of T and, recursively, of every component type. The calls are empty at runtime
// and folded away at -O1; their only purpose is to force instantiation of the guard definitions. Skipped
// members are not touched (cc F4): their types never enter the registry or the dictionary section.
template<class T> [[gnu::always_inline]] inline void touch();

template<class U, std::size_t I> [[gnu::always_inline]] inline void touch_member()
{
    if constexpr (!member_skipped<U, I>())
        touch<part_t<U, I>>();
}
template<class T, std::size_t... B, std::size_t... M>
[[gnu::always_inline]] inline void touch_components(std::index_sequence<B...>,
                                                    std::index_sequence<M...>)
{
    (touch<base_t<T, B>>(), ...);
    (touch_member<T, M>(), ...);
}

template<class T> [[gnu::always_inline]] inline void touch()
{
    check_supported<T>(); // FR-S7: the intended diagnostic fires before anything else
    using U = mapped_t<T>;
    if constexpr (is_scalar_v<U>)
    {
    }
    else if constexpr (array_traits<U>::is_array)
    {
        touch<typename array_traits<U>::elem>();
    }
    else if constexpr (std::is_enum_v<U>)
    {
        static_cast<void>(&registrar<U>::g);
    }
    else if constexpr (is_dyn_member_v<U>)
    { // v2: no entry of its own; the element types (vector/optional element, variant alternatives) register
        if constexpr (dyn_member<U>::k == kind::vector_ || dyn_member<U>::k == kind::optional_)
            touch<typename dyn_member<U>::elem>();
        else if constexpr (dyn_member<U>::k == kind::variant_)
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (touch<std::variant_alternative_t<I, U>>(), ...);
            }(std::make_index_sequence<dyn_member<U>::count>{});
        else if constexpr (dyn_member<U>::k == kind::map_)
        { // 1.6: the key and the mapped type
            touch<typename dyn_member<U>::key>();
            touch<typename dyn_member<U>::elem>();
        }
        else if constexpr (dyn_member<U>::k == kind::set_)
            touch<typename dyn_member<U>::key>();
    }
    else
    {
        static_cast<void>(&registrar<U>::g);
        touch_components<U>(std::make_index_sequence<base_count<U>()>{},
                            std::make_index_sequence<part_count<U>()>{});
    }
}

template<class T> [[gnu::always_inline]] inline void touch_arg()
{
    if constexpr (is_literal_v<T>)
        static_cast<void>(&registrar_lit<T::str>::g);
    else if constexpr (is_constant_v<T>)
        touch_arg<arg_value_t<T>>(); // 1.5: an enum constant registers its enum
    else if constexpr (is_container_arg_v<T>)
    { // 1.6: a container argument has a type entry of its own (the transparent wrapper), plus its elements
        static_cast<void>(&registrar<T>::g);
        touch<T>();
    }
    else if constexpr (is_form_tag_v<T> || is_string_arg_v<T> || is_scalar_arg_v<T>)
    { // 1.1: form tags, runtime strings and scalars have no type entry of their own
    }
    else
        touch<T>();
}

} // namespace detail

// ---- 2.1.16 (A1): registration without a call site -----------------------------------------------------
// Everything serialize()/deserialize()/dispatch() register, they register because they are instantiated. A
// program that only *reads* a type — through srdump, a dictionary-driven walk, or a typed reader that lives in a
// plugin loaded later — can register it explicitly, so that find_type() knows it, the embedded es_serreg_dict
// section of this binary carries it, and export_image() / --elf list it. Ts may be class types (their component
// types register with them), enums, literals (lit<"…">) and constants; scalars and runtime strings have no entry
// and are ignored. The calls are empty at run time: the registration happened during static initialisation of the
// image that instantiated them. Typed readers still need the writer's full call signature; register_call<Sig...>()
// registers that signature (its argument types included), exactly as deserialize<Sig...>() would.
//
//   es::serreg::register_types<ex::Order, ex::Side, es::serreg::lit<"evt">>();
//   es::serreg::register_call<es::serreg::lit<"evt">, ex::Order>();   // the signature of serialize(out, "evt"_, o)
template<class... Ts> [[gnu::always_inline]] inline void register_types() noexcept
{
    (detail::touch_arg<std::remove_cvref_t<Ts>>(), ...);
}
template<class... Sig> [[gnu::always_inline]] inline void register_call() noexcept
{
    (detail::touch_arg<std::remove_cvref_t<Sig>>(), ...);
    static_cast<void>(&detail::registrar_call<std::remove_cvref_t<Sig>...>::g);
}

} // namespace es::serreg
