// control: the same shape as the negative cases but valid — proves the negative harness is not failing everything.
#include <es/serreg/serreg.hpp>

#include <vector>
namespace neg
{
struct Ok
{
    std::uint64_t key;
    [[= es::serreg::skip]] const char* name;
    std::uint32_t n;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::Ok{1, "x", 2}) ? 0 : 1;
}
