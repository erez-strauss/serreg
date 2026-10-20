// negative (2.1.14): a type named both by the annotation and by es::serreg::type_properties<T> is ambiguous.
// expected diagnostic: "the canonical name is given twice"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <string>
#include <vector>
namespace neg
{
struct[[= es::serreg::name("neg::A")]] Twice
{
    std::uint32_t a;
};
} // namespace neg
template<> struct es::serreg::type_properties<neg::Twice>
{
    static consteval std::string name()
    {
        return "neg::B";
    }
};
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::Twice{1}) ? 0 : 1;
}
