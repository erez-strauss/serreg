# `include/es/ipc/layout.hpp`

*(Developed as `include/es/ipc_dynamic/layout.hpp`, namespace `es::ipc_dynamic`, and renamed in the switch of
2026-10-02.)* The shared-memory layout of the variable-size rings: the geometry, the ring header,
the consumer gates, the 16-byte record header and the unit arithmetic. Everything else in `es::ipc` builds on it;
it holds no code that touches memory. Design: `docs/ipc/ipc-design.md` §4–§5. Chapter 11 §11.2–§11.4.

Includes: `common.hpp`, `<bit>`, `<cstddef>`, `<cstdint>`.

| name | meaning |
|---|---|
| `dynamic` | 0: a template argument meaning "decided at run time" (the unit size comes from the geometry or the ring header) |
| `type_data` (0), `type_reserved_first` (`0xFFFFFF00`), `type_skip` (`0xFFFFFFFF`) | message types: applications use `0 … type_reserved_first − 1`; the rest belong to the library; a skip record pads the end of the buffer and is never delivered |
| `ring_kind` | `spmc = 1` (broadcast: every consumer sees every message, the slowest gates the producer), `spsc = 2` (queue: one gate, always active) |
| `geometry` | `{unit_bytes = 64, units = 1 << 16, max_consumers = 8}`: unit 16, 32 or 64 bytes; `units` a power of two in 2 … 2³¹; 1 … 256 consumers |
| `detail::ring_magic`, `ring_version` | `"IPCRING2"` little-endian (es::ipc_old's is `"IPCRING1"`), layout version 2 |
| `detail::cursor_offset` 64, `producer_offset` 128, `ticket_offset` 192, `hwm_offset` 196, `gates_offset` 256 | the fixed offsets of the control area, one cache line each (`hwm` shares the tickets line) |
| `detail::record_header_bytes` 16, `max_units` 2³¹, `max_consumers_limit` 256 | limits |
| `detail::ring_header` | magic (stored last), version, kind, unit, align, units, max_consumers, data_offset, total_bytes, data_align; `static_assert`ed to fit one line |
| `detail::gate`, `gate_free/joining/active/evicted` | one cache line per consumer: `own`, `pos`, `pid`, `pid_ticket` (below) |
| `detail::record_header` | `{size, seq, type, check}`, four `u32` (below) |
| `own_word(ticket, state)`, `state_of(own)`, `ticket_of(own)` | packing of the ownership word |
| `header_check(size, seq, type)` | the check value of a record header |
| `power_of_two`, `valid_unit`, `valid_align` (power of two in 16 … 4096), `valid_data_align` (16 or 64, and ≤ `align`), `granule` (`max(unit, align)`), `valid_geometry` | validation used by `create` and `attach` |
| `record_units(size, unit, granule, data_align)` | units a record of `size` payload bytes occupies |
| `data_offset(g, align)` | where the records start: after the gates, rounded up to `max(granule, 64)` |

## Positions are 32-bit counts of units

A ring is `units` units of `unit` bytes. A position, a sequence number and a gate position are all `u32` counts of
units that wrap modulo 2³². A ring holds at most 2³¹ units (128 GiB with 64-byte units), so the distance between
any two live positions is below 2³¹ and plain unsigned subtraction gives it, even across the wrap
(`layout.hpp:4-6`). The test "ipc: positions wrap across 2^32 (cursor and gates started near the end)" starts the
cursor and the gates just below 2³².

## The record header

```cpp
struct record_header
{
    std::uint32_t size;
    std::uint32_t seq;
    std::uint32_t type;
    std::uint32_t check;
};
static_assert(sizeof(record_header) == record_header_bytes);
```
(`layout.hpp:106-113`.) A record starts on a `granule` = `max(unit, Align)` boundary with this header:

- `size` — the payload bytes. The record occupies `align_up(data_align + size, granule)` bytes.
- `seq` — the record's own position (its first unit number). Readers check it; it is **not** the publish signal.
  Publication is the producer's release store of the cursor, never the header: bytes past the cursor are stale
  data from an earlier lap, and a stale payload can contain any value, including the "expected" `seq`.
- `type` — the application's message type, or a reserved one (`type_skip`).
- `check` — a mix of the three other fields:

```cpp
constexpr std::uint32_t header_check(const std::uint32_t size, const std::uint32_t seq,
                                     const std::uint32_t type) noexcept
{
    return (size * 0x9E3779B1u) ^ (seq + 0x7F4A7C15u) ^ (type * 0x85EBCA77u) ^ 0xC2B2AE3Du;
}
```
(`layout.hpp:128-132`.) A reader refuses a header whose `check` does not match (`consumer.hpp`, "validation"). It
is not a security measure: it catches a buggy peer, a torn or misaligned read and stale bytes, so that wrong data is
counted, never followed.

## Skip records

When a record does not fit before the end of the buffer, the producer first pads the rest of the buffer with a
record of type `type_skip` whose `size` makes it end exactly at the end of the buffer, and starts the real record at
the next lap's position 0 (`producer.hpp`, `claim_impl`). Readers never deliver a skip; they release it at once,
because a producer may be waiting for exactly that room. A skip that does not reach the end of the buffer exactly is
refused as corrupt. Tests: "ipc: a record one unit too big for the end is preceded by a skip record", "ipc: a record
that exactly fills the end of the buffer needs no skip", "ipc: a skip record that does not reach the end of the
buffer is refused".

## DataAlign: where the payload starts

```cpp
constexpr std::uint64_t record_units(const std::uint64_t size, const std::uint32_t unit,
                                     const std::uint32_t gran,
                                     const std::uint32_t data_align = record_header_bytes) noexcept
{
    return ((data_align + size + gran - 1) & ~std::uint64_t{gran - 1}) >> std::countr_zero(unit);
}
```
(`layout.hpp:163-168`.) The payload is at `record + data_align`. `data_align` is the rings' third template
parameter, `DataAlign`:

- **16** (default): the payload follows the header directly, so the header and up to 48 payload bytes share one
  cache line — the best case for small-message latency. Payloads are 16-byte aligned.
- **64**: the payload starts its own cache line, at the cost of one more line per record; this needs `Align ≥ 64`
  (`valid_data_align`, `layout.hpp:146-149`). Test: "ipc: DataAlign 64 puts every payload on its own cache line".

`unit` and `granule` are powers of two, so the computation is a mask and a shift, no division on the hot path
(finding P2 of `docs/ipc/testing-and-benchmarks.md`). `DataAlign` is stored in the ring header (`data_align`) and
checked by `attach()`.

## Layout 2: the gate is an ownership word and a position word

```cpp
inline constexpr std::uint32_t gate_free = 0, gate_joining = 1, gate_active = 2, gate_evicted = 3;
struct gate
{
    std::uint64_t own;
    std::uint32_t pos;
    std::int32_t pid;
    std::uint32_t pid_ticket;
};
```
(`layout.hpp:97-104`.) Each consumer gate has its own cache line and two words with different writers:

- **`own`** = `ticket << 32 | state` (`own_word`, `layout.hpp:115-118`). It is changed **only by compare-and-swap**:
  join (SPMC `free → joining → active`, SPSC `free → active`), leave and the reaper (`→ free`), evict (`active` or `joining → evicted`) and the
  evicted owner's acknowledgement (`evicted → free`). Every join takes a new ticket from the ring's counter
  (never 0), so a CAS that expects an earlier owner's word fails on a new owner's.
- **`pos`** = the owner's next position. The owner publishes progress with a **plain release store** — no locked
  instruction, so no store-buffer drain on the reader's path (chapter 11 §11.5).
- **`pid`** and **`pid_ticket`** — the owner's process, for reaping dead consumers, and the ticket that pid belongs
  to, so a reaper never pairs a stale pid with a new owner (finding B4).

The producer waits only for **active** gates (SPSC: for gate 0's `pos`, owned or not). An evicted gate therefore
stops holding the producer at once, whatever its owner still stores into `pos`; and it becomes free only when its
owner has seen the eviction and stopped writing `pos` (the acknowledgement), so a late store of an evicted reader
never lands on a new owner. A gate whose owner process died is freed by the reaper in any state
(`layout.hpp:85-96`).

Layout 1 (the first version of these rings) had a single gate word `pos | ticket << 32`, changed by CAS on every
release; that CAS's full barrier was the small-message latency problem (`docs/ipc/latency-analysis.md`). Layout 2
was introduced with `DataAlign`, so `ring_version` was bumped once, to 2 (`layout.hpp:54-56`).

## Geometry validation

```cpp
constexpr bool valid_geometry(const geometry g, const std::uint32_t align) noexcept
{
    return valid_unit(g.unit_bytes) && valid_align(align) && power_of_two(g.units) &&
           g.units <= max_units &&
           std::uint64_t{g.units} * g.unit_bytes >= 2u * granule(g.unit_bytes, align) &&
           g.max_consumers >= 1 && g.max_consumers <= max_consumers_limit;
}
```
(`layout.hpp:154-160`.) A ring must hold at least two granules. The limit of 256 readers (maintainer, 2026-10-02)
bounds the producer's gate scan and the control area (256 + 256 × 64 bytes) and leaves room for an 8-bit gate id.

Invariants a change must keep:
1. Everything in shared memory is a plain integer at a fixed offset, accessed only through `std::atomic_ref`
   (`common.hpp`).
2. Any change of a byte here bumps `detail::ring_version`; `attach()` then refuses rings of the other version.
3. `own` is only ever changed by CAS; `pos` only by its owner (and by a joiner before it activates).
