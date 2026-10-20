// negative (FR-S8, v1): as_flat<T> applies to top-level arguments only; a nested member of a mapped type is rejected.
// expected diagnostic: "as_flat<T> applies to top-level arguments only"
#include <es/serreg/serreg.hpp>

#include <chrono>
#include <vector>
namespace neg
{
struct Stamp
{
    std::chrono::nanoseconds t;
};
struct StampFlat
{
    std::int64_t ns;
};
struct Envelope
{
    std::uint32_t id;
    Stamp when;
};
} // namespace neg
template<> struct es::serreg::as_flat<neg::Stamp>
{
    using type = neg::StampFlat;
    static neg::StampFlat to(const neg::Stamp& s) noexcept
    {
        return {s.t.count()};
    }
    static neg::Stamp from(const neg::StampFlat& f) noexcept
    {
        return {std::chrono::nanoseconds{f.ns}};
    }
};
int main()
{
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::serialize(s, neg::Envelope{1, {std::chrono::nanoseconds{2}}}) ? 0 : 1;
}
