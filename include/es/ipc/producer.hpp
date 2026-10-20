// es/ipc/producer.hpp — basic_producer<Unit, Align, DataAlign>: the single writer of a variable-size ring.
//
//   auto prod = es::ipc::producer::attach(*ring);               // errc::device_or_resource_busy: taken
//   if (auto s = prod->claim(n, my_type)) {                               // a span of exactly n payload bytes, in place
//       std::memcpy(s->data(), src, n);                                   // (or serialize into it: es::ipc::sink)
//       prod->commit(n);                                                  // publish (any length <= n)
//   }
//
// claim() reserves one contiguous record: when the record does not fit before the end of the buffer, a skip record
// first pads the rest of the buffer (published at once), and the record starts at the beginning. A record may be
// as large as the whole ring (max_message()); a large one waits until the consumers have drained enough
// (full_policy::spin) or fails with errc::resource_unavailable_try_again (full_policy::fail). A claim that is not
// committed is abandoned by the next claim(). commit(n, false) defers the cursor store; flush() publishes every
// deferred record with one store (batching).
#pragma once

#include "common.hpp"
#include "layout.hpp"
#include "ring.hpp"

#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

namespace es::ipc
{

// Returned by a publish() fill function to publish nothing.
inline constexpr std::size_t abandon = std::numeric_limits<std::size_t>::max();

template<std::uint32_t Unit = dynamic, std::uint32_t Align = 64, std::uint32_t DataAlign = 16>
class basic_producer
{
public:
    using ring_type = basic_ring<Unit, Align, DataAlign>;

    // Takes the producer role (errc::device_or_resource_busy when a live process holds it; the role of a process
    // that died is taken over). The producer continues at the ring's cursor.
    static std::expected<basic_producer, std::errc> attach(
        const ring_type& r, const full_policy p = full_policy::spin) noexcept
    {
        const auto self = static_cast<std::int32_t>(::getpid());
        std::int32_t expected = 0;
        if (!es::ipc::detail::cas(*r.producer_pid_, expected, self))
        {
            if (expected <= 0 || expected == self || !es::ipc::detail::process_gone(expected) ||
                !es::ipc::detail::cas(*r.producer_pid_, expected, self))
                return std::unexpected(std::errc::device_or_resource_busy);
        }
        return basic_producer(r, p, self);
    }
    basic_producer(basic_producer&& o) noexcept
        : r_(o.r_), attached_(std::exchange(o.attached_, false)), owner_(o.owner_), tail_(o.tail_),
          published_(o.published_), limit_(o.limit_), policy_(o.policy_), refusals_(o.refusals_),
          claimed_(std::exchange(o.claimed_, false)), claim_bytes_(o.claim_bytes_),
          claim_type_(o.claim_type_)
    {
    }
    basic_producer& operator=(basic_producer&& o) noexcept
    {
        if (this != &o)
        {
            detach();
            r_ = o.r_;
            attached_ = std::exchange(o.attached_, false);
            owner_ = o.owner_;
            tail_ = o.tail_;
            published_ = o.published_;
            limit_ = o.limit_;
            policy_ = o.policy_;
            refusals_ = o.refusals_;
            claimed_ = std::exchange(o.claimed_, false);
            claim_bytes_ = o.claim_bytes_;
            claim_type_ = o.claim_type_;
        }
        return *this;
    }
    basic_producer(const basic_producer&) = delete;
    basic_producer& operator=(const basic_producer&) = delete;
    ~basic_producer()
    {
        detach();
    }

    // Publishes deferred records and gives the role back — only in the process that attached (a copy inherited
    // through fork() is inert).
    void detach() noexcept
    {
        if (!std::exchange(attached_, false) || static_cast<std::int32_t>(::getpid()) != owner_)
            return;
        flush();
        std::int32_t expected = owner_;
        es::ipc::detail::cas(*r_.producer_pid_, expected, std::int32_t{0});
    }

    // ---- zero-copy writes ----------------------------------------------------------------------------------------
    // A span of exactly `n` writable payload bytes in the ring, for a message of type `type` (< type_reserved_first).
    // errc::message_size: n > max_message(); errc::invalid_argument: a reserved type; errc::resource_unavailable_try_again:
    // not enough room under full_policy::fail. The span is valid until commit() or the next claim().
    std::expected<std::span<std::byte>, std::errc> claim(
        const std::size_t n, const std::uint32_t type = type_data) noexcept
    {
        return claim_impl(n, type, policy_ == full_policy::spin);
    }
    // claim() that never waits, whatever the policy.
    std::expected<std::span<std::byte>, std::errc> try_claim(
        const std::size_t n, const std::uint32_t type = type_data) noexcept
    {
        return claim_impl(n, type, false);
    }
    // Publishes the claimed record with its final payload length n (<= the claimed size). With publish = false the
    // record is written but the cursor is not moved until flush() or the next publishing commit (batching).
    // False, publishing nothing, without an open claim or when n exceeds the claimed size.
    bool commit(const std::size_t n, const bool publish = true) noexcept
    {
        if (!claimed_ || n > claim_bytes_) [[unlikely]]
            return false;
        claimed_ = false;
        write_header(tail_, static_cast<std::uint32_t>(n), claim_type_);
        tail_ += static_cast<std::uint32_t>(r_.record_units(n));
        if (publish)
            flush();
        return true;
    }
    // Drops the open claim (nothing is published).
    void cancel() noexcept
    {
        claimed_ = false;
    }
    // Publishes every record committed with publish = false.
    void flush() noexcept
    {
        if (published_ != tail_)
            es::ipc::detail::store(*r_.cursor_, published_ = tail_, std::memory_order_release);
    }

    // fill(std::span<std::byte>) writes up to max_bytes and returns the bytes used (or abandon). True when published.
    template<class F>
    bool publish(const std::size_t max_bytes, const std::uint32_t type,
                 F&& fill) noexcept(noexcept(fill(std::span<std::byte>{})))
    {
        const auto s = claim(max_bytes, type);
        if (!s)
            return false;
        const std::size_t used = fill(*s);
        if (used == abandon || used > max_bytes)
        {
            cancel();
            return false;
        }
        return commit(used);
    }
    // Constructs a T (trivially copyable, alignment <= DataAlign) in place as one message of type `type`. True when
    // published.
    template<class T, class... Args>
    bool emplace_typed(const std::uint32_t type, Args&&... args) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= DataAlign,
                      "emplace<T>: T must be trivially copyable with alignment <= DataAlign");
        const auto s = claim(sizeof(T), type);
        if (!s)
            return false;
        ::new (static_cast<void*>(s->data())) T(std::forward<Args>(args)...);
        return commit(sizeof(T));
    }
    // emplace_typed() with type_data.
    template<class T, class... Args> bool emplace(Args&&... args) noexcept
    {
        return emplace_typed<T>(type_data, std::forward<Args>(args)...);
    }
    // Copies n bytes as one message.
    bool write(const std::byte* const p, const std::size_t n,
               const std::uint32_t type = type_data) noexcept
    {
        const auto s = claim(n, type);
        if (!s)
            return false;
        if (n)
            std::memcpy(s->data(), p, n);
        return commit(n);
    }

    // ---- state ---------------------------------------------------------------------------------------------------
    std::uint32_t position() const noexcept
    {
        return tail_;
    }
    std::size_t max_message() const noexcept
    {
        return r_.max_message();
    }
    const ring_type& ring() const noexcept
    {
        return r_;
    }
    void policy(const full_policy p) noexcept
    {
        policy_ = p;
    }

private:
    basic_producer(const ring_type& r, const full_policy p, const std::int32_t owner) noexcept
        : r_(r), attached_(true), owner_(owner), tail_(r.cursor()), published_(tail_),
          limit_(tail_), policy_(p)
    {
    }

    void write_header(const std::uint32_t pos, const std::uint32_t size,
                      const std::uint32_t type) noexcept
    { // atomic (relaxed): an evicted SPMC reader may read the header concurrently; the cursor store publishes it
        auto& h = r_.header(pos);
        es::ipc::detail::store(h.size, size, std::memory_order_relaxed);
        es::ipc::detail::store(h.seq, pos, std::memory_order_relaxed);
        es::ipc::detail::store(h.type, type, std::memory_order_relaxed);
        es::ipc::detail::store(h.check, detail::header_check(size, pos, type),
                               std::memory_order_relaxed);
    }

    std::expected<std::span<std::byte>, std::errc> claim_impl(const std::size_t n,
                                                              const std::uint32_t type,
                                                              const bool spin) noexcept
    {
        claimed_ = false; // a previous claim that was not committed is abandoned
        if (n > r_.max_message()) [[unlikely]]
            return std::unexpected(std::errc::message_size);
        if (type >= type_reserved_first) [[unlikely]]
            return std::unexpected(std::errc::invalid_argument);
        const auto need = static_cast<std::uint32_t>(r_.record_units(n));
        const std::uint32_t to_end = r_.units() - (tail_ & r_.mask_);
        if (need > to_end)
        { // pad the rest of the buffer with a skip record, published at once, and start the record at position 0
            if (!wait_for(to_end, spin))
                return std::unexpected(std::errc::resource_unavailable_try_again);
            write_header(tail_, to_end * r_.unit() - DataAlign, type_skip);
            tail_ += to_end;
            flush();
        }
        if (!wait_for(need, spin))
            return std::unexpected(std::errc::resource_unavailable_try_again);
        claimed_ = true;
        claim_bytes_ = n;
        claim_type_ = type;
        return std::span<std::byte>(r_.at(tail_) + DataAlign, n);
    }

    // True when `need` units after tail_ may be written: no gating consumer still needs them.
    bool wait_for(const std::uint32_t need, const bool spin) noexcept
    {
        if (limit_ - tail_ >= need && limit_ - tail_ <= r_.units()) [[likely]]
            return true;
        if (refresh(need))
            return true;
        if (!spin)
        { // deferred records first: consumers waiting for them are what frees the room (review F)
            if (published_ != tail_)
            {
                flush();
                if (refresh(need))
                    return true;
            }
            // reap now and then, or a dead consumer's gate would keep the ring full forever (review I-07)
            if (++refusals_ % 1024 == 0 && r_.reap_dead_consumers() != 0)
                return refresh(need);
            return false;
        }
        flush(); // a consumer waiting for deferred records must see them, or both sides wait forever
        for (std::uint64_t spins = 1;; ++spins)
        {
            es::ipc::cpu_relax();
            if (refresh(need))
                return true;
            if (spins % es::ipc::yield_interval == 0)
                ::sched_yield();
            if (spins % es::ipc::reap_interval == 0)
                r_.reap_dead_consumers();
        }
    }
    bool refresh(const std::uint32_t need) noexcept
    {
        // Publish deferred records before scanning: a consumer joining now starts at the published cursor, and the
        // limit computed here (from tail_) must not let the producer overwrite records between that cursor and tail_
        // before it sees the new gate.
        flush();
        limit_ = tail_ + r_.free_units(tail_);
        return limit_ - tail_ >= need;
    }

    ring_type r_;
    bool attached_ = false;
    std::int32_t owner_ = 0;
    std::uint32_t tail_ = 0;      // start of the next record (committed records end here)
    std::uint32_t published_ = 0; // the cursor value last stored
    std::uint32_t limit_ = 0; // cached: positions before it are writable without reading the gates
    full_policy policy_ = full_policy::spin;
    std::uint64_t refusals_ = 0;
    bool claimed_ = false;
    std::size_t claim_bytes_ = 0;
    std::uint32_t claim_type_ = type_data;
};

using producer = basic_producer<>;

} // namespace es::ipc
