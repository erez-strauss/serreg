// negative (FR-S7, Q4): smart pointers are rejected with a diagnostic that names the alternative.
// expected diagnostic: "smart pointers (std::unique_ptr, std::shared_ptr, std::weak_ptr) are not serialisable"
#include <es/serreg/serreg.hpp>

#include <memory>
#include <vector>
namespace neg
{
struct HasUniquePtr
{
    std::uint64_t key;
    std::unique_ptr<int> p;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasUniquePtr{1, {}}) ? 0 : 1;
}
