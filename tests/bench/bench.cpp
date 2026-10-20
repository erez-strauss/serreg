// tests/bench/bench.cpp — hot-path ceilings (cc §6, NFR-1): serialize() of a 32-byte struct into a span_sink and
// inspect() of a one-record frame. Each is measured as the mean over 1e5 iterations (a plain steady_clock loop,
// REQUIREd against a ceiling loose enough for a CI box: 50 ns / 500 ns) and additionally reported through Catch2's
// BENCHMARK for a distribution. The target is compiled with -O2 whatever the preset (a debug measurement of the hot
// path says nothing); the ctest carries the label `bench` (ctest -L bench / -LE bench).
#include "corpus.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <print>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace
{
constexpr int iterations = 100000;
// The ceilings apply to an optimised libserreg. Under a Debug configuration the registry lookups behind inspect()
// (registry.cpp, -O0) dominate, so CMake passes ES_SERREG_BENCH_DEBUG_LIB and the ceilings are relaxed 4x; the
// numbers are printed either way and the release preset enforces the real ones.
#if defined(ES_SERREG_BENCH_DEBUG_LIB)
constexpr double ceiling_scale = 4.0;
constexpr const char* lib_build = "debug libserreg";
#else
constexpr double ceiling_scale = 1.0;
constexpr const char* lib_build = "optimised libserreg";
#endif
constexpr double serialize_ceiling_ns = 50.0 * ceiling_scale;
constexpr double inspect_ceiling_ns = 500.0 * ceiling_scale;

template<class F> double mean_ns(F&& f)
{
    for (int i = 0; i < 1000; ++i)
        f(); // warm-up
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i)
        f();
    const auto t1 = std::chrono::steady_clock::now();
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           iterations;
}
} // namespace

TEST_CASE("bench NFR-1 serialize 32-byte struct into span_sink < 50 ns mean", "[bench]")
{
    static_assert(sizeof(corpus::Order) == 32);
    alignas(64) std::byte buf[256];
    corpus::Order o = corpus::sample_order();
    std::uint64_t sink_bytes = 0;
    const double ns = mean_ns([&] {
        span_sink s{buf};
        o.id += 1;
        const auto r = serialize(s, o);
        sink_bytes += *r;
    });
    std::println("serialize(span_sink, Order{{32 B}}): mean {:.1f} ns over {} iterations (ceiling "
                 "{:.0f} ns, {})",
                 ns, iterations, serialize_ceiling_ns, lib_build);
    CHECK(sink_bytes == static_cast<std::uint64_t>(iterations + 1000) *
                            frame_size<corpus::Order>()); // incl. warm-up
    REQUIRE(ns < serialize_ceiling_ns);
    // The input changes per sample and the bytes written are read back, so the optimiser cannot hoist the
    // frame out of the loop (it did: the rows read 0.7 ns with "est run time 0 ns" until this was added).
    BENCHMARK_ADVANCED("serialize Order -> span_sink")(Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&](const int i) {
            span_sink s{buf};
            o.id = static_cast<std::uint64_t>(i);
            const auto r = serialize(s, o);
            Catch::Benchmark::keep_memory(buf);
            return r ? *r + std::to_integer<std::size_t>(buf[16]) : 0;
        });
    };
    BENCHMARK_ADVANCED("serialize lit + Order -> span_sink")(Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&](const int i) {
            span_sink s{buf};
            o.id = static_cast<std::uint64_t>(i);
            const auto r = serialize(s, "evt"_lit, o);
            Catch::Benchmark::keep_memory(buf);
            return r ? *r + std::to_integer<std::size_t>(buf[16]) : 0;
        });
    };
}

TEST_CASE("bench NFR-1 inspect one-record frame < 500 ns mean", "[bench]")
{
    std::vector<std::byte> frame;
    vector_sink out{frame};
    REQUIRE(serialize(out, corpus::sample_order()));
    std::uint64_t n = 0;
    const double ns = mean_ns([&] {
        const auto fv = inspect(frame);
        for (const record_view r : fv->records())
            n += r.payload.size();
    });
    std::println(
        "inspect(1-record frame): mean {:.1f} ns over {} iterations (ceiling {:.0f} ns, {})", ns,
        iterations, inspect_ceiling_ns, lib_build);
    CHECK(n ==
          static_cast<std::uint64_t>(iterations + 1000) * sizeof(corpus::Order)); // incl. warm-up
    REQUIRE(ns < inspect_ceiling_ns);
    BENCHMARK("inspect 1-record frame")
    {
        return inspect(frame);
    };
    std::uint64_t seen = 0;
    BENCHMARK("dispatch<Order> 1-record frame")
    {
        return dispatch<corpus::Order>(frame, [&](const corpus::Order& x) { seen += x.qty; });
    };
    corpus::Order back{};
    // The frame's id field changes per sample (the writer's bytes, not a re-serialization) and the output is
    // consumed, so the decode is really performed each time.
    BENCHMARK_ADVANCED("deserialize Order")(Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&](const int i) {
            frame[16] =
                static_cast<std::byte>(i); // the low byte of Order::id, at the record's start
            const auto r = deserialize(frame, back);
            return r ? back.id : 0;
        });
    };
}
