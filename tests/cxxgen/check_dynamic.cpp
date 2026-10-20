// tests/cxxgen/check_dynamic.cpp — 2.1.16 (plan A5): compiles the header `srdump --cxx --cxx-router` generated from
// the dictionary embedded in the golden dynamic.ser (ctest cxxgen_golden_dynamic_compiles, -fsyntax-only): dynamic
// types (std::string, std::vector, std::optional, std::variant members, a flat base), std::monostate spelled back.
#include "dynamic.hpp"

int main()
{
    es::serreg::frame_router r;
    install_handlers(r);
    return r.size() == 4 ? 0 : 1; // the four call signatures of dynamic.ser
}
