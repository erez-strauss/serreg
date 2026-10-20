// tests/dlopen/plugin_nested.cpp — a plugin whose static destructor dlcloses ANOTHER plugin (gk 1.5 / cc F11):
// dlclose(this) → this image's fini → ~closer → dlclose(inner) → inner's guards drop their references from inside
// the outer unload, then this image's own guards. Until 2.1.12 this needed thread_local depth counters around an
// interposed dlclose(); since 2.1.13 every guard destructor only drops a reference, so nesting needs nothing.
//   types: nested::Outer = 1, calls: (Outer) = 1  → 2 new entries at dlopen()
#include <es/serreg/serreg.hpp>

#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nested
{
struct Outer
{
    std::uint64_t a;
    std::uint32_t b;
};
} // namespace nested

namespace
{
void* g_inner = nullptr;
struct closer
{
    ~closer()
    {
        if (g_inner)
        {
            ::dlclose(g_inner);
            g_inner = nullptr;
        }
    }
};
const closer g_closer{};
} // namespace

extern "C" [[gnu::visibility("default")]] void nested_set_inner(void* const h)
{
    g_inner = h;
}

extern "C" [[gnu::visibility("default")]] int nested_run(std::vector<std::byte>* const out)
{
    es::serreg::vector_sink s{*out};
    return es::serreg::serialize(s, nested::Outer{7, 8}) ? 0 : 1;
}

extern "C" [[gnu::visibility("default")]] int nested_expected_entries()
{
    return 2;
}
