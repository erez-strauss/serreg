// tests/unit/premain_check.cpp — FR-R1/R2/R3: every entity this executable serialises is in the registry when
// main() starts, i.e. registration is a load-time effect of template instantiation, not of the first call.
// The first thing main() does is construct premain_check::app, whose first member initialiser reads the size,
// before any serialize() executes. Also produces a small capture (argv[1]) consumed by the srdump ctest entries,
// and is the process used for the ES_SERREG_DUMP_REGISTRY test.
//
// Entities used below (distinct, deduplicated per FR-R2):
//   types:    corpus::Order, corpus::Side (component of Order), corpus::Cancel          = 3
//   literals: "evt", "why"                                                              = 2
//   calls:    (lit, Order), (Cancel, lit)                                               = 2
#include "corpus.hpp"

#include <cstdlib>
#include <print>
#include <string>

namespace premain_check
{
using namespace es::serreg;
using namespace es::serreg::literals;

constexpr std::size_t expected_entries = 7;

// Checks the registry as it was at main() entry, writes the capture, and checks nothing was registered by it.
class app
{
public:
    explicit app(const char* const path) : path_{path}
    {
    }

    int run() const
    {
        if (const int rc = check_counts_at_entry())
            return rc;
        if (const int rc = check_entities_present())
            return rc;
        return write_capture();
    }

private:
    // Declared first, so initialised first: nothing has run yet when the registry is read.
    const std::size_t at_entry_ = registry::instance().size();
    const std::uint64_t version_at_entry_ = registry::instance().version();
    const registry& reg_ = registry::instance();
    const std::string path_;

    int check_counts_at_entry() const
    {
        std::println("registry size at main() entry: {} (expected {}), version {}", at_entry_,
                     expected_entries, version_at_entry_);
        if (at_entry_ != expected_entries)
        {
            std::println(stderr, "FAIL: registration did not happen before main()");
            return 1;
        }
        if (version_at_entry_ != expected_entries)
        {
            std::println(stderr, "FAIL: version should count the pre-main pushes");
            return 1;
        }
        return 0;
    }

    int check_entities_present() const
    {
        if (!reg_.find_type_by_name("corpus::Order") || !reg_.find_type_by_name("corpus::Side") ||
            !reg_.find_type_by_name("corpus::Cancel"))
        {
            std::println(stderr, "FAIL: a type is missing at main() entry");
            return 1;
        }
        if (reg_.find_literal(decltype("evt"_lit)::id) != "evt" ||
            reg_.find_literal(decltype("why"_lit)::id) != "why")
        {
            std::println(stderr, "FAIL: a literal is missing at main() entry");
            return 1;
        }
        if (!reg_.find_call(call_hash_v<decltype("evt"_lit), corpus::Order>) ||
            !reg_.find_call(call_hash_v<corpus::Cancel, decltype("why"_lit)>))
        {
            std::println(stderr, "FAIL: a call signature is missing at main() entry");
            return 1;
        }
        return 0;
    }

    // The capture read by the srdump_* ctests; serialize() must not register anything at run time.
    int write_capture() const
    {
        {
            capture_file f(path_);
            if (!f.is_open())
            {
                std::println(stderr, "FAIL: cannot open {}", path_);
                return 1;
            }
            for (std::uint64_t i = 0; i < 3; ++i)
            {
                corpus::Order o = corpus::sample_order();
                o.id += i;
                if (!serialize(f, "evt"_lit, o))
                    return 1;
            }
            if (!serialize(f, corpus::sample_cancel(), "why"_lit))
                return 1;
        }
        if (reg_.size() != expected_entries || reg_.version() != version_at_entry_)
        {
            std::println(stderr, "FAIL: serialize() must not register anything at run time");
            return 1;
        }
        std::println("wrote {}", path_);
        return 0;
    }
};
} // namespace premain_check

int main(const int argc, char** const argv)
{
    return premain_check::app{argc > 1 ? argv[1] : "premain.ser"}.run();
}
