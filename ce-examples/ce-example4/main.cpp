// ce-example4 — es::serreg dynamic members in frames, and dispatch, on Compiler Explorer.
//
// What it tests (beyond ce-example3): frames whose records are canonical images of dynamic types (std::string,
// std::vector, std::optional, std::variant, a flat base inside a dynamic class), in compact and full records and
// as a println argument — byte for byte against the checked-in golden capture tests/fixtures/golden/dynamic.ser
// (tests/golden/make_dynamic_fixtures.cpp, GCC 16.2); typed deserialize of those frames; and dispatch<Ts...> over
// six types, which takes the compile-time perfect-hash path (detail::hash_index, more than 4 types).
//
// Expected on clang-p2996 + libc++ today: frame 1 differs, because DynArr contains std::variant<std::monostate, …>
// and libc++ names the alternative std::__1::monostate (ce-example2, DECISIONS T-3 / P-3): the call hash in
// the frame header changes, so a GCC-built reader refuses the clang-built frame and vice versa.
//
// Files: library headers (symlinks into include/) + ce_registry_stub.hpp (symlink to ../ce_registry_stub.hpp).
// Exit status: 0 when everything matches; 1 otherwise.
#include "ce_registry_stub.hpp"

#include <es/serreg/deserialize.hpp>
#include <es/serreg/print.hpp>
#include <es/serreg/serialize.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ---- corpus (verbatim from tests/golden/corpus.hpp) ----------------------------------------------------------
namespace corpus
{
struct Scalars
{
    std::int64_t i;
    double d;
    std::uint32_t u;
    std::int32_t s;
};
struct Padded
{
    std::uint8_t tag;
    std::uint64_t value;
    std::uint16_t code;
};
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Order
{
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
    Side side;
    char sym[8];
};
struct Cancel
{
    std::uint64_t id;
    std::uint32_t reason;
};
inline bool operator==(const Order& l, const Order& r) noexcept
{
    return l.id == r.id && l.px == r.px && l.qty == r.qty && l.side == r.side &&
           std::memcmp(l.sym, r.sym, sizeof l.sym) == 0;
}
inline Order sample_order() noexcept
{
    return Order{42, 1050000, 100, Side::buy, "AAPL"};
}
struct DynMsg
{
    std::uint32_t id;
    std::string name;
    std::vector<std::int16_t> xs;
    bool operator==(const DynMsg&) const = default;
};
struct DynNested
{
    std::vector<DynMsg> msgs;
    std::optional<Order> maybe;
    std::variant<std::int32_t, Order, std::string> v;
    bool operator==(const DynNested&) const = default;
};
struct DynArr
{
    std::string names[2];
    std::vector<std::string> tags;
    std::optional<Side> side;
    std::variant<std::monostate, DynMsg> maybe_msg;
    bool operator==(const DynArr&) const = default;
};
struct DynBase : Order
{
    std::string note;
    bool operator==(const DynBase&) const = default;
};
inline DynMsg sample_dyn_msg()
{
    return DynMsg{7, "ab", {1, -2, 3}};
}
inline DynNested sample_dyn_nested()
{
    return DynNested{{sample_dyn_msg(), DynMsg{8, "", {}}}, sample_order(), std::string("var")};
}
inline DynArr sample_dyn_arr()
{
    return DynArr{{"n0", "n1"}, {"x", "yy"}, std::nullopt, sample_dyn_msg()};
}
inline DynBase sample_dyn_base()
{
    DynBase b{};
    static_cast<Order&>(b) = sample_order();
    b.note = "note";
    return b;
}
} // namespace corpus

// ---- expected bytes -----------------------------------------------------------------------------------------
// 4 frames of tests/fixtures/golden/dynamic.ser (after the FILE header and dictionary), as hex
inline constexpr std::string_view golden_dynamic[] = {
    "38000000000001008edbc92a4eb16a751c000000000000000700000014000000020000001600000003000000616201"
    "00"
    "feff030000000000",
    "f80000000000020017efbcaf63374f5973000000000000001800000002000000480000000100000068000000030000"
    "00"
    "0700000040000000020000004200000003000000080000000000000000000000000000000000000061620100feff03"
    "00"
    "2a00000000000000900510000000000064000000014141504c00000000000000700000000300000076617200000000"
    "00"
    "5c0000000000000028000000020000002a000000020000002c00000002000000000000000000000040000000020000"
    "00"
    "6e306e313c000000010000003d00000002000000787979000700000054000000020000005600000003000000616201"
    "00"
    "feff030000000000",
    "58000000010001007a0074f8f730f107419654e34873366d34000000000000002c000000000000002a000000000000"
    "00"
    "900510000000000064000000014141504c0000000000000028000000040000006e6f746500000000",
    "40000000000003003f2d0dde6d7a56e31c000000000000000700000014000000020000001600000003000000616201"
    "00"
    "feff0300000000000400000074657874",
};

// ---- the checks ---------------------------------------------------------------------------------------------
namespace
{
namespace sr = es::serreg;
using namespace sr::literals;
int failures = 0;

void report(const bool ok, const std::string_view what, const std::string_view detail)
{
    if (!ok)
        ++failures;
    std::println("{:4} {:40} {}", ok ? "ok" : "DIFF", what, detail);
}

std::string hex(const std::span<const std::byte> b)
{
    std::string s;
    for (const std::byte x : b)
        s += std::format("{:02x}", std::to_integer<unsigned>(x));
    return s;
}

std::vector<std::byte> frame(const std::size_t i, const std::string_view what,
                             std::vector<std::byte> bytes)
{
    const std::string got = hex(bytes);
    const std::string_view want = golden_dynamic[i];
    std::size_t at = 0;
    while (at < got.size() && at < want.size() && got[at] == want[at])
        ++at;
    const bool ok = got == want;
    report(ok, std::format("frame {} {}", i, what),
           ok ? std::format("{} bytes identical", bytes.size())
              : std::format("{} vs {} bytes; first difference at byte {}{}", bytes.size(),
                            want.size() / 2, at / 2,
                            at / 2 >= 8 && at / 2 < 16 ? " (the call hash)" : ""));
    if (!ok)
        std::println("     got  {}\n     want {}", got, want);
    return bytes;
}

template<class F> std::vector<std::byte> write(F&& f)
{
    std::vector<std::byte> b;
    sr::vector_sink s{b};
    if (!f(s))
        b.clear();
    return b;
}

template<class... Fs> struct overloaded : Fs...
{
    using Fs::operator()...;
};
} // namespace

int main()
{
    // ---- 1. the writer: the 4 frames of make_dynamic_fixtures.cpp, in order ----
    const auto f0 = frame(0, "(DynMsg)", write([](auto& s) {
                              return sr::serialize(s, corpus::sample_dyn_msg()).has_value();
                          }));
    const auto f1 =
        frame(1, "(DynNested, DynArr)", write([](auto& s) {
                  return sr::serialize(s, corpus::sample_dyn_nested(), corpus::sample_dyn_arr())
                      .has_value();
              }));
    const auto f2 = frame(2, "full records (DynBase)", write([](auto& s) {
                              return sr::serialize_with(sr::serialize_options{.full_records = true},
                                                        s, corpus::sample_dyn_base())
                                  .has_value();
                          }));
    const auto f3 = frame(3, "println(\"msg={} note={}\", DynMsg, \"text\")", write([](auto& s) {
                              return sr::println(s, "msg={} note={}"_,
                                                           corpus::sample_dyn_msg(), "text")
                                  .has_value();
                          }));

    // ---- 2. typed reads of the dynamic frames ----
    {
        corpus::DynMsg m{};
        const auto r0 = sr::deserialize(f0, m);
        report(r0 && m == corpus::sample_dyn_msg(), "deserialize frame 0",
               r0 ? "ok" : sr::to_string(r0.error()));
        corpus::DynNested n{};
        corpus::DynArr a{};
        const auto r1 = sr::deserialize(f1, n, a);
        report(r1 && n == corpus::sample_dyn_nested() && a == corpus::sample_dyn_arr(),
               "deserialize frame 1", r1 ? "ok" : sr::to_string(r1.error()));
        corpus::DynBase b{};
        const auto r2 = sr::deserialize(f2, b);
        report(r2 && b == corpus::sample_dyn_base(), "deserialize frame 2 (full records)",
               r2 ? "ok" : sr::to_string(r2.error()));
        corpus::DynMsg m3{};
        std::string t3;
        const auto r3 = sr::deserialize_println(f3, "msg={} note={}"_, m3, t3);
        report(r3 && m3 == corpus::sample_dyn_msg() && t3 == "text", "deserialize_println frame 3",
               r3 ? "ok" : sr::to_string(r3.error()));
    }

    // ---- 3. the golden frame 1 from GCC, read by this compiler (cross-compiler interchange) ----
    {
        std::vector<std::byte> g;
        const std::string_view h = golden_dynamic[1];
        for (std::size_t i = 0; i + 1 < h.size(); i += 2)
            g.push_back(
                static_cast<std::byte>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
        corpus::DynNested n{};
        corpus::DynArr a{};
        const auto r = sr::deserialize(g, n, a);
        report(r && n == corpus::sample_dyn_nested() && a == corpus::sample_dyn_arr(),
               "read GCC's golden frame 1 here", r ? "ok" : sr::to_string(r.error()));
    }

    // ---- 4. dispatch over six types: the compile-time perfect-hash path (detail::hash_index) ----
    {
        std::vector<std::byte> b;
        sr::vector_sink s{b};
        static_cast<void>(sr::serialize(s, corpus::Cancel{9, 1}, corpus::sample_order(),
                                        corpus::sample_dyn_msg(), corpus::Side::sell));
        int cancels = 0, orders = 0, msgs = 0, sides = 0, others = 0;
        const auto r = sr::dispatch<corpus::Scalars, corpus::Padded, corpus::Side, corpus::Order,
                                    corpus::Cancel, corpus::DynMsg>(
            b, overloaded{[&](const corpus::Cancel& c) { cancels += c.id == 9; },
                          [&](const corpus::Order& o) { orders += o == corpus::sample_order(); },
                          [&](const corpus::DynMsg& m) { msgs += m == corpus::sample_dyn_msg(); },
                          [&](const corpus::Side& x) { sides += x == corpus::Side::sell; },
                          [&](const corpus::Scalars&) { ++others; },
                          [&](const corpus::Padded&) { ++others; },
                          [&](const sr::record_view&) { ++others; }});
        const bool ok = r && cancels == 1 && orders == 1 && msgs == 1 && sides == 1 && others == 0;
        report(ok, "dispatch<6 types> over a 4-record frame",
               std::format("cancel {} order {} msg {} side {} other {}{}", cancels, orders, msgs,
                           sides, others, r ? "" : std::format(" ({})", sr::to_string(r.error()))));
    }

    std::println("{} mismatches", failures);
    return failures == 0 ? 0 : 1;
}
