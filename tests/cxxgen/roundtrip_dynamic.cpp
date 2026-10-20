// tests/cxxgen/roundtrip_dynamic.cpp — 2.1.16 (plan A5): the golden dynamic.ser decoded through the types that
// `srdump --cxx --cxx-router` regenerated from its embedded dictionary (ctest cxxgen_golden_dynamic_roundtrip). The
// generated install_handlers() must accept every frame; handlers registered over its signatures check the values
// of make_dynamic_fixtures.cpp (sample_dyn_msg / sample_dyn_nested / sample_dyn_arr / sample_dyn_base).
#include "dynamic_rt.hpp"

#include <cstdint>
#include <cstring>
#include <format>
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
        install_handlers(r);
        const auto is_msg = [](const corpus::DynMsg& m) {
            return m.id == 7 && m.name == "ab" && m.xs == std::vector<std::int16_t>{1, -2, 3};
        };
        bool msg_ok = false, nested_ok = false, arr_ok = false, base_ok = false, print_ok = false;
        r.on([&](const corpus::DynMsg& m) { msg_ok = is_msg(m); });
        r.on([&](const corpus::DynNested& n, const corpus::DynArr& a) {
            nested_ok = n.msgs.size() == 2 && is_msg(n.msgs[0]) && n.msgs[1].id == 8 &&
                        n.msgs[1].name.empty() && n.msgs[1].xs.empty() && n.maybe.has_value() &&
                        n.maybe->id == 42 && n.maybe->qty == 100 && n.v.index() == 2 &&
                        std::get<2>(n.v) == "var";
            arr_ok = a.names[0] == "n0" && a.names[1] == "n1" &&
                     a.tags == std::vector<std::string>{"x", "yy"} && !a.side.has_value() &&
                     a.maybe_msg.index() == 1 && is_msg(std::get<1>(a.maybe_msg));
        });
        r.on([&](const corpus::DynBase& b) {
            base_ok = b.id == 42 && b.px == 1050000 && b.side == corpus::Side::buy &&
                      std::strncmp(b.sym.data(), "AAPL", b.sym.size()) == 0 && b.note == "note";
        });
        r.on_println("msg={} note={}"_, [&](const corpus::DynMsg& m, const std::string_view note) {
            print_ok = is_msg(m) && note == "text";
        });
        // 2.2.0 (format 1.6): the map/set message and the top-level vector frames (sample_dyn_map); the
        // unordered_map member regenerates as std::map (same shape, same hashes)
        const auto is_map = [](const corpus::DynMap& m) {
            return m.id == 9 && m.by_name == decltype(m.by_name){{"a", 1}, {"b", 2}, {"c", 3}} &&
                   m.ids == decltype(m.ids){10, 20, 30} &&
                   m.names == decltype(m.names){{5, "five"}, {4, "four"}, {-1, "minus one"}} &&
                   m.msgs.size() == 2 && m.msgs.at(1).id == 1 && m.msgs.at(1).name == "one" &&
                   m.msgs.at(1).xs == std::vector<std::int16_t>{1, 1} && m.msgs.at(2).id == 2 &&
                   m.msgs.at(2).xs == std::vector<std::int16_t>{2};
        };
        bool map_ok = false, print2_ok = false;
        int ids_frames = 0, ids_empty = 0;
        r.on("map"_, [&](const corpus::DynMap& m) { map_ok = is_map(m); });
        r.on("ids"_, [&](const std::vector<std::int32_t>& ids) {
            if (ids == std::vector<std::int32_t>{3, 1, 2})
                ++ids_frames;
            else if (ids.empty())
                ++ids_empty;
        });
        r.on_println("ids={} map={}"_,
                     [&](const std::vector<std::int32_t>& ids, const corpus::DynMap& m) {
                         print2_ok = ids == std::vector<std::int32_t>{3, 1, 2} && is_map(m);
                     });
        const auto walk = tools::for_each_frame(capture_, r.callback());
        if (!walk)
        {
            std::println(stderr, "cxxgen roundtrip: {}: {}", capture_, to_string(walk.error()));
            return 1;
        }
        const bool ok = r.handled() == 9 && r.unhandled() == 0 && r.failed() == 0 && msg_ok &&
                        nested_ok && arr_ok && base_ok && print_ok && map_ok && ids_frames == 2 &&
                        ids_empty == 1 && print2_ok;
        std::println(
            "cxxgen roundtrip: {} frames handled, {}{}", r.handled(), ok ? "ok" : "FAILED",
            ok ? ""
               : std::format(" (unhandled {}, failed {}, msg {}, nested {}, arr {}, base {}, "
                             "print {}, map {}, ids {}+{}, print2 {})",
                             r.unhandled(), r.failed(), msg_ok, nested_ok, arr_ok, base_ok,
                             print_ok, map_ok, ids_frames, ids_empty, print2_ok));
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
        std::println(stderr, "usage: roundtrip_dynamic <dynamic.ser>");
        return 2;
    }
    return cxxgen_roundtrip::app{argv[1]}.run();
}
