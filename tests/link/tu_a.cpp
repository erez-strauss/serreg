// tests/link/tu_a.cpp — instantiates serialize<Msg> and serialize<Extra>.
#include "link_types.hpp"

#include <es/serreg/serreg.hpp>

void emit_a(std::vector<std::byte>& out)
{
    es::serreg::vector_sink s{out};
    static_cast<void>(es::serreg::serialize(s, lnk::Msg{1, lnk::Kind::a, -1}));
    static_cast<void>(es::serreg::serialize(s, lnk::Extra{1.5, 2.5}));
}
