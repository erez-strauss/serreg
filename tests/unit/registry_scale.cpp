// tests/unit/registry_scale.cpp — 2.1.11: registering many types before main() publishes one index, not one per
// registration. Until 2.1.10 every registration published a new index (a copy of all entries so far) and every
// old index was kept for readers, so n types cost O(n²) time and memory before main(): 400 types were 70 MiB of
// RSS. Here 128 types and their 128 call signatures register during static initialisation; main() checks that
// nothing was published before its first read and exactly one index after it.
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace registry_scale
{
template<int N> struct tick
{
    std::int32_t a;
    double b;
    char name[16];
};
constexpr int types = 128;

// Never called: odr-using serialize<tick<I>> is what registers the type and its call signature.
template<int... I> void register_all(std::integer_sequence<int, I...>)
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    (static_cast<void>(es::serreg::serialize(s, tick<I>{})), ...);
}

class app
{
public:
    int run() const
    {
        const auto& reg = es::serreg::registry::instance();
        const std::uint64_t before = reg.generations(); // nothing has read the registry yet
        const std::size_t size = reg.size();            // the first read publishes
        const std::uint64_t after = reg.generations();
        static_cast<void>(reg.find_type(es::serreg::schema_hash_v<tick<7>>));
        const std::uint64_t later = reg.generations(); // no change since: nothing published
        std::println("registry_scale: {} entries, {} indexes published before main's first read, "
                     "{} after, {} later",
                     size, before, after, later);
        return size >= 2 * types && before == 0 && after == 1 && later == 1 ? 0 : 1;
    }
    static void never_called()
    {
        register_all(std::make_integer_sequence<int, types>{});
    }
};
} // namespace registry_scale

int main(const int argc, char**)
{
    if (argc > 99)
        registry_scale::app::never_called();
    return registry_scale::app{}.run();
}
