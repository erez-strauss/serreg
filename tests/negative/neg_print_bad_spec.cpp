// negative (FR-P2): the format string is checked at compile time by std::format_string against the rendered
// argument types — a floating-point precision spec on an int is rejected.
// expected diagnostic: "check_format"
#include <es/serreg/serreg.hpp>

#include <vector>
int main()
{
    using namespace es::serreg::literals;
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::print(s, "value {:.2f}"_, 42) ? 0 : 1;
}
