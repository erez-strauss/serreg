// negative (2.1.14, review C-05): a class local to a function (or a lambda) has no linkage-unique name, so it must
// be named explicitly.
// expected diagnostic: "has no unique canonical name"
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <vector>
namespace neg
{
inline bool send()
{
    struct Local
    {
        std::uint32_t a;
    };
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return static_cast<bool>(es::serreg::serialize(s, Local{1}));
}
} // namespace neg
int main()
{
    return neg::send() ? 0 : 1;
}
