// negative (2.1.12, cc review 2.1.10 D-05): a vector element that occupies no bytes in the image cannot be
// counted by position; the dictionary refuses it and the typed decoder would accept any count, so it is rejected
// at compile time.
// expected diagnostic: "must occupy bytes in the image"
#include <es/serreg/serreg.hpp>

#include <array>
#include <string>
#include <vector>
struct Z
{
    std::vector<std::array<std::string, 0>> v;
};
int main()
{
    std::vector<std::byte> buf;
    es::serreg::vector_sink s{buf};
    return es::serreg::serialize(s, Z{}) ? 0 : 1;
}
