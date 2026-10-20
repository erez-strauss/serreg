// tests/dlopen/conf_msg.hpp — the type both conflict plugins claim to define.
#pragma once
#include <cstdint>
namespace conf
{
struct Msg
{
    std::uint64_t id;
    std::int32_t qty;
};
} // namespace conf
