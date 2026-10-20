// negative (2.1.14, review C-07): a type that contains itself through std::vector / std::optional / std::variant has
// no finite canonical description; it is rejected with a clear message instead of an opaque recursion error.
// expected diagnostic: "recursive types are not supported"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <vector>
namespace neg
{
struct Tree
{
    std::uint32_t v;
    std::vector<Tree> children;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::Tree{}) ? 0 : 1;
}
