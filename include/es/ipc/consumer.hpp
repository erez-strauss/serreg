// es/ipc/consumer.hpp — basic_consumer<Unit, Align, DataAlign>: one reader of a variable-size ring.
//
//   auto cons = es::ipc::consumer::join(*ring);       // SPMC: a free gate; SPSC: the queue's one gate
//   cons->poll([](const es::ipc::message& m) {          // in place, zero copy: m.data, m.type
//       handle(m.type, m.data);
//   });
//
// SPMC: a joined consumer sees every record published after it joined and gates the producer until it releases it.
// SPSC: the queue keeps its position without a consumer; join() continues where the previous consumer stopped.
//
// Every record is checked before it is handed out (never crash on wrong data): its header must carry the expected
// position and a matching check value, its size must fit the ring, and it must not cross the end of the buffer or
// the cursor. A record that fails is counted in corrupt(); since the next record's position is then unknown, the
// consumer skips to the cursor. A consumer that was evicted or reaped stops: lapped() is true, it never moves the
// gate again, and an evicted consumer frees its gate as soon as it notices — join() again to continue.
//
// Progress (release(), poll()) is a plain release store of the gate's position: no locked instruction, so no
// store-buffer drain right after the application's own stores (a CAS here cost 40–60 ns of half-RTT on small
// messages; docs/ipc/latency-analysis.md). Ownership lives in a separate word that only compare-and-swap
// changes (layout.hpp, `gate`): an eviction stops the producer waiting for this consumer at once, whatever this
// consumer still stores, and the gate is reused only after this consumer acknowledged the eviction.
#pragma once

#include "common.hpp"
#include "layout.hpp"
#include "ring.hpp"

#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

namespace es::ipc
{

// One record as a consumer sees it: the payload in place (valid until it is released), its type and position.
struct message
{
    std::span<const std::byte> data;
    std::uint32_t type = type_data;
    std::uint32_t position = 0;
};

template<std::uint32_t Unit = dynamic, std::uint32_t Align = 64, std::uint32_t DataAlign = 16>
class basic_consumer
{
public:
    using ring_type = basic_ring<Unit, Align, DataAlign>;

    // SPMC: takes a free gate and starts at the cursor (errc::device_or_resource_busy: every gate is held by a live
    // process). SPSC: takes ownership of the gate and continues at the queue's position (busy: another live
    // consumer holds it).
    static std::expected<basic_consumer, std::errc> join(const ring_type& r) noexcept
    {
        const auto pid = static_cast<std::int32_t>(::getpid());
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (attempt == 1 && r.reap_dead_consumers() == 0)
                break;
            if (auto c = r.spsc_ ? join_spsc(r, pid) : join_spmc(r, pid))
                return c;
        }
        return std::unexpected(std::errc::device_or_resource_busy);
    }
    basic_consumer(basic_consumer&& o) noexcept
        : r_(o.r_), joined_(std::exchange(o.joined_, false)), owner_(o.owner_), id_(o.id_),
          ticket_(o.ticket_), published_(o.published_), next_(o.next_), avail_(o.avail_),
          peeked_units_(std::exchange(o.peeked_units_, 0)), lapped_(o.lapped_),
          corrupt_(o.corrupt_), mismatched_(o.mismatched_)
    {
    }
    basic_consumer& operator=(basic_consumer&& o) noexcept
    {
        if (this != &o)
        {
            leave();
            r_ = o.r_;
            joined_ = std::exchange(o.joined_, false);
            owner_ = o.owner_;
            id_ = o.id_;
            ticket_ = o.ticket_;
            published_ = o.published_;
            next_ = o.next_;
            avail_ = o.avail_;
            peeked_units_ = std::exchange(o.peeked_units_, 0);
            lapped_ = o.lapped_;
            corrupt_ = o.corrupt_;
            mismatched_ = o.mismatched_;
        }
        return *this;
    }
    basic_consumer(const basic_consumer&) = delete;
    basic_consumer& operator=(const basic_consumer&) = delete;
    ~basic_consumer()
    {
        leave();
    }

    // Gives the gate back (SPMC: frees it; SPSC: frees it and the position stays for the next consumer) — only in
    // the process that joined, and only if this consumer still owns it (an evicted one acknowledges instead).
    void leave() noexcept
    {
        if (!std::exchange(joined_, false) || static_cast<std::int32_t>(::getpid()) != owner_)
            return;
        publish_progress();
        std::uint64_t mine = active_word();
        if (!es::ipc::detail::cas(gate().own, mine, std::uint64_t{0}))
            acknowledge();
    }

    // ---- reading -------------------------------------------------------------------------------------------------
    // The next message in place, or nullopt (nothing published yet, or lapped()). Valid until release().
    std::optional<message> peek() noexcept
    {
        for (;;)
        {
            if (lapped_)
                return std::nullopt;
            if (next_ == avail_ && (avail_ = r_.cursor()) == next_)
                return std::nullopt;
            if (!owned())
                return std::nullopt;
            const record rec = read_record();
            if (rec.units == 0)
                continue; // corrupt: skipped to the cursor
            if (rec.type == type_skip)
            { // release the skip at once: a producer may be waiting for exactly that room (test B2)
                next_ += rec.units;
                publish_progress();
                continue;
            }
            peeked_units_ = rec.units;
            return message{std::span<const std::byte>(r_.at(next_) + DataAlign, rec.size), rec.type,
                           next_};
        }
    }
    // Spins (pause; yield every yield_interval) until a message is available or the consumer is lapped.
    std::optional<message> wait() noexcept
    {
        for (std::uint64_t spins = 1;; ++spins)
        {
            if (auto m = peek(); m || lapped_)
                return m;
            es::ipc::cpu_relax();
            if (spins % es::ipc::yield_interval == 0)
                ::sched_yield();
        }
    }
    // Hands the peeked message back to the producer. False when there was none, or when it was overwritten while
    // it was held (the consumer was evicted: lapped()).
    bool release() noexcept
    {
        if (peeked_units_ == 0)
            return false;
        const bool ok = intact(next_);
        next_ += std::exchange(peeked_units_, 0);
        if (!ok && !lapped_)
            next_ = r_.cursor(); // a changed header on our own gate: the next position is unknown
        publish_progress();
        return ok && !lapped_;
    }
    // f(const message&) for up to `max` available messages, in place, released with one gate update. f may return
    // bool: false stops after that message. Returns the number of messages delivered.
    template<class F>
    std::size_t poll(F&& f, const std::size_t max = std::numeric_limits<std::size_t>::max())
    {
        std::size_t done = 0;
        if (lapped_ || peeked_units_ != 0)
            return 0;
        avail_ = r_.cursor();
        while (done < max && next_ != avail_)
        {
            if (!owned())
                break;
            const record rec = read_record();
            if (rec.units == 0)
                continue;
            if (rec.type != type_skip)
            {
                const message m{std::span<const std::byte>(r_.at(next_) + DataAlign, rec.size),
                                rec.type, next_};
                ++done;
                const bool more = call(f, m);
                if (!intact(next_))
                { // lapped while f ran (what f saw may be torn; the gate is not ours), or the header changed under a
                    // gate we still own (wrong data: counted in intact(); the next position is unknown — go to the cursor)
                    if (!lapped_)
                        next_ = avail_;
                    break;
                }
                next_ += rec.units;
                if (!more)
                    break;
            }
            else
                next_ += rec.units;
        }
        publish_progress();
        return done;
    }

    // f(const T&) over each message of exactly sizeof(T) bytes, in place (a typed view of the payload; T trivially
    // copyable with alignment <= DataAlign). Messages of another length are released without calling f and counted in
    // mismatched(). Returns the number of messages released.
    template<class T, class F>
    std::size_t poll_as(F&& f, const std::size_t max = std::numeric_limits<std::size_t>::max())
    {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= DataAlign,
                      "poll_as<T>: T must be trivially copyable with alignment <= DataAlign");
        return poll(
            [&](const message& m) {
                if (m.data.size() != sizeof(T))
                {
                    ++mismatched_;
                    return true;
                }
#if defined(__cpp_lib_start_lifetime_as)
                const T& v = *std::start_lifetime_as<T>(static_cast<const void*>(m.data.data()));
#else
                const T& v = *std::launder(reinterpret_cast<const T*>(m.data.data()));
#endif
                return call_as(f, v);
            },
            max);
    }

    // ---- state ---------------------------------------------------------------------------------------------------
    // Units published and not yet released by this consumer (0 = nothing to read).
    std::uint32_t pending_units() noexcept
    {
        avail_ = r_.cursor();
        return avail_ - next_;
    }
    bool lapped() const noexcept
    {
        return lapped_;
    }
    // Messages poll_as<T>() released without calling f because their length was not sizeof(T).
    std::uint64_t mismatched() const noexcept
    {
        return mismatched_;
    }
    // Records refused because their header was inconsistent (a buggy or hostile peer); each skips to the cursor.
    std::uint64_t corrupt() const noexcept
    {
        return corrupt_;
    }
    // Drops the backlog: continues at the cursor. False when the consumer no longer owns its gate (join again).
    bool resync() noexcept
    {
        if (lapped_ || !owned())
            return false;
        peeked_units_ = 0;
        next_ = avail_ = r_.cursor();
        publish_progress();
        return !lapped_;
    }
    std::uint32_t position() const noexcept
    {
        return next_;
    }
    std::uint32_t id() const noexcept
    {
        return id_;
    }
    const ring_type& ring() const noexcept
    {
        return r_;
    }

private:
    // units == 0: the record was refused (a valid record is at least one unit). A plain struct, not std::optional:
    // the optional made GCC 16 build the value with partial stores and reload it whole, a store-forwarding stall
    // on every record (about 8 ns per message; the benchmark's finding, 2026-10-02).
    struct record
    {
        std::uint32_t size, type, units;
    };

    basic_consumer(const ring_type& r, const std::uint32_t id, const std::uint32_t ticket,
                   const std::uint32_t start, const std::int32_t owner) noexcept
        : r_(r), joined_(true), owner_(owner), id_(id), ticket_(ticket), published_(start),
          next_(start), avail_(start)
    {
    }

    static std::uint32_t new_ticket(const ring_type& r) noexcept
    {
        std::uint32_t t;
        do
            t = std::atomic_ref<std::uint32_t>(*r.tickets_)
                    .fetch_add(1, std::memory_order_relaxed) +
                1;
        while (t == 0);
        return t;
    }
    // SPMC join (the I-01 protocol of the slot rings, with a separate position word):
    //   1. take a free gate as `joining` (the producer ignores it) and store the pid entry (reapable from here on);
    //   2. store pos = a recent cursor, then activate (seq_cst): from here the producer waits for pos;
    //   3. raise the high-water mark (seq_cst), re-read the cursor (seq_cst) and move pos there. The producer's scan
    //      either saw the activation (and then a pos <= start) or ran before it, and then this re-read sees every
    //      position that scan allowed it to write: the producer cannot be a whole ring ahead of `start`.
    // An eviction during the join makes the activation fail: the joiner acknowledges it and tries the next gate.
    static std::expected<basic_consumer, std::errc> join_spmc(const ring_type& r,
                                                              const std::int32_t pid) noexcept
    {
        const std::uint32_t t = new_ticket(r);
        for (std::uint32_t i = 0; i < r.max_consumers_; ++i)
        {
            auto& g = r.gates_[i];
            std::uint64_t o = es::ipc::detail::load(g.own, std::memory_order_acquire);
            if (o != 0 ||
                !es::ipc::detail::cas(g.own, o, detail::own_word(t, detail::gate_joining)))
                continue;
            es::ipc::detail::store(g.pid, pid, std::memory_order_relaxed);
            es::ipc::detail::store(g.pid_ticket, t,
                                   std::memory_order_release); // pid now belongs to ticket t
            es::ipc::detail::store(g.pos,
                                   es::ipc::detail::load(*r.cursor_, std::memory_order_seq_cst),
                                   std::memory_order_relaxed);
            std::uint64_t joining = detail::own_word(t, detail::gate_joining);
            if (!std::atomic_ref<std::uint64_t>(g.own).compare_exchange_strong(
                    joining, detail::own_word(t, detail::gate_active), std::memory_order_seq_cst))
            { // evicted while joining (never active, so no pos store of ours can matter): free it, try the next gate
                std::uint64_t evicted = detail::own_word(t, detail::gate_evicted);
                es::ipc::detail::cas(g.own, evicted, std::uint64_t{0});
                continue;
            }
            r.raise_hwm(i + 1);
            const std::uint32_t start =
                es::ipc::detail::load(*r.cursor_, std::memory_order_seq_cst);
            es::ipc::detail::store(g.pos, start, std::memory_order_release);
            return basic_consumer(r, i, t, start, pid);
        }
        return std::unexpected(std::errc::device_or_resource_busy);
    }
    static std::expected<basic_consumer, std::errc> join_spsc(const ring_type& r,
                                                              const std::int32_t pid) noexcept
    {
        auto& g = r.gates_[0];
        std::uint64_t o = es::ipc::detail::load(g.own, std::memory_order_acquire);
        if (o != 0)
            return std::unexpected(std::errc::device_or_resource_busy);
        const std::uint32_t t = new_ticket(r);
        if (!es::ipc::detail::cas(g.own, o, detail::own_word(t, detail::gate_active)))
            return std::unexpected(std::errc::device_or_resource_busy);
        es::ipc::detail::store(g.pid, pid, std::memory_order_relaxed);
        es::ipc::detail::store(g.pid_ticket, t,
                               std::memory_order_release); // pid now belongs to ticket t
        const std::uint32_t start = es::ipc::detail::load(g.pos, std::memory_order_acquire);
        return basic_consumer(r, 0, t, start, pid);
    }

    detail::gate& gate() const noexcept
    {
        return r_.gates_[id_];
    }
    std::uint64_t active_word() const noexcept
    {
        return detail::own_word(ticket_, detail::gate_active);
    }
    // The gate is no longer ours: stop for good (lapped) and, if it was an eviction, free the gate — after this
    // consumer's last store to pos, so the next owner never sees one of ours.
    void lost() noexcept
    {
        lapped_ = true;
        acknowledge();
    }
    void acknowledge() noexcept
    {
        std::uint64_t evicted = detail::own_word(ticket_, detail::gate_evicted);
        es::ipc::detail::cas(gate().own, evicted, std::uint64_t{0});
    }
    // Does this consumer still own its gate? (An eviction or a reap changes the ownership word.)
    bool owned() noexcept
    {
        if (es::ipc::detail::load(gate().own, std::memory_order_acquire) == active_word())
            return true;
        lost();
        return false;
    }
    // After the payload was read: still owned (so the producer could not have overwritten it)?
    bool intact(const std::uint32_t pos) noexcept
    { // Overwritten under us only if the gate is no longer ours (lapped). A changed header on a gate we still own
        // is wrong data, not a lap: counted as corrupt, and the gate stays ours to move and to free (review H).
        es::ipc::fence_acquire(); // the payload reads before the checks
        const bool seq_ok =
            es::ipc::detail::load(r_.header(pos).seq, std::memory_order_relaxed) == pos;
        es::ipc::fence_acquire(); // the seq load before the ownership load
        if (es::ipc::detail::load(gate().own, std::memory_order_relaxed) != active_word())
        {
            lost();
            return false;
        }
        if (!seq_ok)
        {
            ++corrupt_;
            return false;
        }
        return true;
    }
    // A plain release store of the position (the reads of every released record happen before it). If the gate was
    // lost meanwhile the store is harmless: an evicted gate does not hold the producer and is not reused before
    // this consumer acknowledges, which it does at its next ownership check, after this store.
    void publish_progress() noexcept
    {
        if (lapped_ || published_ == next_)
            return;
        es::ipc::detail::store(gate().pos, next_, std::memory_order_release);
        published_ = next_;
    }
    // The record at next_, validated; on an inconsistent header counts it in corrupt_, skips to the cursor and returns
    // units == 0.
    record read_record() noexcept
    {
        const auto& h = r_.header(next_);
        const std::uint32_t size = es::ipc::detail::load(h.size, std::memory_order_relaxed);
        const std::uint32_t seq = es::ipc::detail::load(h.seq, std::memory_order_relaxed);
        const std::uint32_t type = es::ipc::detail::load(h.type, std::memory_order_relaxed);
        const std::uint32_t check = es::ipc::detail::load(h.check, std::memory_order_relaxed);
        const std::uint32_t to_end = r_.units() - (next_ & r_.mask_);
        const std::uint32_t pending = avail_ - next_;
        if (seq == next_ && check == detail::header_check(size, seq, type) &&
            size <= r_.max_message())
        {
            const std::uint64_t units = r_.record_units(size);
            const bool skip_ok = type != type_skip || units == to_end;
            if (units <= to_end && units <= pending && skip_ok)
                return record{size, type, static_cast<std::uint32_t>(units)};
        }
        es::ipc::fence_acquire(); // the header loads before the ownership load
        if (!owned())
            return record{
                0, 0,
                0}; // evicted and overwritten meanwhile: a lap (lapped()), not corruption (review E)
        ++corrupt_;
        next_ = avail_;
        publish_progress();
        return record{0, 0, 0};
    }
    template<class F, class T> static bool call_as(F& f, const T& v)
    {
        if constexpr (std::is_same_v<std::invoke_result_t<F&, const T&>, bool>)
            return f(v);
        else
        {
            f(v);
            return true;
        }
    }
    template<class F> static bool call(F& f, const message& m)
    {
        if constexpr (std::is_same_v<std::invoke_result_t<F&, const message&>, bool>)
            return f(m);
        else
        {
            f(m);
            return true;
        }
    }

    ring_type r_;
    bool joined_ = false;
    std::int32_t owner_ = 0;
    std::uint32_t id_ = 0;
    std::uint32_t ticket_ = 0;
    std::uint32_t published_ = 0; // the position this consumer last stored in its gate
    std::uint32_t next_ = 0;      // position of the next record to read
    std::uint32_t avail_ = 0;     // cached cursor
    std::uint32_t peeked_units_ =
        0; // units of the message handed out by peek() and not yet released
    bool lapped_ = false;
    std::uint64_t corrupt_ = 0;
    std::uint64_t mismatched_ = 0;
};

using consumer = basic_consumer<>;

} // namespace es::ipc
