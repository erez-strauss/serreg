// tests/bench/hrtt_raw.cpp — half-RTT over two es::ipc variable-size rings (SPMC by default) with raw packets: a
// 64-byte POD {seq, t0, t1, t2, order fields} built directly in the claimed ring record (claim/commit) and read in
// place through a typed view. No serialization header, no hash, no validation beyond the length: the floor
// serreg_hrtt_serreg is compared against. Harness: hrtt_common.hpp.
#include "hrtt_common.hpp"

#include <cstdlib>
#include <new>
#include <type_traits>

namespace
{
struct raw_packet
{
    std::uint64_t seq;
    std::int64_t t0, t1, t2;
    std::uint64_t order_id;
    std::int64_t px;
    std::uint32_t qty;
    char sym[12];
};
static_assert(sizeof(raw_packet) == 64 && std::is_trivially_copyable_v<raw_packet>);
// the payload of a record starts at record + 16 (es::ipc::ring's DataAlign), and records are 64-byte aligned
static_assert(alignof(raw_packet) <= 16);

// Claims a record of exactly one packet, lets fill() build the packet in it, publishes it. Under full_policy::spin
// a claim of a size the ring can hold never fails; a failure here is a bug.
template<class F> void publish_packet(es::ipc::producer& out, F&& fill) noexcept
{
    const auto s = out.claim(sizeof(raw_packet));
    if (!s) [[unlikely]]
        std::abort();
    fill(*::new (static_cast<void*>(s->data())) raw_packet);
    out.commit(sizeof(raw_packet));
}

struct raw_codec
{
    static constexpr std::string_view name = "raw packets (zero-copy POD, no serialization)";
    static constexpr std::string_view description =
        "64-byte POD {seq, t0, t1, t2, order_id, px, qty, sym[12]} built in the ring record "
        "(claim/commit) and read in place (typed view); checked by length only";
    static constexpr std::size_t wire_bytes()
    {
        return sizeof(raw_packet);
    }

    // A typed view of the packet in the ring record (no copy); nullptr when the length is not a packet.
    static const raw_packet* view(const std::span<const std::byte> f) noexcept
    {
        if (f.size() != sizeof(raw_packet))
            return nullptr;
#if defined(__cpp_lib_start_lifetime_as)
        return std::start_lifetime_as<raw_packet>(static_cast<const void*>(f.data()));
#else
        return std::launder(reinterpret_cast<const raw_packet*>(f.data()));
#endif
    }
    void send_ping(es::ipc::producer& out, const std::uint64_t seq, const std::int64_t t0) noexcept
    {
        publish_packet(out, [&](raw_packet& pk) {
            pk = {seq, t0, 0, 0, 1000 + seq, 101'250'000, 100, "ES.SERREG"};
        });
    }
    void send_stop(es::ipc::producer& out) noexcept
    {
        publish_packet(out, [](raw_packet& pk) { pk = {hrtt::stop_seq, 0, 0, 0, 0, 0, 0, {}}; });
    }
    bool read_pong(const std::span<const std::byte> f, hrtt::sample& s) const noexcept
    {
        const raw_packet* const pk = view(f);
        if (!pk)
            return false;
        s = {pk->seq, pk->t0, pk->t1, pk->t2};
        return true;
    }
    std::optional<std::uint64_t> echo(const std::span<const std::byte> f, es::ipc::producer& out,
                                      const std::int64_t t1) noexcept
    {
        const raw_packet* const in = view(f);
        if (!in)
            return std::nullopt;
        if (in->seq == hrtt::stop_seq)
            return in->seq;
        publish_packet(out,
                       [&](raw_packet& pk) { // from the a2b record straight into the b2a record
                           pk = *in;
                           pk.t1 = t1;
                           pk.t2 = hrtt::now_ns();
                       });
        return in->seq;
    }
};
} // namespace

int main(const int argc, char** const argv)
{
    const auto opts = hrtt::parse(argc, argv, raw_codec::name);
    if (!opts)
        return 2;
    return hrtt::app<raw_codec>{*opts}.run();
}
