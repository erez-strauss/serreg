// negative (2.1.12, review T-05): a nested replacement field (a width taken from an argument) passes
// std::format_string, but no reader can render it, so print rejects it at compile time.
// expected diagnostic: "nested replacement fields"
#include <es/serreg/serreg.hpp>

#include <vector>
int main()
{
    using namespace es::serreg::literals;
    std::vector<std::byte> v;
    es::serreg::vector_sink s{v};
    return es::serreg::println(s, "nested [{:>{}}]"_, 7, 6) ? 0 : 1;
}
