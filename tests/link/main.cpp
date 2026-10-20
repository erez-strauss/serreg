// tests/link/main.cpp — FR-R2 (dedup across TUs), FR-R3 (count at main() entry), FR-H3 (program_hash is
// independent of registration order: written to argv[1] and compared across differently-ordered executables by ctest).
//
// Distinct entities: types lnk::Msg, lnk::Kind, lnk::Extra (3) + calls (Msg), (Extra) (2) = 5.
// tu_a and tu_b both instantiate registrar<Msg>, registrar<Kind> and registrar_call<Msg>: one node each.
#include "link_types.hpp"

#include <es/serreg/serreg.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <print>

namespace link_test
{
namespace sr = es::serreg;

// link_test [program-hash-file]
struct options
{
    const char* self = nullptr;
    const char* hash_file = nullptr; // program hash for the link-order comparison

    static options parse(const int argc, char** const argv)
    {
        return {.self = argv[0], .hash_file = argc > 1 ? argv[1] : nullptr};
    }
};

class app
{
public:
    explicit app(const options& opts) : opts_{opts}
    {
    }

    int run() const
    {
        if (const int rc = check_count_at_entry())
            return rc;
        if (const int rc = check_frames())
            return rc;
        if (const int rc = check_contents())
            return rc;
        if (const int rc = check_main_image())
            return rc;
        if (opts_.hash_file)
        { // program hash for the link-order comparison
            std::FILE* const f = std::fopen(opts_.hash_file, "w");
            if (!f)
                return 1;
            std::println(f, "{:016x}", reg_.program_hash());
            std::fclose(f);
        }
        std::println("ok");
        return 0;
    }

private:
    static constexpr std::size_t expected = 5;
    // Declared first, so initialised first: the size is read before anything else runs.
    const std::size_t n_at_entry_ = sr::registry::instance().size();
    const options opts_;
    const sr::registry& reg_ = sr::registry::instance();

    int check_count_at_entry() const
    {
        std::println("{}: registry size at main() = {} (expected {})", opts_.self, n_at_entry_,
                     expected);
        if (n_at_entry_ != expected)
        {
            std::println(stderr, "FAIL: dedup across TUs broken or registration missing");
            return 1;
        }
        return 0;
    }

    int check_frames() const
    {
        std::vector<std::byte> buf;
        emit_a(buf);
        emit_b(buf);
        const auto rep =
            sr::tools::verify(buf, nullptr); // bare frames: need the dictionary from the side
        const auto dict = sr::tools::load_dictionary_rri(reg_.export_image());
        if (!dict)
        {
            std::println(stderr, "FAIL: export_image did not parse");
            return 1;
        }
        const auto rep2 = sr::tools::verify(buf, &*dict);
        if (!rep2 || !rep2->ok() || rep2->frames != 3)
        {
            std::println(stderr, "FAIL: frames from both TUs do not verify");
            return 1;
        }
        static_cast<void>(rep);
        return 0;
    }

    int check_contents() const
    {
        if (reg_.size() != expected || !reg_.find_type_by_name("lnk::Msg") ||
            !reg_.find_type_by_name("lnk::Kind") || !reg_.find_type_by_name("lnk::Extra") ||
            reg_.images().size() != 1)
        {
            std::println(stderr, "FAIL: registry contents");
            return 1;
        }
        // Within one image the linker merges the two TUs' registrar<Msg>::g into one guard: one node, one push, and
        // refs counts images (FR-R12), so it is exactly 1 here (it becomes 2 when a dlopen'ed image brings Msg too).
        for (const sr::registry_entry& node : reg_.snapshot().entries)
            if (node.name == "lnk::Msg" && node.refs != 1)
            {
                std::println(stderr, "FAIL: Msg refs = {}, expected 1 (one image)", node.refs);
                return 1;
            }
        return 0;
    }

    // cc F10: the main image recorded for every entry is dladdr's mapped base of the executable — for PIE and
    // (link_nopie) non-PIE builds alike — and the main program is never considered unloadable.
    int check_main_image() const
    {
        Dl_info di{};
        static const int anchor =
            0; // any object of this executable: dladdr gives the main image's mapped base
        if (!dladdr(static_cast<const void*>(&anchor), &di) || !di.dli_fbase)
        {
            std::println(stderr, "FAIL: dladdr(anchor)");
            return 1;
        }
        const auto images = reg_.images();
        if (std::find(images.begin(), images.end(), di.dli_fbase) == images.end())
        {
            std::println(
                stderr,
                "FAIL: images() does not contain the main image {} (F10: dlpi_addr vs dli_fbase)",
                di.dli_fbase);
            return 1;
        }
        for (const sr::registry_entry& node : reg_.snapshot().entries)
            if (node.image_base != di.dli_fbase)
            {
                std::println(stderr, "FAIL: entry {} has image_base {} != main {}", node.name,
                             node.image_base, di.dli_fbase);
                return 1;
            }
        if (sr::registry::unload_guard_active(static_cast<const void*>(&anchor)))
        {
            std::println(stderr, "FAIL: the main executable must never count as unloadable");
            return 1;
        }
        std::println("main image {} ({})", di.dli_fbase, di.dli_fname ? di.dli_fname : "?");
        return 0;
    }
};
} // namespace link_test

int main(const int argc, char** const argv)
{
    return link_test::app{link_test::options::parse(argc, argv)}.run();
}
