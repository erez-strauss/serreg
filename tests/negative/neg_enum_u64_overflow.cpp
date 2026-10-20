// negative (gk 1.14): an enumerator value above INT64_MAX does not fit the i64 dictionary encoding.
// expected diagnostic: "enumerator values must fit int64_t"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <vector>
namespace neg
{
enum class Huge : std::uint64_t
{
    small = 1,
    huge = 0x8000000000000001ull
};
struct HasHuge
{
    Huge h;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasHuge{neg::Huge::huge}) ? 0 : 1;
}
