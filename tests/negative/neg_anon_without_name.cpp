// negative (2.1.14, review C-03/C-05, maintainer 2026-10-02): a class in an unnamed namespace has no linkage-unique
// name — two .cpp files can each define their own `X` — so it must be named explicitly.
// expected diagnostic: "has no unique canonical name"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <vector>
namespace
{
struct X
{
    std::uint32_t a;
};
} // namespace
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, X{1}) ? 0 : 1;
}
