// negative (FR-S7): a pointer member without [[=skip]] is not serialisable; the diagnostic names the rule.
// expected diagnostic: "pointers are not serialisable"
#include <es/serreg/serreg.hpp>

#include <vector>
namespace neg
{
struct HasPtr
{
    std::uint64_t key;
    const char* name;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasPtr{1, "x"}) ? 0 : 1;
}
