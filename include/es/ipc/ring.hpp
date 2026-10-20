// es/ipc/ring.hpp — basic_ring<Unit, Align, DataAlign>: a handle to a variable-size ring in shared or private memory
// (docs/ipc/ipc-design.md). One layout serves both kinds: ring_kind::spmc (broadcast, gated by the
// slowest consumer) and ring_kind::spsc (a queue with one always-active gate).
//
//   es::ipc::geometry g{.unit_bytes = 64, .units = 1u << 20, .max_consumers = 4};    // 64 MiB of records
//   auto mem  = es::ipc::region::create("feed", es::ipc::ring::bytes_for(g));       // or region::anonymous
//   auto ring = es::ipc::ring::create(*mem, g, es::ipc::ring_kind::spmc);
//   // another process: auto ring = es::ipc::ring::attach(*es::ipc::region::open("feed"));
//
// Unit: 0 (`dynamic`) = the unit size comes from the geometry / the ring header; 16, 32 or 64 = fixed at compile
// time (attach() refuses a ring of another unit). Align: the record alignment in bytes (default 64, one cache line);
// a layout property, so attach() refuses a ring created with another one. DataAlign: the payload's offset in a
// record, 16 (default: header and up to 48 payload bytes share a line) or 64 (Align >= 64; the payload starts a
// cache line, one more line per record); also checked by attach().
//
// Memory map (offsets from the start, every field on its own cache line):
//   0      ring_header      magic "IPCRING2" (stored last), version, kind, unit, align, units, max_consumers, ...
//   64     cursor (u32)     position after the last published record (the producer's single release store)
//   128    producer (i32)   pid holding the producer role, 0 = none
//   192    tickets (u32)    ownership ticket counter for consumer gates; +4: hwm (u32), gates ever used
//   256    gates            max_consumers × {u64 own = ticket << 32 | state, u32 pos, i32 pid, u32 pid_ticket}
//   data   units × unit     records (data_offset is a multiple of the granule and of 64)
#pragma once

#include "common.hpp"
#include "layout.hpp"
#include "region.hpp"

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <system_error>

namespace es::ipc
{

template<std::uint32_t Unit, std::uint32_t Align, std::uint32_t DataAlign> class basic_producer;
template<std::uint32_t Unit, std::uint32_t Align, std::uint32_t DataAlign> class basic_consumer;

template<std::uint32_t Unit = dynamic, std::uint32_t Align = 64, std::uint32_t DataAlign = 16>
class basic_ring
{
    static_assert(Unit == dynamic || detail::valid_unit(Unit),
                  "es::ipc: Unit must be 0 (run time), 16, 32 or 64");
    static_assert(detail::valid_align(Align), "es::ipc: Align must be a power of two in 16..4096");
    static_assert(detail::valid_data_align(DataAlign, Align),
                  "es::ipc: DataAlign must be 16 or 64, and at most Align");

public:
    static constexpr std::uint32_t unit_template = Unit;
    static constexpr std::uint32_t align = Align;
    static constexpr std::uint32_t data_align = DataAlign;
    // The alignment create() and attach() require of the memory: records are aligned to Align only if the memory is
    // (a region is page-aligned; a buffer of your own must be aligned to this).
    static constexpr std::size_t mem_align = Align > detail::line ? Align : detail::line;

    // A geometry this ring type accepts: valid, and with the compile-time unit when there is one.
    static constexpr bool valid(const geometry g) noexcept
    {
        return detail::valid_geometry(g, Align) && (Unit == dynamic || g.unit_bytes == Unit);
    }
    // Bytes of memory a ring of geometry g needs; 0 when g is invalid for this ring type.
    static constexpr std::size_t bytes_for(const geometry g) noexcept
    {
        if (!valid(g))
            return 0;
        return detail::data_offset(g, Align) + std::size_t{g.units} * g.unit_bytes;
    }
    // The smallest geometry whose capacity is at least `capacity_bytes` (rounded up to a power-of-two unit count).
    static constexpr geometry for_capacity(const std::size_t capacity_bytes,
                                           const std::uint32_t unit_bytes = Unit == dynamic ? 64
                                                                                            : Unit,
                                           const std::uint32_t max_consumers = 8) noexcept
    {
        std::uint64_t units = 2;
        while (units * unit_bytes < capacity_bytes && units < detail::max_units)
            units *= 2;
        geometry g{unit_bytes, static_cast<std::uint32_t>(units), max_consumers};
        while (std::uint64_t{g.units} * g.unit_bytes < 2u * detail::granule(unit_bytes, Align) &&
               g.units < detail::max_units)
            g.units *= 2;
        return g;
    }

    // Constructs a ring in [mem, mem + bytes): errc::invalid_argument (bad geometry, mem not aligned to
    // max(64, Align): records start at multiples of the granule from mem),
    // errc::no_buffer_space (bytes < bytes_for(g)). The record area is not cleared (readers never look past the
    // cursor), so creating a large ring touches only its first lines.
    static std::expected<basic_ring, std::errc> create(void* const mem, const std::size_t bytes,
                                                       const geometry requested,
                                                       const ring_kind k = ring_kind::spmc) noexcept
    {
        // An SPSC ring has exactly one gate: its layout is computed for one consumer whatever the caller asked, so
        // attach() (which recomputes from the header) finds the same offsets (test B1).
        const geometry g =
            k == ring_kind::spsc ? geometry{requested.unit_bytes, requested.units, 1} : requested;
        if (!valid(requested) || !mem || reinterpret_cast<std::uintptr_t>(mem) % mem_align != 0 ||
            (k != ring_kind::spmc && k != ring_kind::spsc))
            return std::unexpected(std::errc::invalid_argument);
        const std::size_t need = bytes_for(g);
        if (bytes < need)
            return std::unexpected(std::errc::no_buffer_space);
        auto* const base = static_cast<std::byte*>(mem);
        std::memset(base, 0,
                    detail::data_offset(g, Align)); // header, cursor, producer, tickets, gates
        auto* const h = reinterpret_cast<detail::ring_header*>(base);
        h->version = detail::ring_version;
        h->kind = static_cast<std::uint32_t>(k);
        h->unit = g.unit_bytes;
        h->align = Align;
        h->units = g.units;
        h->max_consumers = g.max_consumers;
        h->data_offset = detail::data_offset(g, Align);
        h->total_bytes = need;
        h->data_align = DataAlign;
        es::ipc::detail::store(h->magic, detail::ring_magic, std::memory_order_release);
        return basic_ring(base, *h);
    }
    static std::expected<basic_ring, std::errc> create(const es::ipc::region& r, const geometry g,
                                                       const ring_kind k = ring_kind::spmc) noexcept
    {
        return create(r.data(), r.size(), g, k);
    }

    // Validates a ring another process or thread created; the handle is built from a private copy of the header
    // (a peer changing the shared header later cannot move this process's view). errc::resource_unavailable_try_again:
    // not constructed yet; errc::invalid_argument: not an es::ipc ring, another kind, another unit, alignment or
    // data alignment than this type fixes, or an inconsistent header; errc::not_supported: another layout version;
    // errc::no_buffer_space: the memory is smaller than the ring says.
    static std::expected<basic_ring, std::errc> attach(void* const mem, const std::size_t bytes,
                                                       const ring_kind k = ring_kind::spmc) noexcept
    {
        if (!mem || reinterpret_cast<std::uintptr_t>(mem) % mem_align != 0 || bytes < detail::line)
            return std::unexpected(std::errc::invalid_argument);
        auto* const base = static_cast<std::byte*>(mem);
        const auto* const shared = reinterpret_cast<const detail::ring_header*>(base);
        const std::uint64_t magic = es::ipc::detail::load(shared->magic, std::memory_order_acquire);
        if (magic == 0)
            return std::unexpected(std::errc::resource_unavailable_try_again);
        if (magic != detail::ring_magic)
            return std::unexpected(std::errc::invalid_argument);
        detail::ring_header h{};
        std::memcpy(&h, shared, sizeof h);
        if (h.version != detail::ring_version)
            return std::unexpected(std::errc::not_supported);
        const geometry g{h.unit, h.units, h.max_consumers};
        const bool consumers_ok =
            h.kind == static_cast<std::uint32_t>(ring_kind::spsc) ? h.max_consumers == 1 : true;
        if (h.kind != static_cast<std::uint32_t>(k) || h.align != Align ||
            h.data_align != DataAlign || !valid(g) || !consumers_ok ||
            h.data_offset != detail::data_offset(g, Align) || h.total_bytes != bytes_for(g))
            return std::unexpected(std::errc::invalid_argument);
        if (bytes < h.total_bytes)
            return std::unexpected(std::errc::no_buffer_space);
        return basic_ring(base, h);
    }
    static std::expected<basic_ring, std::errc> attach(const es::ipc::region& r,
                                                       const ring_kind k = ring_kind::spmc) noexcept
    {
        return attach(r.data(), r.size(), k);
    }

    // ---- shape ---------------------------------------------------------------------------------------------------
    geometry shape() const noexcept
    {
        return {unit(), mask_ + 1, max_consumers_};
    }
    ring_kind kind() const noexcept
    {
        return spsc_ ? ring_kind::spsc : ring_kind::spmc;
    }
    bool is_spsc() const noexcept
    {
        return spsc_;
    }
    constexpr std::uint32_t unit() const noexcept
    {
        if constexpr (Unit != dynamic)
            return Unit;
        else
            return unit_;
    }
    std::size_t capacity_bytes() const noexcept
    {
        return std::size_t{mask_ + 1} * unit();
    }
    // The largest payload one record can carry: the whole ring minus its header, and at most 2^32 - 1 bytes (the
    // header's size field; rings can exceed 4 GiB, test B3). A record larger than the space left before the end of
    // the buffer waits until the consumers drained enough (design §5.3).
    std::size_t max_message() const noexcept
    {
        const std::size_t whole = capacity_bytes() - DataAlign;
        return whole < 0xFFFF'FFFFu ? whole : std::size_t{0xFFFF'FFFFu};
    }
    // Units a record with a payload of `size` bytes occupies.
    std::uint64_t record_units(const std::size_t size) const noexcept
    {
        return detail::record_units(size, unit(), granule_, DataAlign);
    }
    std::size_t bytes() const noexcept
    {
        return static_cast<std::size_t>(total_bytes_);
    }

    // ---- state ---------------------------------------------------------------------------------------------------
    // Position after the last published record.
    std::uint32_t cursor() const noexcept
    {
        return es::ipc::detail::load(*cursor_, std::memory_order_acquire);
    }
    // Consumers joined and not evicted.
    std::size_t active_consumers() const noexcept
    {
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < max_consumers_; ++i)
            n +=
                detail::state_of(es::ipc::detail::load(gates_[i].own, std::memory_order_acquire)) ==
                detail::gate_active;
        return n;
    }
    bool has_producer() const noexcept
    {
        return es::ipc::detail::load(*producer_pid_, std::memory_order_acquire) != 0;
    }
    // Frees the gates of consumers whose process no longer exists (never this process's own), in any state (joining,
    // active, evicted). SPSC: the queue position (the gate's pos) stays for the next consumer. The compare-and-swap of
    // the ownership word makes this safe against a consumer joining the same gate meanwhile. Returns the count.
    std::size_t reap_dead_consumers() const noexcept
    {
        std::size_t n = 0;
        const auto self = static_cast<std::int32_t>(::getpid());
        for (std::uint32_t i = 0; i < max_consumers_; ++i)
        {
            auto& g = gates_[i];
            std::uint64_t o = es::ipc::detail::load(g.own, std::memory_order_acquire);
            if (o == 0)
                continue;
            // the pid must be the one stored for this very ticket (test B4); a joiner that has not stored it yet,
            // or a stale pid of an earlier owner, is never reaped
            if (es::ipc::detail::load(g.pid_ticket, std::memory_order_acquire) !=
                detail::ticket_of(o))
                continue;
            const std::int32_t p = es::ipc::detail::load(g.pid, std::memory_order_relaxed);
            if (p <= 0 || p == self || !es::ipc::detail::process_gone(p))
                continue;
            n += es::ipc::detail::cas(g.own, o, std::uint64_t{0});
        }
        return n;
    }
    // Evicts the consumer of gate `id` of an SPMC ring: the producer stops waiting for it at once; the consumer sees
    // lapped() at its next read and frees the gate (a gate whose consumer never reads again stays taken until its
    // process exits and is reaped). False for an SPSC ring (a queue never drops messages), a bad id, or a gate that
    // is free or already evicted.
    bool evict(const std::uint32_t id) const noexcept
    {
        if (spsc_ || id >= max_consumers_)
            return false;
        std::uint64_t o = es::ipc::detail::load(gates_[id].own, std::memory_order_acquire);
        const std::uint32_t st = detail::state_of(o);
        return (st == detail::gate_active || st == detail::gate_joining) &&
               es::ipc::detail::cas(gates_[id].own, o,
                                    detail::own_word(detail::ticket_of(o), detail::gate_evicted));
    }

private:
    friend class basic_producer<Unit, Align, DataAlign>;
    friend class basic_consumer<Unit, Align, DataAlign>;

    basic_ring(std::byte* const base, const detail::ring_header& h) noexcept
        : cursor_(reinterpret_cast<std::uint32_t*>(base + detail::cursor_offset)),
          producer_pid_(reinterpret_cast<std::int32_t*>(base + detail::producer_offset)),
          tickets_(reinterpret_cast<std::uint32_t*>(base + detail::ticket_offset)),
          hwm_(reinterpret_cast<std::uint32_t*>(base + detail::hwm_offset)),
          gates_(reinterpret_cast<detail::gate*>(base + detail::gates_offset)),
          data_(base + h.data_offset), mask_(h.units - 1), unit_(h.unit),
          granule_(detail::granule(h.unit, Align)), max_consumers_(h.max_consumers),
          total_bytes_(h.total_bytes), spsc_(h.kind == static_cast<std::uint32_t>(ring_kind::spsc))
    {
    }

    std::byte* at(const std::uint32_t pos) const noexcept
    {
        return data_ + std::size_t{pos & mask_} * unit();
    }
    detail::record_header& header(const std::uint32_t pos) const noexcept
    {
        return *reinterpret_cast<detail::record_header*>(at(pos));
    }
    std::uint32_t units() const noexcept
    {
        return mask_ + 1;
    }
    // Raises the high-water mark to at least `n` gates (seq_cst; joins only, never lowered).
    void raise_hwm(const std::uint32_t n) const noexcept
    {
        std::atomic_ref<std::uint32_t> h(*hwm_);
        std::uint32_t cur = h.load(std::memory_order_seq_cst);
        while (cur < n && !h.compare_exchange_weak(cur, n, std::memory_order_seq_cst))
        {
        }
    }
    // Free units for a producer at `tail`: capacity minus the largest lag of a gating consumer (an SPMC ring with no
    // active consumer runs free; an SPSC ring always waits for its gate's position). A seq_cst read-modify-write of
    // the cursor first orders the producer's last cursor store before the gate loads; it pairs with join()'s seq_cst
    // activation and cursor load (the I-01 store-buffering race of the slot rings). A lag above the capacity (a
    // consumer in the middle of join(), holding an older position for a moment) counts as full.
    std::uint32_t free_units(const std::uint32_t tail) const noexcept
    {
        std::atomic_ref<std::uint32_t>(*cursor_).fetch_add(0, std::memory_order_seq_cst);
        std::uint32_t lag = 0;
        if (spsc_)
            lag = tail - es::ipc::detail::load(gates_[0].pos, std::memory_order_acquire);
        else
        {
            // Only the gates ever used (Q6b): a joiner raises hwm (seq_cst) between its activation and its cursor
            // re-read, so a scan that misses the raise is ordered before that re-read, like a scan that misses the gate.
            const std::uint32_t used = es::ipc::detail::load(*hwm_, std::memory_order_seq_cst);
            const std::uint32_t scan = used < max_consumers_ ? used : max_consumers_;
            for (std::uint32_t i = 0; i < scan; ++i)
            {
                const std::uint64_t o =
                    es::ipc::detail::load(gates_[i].own, std::memory_order_seq_cst);
                if (detail::state_of(o) != detail::gate_active)
                    continue;
                const std::uint32_t d =
                    tail - es::ipc::detail::load(gates_[i].pos, std::memory_order_acquire);
                if (d > lag)
                    lag = d;
            }
        }
        return lag >= units() ? 0 : units() - lag;
    }

    std::uint32_t* cursor_ = nullptr;
    std::int32_t* producer_pid_ = nullptr;
    std::uint32_t* tickets_ = nullptr;
    std::uint32_t* hwm_ = nullptr; // gates ever used (monotonic)
    detail::gate* gates_ = nullptr;
    std::byte* data_ = nullptr;
    std::uint32_t mask_ = 0;
    std::uint32_t unit_ = 0;
    std::uint32_t granule_ = 0;
    std::uint32_t max_consumers_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool spsc_ = false;
};

using ring = basic_ring<>;

} // namespace es::ipc
