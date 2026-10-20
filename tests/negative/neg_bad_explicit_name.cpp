// negative (2.1.14): an explicit canonical name must be a plain qualified name (letters, digits, '_', ':', '<', '>',
// ',', '-'); spaces and other characters would make names ambiguous in the dictionary.
// expected diagnostic: "an explicit canonical name must be non-empty"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <vector>
namespace neg
{
struct[[= es::serreg::name("neg Bad")]] Bad
{
    std::uint32_t a;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::Bad{1}) ? 0 : 1;
}
