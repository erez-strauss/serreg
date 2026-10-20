# `include/es/ipc/producer.hpp`

*(Developed as `include/es/ipc_dynamic/producer.hpp` and renamed in the switch of 2026-10-02.)*
`basic_producer<Unit, Align, DataAlign>` (alias `producer` = `basic_producer<>`): the single writer of a
variable-size ring. Move-only; the template arguments must match the ring's. Chapter 11 §11.3.

Includes: `common.hpp`, `layout.hpp`, `ring.hpp`, `<sched.h>`, `<unistd.h>`, `<atomic>`, `<cstddef>`,
`<cstdint>`, `<cstring>`, `<expected>`, `<limits>`, `<new>`, `<span>`, `<system_error>`, `<type_traits>`,
`<utility>`.

| member | meaning |
|---|---|
| `attach(ring, policy = full_policy::spin)` | takes the producer role: a CAS of the ring's producer pid from 0. A role held by a process that no longer exists is taken over; a live holder gives `device_or_resource_busy`. The producer continues at the ring's cursor |
| `claim(n, type = type_data)` | a `std::span<std::byte>` of exactly `n` writable payload bytes in the ring, valid until `commit()` or the next `claim()`. Errors: `message_size` (`n > max_message()`), `invalid_argument` (a reserved type), `resource_unavailable_try_again` (no room under `full_policy::fail`) |
| `try_claim(n, type)` | `claim()` that never waits, whatever the policy |
| `commit(n, publish = true)` | publishes the claimed record with its final length `n ≤` the claimed size; `false`, publishing nothing, without an open claim or for a larger `n`. `publish = false` defers the cursor store |
| `cancel()` | drops the open claim; a claim that is never committed is also abandoned by the next `claim()` |
| `flush()` | publishes every deferred record with one cursor store |
| `publish(max, type, fill)` | `fill(span)` writes up to `max` bytes and returns the bytes used, or `abandon` to publish nothing |
| `emplace<T>(args…)`, `emplace_typed<T>(type, args…)` | constructs a trivially copyable `T` (`alignof(T) ≤ DataAlign`) in place as one message |
| `write(p, n, type)` | copies `n` bytes as one message |
| `position()`, `max_message()`, `ring()`, `policy(p)`, `detach()` | the next record's position; the ring's limit; the ring; change the full policy; publish deferred records and give the role back (only in the process that attached, so a copy inherited through `fork()` is inert) |

## Claim and commit

```cpp
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
```
(`producer.hpp:125-135`.) The payload is written first, in place, through the span `claim()` returned. `commit`
then writes the 16-byte header with four relaxed atomic stores (`write_header`, `producer.hpp:219-228`: relaxed but
atomic, because an evicted SPMC reader may read a header concurrently), advances the private tail by the record's
units, and publishes with **one release store of the cursor**:

```cpp
    void flush() noexcept
    {
        if (published_ != tail_)
            es::ipc::detail::store(*r_.cursor_, published_ = tail_, std::memory_order_release);
    }
```
(`producer.hpp:142-146`.) That store is the only publication; the header is never the signal (layout page). With
`commit(n, false)` several records are written and published by one `flush()`, or by the next publishing commit
(batching; test "ipc: deferred commits become visible with flush() or the next publishing commit"). A claim may be
committed shorter than claimed (`publish()` does that with the length its fill function returns).

## Skip records at the end of the buffer

```cpp
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
```
(`producer.hpp:239-248`.) A record is always contiguous. When it does not fit before the end of the buffer, the
producer waits only for the units up to the end, writes a skip record whose size makes it end exactly there,
**publishes it at once**, and then waits for the record's own units at position 0. Because the skip does not wait
for the record behind it, any record up to the whole ring (`max_message()`) eventually fits once the consumers
have drained enough. Tests: "ipc: records of max_message() (the whole ring), from a full and an empty start", "ipc:
many wrap-arounds of a tiny ring with mixed sizes up to max_message()".

## Waiting for room

`wait_for(need)` (`producer.hpp:258-288`) is the producer's gating. The fast path compares against a cached
`limit_`: positions before it are writable without reading any shared line. Only when the limit is reached does
`refresh()` call the ring's gate scan (`free_units`, [ring page](include-es-ipc-ring.hpp.md)):

```cpp
    bool refresh(const std::uint32_t need) noexcept
    {
        // Publish deferred records before scanning: a consumer joining now starts at the published cursor, and the
        // limit computed here (from tail_) must not let the producer overwrite records between that cursor and tail_
        // before it sees the new gate.
        flush();
```
(`producer.hpp:289-294`.) Deferred records are published before every scan (review finding B: a joiner starts at
the published cursor, and must never sit behind records the producer could still overwrite; test "ipc: deferred
records are published before the producer rescans, so a joiner never loses them").

- **`full_policy::spin`** (default): flush, then spin with `cpu_relax()`, a `sched_yield()` every
  `yield_interval` (1024) spins and `reap_dead_consumers()` every `reap_interval` (2²²) spins.
- **`full_policy::fail`** and `try_claim`: flush and rescan once (finding F), reap dead consumers on every 1024th
  refusal (the slot rings' I-07, kept), then return `resource_unavailable_try_again`. Test: "ipc: full_policy::fail
  when full, and try_claim under spin".

## Typed messages

```cpp
    template<class T, class... Args>
    bool emplace_typed(const std::uint32_t type, Args&&... args) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= DataAlign,
                      "emplace<T>: T must be trivially copyable with alignment <= DataAlign");
```
(`producer.hpp:166-170`.) API parity with es::ipc_old's `emplace<T>`: the object is constructed with placement
`new` directly in the claimed record, then committed. The reader's counterpart is `consumer::poll_as<T>`. Test:
"ipc: emplace<T> and poll_as<T> — typed messages in place; other lengths counted in mismatched()".

Invariants a change must keep:
1. Nothing on the claim/commit path locks, allocates or makes a system call; `sched_yield()` and the liveness check
   run only while blocked.
2. The cursor store is the only publication, and it comes after the header stores.
3. Deferred records are flushed before every gate scan and before spinning.
