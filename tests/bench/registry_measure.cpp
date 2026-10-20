// tests/bench/registry_measure.cpp — 2.2.1 (DECISIONS G-5, "K"): what the registry costs at scale, measured.
//
// A program with many registered types pays at start-up (every registration is one push into the registry,
// run from the images' static initialisers), at the first query (the index is published once for the burst),
// and whenever it exports its dictionary (a capture file's opening block, srdump --elf on the reader side).
// This app synthesises N struct entries with blobs that add up to about B bytes — the shape of a program with
// N message types — pushes them exactly as a guard would, and reports:
//
//   push       the N push() calls (an entry per node: a view for the main program, a copy for a plugin; the
//              per-kind lookup insert is deferred to the publish)
//   publish    the first snapshot(): one index for the whole burst (sorted flat_maps, program hash)
//   lookup     find_type() over all N hashes, in a random order (one acquire-load and a flat_map search each)
//   export     export_image(): header + the N blobs
//   parse      dyn::dictionary::add_rri() of that image — the reader's cost (srdump, a capture's dictionary)
//   rss        resident-set growth over the run (/proc/self/statm)
//
// With --assert it enforces the budgets of DECISIONS G-5 (release builds; a debug or sanitizer build only
// reports): 10 000 entries / 10 MB → push + publish ≤ 10 ms; 100 000 entries / 100 MB → ≤ 100 ms. `--elf <bin>`
// measures the reader side on a real binary instead (tools::extract_registry_from_elf).
//
// Usage: registry_measure [--entries N] [--bytes B] [--assert] [--plugin] [--elf <binary>] [--json]
//   --plugin: the descriptors live on the heap, so the registry deep-copies them as it does for a dlopen'd
//   image's entries (the default measures the main program's path: descriptors in this executable's image)
#include <es/serreg/serreg.hpp>
#include <es/serreg/tools.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace registry_measure
{
using namespace es::serreg;
using clock_t = std::chrono::steady_clock;

struct options
{
    std::size_t entries = 100'000;
    std::size_t bytes = 100u << 20;
    bool assert_budget = false;
    bool json = false;
    bool plugin =
        false; // descriptors on the heap: the registry deep-copies them (a dlopen'd image's entries)
    std::string elf;
};

// A registration's descriptor normally lives in the image that registers it: for the main program the registry
// keeps a pointer, for a dlopen'd plugin it deep-copies the descriptor (the plugin may unload). dladdr decides by
// the payload's address, so the descriptors of the "main program" case live in these static pools (.bss, part of
// this executable's image) and the "--plugin" case allocates them on the heap.
inline constexpr std::size_t pool_entries = 131072, pool_fields = 8;
type_desc g_desc_pool[pool_entries];
field_desc g_field_pool[pool_entries * pool_fields];

struct synthetic_type
{ // the blob and names must outlive the push (the registry copies the blob; the names are viewed)
    std::string name;
    std::vector<std::string> field_names;
    std::vector<std::byte> blob;
    std::unique_ptr<field_desc[]> heap_fields; // --plugin
    std::unique_ptr<type_desc> heap_desc;      // --plugin
    type_desc* desc = nullptr;
    reg_node node{};
};

double ms(const clock_t::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

std::size_t rss_bytes()
{
    std::FILE* const f = std::fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    long pages = 0, resident = 0;
    const int n = std::fscanf(f, "%ld %ld", &pages, &resident);
    std::fclose(f);
    return n == 2 ? static_cast<std::size_t>(resident) * 4096u : 0;
}

class app
{
public:
    explicit app(options o) : o_(std::move(o))
    {
    }

    int run()
    {
        if (!o_.elf.empty())
            return run_elf();
        return run_synthetic();
    }

private:
    options o_;

    // One struct ENTRY of pool_fields i64 members whose names pad the blob to about `target` bytes.
    static void make_type(synthetic_type& t, const std::size_t i, const std::size_t target,
                          const bool plugin)
    {
        t.name = "measure::T" + std::to_string(i);
        const std::size_t n_fields = pool_fields;
        const std::size_t per_name =
            target > n_fields * 40 ? (target - n_fields * 40) / n_fields : 1;
        field_desc* fields = &g_field_pool[i * pool_fields];
        if (plugin)
        {
            t.heap_fields = std::make_unique<field_desc[]>(n_fields);
            fields = t.heap_fields.get();
        }
        dyn::type dt;
        dt.name = t.name;
        dt.schema_hash = fingerprint64(t.name);
        dt.layout_hash = fingerprint64(t.name + "#layout");
        dt.size = static_cast<std::uint32_t>(8 * n_fields);
        dt.align = 8;
        dt.is_flat = true;
        t.field_names.reserve(n_fields);
        for (std::size_t f = 0; f < n_fields; ++f)
        {
            std::string nm = "f" + std::to_string(f) + "_";
            nm.append(std::max<std::size_t>(1, per_name) -
                          std::min<std::size_t>(per_name, nm.size()),
                      'x');
            t.field_names.push_back(std::move(nm));
            dyn::field df;
            df.name = t.field_names.back();
            df.k = kind::i64;
            df.offset = static_cast<std::uint32_t>(8 * f);
            df.size = 8;
            df.align = 8;
            dt.fields.push_back(std::move(df));
            fields[f] = field_desc{t.field_names.back(),
                                   static_cast<std::uint32_t>(8 * f),
                                   8,
                                   8,
                                   kind::i64,
                                   0,
                                   0,
                                   0,
                                   0,
                                   nullptr};
        }
        dyn::dictionary d;
        d.types.emplace(dt.schema_hash, dt);
        const std::vector<std::byte> rri = dyn::to_rri(d);
        t.blob.assign(rri.begin() + static_cast<std::ptrdiff_t>(fmt::rri_header_size), rri.end());
        t.desc = &g_desc_pool[i];
        if (plugin)
        {
            t.heap_desc = std::make_unique<type_desc>();
            t.desc = t.heap_desc.get();
        }
        *t.desc = type_desc{t.name,
                            dt.schema_hash,
                            dt.layout_hash,
                            dt.size,
                            dt.align,
                            kind::struct_,
                            kind{},
                            true,
                            false,
                            false,
                            0,
                            0,
                            std::span<const field_desc>(fields, n_fields),
                            {},
                            {},
                            {},
                            t.blob};
        t.node = reg_node{t.desc,  t.blob, dt.schema_hash, t.name, node_kind::type, nullptr,
                          nullptr, 0,      false};
    }

    int run_synthetic()
    {
        if (o_.entries > pool_entries)
        {
            std::println(stderr, "registry_measure: at most {} entries", pool_entries);
            return 2;
        }
        const std::size_t per_entry =
            std::max<std::size_t>(64, o_.bytes / std::max<std::size_t>(1, o_.entries));
        const std::size_t rss0 = rss_bytes();
        std::vector<std::unique_ptr<synthetic_type>> types;
        types.reserve(o_.entries);
        std::size_t blob_bytes = 0;
        for (std::size_t i = 0; i < o_.entries; ++i)
        {
            types.push_back(std::make_unique<synthetic_type>());
            make_type(*types.back(), i, per_entry, o_.plugin);
            blob_bytes += types.back()->blob.size();
        }
        registry& reg = registry::instance();
        const std::size_t before = reg.size();

        const auto t0 = clock_t::now();
        for (auto& t : types)
            reg.push(&t->node);
        const auto t1 = clock_t::now();
        const std::size_t after = reg.size(); // publishes the index
        const auto t2 = clock_t::now();

        std::vector<std::uint64_t> hashes;
        hashes.reserve(types.size());
        for (const auto& t : types)
            hashes.push_back(t->desc->schema_hash);
        std::shuffle(hashes.begin(), hashes.end(), std::mt19937_64{42});
        std::size_t found = 0;
        const auto t3 = clock_t::now();
        for (const std::uint64_t h : hashes)
            found += reg.find_type(h) != nullptr;
        const auto t4 = clock_t::now();

        const std::vector<std::byte> image = reg.export_image();
        const auto t5 = clock_t::now();
        dyn::dictionary d;
        const auto parsed = d.add_rri(image);
        const auto t6 = clock_t::now();
        const std::size_t rss1 = rss_bytes();

        const double push_ms = ms(t1 - t0), publish_ms = ms(t2 - t1), lookup_ms = ms(t4 - t3),
                     export_ms = ms(t5 - t4), parse_ms = ms(t6 - t5);
        if (o_.json)
            std::println(
                "{{\"entries\":{},\"blob_bytes\":{},\"push_ms\":{:.3f},\"publish_ms\":{:.3f},"
                "\"lookup_ms\":{:.3f},\"export_ms\":{:.3f},\"export_bytes\":{},\"parse_ms\":{:.3f},"
                "\"rss_delta_bytes\":{}}}",
                o_.entries, blob_bytes, push_ms, publish_ms, lookup_ms, export_ms, image.size(),
                parse_ms, rss1 > rss0 ? rss1 - rss0 : 0);
        else
        {
            std::println("registry_measure: {} entries, {:.1f} MB of ENTRY blobs ({} bytes each), "
                         "descriptors {}",
                         o_.entries, static_cast<double>(blob_bytes) / 1048576.0, per_entry,
                         o_.plugin ? "on the heap (deep-copied, as a plugin's)"
                                   : "in this image (as the main program's)");
            std::println("  push      {:>10.3f} ms   ({:.2f} µs per entry)", push_ms,
                         1000.0 * push_ms /
                             static_cast<double>(std::max<std::size_t>(1, o_.entries)));
            std::println("  publish   {:>10.3f} ms   (first snapshot after the burst)", publish_ms);
            std::println(
                "  lookup    {:>10.3f} ms   ({:.1f} ns per find_type, random order)", lookup_ms,
                1e6 * lookup_ms / static_cast<double>(std::max<std::size_t>(1, o_.entries)));
            std::println("  export    {:>10.3f} ms   ({:.1f} MB image)", export_ms,
                         static_cast<double>(image.size()) / 1048576.0);
            std::println("  parse     {:>10.3f} ms   (dyn::dictionary::add_rri of that image{})",
                         parse_ms, parsed ? "" : " — FAILED");
            std::println("  rss       {:>10.1f} MB   growth over the run",
                         static_cast<double>(rss1 - rss0) / 1048576.0);
        }
        if (after - before != o_.entries || found != o_.entries || !parsed ||
            d.types.size() < o_.entries)
        {
            std::println(stderr,
                         "registry_measure: registry holds {} new entries, {} found, parse {}",
                         after - before, found, parsed ? "ok" : "failed");
            return 2;
        }
        if (o_.assert_budget)
        {
            // DECISIONS G-5: 10k/10 MB in 10 ms, 100k/100 MB in 100 ms — push + publish, release build
            const double budget_ms = static_cast<double>(o_.entries) / 1000.0;
            const double got = push_ms + publish_ms;
#if defined(__OPTIMIZE__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) &&    \
    !defined(ES_SERREG_BENCH_DEBUG_LIB)
            if (got > budget_ms)
            {
                std::println(
                    stderr,
                    "registry_measure: push + publish {:.3f} ms exceeds the {:.0f} ms budget", got,
                    budget_ms);
                return 1;
            }
            std::println("budget: push + publish {:.3f} ms within {:.0f} ms", got, budget_ms);
#else
            std::println("budget: push + publish {:.3f} ms against {:.0f} ms (not enforced: debug "
                         "or sanitizer "
                         "build)",
                         got, budget_ms);
#endif
        }
        return 0;
    }

    int run_elf()
    {
        const auto t0 = clock_t::now();
        const auto d = tools::extract_registry_from_elf(o_.elf);
        const auto t1 = clock_t::now();
        if (!d)
        {
            std::println(stderr, "registry_measure: {}: {}", o_.elf, to_string(d.error()));
            return 2;
        }
        const auto rri = tools::extract_section_from_elf(o_.elf, "es_serreg_dict");
        const std::size_t section = rri ? rri->size() : 0;
        if (o_.json)
            std::println(
                "{{\"elf\":\"{}\",\"section_bytes\":{},\"types\":{},\"literals\":{},\"calls\":{},"
                "\"load_ms\":{:.3f}}}",
                o_.elf, section, d->types.size(), d->literals.size(), d->calls.size(), ms(t1 - t0));
        else
            std::println("registry_measure: {}: es_serreg_dict {} bytes → {} types, {} literals, "
                         "{} calls in "
                         "{:.3f} ms",
                         o_.elf, section, d->types.size(), d->literals.size(), d->calls.size(),
                         ms(t1 - t0));
        return 0;
    }
};

options parse(const int argc, char** const argv)
{
    options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        const auto value = [&]() -> std::string_view { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--entries")
            o.entries = static_cast<std::size_t>(std::strtoull(value().data(), nullptr, 10));
        else if (a == "--bytes")
            o.bytes = static_cast<std::size_t>(std::strtoull(value().data(), nullptr, 10));
        else if (a == "--assert")
            o.assert_budget = true;
        else if (a == "--json")
            o.json = true;
        else if (a == "--plugin")
            o.plugin = true;
        else if (a == "--elf")
            o.elf = value();
        else
        {
            std::println(stderr,
                         "usage: registry_measure [--entries N] [--bytes B] [--assert] [--plugin] "
                         "[--elf <binary>] [--json]");
            std::exit(2);
        }
    }
    return o;
}
} // namespace registry_measure

int main(int argc, char** argv)
{
    return registry_measure::app(registry_measure::parse(argc, argv)).run();
}
