// tests/bench/hot_inline.cpp — 2.1.12 (cc review 2.1.10, C-08): at -O2 (CMake's RelWithDebInfo) serialize() of a
// flat struct with a literal into a claiming sink compiles to straight-line stores, no call. Compiled to assembly
// by the ctest serialize_inlines_at_O2 (tests/bench/check_inline.cmake), which counts the calls in hot().
#include "corpus.hpp"

#include <es/serreg/serreg.hpp>

#include <cstddef>

using namespace es::serreg::literals;

extern "C" std::size_t hot(es::serreg::span_sink& s, const corpus::Order& o) noexcept
{
    const auto r = es::serreg::serialize(s, "new order"_lit, o);
    return r ? *r : 0;
}
