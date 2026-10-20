# `include/es/ipc/consumer.hpp`

*(Developed as `include/es/ipc_dynamic/consumer.hpp` and renamed in the switch of 2026-10-02.)*
`message`, and `basic_consumer<Unit, Align, DataAlign>` (alias `consumer` = `basic_consumer<>`): one reader of a
variable-size ring. Move-only; the template arguments must match the ring's. Chapter 11 §11.3–§11.4.

Includes: `common.hpp`, `layout.hpp`, `ring.hpp`, `<sched.h>`, `<unistd.h>`, `<atomic>`, `<cstddef>`,
`<cstdint>`, `<expected>`, `<limits>`, `<memory>`, `<new>`, `<optional>`, `<span>`, `<system_error>`,
`<type_traits>`, `<utility>`.

| name | meaning |
|---|---|
| `message` | `{std::span<const std::byte> data, std::uint32_t type, std::uint32_t position}`: one record in place, valid until it is released |
| `join(ring)` | SPMC: takes a free gate and starts at the cursor. SPSC: takes the queue's one gate and continues at its position. `device_or_resource_busy` when every gate is held by a live process (it reaps dead ones and retries once first) |
| `peek()` | the next message, or `nullopt` (nothing published, or lapped). Valid until `release()` |
| `release()` | hands the peeked message back; `false` when there was none, or when it was overwritten while held (lapped) |
| `wait()` | spins (`cpu_relax`, a `sched_yield()` every `yield_interval` spins) until `peek()` returns a message or the consumer is lapped |
| `poll(f, max)` | calls `f(const message&)` for up to `max` available messages, in place, and releases them with **one** gate update; `f` may return `bool` (`false` stops after that message). Returns the count delivered |
| `poll_as<T>(f, max)` | `f(const T&)` over every message of exactly `sizeof(T)` bytes, a typed view in place (`std::start_lifetime_as` when available); other lengths are released unseen and counted in `mismatched()` |
| `pending_units()`, `lapped()`, `corrupt()`, `mismatched()` | units published and not yet released; the gate was lost (evicted or reaped); records refused as inconsistent; wrong-length messages skipped by `poll_as` |
| `resync()` | drops the backlog and continues at the cursor; `false` when the gate is no longer owned |
| `position()`, `id()`, `ring()`, `leave()` | next position, gate index, the ring; give the gate back (only in the process that joined, and only if still owned — an evicted consumer acknowledges instead) |

Semantics: an **SPMC** consumer sees every record published after it joined and gates the producer until it
releases it. An **SPSC** queue keeps its position without a consumer: `join()` continues exactly where the previous
consumer stopped, and a second concurrent consumer is refused.

## Joining

```cpp
            std::uint64_t joining = detail::own_word(t, detail::gate_joining);
            if (!std::atomic_ref<std::uint64_t>(g.own).compare_exchange_strong(
                    joining, detail::own_word(t, detail::gate_active), std::memory_order_seq_cst))
```
(`consumer.hpp:332-334`.) The SPMC join (`join_spmc`, `consumer.hpp:315-347`) keeps the I-01 protocol of the slot
rings, with the separate position word:

1. take a fresh ticket; CAS a free gate (`own == 0`) to *joining* — the producer ignores it — and store `pid`, then
   `pid_ticket` (release): from here the gate is reapable;
2. store `pos` = a recent cursor, then activate with a `seq_cst` CAS: from here the producer waits for `pos`;
3. raise the high-water mark (`seq_cst`), **re-read the cursor** (`seq_cst`) and store `pos` there (release).

The producer's gate scan either saw the activation (and then a `pos` no later than the start), or it ran before it,
and then the re-read sees every position that scan allowed it to write: the producer cannot be a whole ring ahead of
the start. An eviction during the join makes the activation CAS fail; the joiner then frees the gate (an
acknowledgement) and tries the next one. Tests: "ipc threads: consumers joining and leaving a running producer are
never lapped", "ipc threads: a consumer joining a running producer sees from the cursor on, in order".

The SPSC join (`consumer.hpp:348-363`) CASes gate 0 from free straight to *active* and starts at the gate's `pos`,
which the previous consumer left (test "ipc SPSC: messages wait for a consumer; hand-over keeps the position").

## Progress: a plain store

```cpp
    void publish_progress() noexcept
    {
        if (lapped_ || published_ == next_)
            return;
        es::ipc::detail::store(gate().pos, next_, std::memory_order_release);
        published_ = next_;
    }
```
(`consumer.hpp:416-422`.) `release()` and every `poll()` batch publish progress with **one plain release store** of
the gate's position. No locked instruction means no store-buffer drain right after the application's own stores
(often the reply it has just written into another ring); the CAS that layout 1 used here cost 40–60 ns of half-RTT
on small messages (`docs/ipc/latency-analysis.md`; chapter 11 §11.5). Test: "ipc layout 2: readers publish
progress with a plain store of the gate's position".

Why a plain store is safe (layout 2, [layout page](include-es-ipc-layout.hpp.md)): an evicted gate does not hold
the producer, whatever its `pos` says, and it is not reused before this consumer acknowledges the eviction, which
it does at its next ownership check, **after** its last `pos` store. So a store that lands after an eviction (the
reader was preempted between its check and its store) is harmless.

## Eviction and acknowledgement

```cpp
    bool owned() noexcept
    {
        if (es::ipc::detail::load(gate().own, std::memory_order_acquire) == active_word())
            return true;
        lost();
        return false;
    }
```
(`consumer.hpp:386-392`.) The consumer checks its ownership word before it reads each record, and again after the
payload was read (`intact()`, below). When the word is no longer `{ticket, active}`, `lost()` sets `lapped()` for
good — the consumer never moves the gate again — and `acknowledge()` CASes `{ticket, evicted}` to 0, freeing an
evicted gate (`consumer.hpp:375-384`). For a reaped gate (already 0, or reused) that CAS simply fails. `leave()`
CASes `{ticket, active}` to 0 and acknowledges instead when that fails (`consumer.hpp:112-120`). Tests: "ipc SPMC:
evict — the evicted consumer is lapped and can never move the gate", "ipc SPMC: an evicted gate holds the
producer no longer, whatever its position says", "ipc SPSC: a consumer that lost its gate never stores into it".

## Validation: wrong data never crashes a reader

```cpp
        if (seq == next_ && check == detail::header_check(size, seq, type) &&
            size <= r_.max_message())
        {
            const std::uint64_t units = r_.record_units(size);
            const bool skip_ok = type != type_skip || units == to_end;
            if (units <= to_end && units <= pending && skip_ok)
                return record{size, type, static_cast<std::uint32_t>(units)};
        }
```
(`consumer.hpp:434-441`.) Before a record at position `p` is handed out (`read_record`, `consumer.hpp:425-451`),
its header must satisfy all of:

- `seq == p` and `check == header_check(size, seq, type)`;
- `size ≤ max_message()`;
- the record's units fit before the end of the buffer **and** before the cursor;
- a skip record reaches the end of the buffer exactly.

So a record is never read past the end of the buffer or the cursor. On failure the consumer first re-checks its
ownership: a lost gate means the record was overwritten — a lap, not corruption (finding E). Otherwise it counts
the record in `corrupt()`, skips to the cursor (the next record's position is unknown) and publishes that progress.
`read_record` returns a plain struct with `units == 0` for "refused", not a `std::optional`: the optional made GCC
16 build the value with partial stores and reload it whole, a store-forwarding stall of about 8 ns per message
(finding P1, `consumer.hpp:283-289`). Tests: "ipc: corrupt record headers are counted, skipped to the cursor, never
delivered" (11 forged-header variants), "ipc: a forged header that would cross the end of the buffer is refused".

After the payload was read, `intact(p)` (`consumer.hpp:394-412`) orders the payload reads before a reload of the
record's `seq` and that before a reload of the ownership word (`fence_acquire` twice). A lost gate makes the
delivery a lap (`lapped()`; what `f` saw may be torn); a changed `seq` under a gate the consumer still owns is wrong
data, not a lap: it is counted in `corrupt()` and the gate stays the consumer's to move and to free (finding H).
That check relies on multi-copy-atomic hardware (x86-64, ARMv8), as the slot rings' seqlock does.

## Skip records

`peek()` and `poll()` release a skip record at once and publish that progress immediately, even when no message
follows it — a producer may be waiting for exactly that room (finding B2, `consumer.hpp:137-142`; tests "ipc:
peek() releases a skip record even when no message follows it", "ipc: poll() releases a skip record even when no
message follows it").

Invariants a change must keep:
1. Progress is one plain release store per batch; ownership changes only by CAS.
2. Ownership is checked before each record and after its payload was read; nothing overwritten is delivered as
   intact.
3. Every header field is validated before a span is formed; nothing is read past the buffer's end or the cursor.
4. A lost consumer never stores into its gate again, and acknowledges an eviction only after its last store.
