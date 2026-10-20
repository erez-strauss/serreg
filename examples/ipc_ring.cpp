// examples/ipc_ring.cpp — es::serreg frames through an es::ipc SPSC queue between two processes (v2.0.5; until
// 2.0.4 this example carried its own in-process ring; the variable-size es::ipc ring since 2.1.13a). The producer
// process serialize()s each frame straight into the ring record it claimed (es::ipc::sink is a claiming_sink: no
// intermediate copy); the consumer process decodes each frame in place with dispatch<Order, Cancel>().
//
// The queue lives in an anonymous shared mapping inherited across fork(). For two unrelated programs, use a
// named region instead — creator: region::create("orders.q", ring::bytes_for(g)) + ring::create(region, g, spsc);
// other program: region::open("orders.q") + ring::attach(region, spsc) — the rest of the code is identical.
#include <es/ipc/ipc.hpp>
#include <es/serreg/serreg.hpp>

#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <span>

namespace ns
{
struct Order
{
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
    char sym[8];
};
struct Cancel
{
    std::uint64_t id;
    std::uint32_t reason;
};
} // namespace ns

template<class... Fs> struct overloaded : Fs...
{
    using Fs::operator()...;
};

namespace
{
constexpr std::uint64_t n_messages = 100000;

int consume(const es::ipc::ring& q)
{
    using namespace es::serreg;
    auto c = es::ipc::consumer::join(q);
    if (!c)
        return 2;
    std::uint64_t orders = 0, cancels = 0, literals = 0, qty = 0, failed = 0;
    const auto on_frame =
        overloaded{[&](const ns::Order& o) {
                       ++orders;
                       qty += o.qty;
                   },
                   [&](const ns::Cancel&) { ++cancels; }, [&](const literal_view) { ++literals; }};
    while (orders + cancels + failed < n_messages)
        if (!c->poll([&](const es::ipc::message& m) { // in place, in the ring record
                failed += !dispatch<ns::Order, ns::Cancel>(m.data, on_frame);
            }))
            ::sched_yield(); // nothing yet: let the producer run (latency does not matter here)
    std::println("orders={} cancels={} literals={} total_qty={}", orders, cancels, literals, qty);
    return failed == 0 && literals == n_messages ? 0 : 1;
}
} // namespace

namespace ipc_ring
{
using namespace es::serreg;
using namespace es::serreg::literals;

// Creates the queue in an anonymous shared mapping, forks the consumer, produces n_messages frames into the queue
// and returns the consumer's exit status.
class app
{
public:
    int run() const
    {
        // 128 KiB of records (what 1024 slots of 128 bytes were): every frame here is far smaller
        const auto g = es::ipc::ring::for_capacity(128 * 1024, 64, 1);
        auto mem = es::ipc::region::anonymous(es::ipc::ring::bytes_for(g));
        auto q = mem ? es::ipc::ring::create(*mem, g, es::ipc::ring_kind::spsc)
                     : std::unexpected(mem.error());
        if (!q)
        {
            std::println(stderr, "cannot create the queue");
            return 1;
        }
        const pid_t child = ::fork();
        if (child == 0)
        {
            const int rc = consume(*q);
            std::fflush(stdout); // _Exit does not flush stdio
            std::_Exit(rc);
        }
        if (child < 0)
            return 1;
        if (const int rc = produce(*q))
            return rc;
        int st = 0;
        ::waitpid(child, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }

private:
    // Three orders, then a cancel, and again: each frame is serialized straight into the ring record it claimed.
    static int produce(const es::ipc::ring& q)
    {
        // full queue: claim() spins until the consumer frees room
        auto p = es::ipc::producer::attach(q);
        if (!p)
            return 1;
        es::ipc::sink out{*p};
        for (std::uint64_t i = 0; i < n_messages; ++i)
        {
            const auto r =
                (i % 4 == 3)
                    ? serialize(out, "cancel"_lit, ns::Cancel{i, 1})
                    : serialize(out, "order"_lit,
                                ns::Order{i, 1000 + static_cast<std::int64_t>(i), 10, "MSFT"});
            if (!r)
            {
                std::println(stderr, "serialize failed: {}", to_string(r.error()));
                return 1;
            }
        }
        return 0;
    }
};
} // namespace ipc_ring

int main()
{
    return ipc_ring::app{}.run();
}
