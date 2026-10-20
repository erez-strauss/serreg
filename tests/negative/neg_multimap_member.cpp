// negative (FR-S7, S12): a standard-library class outside the serialisable vocabulary is rejected by name —
// std::priority_queue has no wire shape (its order is a heap, not a sequence). std::multimap is a member kind.
// expected diagnostic: "this standard-library type is not in the serialisable vocabulary"
#include <es/serreg/serreg.hpp>

#include <queue>
#include <vector>
namespace neg
{
struct HasPriorityQueue
{
    std::uint64_t key;
    std::priority_queue<int> q;
};
} // namespace neg
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::HasPriorityQueue{1, {}}) ? 0 : 1;
}
