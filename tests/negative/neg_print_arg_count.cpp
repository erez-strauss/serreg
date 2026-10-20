// negative (FR-P2): two placeholders, one argument — rejected at compile time.
// expected diagnostic: "check_format"
#include <es/serreg/serreg.hpp>

#include <vector>
int main()
{
    using namespace es::serreg::literals;
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::println(s, "{} and {}"_, "one") ? 0 : 1;
}
