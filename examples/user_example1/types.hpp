// examples/user_example1/types.hpp — the data model shared by writer.cpp and reader1.cpp. reader2.cpp does NOT
// include it: it decodes everything from the dictionary embedded in the capture file.
//
// Nothing here knows about es::serreg: no macros, no registration, no IDL (the defaulted operator== is for
// reader1's check against sample_book(), not for the library). Reflection describes the types when
// writer.cpp calls serialize() on them. Book and Order hold std::string / std::vector / std::optional /
// std::variant members, which makes them "dynamic" types, written as canonical images; Fill, Limit and Market
// are flat (native bytes).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace demo
{
enum class Side : std::uint8_t
{
    buy,
    sell,
};

struct Fill
{
    std::uint64_t exec_id;
    double px;
    std::int32_t qty;
    bool operator==(const Fill&) const = default;
};

struct Limit
{
    double px;
    std::uint32_t tif_seconds;
    bool operator==(const Limit&) const = default;
};
struct Market
{
    std::uint16_t max_slippage_bp;
    bool operator==(const Market&) const = default;
};

struct Order
{
    std::uint64_t id;
    std::string symbol;
    Side side;
    std::int32_t qty;
    std::variant<Limit, Market> type;
    std::vector<Fill> fills;
    std::optional<std::string> note;
    bool operator==(const Order&) const = default;
};

struct Book
{
    std::string venue;
    std::uint32_t trade_date; // yyyymmdd
    double fee_bps[3];        // a C array inside a dynamic type: fixed part of the image
    std::vector<Order> orders;
    bool operator==(const Book&) const = default;
};

// The sample data both programs agree on (reader1 checks what it reads against it).
inline Book sample_book()
{
    Book b{"XNAS", 20260929, {0.25, 0.30, 0.10}, {}};
    b.orders.push_back({1001,
                        "AAPL",
                        Side::buy,
                        300,
                        Limit{228.15, 3600},
                        {{1, 228.14, 100}, {2, 228.15, 200}},
                        std::nullopt});
    b.orders.push_back({1002,
                        "MSFT",
                        Side::sell,
                        50,
                        Market{15},
                        {{3, 512.40, 50}},
                        std::string("client asked for urgency")});
    b.orders.push_back(
        {1003, "NVDA", Side::buy, 1000, Limit{181.00, 0}, {}, std::string("resting")});
    return b;
}
} // namespace demo
