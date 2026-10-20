// negative (2.2.6, review B4): a volatile member is not serialisable — the writer memcpy's it and a reader would
// assign through it as plain memory, which is not what volatile asks for. The diagnostic names the rule.
// expected diagnostic: "const and volatile members are not serialisable"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <vector>
namespace neg
{
struct HasVolatile
{
    std::uint64_t key;
    volatile std::int32_t flag;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasVolatile{1, 2}) ? 0 : 1;
}
