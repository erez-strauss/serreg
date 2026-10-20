# `include/es/ipc/ring.hpp`

*(Developed as `include/es/ipc_dynamic/ring.hpp` and renamed in the switch of 2026-10-02.)*
`basic_ring<Unit, Align, DataAlign>` (alias `ring` = `basic_ring<>`): a cheap, copyable, non-owning handle to a
variable-size ring in shared or private memory. One layout serves both kinds, `ring_kind::spmc` (broadcast, gated
by the slowest consumer) and `ring_kind::spsc` (a queue with one always-active gate). The memory must outlive every
handle, producer and consumer made from it. Chapter 11 §11.2.

Includes: `common.hpp`, `layout.hpp`, `region.hpp`, `<unistd.h>`, `<atomic>`, `<cstddef>`, `<cstdint>`,
`<cstring>`, `<expected>`, `<system_error>`.

| template parameter | meaning |
|---|---|
| `Unit` | `0` (`dynamic`, default): the unit size comes from the geometry / the ring header; `16`, `32` or `64`: fixed at compile time, and `attach()` refuses a ring of another unit |
| `Align` | record alignment in bytes, a power of two in 16 … 4096 (default 64, one cache line); a layout property, checked by `attach()` |
| `DataAlign` | the payload's offset in a record: 16 (default) or 64 (needs `Align ≥ 64`); checked by `attach()` |

| member | meaning |
|---|---|
| `mem_align` | `max(64, Align)`: the alignment `create()` and `attach()` require of the memory (a region is page-aligned) |
| `valid(g)`, `bytes_for(g)` | constexpr: a geometry this ring type accepts; the bytes it needs, `data_offset + units × unit` (0 if invalid) |
| `for_capacity(bytes, unit = 64, max_consumers = 8)` | the smallest valid geometry with at least `bytes` of records (a power-of-two unit count) |
| `create(mem, bytes, g, kind)` / `create(region, g, kind)` | constructs a ring; `invalid_argument` (bad geometry or kind, memory not aligned to `mem_align`), `no_buffer_space` |
| `attach(mem, bytes, kind)` / `attach(region, kind)` | validates a ring built elsewhere (below) |
| `shape()`, `kind()`, `is_spsc()`, `unit()`, `capacity_bytes()`, `max_message()`, `record_units(size)`, `bytes()` | shape. `max_message()` is the whole ring minus `DataAlign`, and at most 2³² − 1 (the header's `size` field) |
| `cursor()` | the position after the last published record (acquire load) |
| `active_consumers()`, `has_producer()` | gates in state *active*; whether the producer role is taken |
| `reap_dead_consumers()` | frees the gates of consumers whose process no longer exists (below) |
| `evict(id)` | SPMC only: stop waiting for gate `id`'s consumer (below) |

## Memory map

```
0      ring_header      magic "IPCRING2" (stored last), version 2, kind, unit, align, units, max_consumers,
                        data_offset, total_bytes, data_align
64     cursor (u32)     position after the last published record (the producer's single release store)
128    producer (i32)   pid holding the producer role, 0 = none
192    tickets (u32)    ownership ticket counter for consumer gates; +4: hwm (u32), gates ever used
256    gates            max_consumers × {u64 own = ticket << 32 | state, u32 pos, i32 pid, u32 pid_ticket}
data   units × unit     records (data_offset is a multiple of the granule and of 64)
```

Every field on its own cache line; the record header and the gate are described on the
[`layout.hpp`](include-es-ipc-layout.hpp.md) page.

## create and attach

`create()` normalises an SPSC geometry to one gate before it computes the layout, so that `attach()`, which
recomputes the offsets from the header, finds the same ones (finding B1; test "ipc: an SPSC ring created with
the default max_consumers attaches"):

```cpp
        const geometry g =
            k == ring_kind::spsc ? geometry{requested.unit_bytes, requested.units, 1} : requested;
```
(`ring.hpp:99-100`.) It zeroes only the control area (header, cursor, producer, tickets, gates), not the records
(readers never look past the cursor), so a large ring touches only its first lines; it writes the header and stores
the magic **last**, with release (`ring.hpp:107-120`).

`attach()` is the validation of a peer's ring:

```cpp
        detail::ring_header h{};
        std::memcpy(&h, shared, sizeof h);
        if (h.version != detail::ring_version)
            return std::unexpected(std::errc::not_supported);
```
(`ring.hpp:146-149`.) After an acquire load of the magic (0: `resource_unavailable_try_again`, construction not
finished; another value: `invalid_argument`), it copies the shared header into a private one and validates the
copy: the version (`not_supported`), then the kind, `align`, `data_align`, the geometry, one gate for SPSC, the data
offset and the total size recomputed from that geometry (`invalid_argument`), and finally the memory size
(`no_buffer_space`) (`ring.hpp:150-158`). The handle is built from the copy, so a peer that rewrites the shared
header later cannot move this process's view (the slot rings' finding I-09, kept). Test: "ipc: attach validates the
shared header", which forges every field.

## The producer's gate scan

```cpp
    std::uint32_t free_units(const std::uint32_t tail) const noexcept
    {
        std::atomic_ref<std::uint32_t>(*cursor_).fetch_add(0, std::memory_order_seq_cst);
```
(`ring.hpp:311-313`.) The free room for a producer at `tail` is the capacity minus the largest lag of a gating
consumer:

- **SPSC:** always gate 0's `pos`, owned or not — a queue keeps its messages for a later consumer.
- **SPMC:** only gates in state *active*; an SPMC ring with no active consumer runs free. The scan covers only the
  gates ever used, `[0, hwm)` (decision Q6b): readers take the lowest free gate, so a ring created for 16 readers
  and read by 3 scans 3 lines (`ring.hpp:319-333`; test "ipc: 3 readers of a 16-reader ring use gates 0..2 and the
  scan stops there (Q6b)").
- A lag above the capacity — a consumer in the middle of `join()`, holding an older position for a moment — counts
  as full (`ring.hpp:335`).

The scan begins with a `seq_cst` read-modify-write of the cursor (`fetch_add(0)`), which orders the producer's last
cursor store before its gate loads. It pairs with `join()`'s `seq_cst` activation and cursor re-read
([`consumer.hpp`](include-es-ipc-consumer.hpp.md)): the store-buffering (Dekker) race that let the slot rings lap a
consumer that had just joined (their finding I-01) cannot happen. A fence would do on hardware, but GCC rejects
`atomic_thread_fence` under TSan. The scan is on the producer's slow path only: the producer caches the limit and
rescans when it reaches it ([`producer.hpp`](include-es-ipc-producer.hpp.md)). The joiner's `raise_hwm` is a
`seq_cst` CAS loop that never lowers the mark (`ring.hpp:298-305`).

## Eviction and reaping

```cpp
    bool evict(const std::uint32_t id) const noexcept
    {
        if (spsc_ || id >= max_consumers_)
            return false;
```
(`ring.hpp:258-261`.) `evict(id)` CASes the gate's ownership word from *active* or *joining* to *evicted*, keeping
its ticket (`ring.hpp:262-266`). From that moment the producer no longer waits for that consumer. The consumer sees
`lapped()` at its next ownership check and then acknowledges, which frees the gate; a gate whose consumer never reads
again stays taken until its process exits and is reaped. `evict` is refused for SPSC (a queue never drops messages),
for a bad id, and for a free or already evicted gate.

`reap_dead_consumers()` (`ring.hpp:232-253`) frees the gates of consumer processes that no longer exist
(`detail::process_gone` in `common.hpp`, i.e. `kill(pid, 0)` failing with `ESRCH`), in any state, never this
process's own. It acts only
when `pid_ticket` equals the ticket in the ownership word: a joiner that has not stored its pid yet, or the stale pid
of an earlier owner, is never reaped (finding B4; test "ipc processes: a reaper never pairs a stale dead pid with a
new ticket"). The free is a CAS of the whole ownership word, so it is safe against a consumer joining the same gate
meanwhile. For SPSC the queue position (`pos`) stays for the next consumer. The producer reaps while blocked, and
`consumer::join()` reaps once when every gate is taken.

Invariants a change must keep:
1. The magic is the last store of `create()` and the first check of `attach()`; nothing re-reads the geometry from
   shared memory after `attach()`.
2. The `seq_cst` RMW at the start of `free_units` stays, and the scan stays on the slow path.
3. The producer waits only for *active* SPMC gates, and always for the SPSC gate.
