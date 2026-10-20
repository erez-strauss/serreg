// tests/dlopen/host.cpp — dlopen/dlclose behaviour of the registry (spec §6.3b, FR-R6/R7/R10/R11/R12; v1.0.2
// items gk 1.5 / cc F11 / cc F12 / cc F13 / cc F15; 2.1.13 decision A: heap copies at load, no abort, reload).
//   host <plugin.so> keep           A — dlclose never aborts: the plugin is unmapped, its entries are retained
//                                        (refs 0), its descriptors stay readable and its frames still decode
//   host <plugin.so> reload         B — 50 load/unload cycles of one plugin revive the same entries: no growth
//   host <plugin.so> nodelete       C — RTLD_NODELETE: dlclose is a no-op for the registry
//   host <inner.so> nested <nested.so>           D — nested dlclose (nested's destructor closes inner): both unmapped,
//                                                    both retained, images() back to the baseline
//   host <v1.so> changed <v2.so>                 E — the plugin rebuilt with a changed layout: a new entry next to
//                                                    the retained one, no conflict, the loaded one preferred by name
//   host <b.so> concurrent <c.so>                F — two threads dlclose two plugins at once
//   host <a.so> conflict <b.so>                  H — conflicting layouts of one type: stderr warning, conflicts()==1;
//                                                    unloading the kept definition promotes the other
//   host <nounique.so> nounique                  I — plugin built without -fno-gnu-unique/hidden: still unloadable,
//                                                    unload_guard_active() true, dlclose keeps its entries
//   host <b.so> churn                            J — export_image() loop vs. dlopen/dlclose loop for 200 ms
//   host <b.so> churn2 <c.so>                    L — 2.1.5 (G2-5): two threads load and unload two plugins at once for
//                                                    300 ms; a lock-order inversion would hang (alarm 60 s)
//   host <alias.so> alias                        K — 2.1.5 (G2-10): a plugin of aliases only: no new entry, the version
//                                                    moves, on_change fires once
// The host serializes its own type first so the registry is non-empty and the plugin's entries are additive.
// 2.1.13: change callbacks run when the next index is published; the host calls registry::refresh() right after
// each dlopen()/dlclose() to publish and be notified on its own thread.
#include <es/serreg/serreg.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace host
{
struct Ping
{
    std::uint64_t seq;
};
} // namespace host

namespace
{

using namespace es::serreg;

int fail(const char* const what)
{
    std::println(stderr, "FAIL: {}", what);
    return 1;
}

void* open_or_die(const char* const path, const int flags = RTLD_NOW)
{
    void* const h = ::dlopen(path, flags);
    if (!h)
    {
        std::println(stderr, "dlopen({}): {}", path, ::dlerror());
        std::exit(1);
    }
    return h;
}
template<class Fn> Fn sym(void* const h, const char* const name)
{
    void* const p = ::dlsym(h, name);
    if (!p)
    {
        std::println(stderr, "dlsym({}): {}", name, ::dlerror());
        std::exit(1);
    }
    return reinterpret_cast<Fn>(p);
}
bool mapped(const char* const path)
{
    void* const h = ::dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (h)
        ::dlclose(h); // the probe's own reference
    return h != nullptr;
}

// Captures everything written to fd 2 while `fn` runs (the schema-conflict warning goes to stderr).
template<class F> std::string capture_stderr(F&& fn)
{
    char tmpl[] = "/tmp/serreg_stderr_XXXXXX";
    const int tmp = ::mkstemp(tmpl);
    if (tmp < 0)
        return {};
    ::unlink(tmpl);
    std::fflush(stderr);
    const int saved = ::dup(2);
    ::dup2(tmp, 2);
    fn();
    std::fflush(stderr);
    ::dup2(saved, 2);
    ::close(saved);
    std::string out;
    ::lseek(tmp, 0, SEEK_SET);
    char buf[1024];
    for (;;)
    {
        const ssize_t n = ::read(tmp, buf, sizeof buf);
        if (n <= 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(tmp);
    return out;
}

// ---- A/C: single plugin --------------------------------------------------------------------------------------
int single_plugin(const char* const path, const std::string_view mode)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    const std::uint64_t version0 = reg.version();
    const std::size_t images0 = reg.images().size();
    const std::uint64_t program0 = reg.program_hash();
    const std::uint64_t generations0 = reg.generations();
    std::atomic<int> changes{0};
    std::atomic<std::size_t> size_seen{0};
    const std::uint64_t cb = reg.on_change([&](const registry& r) {
        ++changes;
        size_seen = r.size(); // runs after the publish, with every new entry visible
    });
    if (reg.find_type_by_name("plugin::Tick"))
        return fail("host must not know plugin::Tick before dlopen");

    const int flags = RTLD_NOW | (mode == "nodelete" ? RTLD_NODELETE : 0);
    void* const h = open_or_die(path, flags);
    reg.refresh(); // 2.1.13: publish and notify on this thread
    const auto expected_fn = sym<int (*)()>(h, "plugin_expected_entries");
    const auto run_fn = sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run");
    const std::size_t added = static_cast<std::size_t>(expected_fn());

    // FR-R6: the plugin's entities are registered by dlopen itself, before any plugin code is called.
    const std::size_t size1 = reg.size();
    std::println(
        "size {} -> {} (+{} expected), version {} -> {}, images {} -> {}, on_change fired {}",
        size0, size1, added, version0, reg.version(), images0, reg.images().size(), changes.load());
    if (size1 != size0 + added)
        return fail("registry did not grow by the plugin's entry count (FR-R6)");
    if (reg.version() <= version0)
        return fail("version did not increase on dlopen (FR-R3)");
    if (changes.load() != 1)
        return fail("on_change must fire exactly once for the dlopen's generation (2.1.13)");
    if (size_seen.load() != size0 + added)
        return fail("on_change ran before all of the plugin's entries were published");
    // 2.1.11: the plugin's registrations are published as one index, not one each
    if (reg.generations() != generations0 + 1)
        return fail("a dlopen must publish one index for all of its registrations (2.1.11)");
    if (reg.images().size() != images0 + 1)
        return fail("images() did not gain the plugin (FR-R12)");
    if (reg.retained() != 0)
        return fail("nothing is retained before an unload");
    const type_desc* const tick = reg.find_type_by_name("plugin::Tick");
    if (!tick)
        return fail("plugin::Tick not found by name");
    if (tick->fields.size() != 4 || tick->fields[3].k != kind::enum_)
        return fail("plugin::Tick descriptor");
    if (!reg.find_type_by_name("plugin::Flag"))
        return fail("plugin::Flag (component) not found");
    // 2.1.13: the heap entry owns copies of everything; nothing points into the plugin image
    const registry_entry* const e = reg.snapshot().find(node_kind::type, tick->schema_hash);
    if (!e)
        return fail("snapshot().find(plugin::Tick)");
    const void* const plugin_base = e->image_base;
    const auto outside = [&](const void* const p) {
        Dl_info d2{};
        return !p || !dladdr(p, &d2) || d2.dli_fbase != plugin_base;
    };
    if (!outside(e->blob.data()) || !outside(e->name.data()))
        return fail("entry blob/name copy lives inside the plugin image (cc F5/F13)");
    if (e->payload != tick || !outside(tick) || !outside(tick->name.data()) ||
        !outside(tick->fields.data()) || !outside(tick->fields[0].name.data()) ||
        !outside(tick->blob.data()))
        return fail("plugin::Tick descriptor (or its names/fields/blob) lives inside the plugin "
                    "image (G2-4)");
    if (e->refs != 1)
        return fail("the plugin's entry must have one reference while loaded");
    // cc F12: the plugin image is genuinely unloadable (no DF_1_NODELETE) unless RTLD_NODELETE was requested
    // NOTE: unload_guard_active() inspects the image's on-disk DT_FLAGS_1. RTLD_NODELETE is a run-time flag glibc
    // keeps in the private link_map (l_flags_1), so an RTLD_NODELETE-loaded image still reports true here.
    const bool guard_active = registry::unload_guard_active(reinterpret_cast<const void*>(run_fn));
    std::println("unload_guard_active(plugin) = {:d}", guard_active);
    if (!guard_active)
        return fail("plugin must be unloadable (unload_guard_active) — is the DSO DF_1_NODELETE?");
    if (registry::unload_guard_active(reinterpret_cast<const void*>(&fail)))
        return fail("main program must never be unloadable");
    if (registry::unload_guard_active(nullptr))
        return fail("unload_guard_active(nullptr) must be false");

    // The host can inspect the plugin's frames through the shared registry (FR-R7).
    std::vector<std::byte> frame;
    if (run_fn(&frame) != 0)
        return fail("plugin_run");
    const auto fv = inspect(frame);
    if (!fv)
    {
        std::println(stderr, "inspect: {}", to_string(fv.error()));
        return fail("inspect() could not resolve the plugin's call");
    }
    std::vector<record_view> recs;
    for (const record_view r : fv->records())
        recs.push_back(r);
    if (fv->size() != 2 || recs.size() != 2 || !recs[0].is_literal || recs[1].desc != tick)
        return fail("inspect() records");
    if (reg.find_literal(recs[0].hash) != "tick")
        return fail("plugin literal not resolved");
    std::string text;
    const auto dict = tools::load_dictionary_rri(reg.export_image());
    if (!dict)
        return fail("export_image after dlopen");
    if (!tools::dump(frame, &*dict, tools::dump_options{}, text) ||
        text.find("plugin::Tick") == std::string::npos ||
        text.find("px: 101.25") == std::string::npos)
        return fail("dump of the plugin frame");

    if (mode == "keep")
    {
        const std::uint64_t v1 = reg.version();
        const registry_index& before =
            reg.snapshot(); // cc F13: a snapshot taken before the unload stays valid
        const std::string tick_desc_name(tick->name);
        const std::uint64_t tick_hash = tick->schema_hash;
        const std::uint64_t tick_layout = tick->layout_hash;
        std::vector<std::string> tick_fields;
        for (const field_desc& f : tick->fields)
            tick_fields.emplace_back(f.name);
        const std::vector<std::byte> tick_blob(tick->blob.begin(), tick->blob.end());
        const int changes_before = changes.load();
        if (::dlclose(h) != 0) // 2.1.13: the default policy; no abort, no environment variable
            return fail("dlclose failed");
        reg.refresh();
        if (mapped(path))
            return fail(
                "the plugin is still mapped after dlclose (test needs a really unloaded image)");
        std::println("after dlclose: size {}, retained {}, version {}, images {}", reg.size(),
                     reg.retained(), reg.version(), reg.images().size());
        if (changes.load() != changes_before + 1)
            return fail("on_change must fire once for the dlclose's generation");
        if (reg.version() <= v1)
            return fail("version did not move on unload");
        if (reg.size() != size0 + added || reg.retained() != added)
            return fail("the plugin's entries must be retained after dlclose (2.1.13, FR-R11)");
        if (reg.images().size() != images0)
            return fail("images() still lists the unloaded plugin");
        if (reg.program_hash() != program0)
            return fail("program_hash() covers loaded types only");
        // the same descriptor, still readable and unchanged, found by hash and by name
        if (reg.find_type(tick_hash) != tick || reg.find_type_by_name("plugin::Tick") != tick)
            return fail("the retained plugin::Tick must be found (same descriptor)");
        if (tick->name != tick_desc_name || tick->layout_hash != tick_layout ||
            tick->fields.size() != tick_fields.size())
            return fail("the descriptor obtained before dlclose() changed after it (G2-4)");
        for (std::size_t i = 0; i < tick_fields.size(); ++i)
            if (tick->fields[i].name != tick_fields[i])
                return fail("a field name of the pre-unload descriptor changed (G2-4)");
        if (!std::equal(tick_blob.begin(), tick_blob.end(), tick->blob.begin(), tick->blob.end()))
            return fail("the pre-unload descriptor's blob changed (G2-4)");
        const registry_entry* const after = reg.snapshot().find(node_kind::type, tick_hash);
        if (!after || after->refs != 0)
            return fail("the retained entry must have refs == 0");
        // frames written while the plugin was loaded still decode (maximum stability, 2.1.13)
        const auto again = inspect(frame);
        if (!again || again->size() != 2)
            return fail("the plugin's frame must still decode after dlclose (2.1.13)");
        std::string text2;
        const auto dict2 = tools::load_dictionary_rri(reg.export_image());
        if (!dict2 || !tools::dump(frame, &*dict2, tools::dump_options{}, text2) ||
            text2.find("px: 101.25") == std::string::npos)
            return fail("dump of the plugin frame after dlclose");
        if (before.find(node_kind::type, tick_hash) == nullptr)
            return fail("old snapshot changed");
        if (&reg.snapshot() == &before)
            return fail("a new generation must have been published");
        static_cast<void>(reg.remove_on_change(cb));
        std::println("ok: dlclose kept the plugin's entries; descriptors, old snapshot and frames "
                     "stay valid");
        return 0;
    }
    if (mode == "nodelete")
    {
        const std::uint64_t v1 = reg.version();
        if (::dlclose(h) != 0)
            return fail("dlclose failed with RTLD_NODELETE");
        if (reg.size() != size0 + added || reg.retained() != 0)
            return fail("RTLD_NODELETE image must stay registered and loaded");
        if (reg.version() != v1)
            return fail("version must not change when nothing unloads");
        if (!reg.find_type_by_name("plugin::Tick"))
            return fail("plugin::Tick vanished under RTLD_NODELETE");
        if (!inspect(frame))
            return fail("frame no longer decodable under RTLD_NODELETE");
        static_cast<void>(reg.remove_on_change(cb));
        std::println("ok: RTLD_NODELETE keeps the registry intact");
        return 0;
    }
    return fail("unknown mode");
}

// ---- B: reload cycles ------------------------------------------------------------------------------------------
int reload(const char* const path)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    const std::size_t images0 = reg.images().size();
    const type_desc* first = nullptr;
    std::size_t added = 0;
    std::vector<std::byte> frame;
    const std::uint64_t gen0 = reg.generations();
    constexpr int cycles = 50;
    for (int i = 0; i < cycles; ++i)
    {
        void* const h = open_or_die(path);
        reg.refresh();
        if (i == 0)
        {
            added = static_cast<std::size_t>(sym<int (*)()>(h, "plugin_expected_entries")());
            if (sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run")(&frame) != 0)
                return fail("plugin_run");
        }
        const type_desc* const t = reg.find_type_by_name("plugin::Tick");
        if (!t)
            return fail("plugin::Tick after load");
        if (!first)
            first = t;
        if (t != first)
            return fail("a reload must revive the same entry (same descriptor), not add a new one");
        if (reg.size() != size0 + added || reg.retained() != 0 ||
            reg.images().size() != images0 + 1)
            return fail("loaded: size/retained/images");
        if (::dlclose(h) != 0)
            return fail("dlclose");
        reg.refresh();
        if (mapped(path))
            return fail("the plugin is still mapped after dlclose");
        if (reg.size() != size0 + added || reg.retained() != added ||
            reg.images().size() != images0)
            return fail("unloaded: size/retained/images");
        if (!inspect(frame))
            return fail("the first load's frame must decode in every cycle");
    }
    const std::uint64_t gens = reg.generations() - gen0;
    std::println("reload: {} cycles, size {} (baseline {} + {}), {} generations", cycles,
                 reg.size(), size0, added, gens);
    if (gens != 2 * cycles)
        return fail("one generation per load and one per unload");
    std::println("ok: {} load/unload cycles revived the same entries; no growth", cycles);
    return 0;
}

// ---- D: nested dlclose ---------------------------------------------------------------------------------------
int nested(const char* const inner_path, const char* const nested_path)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    const std::size_t images0 = reg.images().size();
    void* const inner = open_or_die(inner_path);
    void* const outer = open_or_die(nested_path);
    sym<void (*)(void*)>(outer, "nested_set_inner")(inner);
    const std::size_t added =
        static_cast<std::size_t>(sym<int (*)()>(inner, "plugin_expected_entries")()) +
        static_cast<std::size_t>(sym<int (*)()>(outer, "nested_expected_entries")());
    if (reg.size() != size0 + added)
        return fail("both plugins must be registered");
    if (reg.images().size() != images0 + 2)
        return fail("two new images expected");
    if (::dlclose(outer) != 0)
        return fail("dlclose(outer)"); // closes inner from inside its own unload
    std::println(
        "after nested dlclose: size {} (baseline {} + {}), retained {}, images {} (baseline {})",
        reg.size(), size0, added, reg.retained(), reg.images().size(), images0);
    if (reg.size() != size0 + added || reg.retained() != added)
        return fail("nested dlclose: both images' entries must be retained");
    if (reg.images().size() != images0)
        return fail("images() still lists an unloaded image");
    if (!reg.find_type_by_name("nested::Outer") || !reg.find_type_by_name("plugin_b::Tick"))
        return fail("retained types must stay resolvable");
    if (mapped(inner_path) || mapped(nested_path))
        return fail("a plugin is still mapped after the nested dlclose");
    std::println("ok: nested dlclose unloaded both images and retained their entries");
    return 0;
}

// ---- E: the plugin rebuilt with a changed layout --------------------------------------------------------------
int changed(const char* const v1_path, const char* const v2_path)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    void* h = open_or_die(v1_path);
    std::vector<std::byte> frame1;
    if (sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run")(&frame1) != 0)
        return fail("v1 plugin_run");
    const type_desc* const t1 = reg.find_type_by_name("plugin::Tick");
    if (!t1 || t1->fields.size() != 4)
        return fail("v1 plugin::Tick");
    const std::uint64_t h1 = t1->schema_hash;
    const std::size_t size1 = reg.size();
    if (::dlclose(h) != 0)
        return fail("dlclose(v1)");
    h = open_or_die(v2_path);
    std::vector<std::byte> frame2;
    if (sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run")(&frame2) != 0)
        return fail("v2 plugin_run");
    const type_desc* const t2 = reg.find_type_by_name("plugin::Tick");
    std::println(
        "changed layout: v1 hash {:016x} ({} fields), v2 {}; size {} -> {} -> {}, conflicts {}", h1,
        t1->fields.size(), t2 ? t2->fields.size() : 0, size0, size1, reg.size(),
        reg.conflicts().size());
    if (!t2 || t2->fields.size() != 5 || t2->schema_hash == h1)
        return fail("v2 plugin::Tick must be a new definition, preferred by name while loaded");
    if (!reg.conflicts().empty())
        return fail("a changed layout has a new schema hash: no conflict");
    if (reg.find_type(h1) != t1 || t1->fields.size() != 4)
        return fail("the retained v1 definition must stay findable by hash and unchanged");
    if (reg.size() != size1 + 2) // Tick and the call get new hashes; Flag and "tick" are revived
        return fail("v2 must add exactly the changed Tick and its call");
    if (!inspect(frame1) || !inspect(frame2))
        return fail("frames of both versions must decode");
    if (::dlclose(h) != 0)
        return fail("dlclose(v2)");
    h = open_or_die(v1_path);
    if (reg.find_type_by_name("plugin::Tick") != t1)
        return fail("reloading v1 must revive its entry and make it the loaded one again");
    if (reg.size() != size1 + 2)
        return fail("reloading v1 must not add entries");
    if (::dlclose(h) != 0)
        return fail("dlclose(v1) again");
    std::println("ok: a rebuilt plugin coexists with its earlier version; each reload revives its "
                 "own entries");
    return 0;
}

// ---- F: concurrent dlclose -----------------------------------------------------------------------------------
int concurrent(const char* const b_path, const char* const c_path)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    const std::size_t images0 = reg.images().size();
    void* const hb = open_or_die(b_path);
    void* const hc = open_or_die(c_path);
    std::vector<std::byte> fb, fc;
    if (sym<int (*)(std::vector<std::byte>*)>(hb, "plugin_run")(&fb) ||
        sym<int (*)(std::vector<std::byte>*)>(hc, "plugin_run")(&fc))
        return fail("plugin_run");
    if (!reg.find_type_by_name("plugin_b::Tick") || !reg.find_type_by_name("plugin_c::Tick"))
        return fail("both plugins registered");
    const std::size_t loaded = reg.size();
    std::atomic<int> go{0};
    const auto closer = [&](void* const h) {
        ++go;
        while (go.load() < 2)
        {
        }
        ::dlclose(h);
    };
    std::thread t1(closer, hb), t2(closer, hc);
    t1.join();
    t2.join();
    std::println("after concurrent dlclose: size {} (loaded {}, baseline {}), retained {}",
                 reg.size(), loaded, size0, reg.retained());
    if (reg.size() != loaded || reg.retained() != loaded - size0)
        return fail("concurrent dlclose: every plugin entry must be retained");
    if (reg.images().size() != images0)
        return fail("images() back to the baseline");
    if (reg.find_literal(fingerprint64("tick")) != "tick")
        return fail("the shared literal \"tick\" is retained once both images are gone");
    if (!inspect(fb) || !inspect(fc))
        return fail("frames must still decode");
    if (mapped(b_path) || mapped(c_path))
        return fail("a plugin is still mapped");
    std::println("ok: two threads dlclosed two plugins; their entries are retained");
    return 0;
}

// ---- H: conflicting layouts across two images ----------------------------------------------------------
int conflict_mode(const char* const a_path, const char* const b_path)
{
    registry& reg = registry::instance();
    if (!reg.conflicts().empty())
        return fail("registry must start without conflicts");
    void* const ha = open_or_die(a_path);
    const type_desc* const msg = reg.find_type_by_name("conf::Msg");
    if (!msg)
        return fail("conf::Msg from a.so");
    const std::size_t size1 = reg.size();
    void* hb = nullptr;
    const std::string err = capture_stderr([&] { hb = ::dlopen(b_path, RTLD_NOW); });
    if (!hb)
        return fail("dlopen(b.so)");
    std::print("stderr during dlopen(b): {}", err);
    if (err.find("schema conflict") == std::string::npos ||
        err.find("conf::Msg") == std::string::npos)
        return fail("no 'schema conflict' warning on stderr (cc F15)");
    Dl_info dia{}, dib{};
    dladdr(reinterpret_cast<const void*>(sym<int (*)(std::vector<std::byte>*)>(ha, "conf_run")),
           &dia);
    dladdr(reinterpret_cast<const void*>(sym<std::uint64_t (*)()>(hb, "conf_b_schema_hash")), &dib);
    if (err.find(dia.dli_fname) == std::string::npos ||
        err.find(dib.dli_fname) == std::string::npos)
        return fail("warning must name both images");
    const std::string a_name = dia.dli_fname, b_name = dib.dli_fname;
    const auto cs = reg.conflicts();
    if (cs.size() != 1)
        return fail("conflicts().size() != 1");
    if (cs[0].schema_hash != msg->schema_hash || cs[0].name != "conf::Msg")
        return fail("conflict record contents");
    if (cs[0].image_a != dia.dli_fbase || cs[0].image_b != dib.dli_fbase)
        return fail("conflict record images");
    if (!reg.has_conflict(msg->schema_hash))
        return fail("has_conflict()");
    if (reg.find_type(msg->schema_hash) != msg)
        return fail("the first definition stays authoritative");
    if (reg.size() != size1)
        return fail("a conflicting definition must not be counted as an entry");
    if (sym<std::uint64_t (*)()>(hb, "conf_b_schema_hash")() != msg->schema_hash)
        return fail("test misconfigured: b's hash differs");
    // the registry's RRI export names only the kept definition; the ELF dictionaries of a and b disagree
    dyn::dictionary d;
    if (!d.add_rri(reg.export_image()) || !d.conflicts.empty() ||
        d.find_type(msg->schema_hash)->layout_hash != msg->layout_hash)
        return fail("export after conflict");
    const auto ea = tools::extract_registry_from_elf(a_name),
               eb = tools::extract_registry_from_elf(b_name);
    if (!ea || !eb)
        return fail("extract_registry_from_elf on the plugins");
    if (!ea->conflicts.empty() || !eb->conflicts.empty())
        return fail("each image alone is conflict-free");
    dyn::dictionary merged = *ea;
    merged.merge(*eb);
    if (merged.conflicts.size() != 1)
        return fail(
            "merging a's and b's ELF dictionaries must record exactly one conflict (cc F16)");
    // 2.1.13: the kept definition unloads; the still-loaded conflicting one takes its place
    const std::uint64_t layout_a = msg->layout_hash;
    if (::dlclose(ha) != 0)
        return fail("dlclose(a)");
    const type_desc* const now = reg.find_type(msg->schema_hash);
    if (!reg.conflicts().empty() || reg.has_conflict(msg->schema_hash))
        return fail("the conflict must end when one side unloads");
    if (!now || now->layout_hash == layout_a)
        return fail("b's definition must take the place of the unloaded a");
    if (msg->layout_hash != layout_a)
        return fail("a's descriptor must stay readable after it was replaced");
    if (::dlclose(hb) != 0)
        return fail("dlclose(b)");
    if (reg.find_type(msg->schema_hash) != now)
        return fail("b's definition is retained after b unloads");
    std::println("ok: conflicting second image warned, recorded once, first definition kept; it "
                 "took over after "
                 "the first unloaded");
    return 0;
}

// ---- I: plugin built without -fno-gnu-unique / hidden visibility ----------------------------------------
int nounique(const char* const path)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    void* const h = open_or_die(path);
    const auto run_fn = sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run");
    if (reg.size() != size0 + 4)
        return fail("nounique plugin did not register 4 entries");
    if (!reg.find_type_by_name("plugin_nu::Tick"))
        return fail("plugin_nu::Tick");
    const bool active = registry::unload_guard_active(reinterpret_cast<const void*>(run_fn));
    std::println("unload_guard_active(nounique plugin) = {:d}", active);
    if (!active)
        return fail("attribute-only mitigation failed: the DSO is DF_1_NODELETE (some per-image "
                    "symbol is still STB_GNU_UNIQUE) — cc F12");
    if (::dlclose(h) != 0)
        return fail("dlclose");
    if (mapped(path))
        return fail("the plugin without extra flags must really unload (reload needs it)");
    if (reg.size() != size0 + 4 || reg.retained() != 4 || !reg.find_type_by_name("plugin_nu::Tick"))
        return fail("its entries must be retained");
    std::println("ok: plugin built without extra flags unloads, and its entries are retained");
    return 0;
}

// ---- K (2.2.1, P-16/A-9): a plugin's scoped sink ------------------------------------------------------------
int scoped(const char* const path)
{
    registry& reg = registry::instance();
    const std::uint64_t v0 = reg.version();
    void* const h = open_or_die(path);
    const auto run_fn = sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run");
    const std::uint64_t scope = sym<std::uint64_t (*)()>(h, "plugin_scope_hash")();
    if (reg.scope_calls(scope) != 1)
        return fail("the plugin's scoped call must register one (scope, call) node");
    std::vector<std::byte> frame;
    if (run_fn(&frame) != 0)
        return fail("plugin_run");
    // the scope's full dictionary: the plugin's call, its literal, Quote and the host's Ping (a member)
    dyn::dictionary full;
    if (!full.add_rri(reg.export_image_scope(scope)))
        return fail("export_image_scope does not parse");
    if (full.calls.size() != 1 || full.literals.size() != 1 ||
        !full.find_type(schema_hash_v<host::Ping>) || full.types.size() != 2)
        return fail("the scope's dictionary must hold exactly the plugin's call, its literal, "
                    "Quote and Ping");
    // the delta since before the dlopen: Ping was registered long ago by the host, but it entered the scope now
    dyn::dictionary delta;
    if (!delta.add_rri(reg.export_image_scope(scope, v0)))
        return fail("scoped delta does not parse");
    if (!delta.find_type(schema_hash_v<host::Ping>) || delta.calls.size() != 1 ||
        delta.types.size() != 2)
        return fail("a scoped delta must carry the old types a new call brought into the scope");
    // a frame written into the scoped sink decodes with the scope's dictionary alone
    const auto rep = tools::verify(frame, &full);
    if (!rep || !rep->ok() || rep->frames != 1)
        return fail("the scoped sink's frame must verify against the scope's dictionary");
    if (::dlclose(h) != 0)
        return fail("dlclose");
    reg.refresh();
    if (mapped(path))
        return fail("the scoped plugin must really unload");
    // retained: the scope node's name was copied, the export still works and reads the same
    dyn::dictionary after;
    if (reg.scope_calls(scope) != 1 || !after.add_rri(reg.export_image_scope(scope)) ||
        after.to_json() != full.to_json())
        return fail("a retained scope node must export the same scoped dictionary after dlclose");
    std::println(
        "ok: a plugin's scoped sink registers its (scope, call) node, exports {} types, {} "
        "literal, "
        "{} call; the delta since before the dlopen carries host::Ping; retained after dlclose",
        full.types.size(), full.literals.size(), full.calls.size());
    return 0;
}

// ---- J: readers vs. load/unload churn ---------------------------------------------------------------------
int churn(const char* const path)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0}, loads{0}, seen_plugin{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed))
        {
            const std::vector<std::byte> img = reg.export_image();
            dyn::dictionary d;
            if (!d.add_rri(img))
            {
                std::println(stderr, "export_image produced an unparsable RRI during churn");
                std::abort();
            }
            const registry_index& ix = reg.snapshot();
            for (const registry_entry& e : ix.entries)
            {
                volatile std::size_t n = e.name.size() + e.blob.size();
                static_cast<void>(n);
            }
            if (const type_desc* const t = reg.find_type_by_name("plugin_b::Tick"))
            {
                volatile std::size_t n = t->fields.size() + t->fields[0].name.size();
                static_cast<void>(n);
                ++seen_plugin;
            }
            static_cast<void>(reg.program_hash());
            static_cast<void>(reg.to_json());
            ++reads;
        }
    });
    std::size_t added = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < until)
    {
        void* const h = open_or_die(path);
        added = static_cast<std::size_t>(sym<int (*)()>(h, "plugin_expected_entries")());
        std::vector<std::byte> f;
        if (sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run")(&f) != 0)
            return fail("plugin_run during churn");
        if (::dlclose(h) != 0)
            return fail("dlclose during churn");
        ++loads;
    }
    stop.store(true);
    reader.join();
    std::println("churn: {} load/unload cycles, {} reader iterations ({} saw the plugin), size {} "
                 "(baseline {} + {}), retained {}",
                 loads.load(), reads.load(), seen_plugin.load(), reg.size(), size0, added,
                 reg.retained());
    if (loads.load() == 0 || reads.load() == 0)
        return fail("both threads must have made progress");
    if (reg.size() != size0 + added || reg.retained() != added)
        return fail("after the churn the registry holds the plugin's entries once, retained");
    std::println("ok: concurrent readers survived 200 ms of dlopen/dlclose churn");
    return 0;
}

// L (2.1.5, G2-5): registration inside one thread's dlopen() and dropping references inside another's dlclose() at
// the same time; a lock-order inversion with the loader lock would hang (alarm). 2.1.13: every published generation
// notifies exactly once.
int churn2(const char* const path_b, const char* const path_c)
{
    ::alarm(60); // a deadlock ends the test (SIGALRM) instead of hanging ctest
    registry& reg = registry::instance();
    for (const char* const p :
         {path_b, path_c}) // first load: the entries every later cycle revives
        if (::dlclose(open_or_die(p)) != 0)
            return fail("first dlclose");
    reg.refresh();
    const std::size_t size1 = reg.size();
    const std::uint64_t gen1 = reg.generations();
    std::atomic<int> notified{0};
    const std::uint64_t id = reg.on_change([&](const registry&) { ++notified; });
    std::atomic<std::uint64_t> cycles{0};
    std::atomic<bool> bad{false};
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    const auto loop = [&](const char* const path) {
        while (std::chrono::steady_clock::now() < until && !bad)
        {
            void* const h = ::dlopen(path, RTLD_NOW);
            reg.refresh();
            std::vector<std::byte> f;
            if (!h || sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run")(&f) != 0 ||
                ::dlclose(h) != 0)
                bad = true;
            reg.refresh();
            ++cycles;
        }
    };
    std::thread tb(loop, path_b), tc(loop, path_c);
    tb.join();
    tc.join();
    ::alarm(0);
    static_cast<void>(reg.remove_on_change(id));
    const std::uint64_t gens = reg.generations() - gen1;
    std::println(
        "churn2: {} load/unload cycles on two threads, {} generations, {} notifications, size {} "
        "(after the first loads {})",
        cycles.load(), gens, notified.load(), reg.size(), size1);
    if (bad)
        return fail("dlopen/plugin_run/dlclose failed during two-thread churn");
    if (reg.size() != size1)
        return fail("reloads must revive entries, not add them");
    if (static_cast<std::uint64_t>(notified.load()) != gens)
        return fail("every published generation must notify observers exactly once");
    if (gens == 0 || gens > 2 * cycles.load())
        return fail("at least one and at most two generations per cycle");
    std::println("ok: two threads loaded and unloaded plugins concurrently without a deadlock");
    return 0;
}

// K (2.1.5, G2-10): every entry of the plugin already exists in the host (host::Ping and its call).
int alias_only(const char* const path)
{
    registry& reg = registry::instance();
    const std::size_t size0 = reg.size();
    const std::uint64_t v0 = reg.version();
    std::atomic<int> changes{0};
    static_cast<void>(reg.on_change([&](const registry&) { ++changes; }));
    void* const h = open_or_die(path, RTLD_NOW);
    reg.refresh();
    std::println("alias plugin: size {} -> {}, version {} -> {}, on_change fired {}", size0,
                 reg.size(), v0, reg.version(), changes.load());
    if (reg.size() != size0)
        return fail("an alias-only plugin must not add entries");
    if (reg.version() <= v0)
        return fail("aliases publish a new generation: the version must move");
    if (changes.load() != 1)
        return fail("on_change must fire once for an alias-only dlopen (G2-10)");
    std::vector<std::byte> frame;
    if (sym<int (*)(std::vector<std::byte>*)>(h, "plugin_run")(&frame) != 0)
        return fail("plugin_run");
    host::Ping p{};
    if (!deserialize(frame, p) || p.seq != 2)
        return fail("the plugin's frame decodes as the host's type");
    if (::dlclose(h) != 0)
        return fail("dlclose");
    reg.refresh();
    if (changes.load() != 2 || reg.size() != size0 || reg.retained() != 0)
        return fail("dropping the aliases notifies once and leaves the host's entries loaded");
    std::println("ok: alias-only plugin notified observers once per load and unload");
    return 0;
}

} // namespace

namespace host
{
// host <plugin.so> <mode> [<second.so>]  — the modes are listed at the top of this file
struct options
{
    int argc = 0;
    const char* plugin = nullptr;
    std::string_view mode;
    const char* second = nullptr;

    static options parse(const int argc, char** const argv)
    {
        options o;
        o.argc = argc;
        if (argc >= 3)
        {
            o.plugin = argv[1];
            o.mode = argv[2];
        }
        if (argc >= 4)
            o.second = argv[3];
        return o;
    }
};

// Serializes the host's own type, then runs the scenario the mode names.
class app
{
public:
    explicit app(const options& opts) : opts_{opts}
    {
    }

    int run() const
    {
        if (opts_.argc < 3)
            return fail("usage: host <plugin.so> keep|reload|nodelete|nounique|scoped|churn|alias "
                        "| host <a.so> "
                        "nested|changed|concurrent|conflict|churn2 <b.so>");
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::vector<std::byte> mine;
        vector_sink ms{mine};
        if (!serialize(ms, Ping{1}))
            return fail("host serialize");
        return dispatch();
    }

private:
    options opts_;

    int dispatch() const
    {
        const std::string_view mode = opts_.mode;
        const char* const first = opts_.plugin;
        if (mode == "keep" || mode == "nodelete")
            return single_plugin(first, mode);
        if (mode == "reload")
            return reload(first);
        if (mode == "nounique")
            return nounique(first);
        if (mode == "scoped")
            return scoped(first);
        if (mode == "churn")
            return churn(first);
        if (mode == "alias")
            return alias_only(first);
        if (opts_.argc < 4)
            return fail("this mode needs a second plugin");
        const char* const second = opts_.second;
        if (mode == "nested")
            return nested(first, second);
        if (mode == "changed")
            return changed(first, second);
        if (mode == "concurrent")
            return concurrent(first, second);
        if (mode == "conflict")
            return conflict_mode(first, second);
        if (mode == "churn2")
            return churn2(first, second);
        return fail("unknown mode");
    }
};
} // namespace host

int main(const int argc, char** const argv)
{
    return host::app{host::options::parse(argc, argv)}.run();
}
