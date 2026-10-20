// src/registry.cpp — registry storage, the published lock-free index, deduplication/conflicts, export, unload
// handling and the ES_SERREG_* hooks (spec §6.3, §6.3b, §6.7, §6.11, FR-R2..R12).
//
// Storage (2.1.13, DECISIONS A-3). push() copies every registration into a heap
// `entry` — the ENTRY blob and the name for every image, and a deep copy of the descriptor (names, fields, element
// descriptors, enumerators, gap and bool tables, call arguments) for every image other than the main program, whose
// descriptors live as long as the process. One entry per (kind, hash); every guard that registered it holds a
// reference (`nodes`, `refs`). The registry never reads an image's memory after the image's guard is destroyed.
//
// Unload. A guard destructor runs when its image is unloaded by dlclose() and at process exit; both drop the
// reference and nothing else. There is no abort mode and no dlopen()/dlclose() interposer: C++ cannot tell the two
// cases apart, and since 2.1.13 it does not need to. An entry whose references are gone stays in the registry
// (refs == 0, "retained"): lookups find it, pointers to it stay valid, frames written by the unloaded image still
// decode. A reload of the image revives the same entries (same key, same layout); a changed type has a new schema
// hash and becomes a new entry. A definition with the same schema hash but another layout replaces a retained
// entry; against a loaded entry it is a conflict (FR-R4), and when the kept definition unloads, a still-loaded
// conflicting definition takes its place.
//
// Publication (2.1.11). A change only marks the registry stale and bumps the version; the next reader builds one
// immutable index (views into the heap entries, sorted by (kind, hash), one std::flat_map per kind for lookup),
// publishes it with a release store, and runs the change callbacks once for that generation, outside the mutex.
// Readers acquire-load the index and binary-search: lock-free.
//
// Loader lock (review G2-5). Guard constructors and destructors of a dlopen()ed image run inside dlopen()/dlclose(),
// under glibc's loader lock. Nothing here takes the loader lock while holding `mu` (dladdr and the main image are
// looked up before `mu` is taken). A destructor neither allocates nor frees: it erases from vectors in place.
#include "image.hpp"

#include <es/serreg/dictionary.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/sink.hpp> // detail::report_line
#include <es/serreg/tools.hpp>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Q10 marker of this copy of libserreg (defined at the end of this file): a function returning the address of this
// copy's own_marker. The exported name resolves to the first copy in the global scope; what it returns names that
// copy. 2.1.9: a function, not a data symbol — two copies defining one exported *object* is an ODR violation that
// AddressSanitizer reports (and aborts on) in every program that has two copies; two definitions of a function are
// not registered globals.
extern "C" const void* es_serreg_registry_marker_v3() noexcept;

namespace es::serreg
{

// B3 link-time fuse (registry.hpp): the one definition of validate_read_build<V>, for the value this libserreg was
// compiled with (reflect.hpp's default, 0, unless the build passed -DES_SERREG_VALIDATE_READ). A consumer TU built
// with the other value references validate_read_build<other> and does not link.
namespace detail
{
template<int V> void validate_read_build() noexcept
{
}
template void validate_read_build<ES_SERREG_VALIDATE_READ>() noexcept;
} // namespace detail

namespace
{

enum class conflict_policy : std::uint8_t
{
    warn,
    abort_,
    silent
};

struct node_key
{
    node_kind nk;
    std::uint64_t hash;
    bool operator==(const node_key&) const = default;
};
struct node_key_hash
{
    std::size_t operator()(const node_key& k) const noexcept
    {
        return static_cast<std::size_t>(k.hash ^
                                        (std::uint64_t{static_cast<std::uint8_t>(k.nk)} << 56));
    }
};

// ---- index-independent deep copies of descriptors (G2-4, 2.1.8; made at push since 2.1.13) ------------------------
struct owned_desc
{
    std::deque<std::string> text; // names (stable addresses: a deque never moves elements)
    std::deque<std::vector<field_desc>> field_arrays; // fields, and every nested `elem` array
    std::vector<enumerator_desc> enumerators;
    std::vector<gap> gaps;
    std::vector<std::uint32_t> bools;
    std::vector<call_arg_desc> args;
    std::vector<std::byte> blob;
    type_desc type{};
    call_desc call{};
    scope_desc scope{}; // 2.2.1: a plugin's (scope, call) node

    std::string_view own(const std::string_view s)
    {
        return text.emplace_back(s);
    }
    // Copies `n` field descriptors and, recursively, what their `elem` points at: one element for array_,
    // vector_ and optional_, `extent` alternatives for variant_ (reflect.hpp, make_field).
    const field_desc* own_fields(const field_desc* const src, const std::size_t n)
    {
        if (!src || n == 0)
            return nullptr;
        std::vector<field_desc>& out = field_arrays.emplace_back(src, src + n);
        for (field_desc& f : out)
        {
            f.name = own(f.name);
            if (f.elem)
                f.elem = own_fields(f.elem, f.k == kind::variant_ ? f.extent : 1);
        }
        return out.data();
    }
};

std::unique_ptr<owned_desc> deep_copy(const reg_node* const n)
{
    auto o = std::make_unique<owned_desc>();
    o->blob.assign(n->blob.begin(), n->blob.end());
    const std::span<const std::byte> blob{o->blob.data(), o->blob.size()};
    if (n->nk == node_kind::type)
    {
        const type_desc& t = *static_cast<const type_desc*>(n->payload);
        o->type = t;
        o->type.name = o->own(t.name);
        o->type.fields = {o->own_fields(t.fields.data(), t.fields.size()), t.fields.size()};
        o->enumerators.assign(t.enumerators.begin(), t.enumerators.end());
        for (enumerator_desc& e : o->enumerators)
            e.name = o->own(e.name);
        o->type.enumerators = o->enumerators;
        o->gaps.assign(t.padding_gaps.begin(), t.padding_gaps.end());
        o->type.padding_gaps = o->gaps;
        o->bools.assign(t.bool_offsets.begin(), t.bool_offsets.end());
        o->type.bool_offsets = o->bools;
        o->type.blob = blob;
        return o;
    }
    if (n->nk == node_kind::scope)
    { // 2.2.1: the scope's name must outlive the plugin that registered the node
        const scope_desc& sd = *static_cast<const scope_desc*>(n->payload);
        o->scope = sd;
        o->scope.scope_name = o->own(sd.scope_name);
        return o;
    }
    const call_desc& c = *static_cast<const call_desc*>(n->payload);
    o->call = c;
    o->args.assign(c.args.begin(), c.args.end());
    o->call.args = o->args;
    o->call.blob = blob;
    return o;
}

// ---- heap entries (2.1.13) ------------------------------------------------------------------------------------
// One per logical (kind, hash) definition. Never freed before process exit (indexes and callers point into it).
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
    std::uint64_t added =
        0; // 2.2.1: the registry version at which this definition appeared (delta exports)
    std::vector<reg_node*>
        nodes; // the guards holding a reference (one per image/TU that registered it)
};

std::unique_ptr<entry> make_entry(const reg_node* const n, const void* const main_base)
{
    auto e = std::make_unique<entry>();
    e->nk = n->nk;
    e->hash = n->hash;
    if (n->image_base == main_base)
    { // 2.2.1 (G-5): the main program is never unmapped before exit, so its ENTRY blob and name are viewed in
        // place — no heap copy per registration (100k entries of 1 KB: 100 MB and ~50 ms less at start-up)
        e->blob = n->blob;
        e->name = n->name;
    }
    else
    {
        e->blob_copy.assign(n->blob.begin(), n->blob.end());
        e->blob = {e->blob_copy.data(), e->blob_copy.size()};
        if (n->nk == node_kind::literal && e->blob.size() >= fmt::literal_text_offset)
            e->name = {reinterpret_cast<const char*>(e->blob_copy.data()) +
                           fmt::literal_text_offset,
                       n->name.size()}; // the literal's text is viewed inside the ENTRY blob copy
        else
        {
            e->name_copy.assign(n->name);
            e->name = e->name_copy;
        }
    }
    if (n->payload)
    {
        if (n->image_base == main_base)
            e->payload = n->payload; // the main program is never unmapped before exit
        else
        {
            e->desc_copy = deep_copy(n);
            e->payload = n->nk == node_kind::type ? static_cast<const void*>(&e->desc_copy->type)
                         : n->nk == node_kind::scope
                             ? static_cast<const void*>(&e->desc_copy->scope)
                             : static_cast<const void*>(&e->desc_copy->call);
        }
    }
    if (n->nk == node_kind::type)
        e->layout = static_cast<const type_desc*>(n->payload)->layout_hash;
    e->image_base = n->image_base;
    return e;
}

struct state
{
    std::mutex mu;
    std::vector<std::unique_ptr<entry>> all; // every entry ever made; freed at exit
    std::unordered_map<node_key, entry*, node_key_hash>
        by_key; // the current entry of each key (never null)
    std::vector<std::pair<entry*, entry*>>
        conflicts; // (current entry, rejected definition), both loaded
    std::uint64_t next_callback_id = 1;
    std::vector<std::pair<std::uint64_t, registry::change_callback>> callbacks;
    std::atomic<const registry_index*> index{nullptr};
    std::vector<const registry_index*> retired; // old generations; freed at exit
    std::atomic<bool> stale{false}; // the entries changed since the index was published (2.1.11)
    std::atomic<std::uint64_t> generations{0}; // indexes published so far (registry::generations())
    conflict_policy on_conflict = conflict_policy::warn;
    std::atomic<bool> exiting{false}; // set by the exit hook: no more callbacks
    std::atomic<bool> pushed{
        false}; // 2.1.5: something was registered, so the exit dump has something to say
    // signal-triggered dump (§6.11): the signal is blocked process-wide at init; a helper thread sigwait()s.
    int dump_signo = 0;
    pthread_t helper{};
    std::atomic<bool> helper_running{false};
    std::atomic<bool> helper_stop{false};
};
// Never destroyed: guards of other TUs/images may run before or after this TU's static (de)initialisation,
// and the exit hook must be able to use it at the very end (gk 1.13).
state& G() noexcept
{
    static state* const s = new state{};
    return *s;
}
constinit registry_index g_empty_index{};

// The executable-format questions go to the backend (src/image.hpp; ELF by default, 2.1.8).
const void* image_of(const void* const p) noexcept
{
    return image::current(p);
}
const char* image_name(const void* const base) noexcept
{
    return image::name(base);
}
const void* main_image() noexcept
{
    return image::main_image(); // never under `mu`: it may take a loader lock (G2-5)
}

std::size_t kind_slot(const node_kind nk) noexcept
{
    switch (nk)
    {
    case node_kind::type:
        return 0;
    case node_kind::literal:
        return 1;
    case node_kind::call:
        return 2;
    case node_kind::scope:
        return 3;
    }
    return 0;
}

} // namespace

struct registry_state
{
    static constinit registry inst;
    static void bump() noexcept
    {
        inst.version_.fetch_add(1, std::memory_order_acq_rel);
    }
};
constinit registry registry_state::inst{};

registry& registry::instance() noexcept
{
    return registry_state::inst;
}

// ---- index ----------------------------------------------------------------------------------------------
const registry_entry* registry_index::find(const node_kind nk,
                                           const std::uint64_t hash) const noexcept
{
    const auto& m = lookup[kind_slot(nk)];
    const auto it = m.find(hash);
    if (it == m.end())
        return nullptr;
    const registry_entry& e = entries[it->second];
    return e.nk == nk ? &e : nullptr;
}

namespace
{

// Called with G().mu held. Builds the next generation from the heap entries and publishes it.
void publish_locked()
{
    auto* const ix = new registry_index;
    ix->entries.reserve(G().by_key.size());
    for (const auto& [key, e] : G().by_key)
    {
        ix->entries.push_back(registry_entry{e->nk, e->hash, e->name, e->blob, e->payload,
                                             e->image_base, e->refs, e->added});
        if (e->refs == 0)
            ++ix->retained;
    }
    std::sort(ix->entries.begin(), ix->entries.end(),
              [](const registry_entry& a, const registry_entry& b) {
                  return std::pair{a.nk, a.hash} < std::pair{b.nk, b.hash};
              });
    std::array<std::vector<std::uint64_t>, 4> keys;
    std::array<std::vector<std::uint32_t>, 4> pos;
    for (std::uint32_t i = 0; i < ix->entries.size(); ++i)
    {
        const std::size_t k = kind_slot(ix->entries[i].nk);
        keys[k].push_back(ix->entries[i].hash);
        pos[k].push_back(i);
    }
    for (std::size_t k = 0; k < 4; ++k) // already sorted and unique: adopt the vectors as they are
        ix->lookup[k] = std::flat_map<std::uint64_t, std::uint32_t>(
            std::sorted_unique, std::move(keys[k]), std::move(pos[k]));
    for (const auto& [kept, rejected] : G().conflicts)
        ix->conflicts.push_back(
            conflict{kept->hash, kept->name, kept->image_base, rejected->image_base});
    ix->version = registry_state::inst.version();
    G().generations.fetch_add(1, std::memory_order_relaxed);
    if (const registry_index* const old = G().index.exchange(ix, std::memory_order_acq_rel);
        old && old != &g_empty_index)
        G().retired.push_back(old);
    // After the exchange, with release: a reader that sees `stale == false` also sees this index; a reader that
    // still sees `true` takes `mu`, waits for this publish and finds nothing left to do.
    G().stale.store(false, std::memory_order_release);
}

// Called with G().mu held after the entries (or the conflict set) changed. Publishing is deferred to the next reader
// (2.1.11): registering n types before main() — or inside one dlopen() — costs one index, not n. The version moves at
// once, so version()-based observers (dictionary_tracker) see every change. No allocation (guard destructors call
// it inside dlclose()).
void mark_stale_locked() noexcept
{
    registry_state::bump();
    G().stale.store(true, std::memory_order_release);
}

// Q10 (2.2.6): callbacks must not register types or publish (registry.hpp, on_change). The depth counter is per
// thread: a callback that pushes a node and then reads the registry re-enters publish_if_stale() on the thread
// that is running the callbacks; the re-entry publishes (so the read is current) and notifies nobody. Another
// thread's publish is not a re-entry and notifies as usual.
constinit thread_local int notifying = 0;
[[maybe_unused]] constinit std::atomic<bool> reentry_reported{false}; // debug builds report once

struct notifying_scope
{
    notifying_scope() noexcept
    {
        ++notifying;
    }
    ~notifying_scope()
    {
        --notifying;
    }
    notifying_scope(const notifying_scope&) = delete;
    notifying_scope& operator=(const notifying_scope&) = delete;
};

// Publishes the next generation if anything changed since the last one, then runs the change callbacks once for it,
// on this thread, outside `mu` (2.1.13: this is where every notification comes from).
void publish_if_stale()
{
    if (!G().stale.load(std::memory_order_acquire))
        return;
    const bool reentered = notifying > 0; // this thread is inside a change callback
    std::vector<registry::change_callback> cbs;
    {
        std::lock_guard lock(G().mu);
        if (!G().stale.load(std::memory_order_relaxed))
            return;
        publish_locked();
        if (!reentered && !G().exiting.load(std::memory_order_acquire))
            for (const auto& [id, cb] : G().callbacks)
                cbs.push_back(cb);
    }
    if (reentered)
    {
#ifndef NDEBUG
        if (!reentry_reported.exchange(true, std::memory_order_relaxed))
            detail::report_line(
                "es::serreg: a change callback changed the registry (registered a type or "
                "published); callbacks are not re-notified for that change (on_change contract)");
#endif
        return;
    }
    const notifying_scope scope;
    for (const auto& cb : cbs)
        cb(registry::instance());
}

void free_at_exit()
{
    std::lock_guard lock(G().mu);
    for (const registry_index* ix : G().retired)
        delete ix;
    G().retired.clear();
}

void report_conflict(const std::string_view name, const std::uint64_t hash,
                     const void* const kept_image, const void* const rejected_image) noexcept
{
    if (G().on_conflict == conflict_policy::silent)
        return;
    detail::report_line("es::serreg: schema conflict: '{}' (0x{:016x}) is defined with a different "
                        "layout in {} and {}{}",
                        name, hash, image_name(kept_image), image_name(rejected_image),
                        G().on_conflict == conflict_policy::abort_ ? " (ES_SERREG_CONFLICT=abort)"
                                                                   : "");
    if (G().on_conflict == conflict_policy::abort_)
        std::abort();
}

entry* add_entry(std::unique_ptr<entry> e)
{
    return G().all.emplace_back(std::move(e)).get();
}

// `linked` (reg_node): the node's entry is the registry's current entry of its key; a rejected conflicting
// definition holds a reference to its own entry but stays unlinked until it takes over.
void attach(entry* const e, reg_node* const n, const bool linked = true)
{
    e->nodes.push_back(n);
    ++e->refs;
    n->entry = e;
    n->linked = linked;
}

bool same_definition(const entry* const e, const reg_node* const n) noexcept
{
    return n->nk != node_kind::type ||
           e->layout == static_cast<const type_desc*>(n->payload)->layout_hash;
}

} // namespace

// ---- population -------------------------------------------------------------------------------------------
void registry::push(reg_node* const n) noexcept
{
    if (n->nk == node_kind::literal && n->blob.size() >= fmt::literal_text_offset)
    { // the literal's text is viewed inside its ENTRY blob — no second static copy (v1.0.3)
        std::uint32_t len = 0;
        std::memcpy(&len, n->blob.data() + fmt::literal_len_offset, sizeof len);
        n->name = {reinterpret_cast<const char*>(n->blob.data()) + fmt::literal_text_offset,
                   std::min<std::size_t>(len, n->blob.size() - fmt::literal_text_offset)};
    }
    // outside the lock: dladdr and dl_iterate_phdr may take the loader lock (G2-5: never under `mu`)
    n->image_base = image_of(n->payload ? n->payload : static_cast<const void*>(n->blob.data()));
    const void* const main_base = main_image();
    bool conflicted = false;
    std::string_view conflict_name;
    const void* kept_image = nullptr;
    {
        std::lock_guard lock(G().mu);
        G().pushed.store(true, std::memory_order_relaxed);
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
    }
    if (conflicted)
        report_conflict(conflict_name, n->hash, kept_image, n->image_base);
}

void registry::on_guard_destroyed(reg_node* const n) noexcept
{
    // dlclose() of the image, or process exit: drop the reference. In place, no allocation or free (G2-5).
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
    if (e->refs == 0)
    {
        // a rejected definition that unloads leaves no conflict behind
        std::erase_if(G().conflicts, [e](const auto& c) { return c.second == e; });
        // the kept definition unloaded: a still-loaded conflicting definition takes its place
        entry* successor = nullptr;
        for (auto it = G().conflicts.begin(); it != G().conflicts.end();)
        {
            if (it->first != e)
            {
                ++it;
                continue;
            }
            if (!successor)
            {
                successor = it->second;
                it = G().conflicts.erase(it);
                continue;
            }
            it->first =
                successor; // any further rejected definition now conflicts with the successor
            ++it;
        }
        if (successor)
            if (const auto k = G().by_key.find(node_key{e->nk, e->hash});
                k != G().by_key.end() && k->second == e)
            {
                k->second =
                    successor; // the replaced entry stays alive (pointers to it remain valid)
                for (reg_node* const m : successor->nodes)
                    m->linked = true;
            }
    }
    mark_stale_locked(); // no allocation: the next reader publishes
}

// ---- queries (lock-free) ---------------------------------------------------------------------------------
const registry_index& registry::snapshot() const noexcept
{
    publish_if_stale(); // 2.1.11: the first reader after a change publishes (and notifies, 2.1.13)
    const registry_index* const ix = G().index.load(std::memory_order_acquire);
    return ix ? *ix : g_empty_index;
}

void registry::refresh() const
{
    publish_if_stale();
}

std::size_t registry::retained() const noexcept
{
    return snapshot().retained;
}

std::vector<const void*> registry::images() const
{
    std::vector<const void*> out;
    for (const registry_entry& e : snapshot().entries)
        if (e.refs > 0 && std::find(out.begin(), out.end(), e.image_base) == out.end())
            out.push_back(e.image_base);
    return out;
}

std::uint64_t registry::program_hash() const
{
    std::vector<std::uint64_t> hs;
    for (const registry_entry& e : snapshot().entries)
        if (e.nk == node_kind::type && e.refs > 0)
            hs.push_back(e.hash); // already sorted by hash within kind
    return fingerprint64_u64s(hs);
}

const type_desc* registry::find_type(const std::uint64_t h) const noexcept
{
    const registry_entry* const e = snapshot().find(node_kind::type, h);
    return e ? static_cast<const type_desc*>(e->payload) : nullptr;
}

const type_desc* registry::find_type_by_name(const std::string_view name) const noexcept
{
    const type_desc* retained_match = nullptr;
    for (const registry_entry& e : snapshot().entries)
        if (e.nk == node_kind::type && e.name == name)
        {
            if (e.refs > 0)
                return static_cast<const type_desc*>(e.payload);
            if (!retained_match)
                retained_match = static_cast<const type_desc*>(e.payload);
        }
    return retained_match;
}

const call_desc* registry::find_call(const std::uint64_t h) const noexcept
{
    const registry_entry* const e = snapshot().find(node_kind::call, h);
    return e ? static_cast<const call_desc*>(e->payload) : nullptr;
}
std::string_view registry::find_literal(const std::uint64_t id) const noexcept
{
    const registry_entry* const e = snapshot().find(node_kind::literal, id);
    return e ? e->name : std::string_view{};
}
bool registry::has_conflict(const std::uint64_t h) const noexcept
{
    for (const conflict& c : snapshot().conflicts)
        if (c.schema_hash == h)
            return true;
    return false;
}

// ---- export -----------------------------------------------------------------------------------------------
std::vector<std::byte> registry::export_image() const
{
    return export_image_since(0);
}

// 2.2.1: `since` = 0 is the full image; otherwise an RRI delta (rri_flag_delta) holding the entries that appeared
// after that registry version — what a capture_file or a dictionary_tracker writes as a dictionary frame when the
// registry moved. The header's program hash is always the current one.
std::vector<std::byte> registry::export_image_since(const std::uint64_t since) const
{
    const registry_index& ix = snapshot(); // one generation for header and body
    std::vector<std::uint64_t> hs;
    std::uint32_t n = 0;
    for (const registry_entry& e : ix.entries)
    {
        if (e.nk == node_kind::scope)
            continue; // 2.2.1: scope nodes have no ENTRY; export_image_scope reads them
        if (e.nk == node_kind::type)
            hs.push_back(e.hash);
        if (e.added > since)
            ++n;
    }
    std::vector<std::byte> out(fmt::rri_header_size);
    fmt::writer w{out.data()};
    w.put_bytes(fmt::rri_magic.data(), 4);
    w.put(fmt::format_major);
    w.put(fmt::format_minor);
    w.put(since ? fmt::rri_flag_delta : std::uint32_t{0});
    w.put(fmt::hash_algo_xxh3_64);
    w.put(n);
    w.put(fingerprint64_u64s(hs));
    w.put(std::uint32_t{0});
    w.put(fmt::host_abi_tag); // format 1.3: the ABI of this process (FR-W4)
    for (const registry_entry& e : ix.entries)
        if (e.nk != node_kind::scope && e.added > since)
            out.insert(out.end(), e.blob.begin(), e.blob.end());
    return out;
}

// 2.2.1: a scope's dictionary. The closure is computed on the parsed form of the full image (field type hashes
// and call arguments), then the selected entries' blobs are emitted in index order.
std::size_t registry::scope_calls(const std::uint64_t scope_hash) const noexcept
{
    std::size_t n = 0;
    for (const registry_entry& e : snapshot().entries)
        if (e.nk == node_kind::scope && e.payload &&
            static_cast<const scope_desc*>(e.payload)->scope_hash == scope_hash)
            ++n;
    return n;
}

std::vector<std::byte> registry::export_image_scope(const std::uint64_t scope_hash,
                                                    const std::uint64_t since) const
{
    // An entry belongs to the scope's dictionary from the version at which some (scope, call) node reaching it
    // appeared, or the entry itself appeared, whichever is later — and the earliest such version over all the
    // nodes decides. A delta since `since` therefore also carries an old type that a call registered later (a
    // plugin's) brought into the scope.
    const registry_index& ix = snapshot();
    dyn::dictionary all;
    static_cast<void>(all.add_rri(export_image_since(0)));
    std::map<std::uint64_t, std::uint64_t> types, literals,
        calls; // hash → version it entered the scope
    const auto enter = [](std::map<std::uint64_t, std::uint64_t>& m, const std::uint64_t h,
                          const std::uint64_t at) {
        const auto [it, inserted] = m.try_emplace(h, at);
        if (!inserted && at < it->second)
            it->second = at;
    };
    std::map<std::uint64_t, std::uint64_t>
        added_of[3]; // type, literal, call → registry_entry::added
    for (const registry_entry& e : ix.entries)
        if (e.nk == node_kind::type || e.nk == node_kind::literal || e.nk == node_kind::call)
            added_of[kind_slot(e.nk)].emplace(e.hash, e.added);
    const auto added = [&](const node_kind nk, const std::uint64_t h) {
        const auto it = added_of[kind_slot(nk)].find(h);
        return it == added_of[kind_slot(nk)].end() ? std::uint64_t{0} : it->second;
    };
    for (const registry_entry& e : ix.entries)
    {
        if (e.nk != node_kind::scope || !e.payload)
            continue;
        const auto* const sd = static_cast<const scope_desc*>(e.payload);
        if (sd->scope_hash != scope_hash)
            continue;
        const auto at = [&](const node_kind nk, const std::uint64_t h) {
            return std::max(e.added, added(nk, h));
        };
        enter(calls, sd->call_hash, at(node_kind::call, sd->call_hash));
        std::vector<std::uint64_t> pending, seen;
        const auto reach_type = [&](const std::uint64_t h) {
            if (std::find(seen.begin(), seen.end(), h) == seen.end())
            {
                seen.push_back(h);
                pending.push_back(h);
                enter(types, h, at(node_kind::type, h));
            }
        };
        if (const dyn::call* const k = all.find_call(sd->call_hash))
            for (const dyn::call_arg& a : k->args)
            {
                if (a.is_literal)
                    enter(literals, a.hash, at(node_kind::literal, a.hash));
                else if (a.ak == fmt::arg_type || (a.ak == fmt::arg_const && kind_size(a.k) == 0))
                    reach_type(a.hash);
            }
        while (!pending.empty())
        { // the types' members, recursively: every nested type hash a FIELD or an element descriptor names
            const std::uint64_t h = pending.back();
            pending.pop_back();
            const dyn::type* const t = all.find_type(h);
            if (!t)
                continue;
            std::vector<const dyn::field*> stack;
            for (const dyn::field& f : t->fields)
                stack.push_back(&f);
            while (!stack.empty())
            {
                const dyn::field* const f = stack.back();
                stack.pop_back();
                if (f->type_hash != 0)
                    reach_type(f->type_hash);
                for (const dyn::field& e2 : f->elem)
                    stack.push_back(&e2);
            }
        }
    }
    const auto entered = [&](const registry_entry& e) -> std::uint64_t {
        const std::map<std::uint64_t, std::uint64_t>* set = e.nk == node_kind::type      ? &types
                                                            : e.nk == node_kind::literal ? &literals
                                                            : e.nk == node_kind::call    ? &calls
                                                                                         : nullptr;
        if (!set)
            return 0;
        const auto it = set->find(e.hash);
        return it == set->end() ? 0 : it->second; // 0: not in the scope (every real version is ≥ 1)
    };
    std::vector<std::uint64_t> hs;
    std::uint32_t n = 0;
    for (const registry_entry& e : ix.entries)
        if (const std::uint64_t v = entered(e); v != 0)
        {
            if (e.nk == node_kind::type)
                hs.push_back(e.hash);
            if (v > since)
                ++n;
        }
    std::vector<std::byte> out(fmt::rri_header_size);
    fmt::writer w{out.data()};
    w.put_bytes(fmt::rri_magic.data(), 4);
    w.put(fmt::format_major);
    w.put(fmt::format_minor);
    w.put(since ? fmt::rri_flag_delta : std::uint32_t{0});
    w.put(fmt::hash_algo_xxh3_64);
    w.put(n);
    w.put(fingerprint64_u64s(
        hs)); // the scope's program hash: over the scope's types (not the FILE header's)
    w.put(std::uint32_t{0});
    w.put(fmt::host_abi_tag);
    for (const registry_entry& e : ix.entries)
        if (const std::uint64_t v = entered(e); v != 0 && v > since)
            out.insert(out.end(), e.blob.begin(), e.blob.end());
    return out;
}

result<void> registry::export_image(const int fd) const
{
    return tools::write_all(fd, export_image());
}

result<void> registry::export_image(const std::string_view path) const
{
    const std::string p(path);
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(error::io_error);
    const auto r = export_image(fd);
    ::close(fd);
    return r;
}

namespace
{
dyn::dictionary to_dictionary(const registry& r)
{
    dyn::dictionary d;
    static_cast<void>(d.add_rri(r.export_image()));
    return d;
}
} // namespace
std::string registry::to_json() const
{
    return to_dictionary(*this).to_json();
}
std::string registry::to_toml() const
{
    return to_dictionary(*this).to_toml();
}

std::uint64_t registry::on_change(change_callback cb)
{
    std::lock_guard lock(G().mu);
    const std::uint64_t id = G().next_callback_id++;
    G().callbacks.emplace_back(id, std::move(cb));
    return id;
}

bool registry::remove_on_change(const std::uint64_t id)
{
    std::lock_guard lock(G().mu);
    return std::erase_if(G().callbacks, [id](const auto& c) { return c.first == id; }) > 0;
}

std::uint64_t registry::generations() const noexcept
{
    return G().generations.load(std::memory_order_relaxed);
}

bool registry::unload_guard_active(const void* const addr) noexcept
{
    return image::can_unload(addr);
}

// ---- environment + signal hooks (FR-R9, §6.11) ------------------------------------------------------------
namespace
{

bool dump_configured()
{
    return std::getenv("ES_SERREG_DUMP_REGISTRY") || std::getenv("ES_SERREG_DUMP_REGISTRY_FD");
}

void do_dump(const registry& r)
{
    const char* const fmt_env = std::getenv("ES_SERREG_DUMP_FORMAT");
    const char* const path = std::getenv("ES_SERREG_DUMP_REGISTRY");
    const char* const fd_env = std::getenv("ES_SERREG_DUMP_REGISTRY_FD");
    auto render = [&](const std::string_view f) -> std::vector<std::byte> {
        if (f == "json" || f == "toml")
        {
            const std::string s = f == "json" ? r.to_json() : r.to_toml();
            return {reinterpret_cast<const std::byte*>(s.data()),
                    reinterpret_cast<const std::byte*>(s.data() + s.size())};
        }
        return r.export_image();
    };
    if (path)
    {
        const std::string_view f = fmt_env ? std::string_view(fmt_env) : std::string_view("json");
        if (std::strcmp(path, "-") == 0)
            static_cast<void>(tools::write_all(1, render(f)));
        else
        {
            const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0)
            {
                static_cast<void>(tools::write_all(fd, render(f)));
                ::close(fd);
            }
        }
    }
    if (fd_env)
    {
        const std::string_view f = fmt_env ? std::string_view(fmt_env) : std::string_view("rri");
        static_cast<void>(tools::write_all(std::atoi(fd_env), render(f)));
    }
    if (const char* const e = std::getenv("ES_SERREG_DUMP_EXIT"); e && *e == '1')
        std::_Exit(0);
}

int parse_signal(const char* const s)
{
    if (std::strcmp(s, "USR1") == 0 || std::strcmp(s, "SIGUSR1") == 0)
        return SIGUSR1;
    if (std::strcmp(s, "USR2") == 0 || std::strcmp(s, "SIGUSR2") == 0)
        return SIGUSR2;
    if (std::strcmp(s, "RTMIN") == 0)
        return SIGRTMIN;
    if (std::strncmp(s, "RTMIN+", 6) == 0)
    {
        const int n = std::atoi(s + 6);
        return (n >= 0 && SIGRTMIN + n <= SIGRTMAX) ? SIGRTMIN + n : 0;
    }
    return 0;
}

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

void start_helper_if_configured()
{
    if (G().dump_signo == 0 || G().helper_running.load())
        return;
    pthread_t th;
    if (pthread_create(&th, nullptr, &helper_main, nullptr) != 0)
    {
        detail::report_line(
            "es::serreg: ES_SERREG_DUMP_SIGNAL: could not start the dump thread; feature disabled");
        G().dump_signo = 0;
        return;
    }
    G().helper = th;
    G().helper_running.store(true);
}

// 2.1.13: registered from the library constructor (there is no dlopen() interposer to defer it to). Registered
// before any guard of a program linked against libserreg, so it runs after their destructors: the exit dump sees
// every entry (retained ones included), and no callback runs after it started.
void exit_hook()
{
    G().exiting.store(true, std::memory_order_release);
    if (G().helper_running.load())
    {
        G().helper_stop.store(true);
        pthread_kill(G().helper, G().dump_signo);
        pthread_join(G().helper, nullptr);
        G().helper_running.store(false);
    }
    if (G().pushed.load(std::memory_order_relaxed))
        registry::instance().dump_if_requested();
    free_at_exit();
}

// Q10 (2.1.5): one line on stderr when a second copy of libserreg — a static libserreg.a linked into a plugin,
// or into an executable next to libserreg.so — starts in a process that already has one. Each copy keeps its own
// registry, so types registered through one are invisible to the other (deviations.md, "static libserreg in two
// images"). The first marker in the global scope names the first copy; a copy whose marker is not in the global
// scope (a static executable linked without -rdynamic) cannot be seen.
const char own_marker = 2;
void warn_if_second_copy()
{
    const void* const first = image::first_registry_marker();
    if (!first || first == &own_marker)
        return;
    detail::report_line("es::serreg: two copies of libserreg in one process ({} and {}): each has "
                        "its own registry; link every image against libserreg.so",
                        image_name(image_of(first)), image_name(image_of(&own_marker)));
}

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
    if (const char* const c = std::getenv("ES_SERREG_CONFLICT"))
    {
        if (std::strcmp(c, "abort") == 0)
            G().on_conflict = conflict_policy::abort_;
        else if (std::strcmp(c, "silent") == 0)
            G().on_conflict = conflict_policy::silent;
    }
    if (const char* const on = std::getenv("ES_SERREG_DUMP_ON");
        on && dump_configured() && (std::strcmp(on, "change") == 0 || std::strcmp(on, "both") == 0))
        static_cast<void>(registry::instance().on_change([](const registry& r) { do_dump(r); }));
    if (const char* const sig = std::getenv("ES_SERREG_DUMP_SIGNAL"))
    {
        const int signo = parse_signal(sig);
        if (signo <= 0)
            detail::report_line(
                "es::serreg: ES_SERREG_DUMP_SIGNAL={} is not USR1|USR2|RTMIN+n; ignored", sig);
        else
        {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, signo);
            if (pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0)
                detail::report_line("es::serreg: could not block the dump signal; ignored");
            else
                G().dump_signo = signo;
        }
    }
    start_helper_if_configured();
    std::atexit(&exit_hook);
}

} // namespace

void registry::dump_if_requested() const
{
    const char* const on = std::getenv("ES_SERREG_DUMP_ON");
    if (dump_configured() && !(on && std::strcmp(on, "change") == 0))
        do_dump(*this);
}

} // namespace es::serreg

// The marker warn_if_second_copy() looks for: one per copy of libserreg, always in the dynamic symbol table.
extern "C" [[gnu::visibility("default"), gnu::used, gnu::noinline]] const void*
es_serreg_registry_marker_v3() noexcept
{
    return &es::serreg::own_marker;
}
