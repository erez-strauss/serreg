// tests/link/tu_extra_first.cpp — linked FIRST in link_alt_order so that lnk::Extra registers before lnk::Msg
// (static initialisation follows link order across TUs): program_hash must not notice (FR-H3).
#include "link_types.hpp"

#include <es/serreg/serreg.hpp>

void emit_extra_first(std::vector<std::byte>& out)
{
    es::serreg::vector_sink s{out};
    static_cast<void>(es::serreg::serialize(s, lnk::Extra{-1.0, -2.0}));
}
