// negative (FR-A6): std::tuple<> has no parts and therefore no wire shape; it is rejected by name.
// expected diagnostic: "std::tuple<> has no parts to serialise"
#include <es/serreg/serreg.hpp>

#include <tuple>
#include <vector>
namespace neg
{
struct HasEmptyTuple
{
    std::uint64_t key;
    std::tuple<> t;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasEmptyTuple{1, {}}) ? 0 : 1;
}
