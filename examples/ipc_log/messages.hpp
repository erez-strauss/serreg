// examples/ipc_log/messages.hpp — what the producer and the consumer of the ipc_log example share: the ring record
// types, the one message type the consumer is compiled with (Fill), and its text form.
//
//   producer: serialize(sink, "fill"_, ipc_log::Fill{...});             // record type ipc_log::record_frame
//   consumer: router.on("fill"_, [](const ipc_log::Fill& f) { std::print("{}", ipc_log::fill_text(f)); });
//
// Everything else the producer sends (dozens of println formats, structs with std::string and
// std::vector members, one large batch) is NOT declared here: the consumer renders it from the producer's
// dictionary, which arrives in band as the first record (record_dictionary).
#pragma once

#include <cstdint>
#include <format>
#include <string>

namespace ipc_log
{

// Ring record types (es::ipc message types; 0 = type_data is not used by this example):
//   record_dictionary  payload: the producer's RRI (registry::export_image())
//   record_frame       payload: one es::serreg frame (header included)
//   record_end         payload: u64, the number of record_frame records sent
inline constexpr std::uint32_t record_dictionary = 1;
inline constexpr std::uint32_t record_frame = 2;
inline constexpr std::uint32_t record_end = 3;

// The one type both sides know.
struct Fill
{
    std::uint64_t order_id;
    std::uint64_t exec_id;
    std::int32_t qty;
    double px;
};

// The consumer's text for a Fill (its typed handler) — and the producer's reference text for it.
inline std::string fill_text(const Fill& f)
{
    return std::format("FILL order={} exec={} qty={} px={:.4f}\n", f.order_id, f.exec_id, f.qty,
                       f.px);
}

} // namespace ipc_log
