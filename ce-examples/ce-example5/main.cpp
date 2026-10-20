// ce-example5 — es::serreg frames through an es::ipc SPSC queue between two threads, on Compiler Explorer.
//
// One process, two threads, one anonymous shared-memory region holding a variable-size SPSC ring:
//   * the producer thread serializes 1000 messages of mixed sizes straight into ring records
//     (es::ipc::sink): println frames with scalars, a struct and a runtime string (2 .. 1500 bytes), plain
//     struct frames, and ONE large struct with a std::vector of 30000 samples (~120 KB) in the middle; then an
//     end-of-stream record;
//   * the consumer thread decodes each record in place with a frame_router (typed handlers: the TYPED path,
//     header-only) and prints one line of text per message.
// Self-check: the expected text is built independently from the same message generator; the program exits 1 on
// any difference, a lost or reordered message (every message carries its sequence number), an unhandled or
// undecodable frame, or a time-out; 0 when everything matches.
//
// Header-only: Compiler Explorer cannot link libserreg, so ce_registry_stub.hpp stands in for the registry and the
// dictionary-driven renderer (tools::render_frame, src/formatters.cpp) is not available here. The dictionary-driven
// path — a consumer that renders messages it was not compiled with — is examples/ipc_log in the repository.
//
// Ring record types: 2 = one es::serreg frame, 3 = end of stream (payload: u64 message count). The ring is sized at
// run time: at least twice the largest frame (es::ipc::ring::for_capacity). Both threads back off with
// std::this_thread::yield() when the ring is full or empty (Compiler Explorer may give the program one CPU).
#include "ce_registry_stub.hpp"

#include <es/ipc/ipc.hpp>
#include <es/ipc/region.hpp>
#include <es/serreg/print.hpp>
#include <es/serreg/router.hpp>
#include <es/serreg/serialize.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace demo
{
struct Fill
{
    std::uint64_t order;
    std::int32_t qty;
    double px;
};
struct Tick
{
    std::uint64_t seq;
    std::int64_t bid;
    std::int64_t ask;
};
struct Batch // the large message
{
    std::uint64_t seq;
    std::string source;
    std::vector<std::int32_t> samples;
};
} // namespace demo

namespace ce5
{
namespace sr = es::serreg;
using namespace es::serreg::literals;

inline constexpr std::uint64_t n_messages = 1000;
inline constexpr std::uint64_t batch_at = 500;
inline constexpr std::size_t batch_samples = 30000;
inline constexpr std::uint32_t record_frame = 2;
inline constexpr std::uint32_t record_end = 3;

// ---- the messages: one deterministic generator, used by the producer and by the expected text ------------------
std::string chatter(const std::uint64_t i)
{
    std::string s = std::format("msg{}:", i);
    s.append(2 + (i * 37) % 1500, static_cast<char>('a' + i % 26));
    return s;
}
demo::Fill fill_of(const std::uint64_t i)
{
    return {9000 + i, static_cast<std::int32_t>(i % 7) - 3, 100.0 + 0.25 * static_cast<double>(i)};
}
demo::Tick tick_of(const std::uint64_t i)
{
    return {i, 1000 + static_cast<std::int64_t>(i), 1002 + static_cast<std::int64_t>(i)};
}
demo::Batch batch_of(const std::uint64_t i)
{
    demo::Batch b{i, "adc-0", {}};
    b.samples.reserve(batch_samples);
    for (std::size_t k = 0; k < batch_samples; ++k)
        b.samples.push_back(static_cast<std::int32_t>((k * 7919) % 20001) - 10000);
    return b;
}
std::int64_t sum_of(const std::vector<std::int32_t>& v)
{
    std::int64_t s = 0;
    for (const auto x : v)
        s += x;
    return s;
}

// The text of each message — what the consumer prints, computed here from the generator.
std::string fill_text(const std::uint64_t seq, const demo::Fill& f)
{
    return std::format("#{} fill order={} qty={} px={:.2f}\n", seq, f.order, f.qty, f.px);
}
std::string tick_text(const demo::Tick& t)
{
    return std::format("#{} tick bid={} ask={}\n", t.seq, t.bid, t.ask);
}
std::string chatter_text(const std::uint64_t seq, const std::string_view s)
{
    return std::format("#{} chatter {} bytes: {}...{}\n", seq, s.size(), s.substr(0, 8),
                       s.substr(s.size() - 3));
}
std::string batch_text(const demo::Batch& b)
{
    return std::format("#{} batch {} samples={} sum={} first={} last={}\n", b.seq, b.source,
                       b.samples.size(), sum_of(b.samples), b.samples.front(), b.samples.back());
}
std::string order_text(const std::uint64_t seq, const double px, const std::int32_t qty)
{
    return std::format("#{} order px={:.2f} qty={}\n", seq, px, qty);
}
std::string status_text(const std::uint64_t seq, const bool ok, const std::uint8_t level)
{
    return std::format("#{} status ok={} level={}\n", seq, ok, level);
}
std::string expected_text()
{
    std::string s;
    for (std::uint64_t i = 0; i < n_messages; ++i)
    {
        if (i == batch_at)
            s += batch_text(batch_of(i));
        else
            switch (i % 5)
            {
            case 0:
                s += order_text(i, 50.0 + static_cast<double>(i) / 8,
                                static_cast<std::int32_t>(i % 100));
                break;
            case 1:
                s += fill_text(i, fill_of(i));
                break;
            case 2:
                s += chatter_text(i, chatter(i));
                break;
            case 3:
                s += tick_text(tick_of(i));
                break;
            default:
                s += status_text(i, i % 3 != 0, static_cast<std::uint8_t>(i % 5));
                break;
            }
    }
    return s;
}

// ---- the application -----------------------------------------------------------------------------------------
class app
{
public:
    int run()
    {
        // Size the ring: at least twice the largest frame (the batch), rounded up to a power-of-two unit count.
        const std::size_t largest =
            sr::frame_size(sr::serialize_options{}, "batch"_, batch_of(batch_at));
        const auto g = es::ipc::ring::for_capacity(2 * (largest + 64), 64, 1);
        auto mem = es::ipc::region::anonymous(es::ipc::ring::bytes_for(g));
        if (!mem)
            return fail("anonymous region");
        auto ring = es::ipc::ring::create(*mem, g, es::ipc::ring_kind::spsc);
        if (!ring)
            return fail("ring");
        std::println("ring: {} bytes ({} units of {}), largest frame {} bytes",
                     ring->capacity_bytes(), g.units, g.unit_bytes, largest);
        const std::string want = expected_text();

        std::string got;
        bool consumer_ok = false, producer_ok = false;
        std::thread consumer([&] { consumer_ok = consume(*ring, got); });
        std::thread producer([&] { producer_ok = produce(*ring); });
        producer.join();
        consumer.join();

        if (!producer_ok || !consumer_ok)
            return fail("a thread");
        if (got != want)
        {
            std::size_t at = 0;
            while (at < got.size() && at < want.size() && got[at] == want[at])
                ++at;
            std::println("DIFF at byte {}: got {} bytes, want {} bytes", at, got.size(),
                         want.size());
            return 1;
        }
        std::println("ok: {} messages, {} bytes of text, identical to the expected text; {} "
                     "producer full waits, "
                     "{} consumer empty polls",
                     n_messages, got.size(), full_waits_, empty_polls_);
        return 0;
    }

private:
    static int fail(const std::string_view what)
    {
        std::println("FAILED: {}", what);
        return 1;
    }
    static auto deadline()
    {
        return std::chrono::steady_clock::now() + std::chrono::seconds(20);
    }

    // Retries a serialize while the ring is full (full_policy::fail: the sink refuses, nothing is published).
    template<class F> bool send(F&& write)
    {
        const auto until = deadline();
        for (;;)
        {
            const auto r = write();
            if (r)
                return true;
            if (r.error() != sr::error::sink_full || std::chrono::steady_clock::now() > until)
            {
                std::println("producer: serialize failed: {}", sr::to_string(r.error()));
                return false;
            }
            ++full_waits_;
            std::this_thread::yield();
        }
    }

    bool produce(const es::ipc::ring& ring)
    {
        auto prod = es::ipc::producer::attach(ring, es::ipc::full_policy::fail);
        if (!prod)
            return false;
        es::ipc::sink out{*prod, record_frame};
        for (std::uint64_t i = 0; i < n_messages; ++i)
        {
            bool ok = false;
            if (i == batch_at)
            {
                const demo::Batch b = batch_of(i);
                ok = send([&] { return sr::serialize(out, "batch"_, b); });
            }
            else
                switch (i % 5)
                {
                case 0: {
                    const double px = 50.0 + static_cast<double>(i) / 8;
                    const auto qty = static_cast<std::int32_t>(i % 100);
                    ok = send([&] {
                        return sr::println(out, "#{} order px={:.2f} qty={}"_, i, px,
                                                     qty);
                    });
                    break;
                }
                case 1: {
                    const demo::Fill f = fill_of(i);
                    ok = send([&] { return sr::println(out, "#{} fill {}"_, i, f); });
                    break;
                }
                case 2: {
                    const std::string s = chatter(i);
                    ok = send([&] { return sr::println(out, "#{} chatter {}"_, i, s); });
                    break;
                }
                case 3: {
                    const demo::Tick t = tick_of(i);
                    ok = send([&] { return sr::serialize(out, "tick"_, t); });
                    break;
                }
                default: {
                    const bool good = i % 3 != 0;
                    const auto level = static_cast<std::uint8_t>(i % 5);
                    ok = send([&] {
                        return sr::println(out, "#{} status ok={} level={}"_, i, good,
                                                     level);
                    });
                    break;
                }
                }
            if (!ok)
                return false;
        }
        const std::uint64_t n = n_messages;
        const auto until = deadline();
        while (!prod->write(reinterpret_cast<const std::byte*>(&n), sizeof n, record_end))
        {
            if (std::chrono::steady_clock::now() > until)
                return false;
            std::this_thread::yield();
        }
        return true;
    }

    bool consume(const es::ipc::ring& ring, std::string& text)
    {
        auto cons = es::ipc::consumer::join(ring);
        if (!cons)
            return false;
        std::uint64_t next = 0, problems = 0, sent = 0;
        bool ended = false;
        const auto line = [&](const std::uint64_t seq, const std::string& s) {
            if (seq != next)
            {
                std::println(
                    "consumer: message #{} arrived when #{} was expected (lost or reordered)", seq,
                    next);
                ++problems;
            }
            next = seq + 1;
            std::print("{}", s);
            text += s;
        };
        sr::frame_router router;
        router
            .on_println("#{} order px={:.2f} qty={}"_,
                        [&](const std::uint64_t seq, const double px, const std::int32_t qty) {
                            line(seq, order_text(seq, px, qty));
                        })
            .on_println("#{} fill {}"_, [&](const std::uint64_t seq,
                                            const demo::Fill& f) { line(seq, fill_text(seq, f)); })
            .on_println("#{} chatter {}"_,
                        [&](const std::uint64_t seq, const std::string& s) {
                            line(seq, chatter_text(seq, s));
                        })
            .on("tick"_, [&](const demo::Tick& t) { line(t.seq, tick_text(t)); })
            .on("batch"_, [&](const demo::Batch& b) { line(b.seq, batch_text(b)); })
            .on_println("#{} status ok={} level={}"_,
                        [&](const std::uint64_t seq, const bool ok, const std::uint8_t level) {
                            line(seq, status_text(seq, ok, level));
                        });
        const auto until = deadline();
        while (!ended)
        {
            const std::size_t n = cons->poll([&](const es::ipc::message& m) {
                if (m.type == record_end)
                {
                    std::memcpy(&sent, m.data.data(), sizeof sent);
                    ended = true;
                    return false;
                }
                const auto r = router.route(m.data);
                if (!r || !*r)
                {
                    std::println("consumer: frame at position {} {}", m.position,
                                 r ? "has no handler" : sr::to_string(r.error()));
                    ++problems;
                }
                return true;
            });
            if (n == 0)
            {
                if (std::chrono::steady_clock::now() > until)
                {
                    std::println("consumer: timed out after #{}", next);
                    return false;
                }
                ++empty_polls_;
                std::this_thread::yield();
            }
        }
        return problems == 0 && next == n_messages && sent == n_messages;
    }

    std::uint64_t full_waits_ = 0, empty_polls_ = 0; // each written by one thread only
};
} // namespace ce5

int main()
{
    return ce5::app{}.run();
}
