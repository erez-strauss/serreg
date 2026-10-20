// 2.2.6 (review B1): the canonical sort of an unordered container builds its order table on the heap above 64
// entries, on a claiming sink too. serialize() of such a type is not noexcept (detail::may_sort_v), so a failing
// allocation there must propagate as std::bad_alloc, not terminate. This TU replaces the global operator new
// with one that fails on demand — its own executable (serreg_alloc_tests), so the hook touches no other test.
#include "corpus.hpp"

#include <es/serreg/serreg.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <unordered_map>
#include <vector>

namespace
{
// armed: the next allocation throws std::bad_alloc (and disarms itself)
constinit std::atomic<bool> fail_next{false};
} // namespace

void* operator new(const std::size_t n)
{
    if (fail_next.exchange(false, std::memory_order_relaxed))
        throw std::bad_alloc{};
    if (void* const p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](const std::size_t n)
{
    return ::operator new(n);
}
void operator delete(void* const p) noexcept
{
    std::free(p);
}
void operator delete[](void* const p) noexcept
{
    std::free(p);
}
void operator delete(void* const p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* const p, std::size_t) noexcept
{
    std::free(p);
}

namespace alloc_test
{
struct Table
{
    std::unordered_map<std::int32_t, std::int32_t> m;
};
template<class S, class T> es::serreg::result<std::size_t> armed_serialize(S& out, const T& v)
{
    fail_next.store(true, std::memory_order_relaxed);
    return es::serreg::serialize(out, v);
}
} // namespace alloc_test

using namespace es::serreg;

TEST_CASE(
    "B1 a failing allocation of the canonical sort's order table propagates as std::bad_alloc "
    "from serialize() into a claiming sink",
    "[alloc][containers][B1]")
{
    STATIC_REQUIRE_FALSE(detail::serialize_noexcept<span_sink, alloc_test::Table>);
    STATIC_REQUIRE_FALSE(noexcept(alloc_test::armed_serialize(
        std::declval<span_sink&>(), std::declval<const alloc_test::Table&>())));
    alignas(16) std::byte buf[8192];
    alloc_test::Table t64, t65;
    for (std::int32_t i = 0; i < 64; ++i)
        t64.m.emplace(i * 7919 % 1009, i);
    t65.m = t64.m;
    t65.m.emplace(-1, 65);
    REQUIRE(t64.m.size() == 64);
    REQUIRE(t65.m.size() == 65);

    SECTION("64 entries: the table is on the stack, nothing allocates")
    {
        span_sink out{buf};
        const auto r = alloc_test::armed_serialize(out, t64);
        const bool still_armed = fail_next.exchange(false, std::memory_order_relaxed);
        REQUIRE(r.has_value());
        CHECK(still_armed);
    }
    SECTION(
        "65 entries: the table is on the heap; std::bad_alloc propagates, the sink is untouched")
    {
        span_sink out{buf};
        REQUIRE_THROWS_AS(alloc_test::armed_serialize(out, t65), std::bad_alloc);
        fail_next.store(false, std::memory_order_relaxed);
        CHECK(out.used == 0);
        // and with memory the same call succeeds
        const auto r = serialize(out, t65);
        REQUIRE(r.has_value());
        CHECK(out.used == *r);
    }
    SECTION("canonical_unordered = false: no sort, no table, no allocation at any size")
    {
        span_sink out{buf};
        fail_next.store(true, std::memory_order_relaxed);
        const auto r = serialize_with(serialize_options{.canonical_unordered = false}, out, t65);
        const bool still_armed = fail_next.exchange(false, std::memory_order_relaxed);
        REQUIRE(r.has_value());
        CHECK(still_armed);
    }
}
