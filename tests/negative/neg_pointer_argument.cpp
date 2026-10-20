// negative (2.1.16 B10, retargeted in 2.2.0): a pointer passed as a top-level argument is no argument category;
// the deleted overload names the rule instead of an overload-resolution failure. (Until 2.2.0 this test used a
// std::vector; since format 1.6 a container is an argument of its own.)
// expected diagnostic: "never serialised"
#include <es/serreg/serreg.hpp>

#include <cstddef>
#include <vector>

int main()
{
    std::vector<std::byte> buf;
    es::serreg::vector_sink out{buf};
    int value = 1;
    int* const p = &value;
    return es::serreg::serialize(out, p) ? 0 : 1;
}
