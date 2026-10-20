// tests/fuzz/fuzz_driver.cpp — portable fuzz driver (GCC has no libFuzzer): replays every file of a corpus
// directory through one target, then runs seeded random mutations of each file (bit flips, byte overwrite,
// insert, delete, truncation, chunk duplication). The target must return normally every time; a crash, a
// sanitizer report or an address-space blow-up (RLIMIT_AS, 2 GiB unless under a sanitizer) fails the run.
// 2.1.16 (FR-D5, maintainer answer O2): every input is also gated on *time* and *memory* — a single input that
// takes longer than `max_ms` (default 2000, 10× under a sanitizer) or raises the process's high-water RSS by
// more than `max_rss_mib` (default 512) fails the run and is written next to the corpus as <target>-slow-N.bin /
// <target>-big-N.bin, because amplification is a bug under the never-crash contract even when nothing crashes.
//
//   serreg_fuzz <rri|capture|elf|config> <corpus-dir> [iterations=2000] [seed=1] [max_ms=2000] [max_rss_mib=512]
#include "fuzz_targets.hpp"

#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using target_fn = int (*)(const std::uint8_t*, std::size_t);

struct xorshift64
{
    std::uint64_t s;
    std::uint64_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::size_t below(const std::size_t n)
    {
        return n ? static_cast<std::size_t>(next() % n) : 0;
    }
};

bool read_whole(const std::string& path, std::vector<std::uint8_t>& out)
{
    std::FILE* const f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::uint8_t buf[65536];
    for (;;)
    {
        const std::size_t n = std::fread(buf, 1, sizeof buf, f);
        out.insert(out.end(), buf, buf + n);
        if (n < sizeof buf)
            break;
    }
    std::fclose(f);
    return true;
}

std::vector<std::string> corpus_files(const std::string& dir)
{
    std::vector<std::string> files;
    DIR* const d = ::opendir(dir.c_str());
    if (!d)
        return files;
    while (const dirent* const e = ::readdir(d))
    {
        if (e->d_name[0] == '.')
            continue;
        const std::string p = dir + "/" + e->d_name;
        struct stat st = {};
        if (::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            files.push_back(p);
    }
    ::closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

// One mutation step; keeps the input between 0 and 4× its original size so a run stays bounded.
void mutate(std::vector<std::uint8_t>& v, xorshift64& rng, const std::size_t max_size)
{
    switch (rng.below(6))
    {
    case 0:
        if (!v.empty())
            v[rng.below(v.size())] ^= static_cast<std::uint8_t>(1u << rng.below(8));
        break; // bit flip
    case 1:
        if (!v.empty())
            v[rng.below(v.size())] = static_cast<std::uint8_t>(rng.next());
        break; // byte overwrite
    case 2:
        if (v.size() < max_size)
            v.insert(v.begin() + static_cast<std::ptrdiff_t>(rng.below(v.size() + 1)),
                     static_cast<std::uint8_t>(rng.next()));
        break;
    case 3:
        if (!v.empty())
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(rng.below(v.size())));
        break; // byte delete
    case 4:
        if (!v.empty())
            v.resize(rng.below(v.size()));
        break; // truncation
    default: { // chunk duplication
        if (v.empty())
            break;
        const std::size_t from = rng.below(v.size()),
                          len = std::min(rng.below(64) + 1, v.size() - from);
        if (v.size() + len > max_size)
            break;
        const std::vector<std::uint8_t> chunk(v.begin() + static_cast<std::ptrdiff_t>(from),
                                              v.begin() + static_cast<std::ptrdiff_t>(from + len));
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(rng.below(v.size() + 1)), chunk.begin(),
                 chunk.end());
    }
    }
}

// Interesting 8-byte values are common in this format (ids, lengths, magic): overwrite an aligned word sometimes.
void mutate_word(std::vector<std::uint8_t>& v, xorshift64& rng)
{
    static constexpr std::uint64_t specials[] = {0,
                                                 1,
                                                 7,
                                                 8,
                                                 0xff,
                                                 0xffff,
                                                 0xffffffffull,
                                                 0xfffffffffffffffful,
                                                 0x8000000000000000ull,
                                                 0x4553535244494354ull /*dict frame sentinel*/,
                                                 0x52535345ull /*"ESSR"*/,
                                                 0x47525345ull /*"ESRG"*/};
    if (v.size() < 8)
        return;
    const std::size_t at = rng.below(v.size() - 7) & ~std::size_t{1};
    const std::uint64_t w = specials[rng.below(sizeof specials / sizeof specials[0])];
    const std::size_t width = std::size_t{1} << rng.below(4); // 1, 2, 4 or 8 bytes
    std::memcpy(v.data() + at, &w, width);
}

bool cap_address_space()
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return false; // sanitizer shadow memory does not fit under a 2 GiB address-space limit
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return false;
#endif
#endif
    rlimit lim{};
    lim.rlim_cur = lim.rlim_max = std::uint64_t{2} << 30;
    return ::setrlimit(RLIMIT_AS, &lim) == 0;
}

} // namespace

namespace fuzz_driver
{
// The command line: serreg_fuzz <rri|capture|elf|config> <corpus-dir> [iterations=2000] [seed=1]
struct options
{
    const char* name = nullptr;
    target_fn target = nullptr;
    const char* corpus_dir = nullptr;
    std::size_t iterations = 2000;
    std::uint64_t seed = 1;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    std::uint64_t max_ms = 20000; // sanitizers are 5-20× slower
#else
    std::uint64_t max_ms = 2000;
#endif
    std::uint64_t max_rss_mib = 512;

    // Prints the usage or the unknown-target diagnostic and returns nothing when the command line is wrong.
    static std::optional<options> parse(const int argc, char** const argv)
    {
        if (argc < 3)
        {
            std::println(
                stderr,
                "usage: {} <rri|capture|elf|config> <corpus-dir> [iterations=2000] [seed=1]",
                argv[0]);
            return std::nullopt;
        }
        options o;
        o.name = argv[1];
        const std::string_view name = o.name;
        if (name == "rri")
            o.target = &fuzz_rri;
        else if (name == "capture")
            o.target = &fuzz_capture;
        else if (name == "elf")
            o.target = &fuzz_elf;
        else if (name == "config")
            o.target = &fuzz_config;
        else
        {
            std::println(stderr, "unknown target {}", o.name);
            return std::nullopt;
        }
        o.corpus_dir = argv[2];
        if (argc > 3)
            o.iterations = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
        if (argc > 4)
            o.seed = std::strtoull(argv[4], nullptr, 10);
        if (argc > 5)
            o.max_ms = std::strtoull(argv[5], nullptr, 10);
        if (argc > 6)
            o.max_rss_mib = std::strtoull(argv[6], nullptr, 10);
        return o;
    }
};

// Replays the corpus through the target, then the seeded mutations of every corpus file.
class app
{
public:
    explicit app(const options& opts) : opts_{opts}
    {
    }

    int run()
    {
        const bool capped = cap_address_space();
        if (const int rc = load_corpus())
            return rc;
        replay_prefixes();
        const std::size_t runs = run_mutations();
        std::println("fuzz {}: {} corpus files, {} mutated runs, seed {}, RLIMIT_AS {}, slowest "
                     "input {} ms, "
                     "RSS high-water growth {} MiB (limits {} ms / {} MiB)",
                     opts_.name, corpus_.size(), runs, opts_.seed,
                     capped ? "2 GiB" : "not applied (sanitizer build)", slowest_ms_,
                     rss_growth_mib(), opts_.max_ms, opts_.max_rss_mib);
        if (violations_)
        {
            std::println(stderr,
                         "fuzz {}: {} input(s) exceeded the time or memory limit (saved in {})",
                         opts_.name, violations_, opts_.corpus_dir);
            return 1;
        }
        return 0;
    }

private:
    options opts_;
    std::vector<std::vector<std::uint8_t>> corpus_;
    std::uint64_t slowest_ms_ = 0;
    std::size_t violations_ = 0;
    long rss_start_kib_ = -1;

    static long maxrss_kib()
    {
        rusage u{};
        ::getrusage(RUSAGE_SELF, &u);
        return u.ru_maxrss;
    }
    std::uint64_t rss_growth_mib() const
    {
        const long now = maxrss_kib();
        return rss_start_kib_ < 0 || now < rss_start_kib_
                   ? 0
                   : static_cast<std::uint64_t>(now - rss_start_kib_) / 1024;
    }
    void save_input(const char* const why, const std::uint8_t* const p, const std::size_t n) const
    {
        const std::string path = std::string(opts_.corpus_dir) + "/" + opts_.name + "-" + why +
                                 "-" + std::to_string(violations_) + ".bin";
        if (std::FILE* const f = std::fopen(path.c_str(), "wb"))
        {
            static_cast<void>(std::fwrite(p, 1, n, f));
            std::fclose(f);
        }
    }
    // Runs the target on one input under the time and memory gates (FR-D5). The RSS gate compares the process
    // high-water mark against its value when the run started, so one input that allocates far beyond the
    // corpus files it was mutated from is caught even when it frees everything before returning.
    void run_one(const std::uint8_t* const p, const std::size_t n)
    {
        if (rss_start_kib_ < 0)
            rss_start_kib_ = maxrss_kib();
        const auto t0 = std::chrono::steady_clock::now();
        opts_.target(p, n);
        const auto ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
        slowest_ms_ = std::max(slowest_ms_, ms);
        if (ms > opts_.max_ms)
        {
            ++violations_;
            std::println(stderr, "fuzz {}: input of {} bytes took {} ms (limit {})", opts_.name, n,
                         ms, opts_.max_ms);
            save_input("slow", p, n);
        }
        if (rss_growth_mib() > opts_.max_rss_mib)
        {
            ++violations_;
            std::println(
                stderr,
                "fuzz {}: input of {} bytes raised the RSS high-water mark to +{} MiB (limit {})",
                opts_.name, n, rss_growth_mib(), opts_.max_rss_mib);
            save_input("big", p, n);
            rss_start_kib_ = maxrss_kib(); // report each offender once
        }
    }

    // Reads every corpus file, running the target on each as it is read.
    int load_corpus()
    {
        const auto files = corpus_files(opts_.corpus_dir);
        if (files.empty())
        {
            std::println(stderr, "no corpus files in {}", opts_.corpus_dir);
            return 2;
        }
        for (const auto& f : files)
        {
            std::vector<std::uint8_t> bytes;
            if (!read_whole(f, bytes))
            {
                std::println(stderr, "cannot read {}", f);
                return 2;
            }
            run_one(bytes.data(), bytes.size());
            corpus_.push_back(std::move(bytes));
        }
        return 0;
    }

    // The empty input and a few short prefixes are always part of the run.
    void replay_prefixes()
    {
        run_one(nullptr, 0);
        for (const auto& c : corpus_)
            for (std::size_t n = 1; n < std::min<std::size_t>(c.size(), 64); ++n)
                run_one(c.data(), n);
    }

    // Returns the number of mutated inputs run.
    std::size_t run_mutations()
    {
        std::size_t runs = 0;
        xorshift64 rng{opts_.seed * 0x9E3779B97F4A7C15ull + 1};
        for (std::size_t i = 0; i < opts_.iterations; ++i)
        {
            for (const auto& c : corpus_)
            {
                std::vector<std::uint8_t> m = c;
                const std::size_t steps = 1 + rng.below(4);
                for (std::size_t s = 0; s < steps; ++s)
                {
                    if (rng.below(4) == 0)
                        mutate_word(m, rng);
                    else
                        mutate(m, rng, std::max<std::size_t>(c.size() * 4, 64));
                }
                run_one(m.data(), m.size());
                ++runs;
            }
        }
        return runs;
    }
};
} // namespace fuzz_driver

int main(const int argc, char** const argv)
{
    const auto opts = fuzz_driver::options::parse(argc, argv);
    if (!opts)
        return 2;
    return fuzz_driver::app{*opts}.run();
}
