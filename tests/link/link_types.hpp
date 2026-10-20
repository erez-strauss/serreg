// tests/link/link_types.hpp — types shared by the two-TU dedup tests (spec §10.2 #3, FR-R2).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lnk
{
enum class Kind : std::uint8_t
{
    a = 1,
    b = 2
};
struct Msg
{
    std::uint64_t seq;
    Kind k;
    std::int32_t v;
};
struct Extra
{
    double x;
    double y;
};
} // namespace lnk

void emit_a(std::vector<std::byte>& out); // serializes Msg and Extra   (tu_a.cpp)
void emit_b(std::vector<std::byte>& out); // serializes Msg only        (tu_b.cpp)
