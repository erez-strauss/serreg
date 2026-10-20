// tests/link/tu_b.cpp — instantiates serialize<Msg> again (same signature as tu_a: must deduplicate).
#include "link_types.hpp"

#include <es/serreg/serreg.hpp>

void emit_b(std::vector<std::byte>& out)
{
    es::serreg::vector_sink s{out};
    static_cast<void>(es::serreg::serialize(s, lnk::Msg{2, lnk::Kind::b, -2}));
}
