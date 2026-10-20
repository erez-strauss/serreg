// tests/cxxgen/roundtrip_orders.cpp — 2.1.16 (plan A5): the golden orders.ser decoded through the types that
// `srdump --cxx --cxx-router` regenerated from orders.rri (ctest cxxgen_golden_roundtrip: compiled, linked and run
// by compile_generated.cmake). install_handlers(), the generated skeleton, must accept every frame of the capture;
// handlers registered over three of its signatures check the values srdump --json shows for those frames.
#include "orders_rt.hpp"

#include <cstring>
#include <print>
#include <string>
#include <vector>

namespace cxxgen_roundtrip
{
using namespace es::serreg;
using namespace es::serreg::literals;

class app
{
public:
    explicit app(const std::string_view capture) : capture_(capture)
    {
    }

    int run()
    {
        frame_router r;
        install_handlers(r); // every signature of the dictionary, bodies empty
        std::vector<corpus::Order> orders, fills;
        double px = 0;
        std::uint32_t qty = 0;
        corpus::Side side{};
        r.on("new order"_, [&](const corpus::Order& o) { orders.push_back(o); });
        r.on<lit<"fill">, constant<42>, corpus::Order, constant<corpus::Side::sell>,
             constant<-1.5>>([&](const corpus::Order& o) { fills.push_back(o); });
        r.on_println("px={:.2f} qty={} side={}"_,
                     [&](const double p, const std::uint32_t q, const corpus::Side s) {
                         px = p;
                         qty = q;
                         side = s;
                     });
        const auto walk = tools::for_each_frame(capture_, r.callback());
        if (!walk)
        {
            std::println(stderr, "cxxgen roundtrip: {}: {}", capture_, to_string(walk.error()));
            return 1;
        }
        const auto is = [](const corpus::Order& o, const std::uint64_t id, const std::int64_t p,
                           const std::uint32_t q, const corpus::Side s, const char* const sym) {
            return o.id == id && o.px == p && o.qty == q && o.side == s &&
                   std::strncmp(o.sym.data(), sym, o.sym.size()) == 0;
        };
        const bool ok = r.handled() == 11 && r.unhandled() == 0 && r.failed() == 0 &&
                        orders.size() == 2 && fills.size() == 2 &&
                        is(orders[0], 42, 1050000, 100, corpus::Side::buy, "AAPL") &&
                        is(orders[1], 43, -1, 0, corpus::Side::sell, "MSFT") &&
                        is(fills[0], 42, 1050000, 100, corpus::Side::buy, "AAPL") &&
                        is(fills[1], 42, 1050000, 100, corpus::Side::buy, "AAPL") && px == 2.5 &&
                        qty == 7 && side == corpus::Side::sell;
        std::println(
            "cxxgen roundtrip: {} frames handled, {}{}", r.handled(), ok ? "ok" : "FAILED",
            ok ? ""
               : std::format(" (unhandled {}, failed {}, orders {}, fills {}, px {}, qty {})",
                             r.unhandled(), r.failed(), orders.size(), fills.size(), px, qty));
        return ok ? 0 : 1;
    }

private:
    std::string capture_;
};
} // namespace cxxgen_roundtrip

int main(const int argc, char** const argv)
{
    if (argc < 2)
    {
        std::println(stderr, "usage: roundtrip_orders <orders.ser>");
        return 2;
    }
    return cxxgen_roundtrip::app{argv[1]}.run();
}
