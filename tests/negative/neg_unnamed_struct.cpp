// negative (gk 1.17, §6.2b): an unnamed type has no canonical name and must be rejected with a static_assert.
// (A `typedef struct {...} Name;` is NOT unnamed: the typedef name for linkage purposes is its identifier.)
// expected diagnostic: "unnamed types cannot be serialised"
#include <es/serreg/serreg.hpp>

#include <vector>
namespace neg
{
struct
{
    std::uint32_t a;
    std::uint32_t b;
} unnamed_instance;
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::unnamed_instance) ? 0 : 1;
}
