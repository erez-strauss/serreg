// tests/dlopen/second_copy.cpp — review Q10 (2.1.5): an executable that exports its own
// es_serreg_registry_marker_v3() (a function since 2.1.9), as a static libserreg.a linked with -rdynamic next to
// libserreg.so would. Under ASan this must not be an ODR violation (the 2.1.8 data marker was). The
// shared library's constructor must print the one-line "two copies of libserreg" diagnostic on stderr (checked by
// the ctest's PASS_REGULAR_EXPRESSION); the program itself still works against libserreg.so's registry.
#include <es/serreg/serreg.hpp>

#include <print>
#include <vector>

namespace
{
const char fake_copy = 2;
}
extern "C" [[gnu::visibility("default"), gnu::used, gnu::noinline]] const void*
es_serreg_registry_marker_v3() noexcept
{
    return &fake_copy;
}

namespace second_copy
{
struct Probe
{
    int x;
};

// Serialises one Probe: the registry used is libserreg.so's, whatever marker this executable exports.
class app
{
public:
    int run() const
    {
        std::vector<std::byte> buf;
        es::serreg::vector_sink s{buf};
        if (!es::serreg::serialize(s, Probe{7}))
            return 1;
        std::println("second_copy: serialized through libserreg.so");
        return 0;
    }
};
} // namespace second_copy

int main()
{
    return second_copy::app{}.run();
}
