// negative (2.2.6, review B4): a const member is not serialisable — a reader would have to modify a const
// object; [[=skip]] or removing the qualifier is the fix. The diagnostic names the rule.
// expected diagnostic: "const and volatile members are not serialisable"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <vector>
namespace neg
{
struct HasConst
{
    std::uint64_t key;
    const std::int32_t version;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasConst{1, 2}) ? 0 : 1;
}
