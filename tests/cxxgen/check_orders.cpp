// tests/cxxgen/check_orders.cpp — 2.1.16 (plan A5): compiles the header `srdump --cxx --cxx-router` generated from
// the golden orders.rri (ctest cxxgen_golden_compiles, -fsyntax-only). The header's own static_asserts check every
// size, offset, canonical name and hash; this file adds the router skeleton's instantiation.
#include "orders.hpp"

int main()
{
    es::serreg::frame_router r;
    install_handlers(r);
    return r.size() == 9 ? 0 : 1; // the nine call signatures of orders.rri
}
