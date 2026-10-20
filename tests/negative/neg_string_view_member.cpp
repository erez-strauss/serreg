// negative (FR-S7, v2): std::string_view as a member cannot be read back; use std::string.
// expected diagnostic: "std::string_view is not serialisable as a member"
#include <es/serreg/serreg.hpp>

#include <string_view>
#include <vector>
namespace neg
{
struct HasView
{
    std::uint64_t key;
    std::string_view name;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasView{1, "x"}) ? 0 : 1;
}
