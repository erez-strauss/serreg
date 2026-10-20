// tests/bench/hrtt_serreg.cpp — half-RTT over two es::ipc variable-size rings (SPMC by default) with es::serreg
// frames: the same 64 bytes of content as serreg_hrtt_raw, as a reflected struct. serialize() into an
// es::ipc::sink builds the frame (16-byte frame header with the call hash + the record) directly in the claimed
// ring record; deserialize() checks header, format, call hash and length before copying the record out of the
// ring record in place. Harness: hrtt_common.hpp.
#include "hrtt_common.hpp"

#include <es/serreg/serreg.hpp>

namespace hrtt::msg
{
struct Tick
{
    std::uint64_t seq;
    std::int64_t t0, t1, t2;
    std::uint64_t order_id;
    std::int64_t px;
    std::uint32_t qty;
    char sym[12];
};
} // namespace hrtt::msg

namespace
{
using hrtt::msg::Tick;
static_assert(sizeof(Tick) == 64);

struct serreg_codec
{
    static constexpr std::string_view name = "es::serreg frames (serialize / deserialize in place)";
    static constexpr std::string_view description =
        "hrtt::msg::Tick {seq, t0, t1, t2, order_id, px, qty, sym[12]} (64 B) as a compact frame: "
        "16-byte header (call hash) + record, serialized into the ring record (es::ipc::sink); "
        "decode validates header, call hash and length";
    static constexpr std::size_t wire_bytes()
    {
        return es::serreg::frame_size<Tick>();
    }

    static void put(es::ipc::producer& out, const Tick& t) noexcept
    {
        es::ipc::sink s{out}; // claim → frame header + record in the ring record → commit
        const auto r = es::serreg::serialize(s, t);
        if (!r) [[unlikely]]
            std::abort();
    }
    void send_ping(es::ipc::producer& out, const std::uint64_t seq, const std::int64_t t0) noexcept
    {
        put(out, Tick{seq, t0, 0, 0, 1000 + seq, 101'250'000, 100, "ES.SERREG"});
    }
    void send_stop(es::ipc::producer& out) noexcept
    {
        put(out, Tick{hrtt::stop_seq, 0, 0, 0, 0, 0, 0, {}});
    }
    bool read_pong(const std::span<const std::byte> f, hrtt::sample& s) const noexcept
    {
        Tick t;
        if (!es::serreg::deserialize(f, t))
            return false;
        s = {t.seq, t.t0, t.t1, t.t2};
        return true;
    }
    std::optional<std::uint64_t> echo(const std::span<const std::byte> f, es::ipc::producer& out,
                                      const std::int64_t t1) noexcept
    {
        Tick t;
        if (!es::serreg::deserialize(f, t))
            return std::nullopt;
        if (t.seq == hrtt::stop_seq)
            return t.seq;
        t.t1 = t1;
        t.t2 = hrtt::now_ns();
        put(out, t);
        return t.seq;
    }
};
static_assert(es::serreg::claiming_sink<es::ipc::sink>);
} // namespace

int main(const int argc, char** const argv)
{
    const auto opts = hrtt::parse(argc, argv, serreg_codec::name);
    if (!opts)
        return 2;
    return hrtt::app<serreg_codec>{*opts}.run();
}
