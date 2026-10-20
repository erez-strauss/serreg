// ce-example3 — es::serreg frame layer on Compiler Explorer: serialize / deserialize / print frames, byte for byte.
//
// What it tests (the parts ce-example1/2 do not): the frame writer (16-byte header, compact and full records,
// padding scrub and bool normalisation from the compile-time gap tables, 0-byte literals, string and scalar
// records, print/println frames with the compile-time std::format check), frame_size, and the typed readers
// (deserialize, deserialize_print/println, inspect + view<T>). Every frame written here must equal, byte for byte,
// the frame of the checked-in golden capture tests/fixtures/golden/orders.ser (written by
// tests/golden/make_fixtures.cpp with GCC 16.2), whatever compiler and standard library build this file.
//
// Files: 15 library headers (symlinks into include/) + ce_registry_stub.hpp (a minimal registry replacing
// libserreg, so that this is one translation unit). Lanes: see ce-run.py (gcc, clang-libcxx, clang-libstdcxx).
// Exit status: 0 when everything matches; 1 otherwise (each mismatch is printed as DIFF with the first differing
// byte).
#include "ce_registry_stub.hpp"

#include <es/serreg/deserialize.hpp>
#include <es/serreg/print.hpp>
#include <es/serreg/serialize.hpp>

#include <cstdint>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ---- corpus (verbatim from tests/golden/corpus.hpp) ----------------------------------------------------------
namespace corpus
{
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
inline bool operator==(const Cancel& l, const Cancel& r) noexcept
{
    return l.id == r.id && l.reason == r.reason;
}
inline Order sample_order() noexcept
{
    return Order{42, 1050000, 100, Side::buy, "AAPL"};
}
inline Cancel sample_cancel() noexcept
{
    return Cancel{42, 7};
}
} // namespace corpus

// ---- expected bytes -----------------------------------------------------------------------------------------
// 8 frames of tests/fixtures/golden/orders.ser (after the FILE header and dictionary), as hex
inline constexpr std::string_view golden_orders[] = {
    "3000000000000200b2fd66de5be2f8bc2a00000000000000900510000000000064000000014141504c000000000000"
    "00",
    "3000000000000200b2fd66de5be2f8bc2b00000000000000ffffffffffffffff00000000024d534654000000000000"
    "00",
    "2000000000000200b40c83b7d16b8a982a000000000000000700000000000000",
    "1800000000000100eacad4802c6134360100000000000000",
    "58000000010002002e3be2684d492120357284a06d4c8e190800000000000000357284a06d4c8e19d21d44c6d88c7f"
    "36"
    "20000000000000002a00000000000000900510000000000064000000014141504c00000000000000",
    "20000000000004005e56001b91497fe9030000006e6577006400000000000000",
    "28000000000004005b8b01d56f1467e5000000000000044007000000000000000200000000000000",
    "280000000000040057d9d0c03451439903000000616263002a00000000000000000000000000f83f",
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

// Compares a written frame with golden frame i; returns the bytes for the read-back checks.
std::vector<std::byte> frame(const std::size_t i, const std::string_view what,
                             std::vector<std::byte> bytes)
{
    const std::string got = hex(bytes);
    const std::string_view want = golden_orders[i];
    std::size_t at = 0;
    while (at < got.size() && at < want.size() && got[at] == want[at])
        ++at;
    const bool ok = got == want;
    report(ok, std::format("frame {} {}", i, what),
           ok ? std::format("{} bytes identical", bytes.size())
              : std::format("{} vs {} bytes; first difference at byte {}", bytes.size(),
                            want.size() / 2, at / 2));
    if (!ok)
        std::println("     got  {}\n     want {}", got, want);
    return bytes;
}

template<class... Ts> std::vector<std::byte> write(const Ts&... vs)
{
    std::vector<std::byte> b;
    sr::vector_sink s{b};
    if (!sr::serialize(s, vs...))
        b.clear();
    return b;
}

// An Order whose padding bytes are 0xAB (and qty/side assigned field by field): the scrub must zero them.
corpus::Order dirty_order()
{
    corpus::Order o;
    std::memset(static_cast<void*>(&o), 0xAB, sizeof o);
    const corpus::Order s = corpus::sample_order();
    o.id = s.id;
    o.px = s.px;
    o.qty = s.qty;
    o.side = s.side;
    std::memcpy(o.sym, s.sym, sizeof o.sym);
    return o;
}
} // namespace

int main()
{
    // ---- 1. the writer: the 8 frames of make_fixtures.cpp, in order ----
    const auto f0 =
        frame(0, "(\"new order\", Order), dirty padding", write("new order"_lit, dirty_order()));
    corpus::Order o2 = corpus::sample_order();
    o2.id = 43;
    o2.px = -1;
    o2.qty = 0;
    o2.side = corpus::Side::sell;
    std::memcpy(o2.sym, "MSFT\0\0\0", 8);
    const auto f1 = frame(1, "(\"new order\", Order) #2", write("new order"_lit, o2));
    const auto f2 = frame(2, "(Cancel, \"why\")", write(corpus::sample_cancel(), "why"_lit));
    const auto f3 = frame(3, "(Side)", write(corpus::Side::buy));
    std::vector<std::byte> b4;
    {
        sr::vector_sink s{b4};
        static_cast<void>(sr::serialize_with(sr::serialize_options{.full_records = true}, s,
                                             "evt"_lit, corpus::sample_order()));
    }
    const auto f4 = frame(4, "full records (\"evt\", Order)", b4);
    std::vector<std::byte> b5, b6;
    {
        sr::vector_sink s{b5};
        static_cast<void>(sr::print(s, "hello {} {} {}"_, "new", 100, "worlds"_));
    }
    const auto f5 = frame(5, "print(\"hello {} {} {}\", ...)", b5);
    {
        sr::vector_sink s{b6};
        static_cast<void>(
            sr::println(s, "px={:.2f} qty={} side={}"_, 2.5, 7u, corpus::Side::sell));
    }
    const auto f6 = frame(6, "println(\"px={:.2f} ...\", ...)", b6);
    const auto f7 = frame(7, "(\"abc\", 42, 1.5, \"evt\")", write("abc", 42, 1.5, "evt"_));

    // ---- 2. frame_size: exact and constexpr for flat signatures (FR-S3) ----
    constexpr std::size_t fs0 = sr::frame_size<decltype("new order"_lit), corpus::Order>();
    report(fs0 == 48, "frame_size<\"new order\", Order>()", std::format("{}", fs0));

    // ---- 3. the typed readers, on the bytes just written ----
    {
        corpus::Order o{};
        const auto r = sr::deserialize(f0, "new order"_lit, o);
        report(r && o == corpus::sample_order(), "deserialize frame 0",
               r ? "ok" : sr::to_string(r.error()));
        corpus::Cancel c{};
        const auto r2 = sr::deserialize(f2, c, "why"_lit);
        report(r2 && c == corpus::sample_cancel(), "deserialize frame 2",
               r2 ? "ok" : sr::to_string(r2.error()));
        corpus::Side side{};
        const auto r3 = sr::deserialize(f3, side);
        report(r3 && side == corpus::Side::buy, "deserialize frame 3",
               r3 ? "ok" : sr::to_string(r3.error()));
        corpus::Order o4{};
        const auto r4 = sr::deserialize(f4, "evt"_lit, o4);
        report(r4 && o4 == corpus::sample_order(), "deserialize frame 4 (full records)",
               r4 ? "ok" : sr::to_string(r4.error()));
        std::string s5;
        int i5 = 0;
        const auto r5 = sr::deserialize_print(f5, "hello {} {} {}"_, s5, i5, "worlds"_);
        report(r5 && s5 == "new" && i5 == 100, "deserialize_print frame 5",
               r5 ? "ok" : sr::to_string(r5.error()));
        double d6 = 0;
        unsigned u6 = 0;
        corpus::Side side6{};
        const auto r6 = sr::deserialize_println(f6, "px={:.2f} qty={} side={}"_, d6, u6, side6);
        report(r6 && d6 == 2.5 && u6 == 7 && side6 == corpus::Side::sell,
               "deserialize_println frame 6", r6 ? "ok" : sr::to_string(r6.error()));
        std::string_view s7;
        int i7 = 0;
        double d7 = 0;
        const auto r7 = sr::deserialize(f7, s7, i7, d7, "evt"_);
        report(r7 && s7 == "abc" && i7 == 42 && d7 == 1.5, "deserialize frame 7 (string_view)",
               r7 ? "ok" : sr::to_string(r7.error()));
        corpus::Cancel
            wrong{}; // same arity, wrong type: the call hash must refuse it before any output is written
        const auto bad = sr::deserialize(f0, "new order"_lit, wrong);
        report(!bad && bad.error() == sr::error::hash_mismatch,
               "frame 0 read as (\"new order\", Cancel)",
               bad ? "accepted!" : sr::to_string(bad.error()));
    }

    // ---- 4. inspect + zero-copy view (needs the registry stub's find_call / find_type) ----
    {
        const auto fv = sr::inspect(f0);
        bool ok = fv && fv->size() == 2;
        const corpus::Order* p = nullptr;
        if (ok)
            for (const sr::record_view r : fv->records())
                if (!r.is_literal)
                    if (const auto v = sr::view<corpus::Order>(r))
                        p = *v;
        ok = ok && p && *p == corpus::sample_order();
        report(ok, "inspect frame 0 + view<Order>",
               ok ? "zero-copy view equals the sample" : "failed");
    }

    std::println("{} mismatches", failures);
    return failures == 0 ? 0 : 1;
}
