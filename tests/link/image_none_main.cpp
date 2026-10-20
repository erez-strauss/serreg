// tests/link/image_none_main.cpp — A4 (2.1.8): the same two TUs as link_plain, linked against a static libserreg
// built with the null image backend (src/image_none.cpp) and ES_SERREG_EMBED_NONE. Registration, dedup, export
// and program_hash must be exactly those of the ELF build; there is one image, nothing is unloadable, and the
// executable carries no es_serreg_dict section.
#include "link_types.hpp"

#include <es/serreg/serreg.hpp>

#include <cstdio>
#include <print>

namespace
{
int fail(const char* const what)
{
    std::println(stderr, "FAIL: {}", what);
    return 1;
}
} // namespace

namespace image_none
{
namespace sr = es::serreg;

// image_none_main [program-hash-file]
class app
{
public:
    explicit app(const char* const hash_file) : hash_file_{hash_file}
    {
    }

    int run() const
    {
        if (const int rc = check_registry())
            return rc;
        if (const int rc = check_frames())
            return rc;
        if (const int rc = check_no_section())
            return rc;
        if (hash_file_)
        {
            std::FILE* const f = std::fopen(hash_file_, "w");
            if (!f)
                return 1;
            std::println(f, "{:016x}", reg_.program_hash());
            std::fclose(f);
        }
        std::println("ok");
        return 0;
    }

private:
    const char* hash_file_;
    const sr::registry& reg_ = sr::registry::instance();

    int check_registry() const
    {
        if (reg_.size() != 5)
            return fail("registry size at main() is not 5 (dedup/registration)");
        if (reg_.images().size() != 1 || !reg_.images()[0])
            return fail("the null backend reports exactly one non-null image");
        for (const sr::registry_entry& node : reg_.snapshot().entries)
            if (node.image_base != reg_.images()[0] || node.refs != 1)
                return fail("every entry belongs to the one image, refs 1");
        static const int anchor = 0;
        if (sr::registry::unload_guard_active(&anchor))
            return fail("nothing is unloadable with the null backend");
        return 0;
    }

    int check_frames() const
    {
        std::vector<std::byte> buf;
        emit_a(buf);
        emit_b(buf);
        const auto dict = sr::tools::load_dictionary_rri(reg_.export_image());
        if (!dict)
            return fail("export_image did not parse");
        const auto rep = sr::tools::verify(buf, &*dict);
        if (!rep || !rep->ok() || rep->frames != 3)
            return fail("frames do not verify against the exported dictionary");
        return 0;
    }

    // ES_SERREG_EMBED_NONE: no section, so the offline reader finds nothing (the ELF reader itself still works).
    static int check_no_section()
    {
        const auto emb = sr::tools::extract_embedded_dictionary("/proc/self/exe");
        if (emb || emb.error() != sr::error::not_found)
            return fail("the executable must carry no es_serreg_dict section");
        return 0;
    }
};
} // namespace image_none

int main(const int argc, char** const argv)
{
    return image_none::app{argc > 1 ? argv[1] : nullptr}.run();
}
