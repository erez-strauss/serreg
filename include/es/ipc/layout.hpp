// es/ipc/layout.hpp — the shared-memory layout of the variable-size rings (docs/ipc/ipc-design.md
// §4): geometry, the ring header, the consumer gates, the 16-byte record header, and the unit arithmetic.
//
// A ring is a run of `units` units of `unit` bytes (16, 32 or 64). Positions and sequence numbers are 32-bit counts
// of units that wrap modulo 2^32; a ring holds at most 2^31 units (128 GiB with 64-byte units), so the distance
// between any two live positions is below 2^31 and unsigned subtraction compares them. A record starts on a
// `granule` = max(unit, align) boundary with its header; the payload follows at record + data_align (16 by default,
// or 64: a payload on its own cache line):
//
//   u32 size   payload bytes (the content length; the record occupies align_up(data_align + size, granule) bytes)
//   u32 seq    the record's position (its first unit number): checked by readers, not the publish signal
//   u32 type   application message type; values >= type_reserved_first belong to the library (type_skip pads the
//              end of the buffer when a record does not fit before it)
//   u32 check  a mix of the three fields above: a reader refuses a header that does not match (never crash)
//
// Like es::ipc, everything in shared memory is a plain integer accessed through std::atomic_ref (es/ipc/common.hpp).
#pragma once

#include "common.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace es::ipc
{

// Template argument value meaning "decided at run time" (the unit size, from the geometry or the ring header).
inline constexpr std::uint32_t dynamic = 0;

// Message types. Applications use 0 .. type_reserved_first - 1; type_data (0) is the default of claim().
inline constexpr std::uint32_t type_data = 0;
inline constexpr std::uint32_t type_reserved_first = 0xFFFF'FF00u;
inline constexpr std::uint32_t type_skip =
    0xFFFF'FFFFu; // padding to the end of the buffer (never delivered)

enum class ring_kind : std::uint32_t
{
    spmc = 1, // broadcast: every consumer sees every message; the slowest gates the producer
    spsc = 2, // queue: one gate, always active; messages wait for a (possibly later) consumer
};

struct geometry
{
    std::uint32_t unit_bytes = 64;   // 16, 32 or 64: the unit of positions and of record sizes
    std::uint32_t units = 1u << 16;  // ring capacity in units: a power of two, 2 .. 2^31
    std::uint32_t max_consumers = 8; // 1 .. max_consumers_limit (an SPSC ring uses one)
};

namespace detail
{
inline constexpr std::uint64_t ring_magic =
    0x32474E4952435049; // "IPCRING2" little-endian (es::ipc_old: "IPCRING1")
// 2 (2026-10-02): gates split into an ownership word and a position word (readers publish progress with plain
// stores); data_align in the ring header.
inline constexpr std::uint32_t ring_version = 2;
inline constexpr std::size_t line = es::ipc::cache_line;
inline constexpr std::size_t cursor_offset = 64, producer_offset = 128, ticket_offset = 192,
                             gates_offset = 256;
// High-water mark of the gates ever used (u32, in the tickets line; joins only): readers take the lowest free gate,
// so the producer scans `hwm` gates, not max_consumers — 3 lines, not 16, for 3 readers of a 16-reader ring (Q6b).
inline constexpr std::size_t hwm_offset = ticket_offset + 4;
inline constexpr std::uint32_t record_header_bytes = 16;
inline constexpr std::uint32_t max_units = 1u << 31;
// At most 256 readers per ring (maintainer, 2026-10-02): bounds the producer's gate scan and the control area
// (256 + 256 × 64 bytes) — plenty for a broadcast feed, and it leaves room for an 8-bit gate id.
inline constexpr std::uint32_t max_consumers_limit = 256;

struct ring_header
{
    std::uint64_t magic; // stored last by create(): attach() waits for it
    std::uint32_t version;
    std::uint32_t kind;
    std::uint32_t unit;  // bytes per unit
    std::uint32_t align; // record alignment in bytes (granule = max(unit, align))
    std::uint32_t units; // capacity in units
    std::uint32_t max_consumers;
    std::uint64_t data_offset;
    std::uint64_t total_bytes;
    std::uint32_t data_align; // payload offset in a record (16 or 64)
    std::uint32_t reserved;
};
static_assert(sizeof(ring_header) <= line);

// One cache line per gate (layout 2).
//
//   own   ticket << 32 | state. Changed only by compare-and-swap: join (free -> joining -> active), leave and the reaper
//         (-> free), evict (active or joining -> evicted) and the evicted owner's acknowledgement (evicted -> free).
//   pos   the owner's next position. The owner publishes progress with a plain release store — no locked instruction,
//         so no store-buffer drain on the reader's path (docs/ipc/latency-analysis.md).
//   pid   the owner's process, for reaping dead ones, and pid_ticket the ticket that pid belongs to.
//
// The producer waits only for active gates (SPSC: for gate 0's pos, owned or not). An evicted gate stops holding the
// producer at once, and becomes free only when its owner has seen the eviction and stopped writing pos, so a late
// store of an evicted reader never lands on a new owner. A gate whose owner process died is freed by the reaper in any
// state.
inline constexpr std::uint32_t gate_free = 0, gate_joining = 1, gate_active = 2, gate_evicted = 3;
struct alignas(line)
    gate // one cache line each: gates_[i] is at gates_offset + i × line (data_offset counts so)
{
    std::uint64_t own;
    std::uint32_t pos;
    std::int32_t pid;
    std::uint32_t pid_ticket;
};
static_assert(sizeof(gate) == line && alignof(gate) == line,
              "es::ipc: a gate is one cache line (2.1.14 fix: it was 24 bytes, so the readers' "
              "gates shared lines)");

struct record_header
{
    std::uint32_t size;
    std::uint32_t seq;
    std::uint32_t type;
    std::uint32_t check;
};
static_assert(sizeof(record_header) == record_header_bytes);

constexpr std::uint64_t own_word(const std::uint32_t ticket, const std::uint32_t state) noexcept
{
    return (std::uint64_t{ticket} << 32) | state;
}
constexpr std::uint32_t state_of(const std::uint64_t own) noexcept
{
    return static_cast<std::uint32_t>(own);
}
constexpr std::uint32_t ticket_of(const std::uint64_t own) noexcept
{
    return static_cast<std::uint32_t>(own >> 32);
}

constexpr std::uint32_t header_check(const std::uint32_t size, const std::uint32_t seq,
                                     const std::uint32_t type) noexcept
{
    return (size * 0x9E3779B1u) ^ (seq + 0x7F4A7C15u) ^ (type * 0x85EBCA77u) ^ 0xC2B2AE3Du;
}

constexpr bool power_of_two(const std::uint64_t v) noexcept
{
    return v != 0 && (v & (v - 1)) == 0;
}
constexpr bool valid_unit(const std::uint32_t u) noexcept
{
    return u == 16 || u == 32 || u == 64;
}
constexpr bool valid_align(const std::uint32_t a) noexcept
{
    return power_of_two(a) && a >= 16 && a <= 4096;
}
constexpr bool valid_data_align(const std::uint32_t d, const std::uint32_t align) noexcept
{
    return (d == 16 || d == 64) && align >= d;
}
constexpr std::uint32_t granule(const std::uint32_t unit, const std::uint32_t align) noexcept
{
    return unit > align ? unit : align;
}
constexpr bool valid_geometry(const geometry g, const std::uint32_t align) noexcept
{
    return valid_unit(g.unit_bytes) && valid_align(align) && power_of_two(g.units) &&
           g.units <= max_units &&
           std::uint64_t{g.units} * g.unit_bytes >= 2u * granule(g.unit_bytes, align) &&
           g.max_consumers >= 1 && g.max_consumers <= max_consumers_limit;
}
// Units a record with a payload of `size` bytes occupies (header and padding to the payload included, rounded up to
// the granule). unit and granule are powers of two: a mask and a shift, no division on the hot path.
constexpr std::uint64_t record_units(const std::uint64_t size, const std::uint32_t unit,
                                     const std::uint32_t gran,
                                     const std::uint32_t data_align = record_header_bytes) noexcept
{
    return ((data_align + size + gran - 1) & ~std::uint64_t{gran - 1}) >> std::countr_zero(unit);
}
constexpr std::size_t data_offset(const geometry g, const std::uint32_t align) noexcept
{
    return es::ipc::detail::align_up(
        gates_offset + std::size_t{g.max_consumers} * line,
        granule(g.unit_bytes, align) > line ? granule(g.unit_bytes, align) : line);
}
} // namespace detail

} // namespace es::ipc
