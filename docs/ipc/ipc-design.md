# es::ipc — variable-size shared-memory rings: design, decisions, action plan

**Status:** this is **es::ipc** (`include/es/ipc/`, namespace `es::ipc`; it was developed as `es::ipc_dynamic`).
The earlier fixed-slot rings of es::ipc (**es::ipc_old**) are out of the build (G-17; the headers wait under
`remove-me/include/es/ipc_old/` until 2.3.0); §7 keeps the comparison that justified the switch.
- Header-only, C++23, Linux; compiles with GCC 16 and clang-p2996.
- Ring layout 2: gates with separate ownership and position words, and readers that publish progress with plain
  stores (§4, §5.5).
- Tested: 55 cases; TSan, ASan/UBSan; 14-core stress and kill harnesses.
- Benchmarked: `testing-and-benchmarks.md`, `latency-analysis.md`.
- Next optimizations: `ipc-optimization-directions.md`.

**Owner of the decisions:** the maintainer (discussion of 2026-10-01/02, summarised in §1). **Companion files:**
`docs/ipc/testing-and-benchmarks.md`, `docs/ipc/latency-analysis.md`, `docs/ipc/ipc-optimization-directions.md`,
`examples/ipc_ring.cpp`, `examples/ipc_log/`, `ce-examples/ce-example5/`.

---

## 1. Why, and how we got here (discussion summary)

1. **The slot rings (then `es::ipc`, now `es::ipc_old`) size every slot to the largest message.**
   - `slot_bytes` is a run-time value, but each message must fit one slot.
   - A feed of 64-byte ticks with an occasional 1 MiB snapshot needs 1 MiB slots, so the ring is almost all waste,
     and large structures are impractical.
   - The two examples planned after an external review (a Compiler Explorer demo with two threads, and a
     two-process deployment) also needed to send the producer's **dictionary in band**, and a whole dictionary (tens
     of KB) cannot fit a 256-byte slot.
2. **Run-time vs compile-time sizes.** Sizes are run-time settings at the library level. The maintainer's pattern
   is: a template parameter of **0 means "set at run time"**, a non-zero value is fixed at compile time (cf.
   `std::span`'s `dynamic_extent`; 0 is safe as the sentinel because a 0-byte unit or a 0-unit ring is never
   valid).
3. **The variable-size ring** (Aeron's `OneToOneRingBuffer` / broadcast buffer family): every message starts with
   its size and type, and a **skip** record fills the end of the buffer when the next message does not fit there.
   The maintainer decided:
   1. implement **`ipc_dynamic` with both SPSC and SPMC**;
   2. positions and sequence numbers count **units of 16, 32 or 64 bytes**, not bytes, so they stay **32-bit**
      while a ring can exceed 4 GB (2³¹ units × 64 B = 128 GiB);
   3. the record header carries **size** (the content length in bytes; the record is rounded up to units),
      **sequence number** (the unit number of the record) and **message type** (application-defined; a few values
      reserved, e.g. skip). The unit size is a run-time or compile-time parameter (0 = run time);
   4. a single message may be **larger than C/2** (e.g. 80 % of the ring), followed by small ones;
   5. **alignment** is a template parameter, **64 by default** (with 64-byte units, trivial);
   6. **zero-copy API**: a span to write into and a commit; a span to read in place;
   7. documented here, implemented in `include/es/ipc_dynamic/`, tested and benchmarked. Once it works, the examples
      and code use it, and it may **replace `es::ipc`** if it has no significant disadvantage (the evaluation
      criteria are in §9).
4. **Examples** (decided earlier in the same discussion):
   - a Compiler Explorer demo with **two threads** (ease of demonstration, not a compiler check);
   - an `examples/` program with **two processes**, where the reader has **partial knowledge** of the types and
     learns the rest **in band**;
   - **the ring is at least twice the registry (dictionary)**, and data travels in records that fit the ring.

---

## 2. Requirements

| ID | Requirement |
|---|---|
| IPD-R1 | SPSC queue and SPMC broadcast ring (at most **256 readers**, maintainer 2026-10-02) over the same memory layout; shared memory between processes (es::ipc regions: tmpfs, hugetlbfs, anonymous) and threads. |
| IPD-R2 | Positions, sequence numbers and gate positions are **u32 counts of units**; unit ∈ {16, 32, 64} bytes; capacity a power of two ≤ 2³¹ units. |
| IPD-R3 | Unit size: template parameter `Unit`, **0 = run time** (from the geometry / the ring header), else fixed; `attach()` refuses a ring of another unit. |
| IPD-R4 | Record alignment: template parameter `Align`, **default 64**; records start on `max(unit, Align)` boundaries; a layout property checked by `attach()`. |
| IPD-R5 | Record header: `size` (payload bytes), `seq` (record position), `type` (application; `≥ 0xFFFFFF00` reserved; `0xFFFFFFFF` = skip), plus a `check` word. |
| IPD-R6 | Skip record when a record does not fit before the end of the buffer; a record may be as large as the whole ring (`max_message()` = capacity − 16). |
| IPD-R7 | Zero copy: `claim(n, type)` → `std::span<std::byte>` in the ring, `commit(n)`; readers get `message{span, type, position}` in place (`peek`/`release`, `poll`). |
| IPD-R8 | No lock, no allocation, no system call on the hot path; one release store per publish (or per batch with deferred commits); one gate update per consumer batch. |
| IPD-R9 | **Never crash on wrong data** (maintainer rule O2): a corrupt or hostile header is detected and counted, never dereferenced out of bounds. |
| IPD-R10 | Robust membership: an evicted or reaped consumer can never move a gate it no longer owns (the slot rings' review findings I-02/I-10); dead producers/consumers are taken over or reaped; fork-safe handles. |
| IPD-R11 | Independent of es::serreg (decision G): no dependency; a claiming-sink adaptor lets `serialize()` write in place. |
| IPD-R12 | C++23, header-only, compiles with GCC and clang (to be checked on the clang lanes), clean under `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`, TSan, ASan/UBSan. |

---

## 3. Files

| File | Content |
|---|---|
| `include/es/ipc/layout.hpp` | `dynamic`, message types, `ring_kind`, `geometry`; detail: magic/version, offsets, `ring_header`, `gate` (`own`, `pos`, `pid`, `pid_ticket`) and its states, `record_header`, `own_word`/`state_of`/`ticket_of`, `header_check`, validity (`valid_data_align`), `record_units`, `data_offset` |
| `include/es/ipc/ring.hpp` | `basic_ring<Unit, Align, DataAlign>` (alias `ring`): `bytes_for`, `for_capacity`, `create`, `attach`, `mem_align`, shape, `cursor`, `active_consumers`, `has_producer`, `reap_dead_consumers`, `evict`; the producer's gate scan `free_units` |
| `include/es/ipc/producer.hpp` | `basic_producer<…>` (alias `producer`): `attach`, `claim`, `try_claim`, `commit`, `cancel`, `flush`, `publish`, `write`, `emplace<T>`, `emplace_typed<T>`, `detach` |
| `include/es/ipc/consumer.hpp` | `message`; `basic_consumer<…>` (alias `consumer`): `join`, `peek`, `release`, `poll`, `poll_as<T>`, `wait`, `pending_units`, `lapped`, `corrupt`, `mismatched`, `resync`, `leave` |
| `include/es/ipc/sink.hpp` | `basic_sink<…>` (alias `sink`): a producer as a claiming sink for `es::serreg::serialize()` |
| `include/es/ipc/ipc.hpp` | umbrella (with `region.hpp`, `common.hpp`) |

**Build:**
- CMake target `es::ipc` (INTERFACE), exported in the `ipc` component; `es-ipc.pc` covers it.
- `tests/headers` compiles every header alone, as C++23 without reflection flags.
- Tests: `serreg_ipc_tests` (`tests/unit/test_ipc.cpp`; the region and huge-page cases included).
- Benchmarks: the reference benchmarks `serreg_hrtt_*`; the old-vs-new comparison (`ipc_compare_bench.cpp`, now
  under `remove-me/tests/bench/`) produced the numbers of §9.

---

## 4. Memory layout

Layout 2 (2026-10-02; `ring_version` 2; layout 1 had one gate word `pos | ticket << 32` changed by CAS):

```
offset   size        field
0        64          ring_header: u64 magic "IPCRING2" (stored last), u32 version (2), u32 kind (1 spmc, 2 spsc),
                     u32 unit, u32 align, u32 units, u32 max_consumers, u64 data_offset, u64 total_bytes,
                     u32 data_align (16 or 64)
64       64          cursor: u32 — position after the last published record (the producer's release store)
128      64          producer: i32 pid holding the producer role (0 = none)
192      64          tickets: u32 counter handing out consumer ownership tickets; +4: u32 hwm (gates ever used)
256      64 × M      gates[M]: { u64 own = ticket << 32 | state ; u32 pos ; i32 pid ; u32 pid_ticket }
data     units×unit  records; data_offset = align_up(256 + 64·M, max(64, granule))
```

Each gate is one cache line (`struct alignas(64) gate`; an earlier version declared a 24-byte struct, so the
gates were packed three per line while the offsets assumed 64). Gate states: `free` (own = 0), `joining`, `active`,
`evicted`.
- `own` is changed only by compare-and-swap.
- `pos` is written by its owner with plain release stores, and by a joiner before it activates.

**Record** (at a granule boundary; granule = max(unit, Align)):

```
+0   u32 size    payload bytes
+4   u32 seq     the record's position (unit number) — verification only
+8   u32 type    application type; 0xFFFFFF00.. reserved; 0xFFFFFFFF = skip (padding to the end of the buffer)
+12  u32 check   header_check(size, seq, type): a mix of the three fields
+16  (DataAlign 64: padding to +64)
+DataAlign  payload  size bytes
     ...         padding to record_units(size) × unit = align_up(DataAlign + size, granule)
```

`index(pos) = pos & (units − 1)`, `address = data + index × unit`.

**Payload alignment:** the payload is at record + `DataAlign`:
- **16 (default, Q10):** the header and up to 48 payload bytes share a cache line.
- **64:** the payload starts its own line, at the cost of one more line per record; this needs `Align` ≥ 64.

Records themselves are granule-aligned, in memory aligned to `mem_align` = max(64, Align).

---

## 5. Protocols

### 5.1 Positions: units, 32 bits, wrap

- A position is a `u32` count of units, wrapping modulo 2³².
- A ring has at most 2³¹ units. The distance between any two live positions (cursor, gates, the producer's tail) is
  ≤ capacity ≤ 2³¹, so `a − b` in unsigned 32-bit arithmetic is that distance even across the wrap.
- 64-byte units give rings up to 128 GiB with 32-bit indexes.
- **Why units and not bytes:** the same 32 bits address 16–64 × more memory, and a 32-bit position fits into a
  64-bit gate word together with an ownership ticket (§5.5).

### 5.2 Publishing and reading

**Producer** (single writer; its role is a pid CAS, as in es::ipc):
- `claim(n, type)`: `R = record_units(n)`, `E` = units left to the end of the buffer.
  - If `R > E`: wait until `E` units are free, write a **skip** record of `E` units, publish it at once, move to
    position 0.
  - Then wait until `R` units are free and return the span `[record + 16, + n)`.
- `commit(n')` (n' ≤ n): write the header with relaxed atomic stores (`size`, `seq`, `type`, `check`), advance the
  tail by `record_units(n')`, then **one release store of the cursor**.
- `commit(n', false)` defers the cursor store; `flush()` publishes all deferred records with one store. A producer
  about to spin flushes first, so a consumer is never left waiting for records it cannot see.

**Consumer:**
- acquire-load the cursor;
- for each record below it: read the header (relaxed atomic loads, ordered by the acquire), validate it (§5.7),
  skip skip-records, deliver the payload in place, advance by `record_units(size)`;
- at the end of a batch, publish progress with **one CAS of the gate word** (§5.5).

**Publication is the cursor, never the header.** Bytes past the cursor are stale data from an earlier lap: a
payload there can contain any value, including the "expected" `seq`. So `seq` and `check` are verification only.

### 5.3 Large messages and the skip record

- **The skip is published as soon as its own `E` units are free**, independently of the record that follows. Then
  the record waits for `R` units at position 0.
- Consequently any record up to the **whole ring** eventually fits, once the consumers have drained. The
  maintainer's ~80 % is a sizing guideline, not a limit.
- The price of near-capacity messages is waiting: an 80 % message needs 80 % of the ring free, plus possibly the
  skip before it.
- **Sizing rule** for applications (the examples): `capacity ≥ 2 × max(largest message, dictionary)`, so a large
  record never waits for more than one ring's worth of drained data. `ring::for_capacity(bytes)` rounds up to a
  power of two.

### 5.4 SPMC gating and joining (the I-01 protocol, kept)

- **Producer's free space:** `free = units − max over active gates (tail − gate.pos)`, scanning only gates
  `[0, hwm)` (Q6b). An SPMC ring with no active consumer runs free. An SPSC ring always waits for gate 0's `pos`,
  owned or not.
- The scan begins with a **seq_cst read-modify-write of the cursor** (`fetch_add(0)`), ordering the producer's last
  cursor store before its gate loads.
- **Joining (layout 2):**
  1. CAS a free gate to `joining` (the producer ignores it);
  2. store `pid`, then `pid_ticket` (release);
  3. store `pos` = a recent cursor;
  4. CAS to `active` (seq_cst);
  5. raise `hwm` (seq_cst);
  6. **re-read the cursor** (seq_cst) and store `pos` there (release).

  If an eviction hits during the join, the activation CAS fails: the joiner frees the gate and tries the next one.
- This is the store-buffering (Dekker) pairing that fixed the slot rings' review finding I-01. The producer's scan
  either saw the activation (and then a `pos` no later than `start`), or it ran before it, and then the re-read sees
  every position that scan allowed. The producer cannot be a whole ring ahead of `start`.
- During the short window where `pos` holds the older snapshot, a lag ≥ capacity counts as "full", so the producer
  waits a moment rather than misreading the distance.

### 5.5 Ownership and progress (layout 2)

Tickets come from a per-ring counter (skipping 0).

| Action | Operation |
|---|---|
| progress (`release`, `poll`) | plain release store of `pos` — **no locked instruction** (§5.11, `latency-analysis.md`) |
| leave | `CAS(own: {t, active} → 0)`; SPSC: `pos` stays for the next consumer |
| evict (SPMC) | `CAS(own: {t, active or joining} → {t, evicted})` |
| acknowledge (the evicted consumer, at its next ownership check, or in `leave`) | `CAS(own: {t, evicted} → 0)` |
| reap dead | load `own`; require `pid_ticket == ticket(own)`; `process_gone(pid)` → `CAS(own → 0)`, in any state |

**Why the plain store is safe:**
- **Eviction:** an evicted gate stops holding the producer at once, whatever its owner still stores in `pos`.
- **Reuse:** a gate is reused only after its evicted owner has acknowledged, and the owner acknowledges after its
  last `pos` store. So a late store (a preemption between the owner's check and its store) never lands on a new
  owner.
- **Readers:** a reader checks `own` before each record and after reading it (`intact()`). A reader that lost its
  gate reports `lapped()` and never delivers an overwritten record as intact.

Layout 1 needed a CAS per release for this guarantee. Its full barrier cost 40–60 ns of half-RTT on small messages,
and layout 2 removes it.

**Known limits:**
- **A joiner that dies before storing its pid** leaves a `joining` gate that the producer ignores; only `evict()`
  frees it.
- **An evicted consumer that is alive but never reads again** keeps its gate (not reusable) until its process exits
  and is reaped.
- **Liveness by `kill(pid, 0)`** keeps the PID-reuse and PID-namespace limits of es::ipc_old (I-03): a live process
  reaped by mistake (another pid namespace) can store into a reused gate. See Q3.
- **One PID namespace** (2.2.6, review B5). Liveness is `kill(pid, 0)` (`process_gone`: `kill(pid, 0) != 0 &&
  errno == ESRCH`), and `getpid()` as well as `kill()` are interpreted in the caller's PID namespace. Every process
  that maps one ring — producer, consumers, reapers — must therefore live in the same PID namespace. Across PID
  namespaces that share a mapping (two containers with one `/dev/shm`), a dead owner is not reaped (its pid is
  unknown, or known and alive, in the checker's namespace) and a live one may be mistaken for dead and have its
  producer role or gate taken over. Inside one namespace the pid-reuse race fails closed (`busy`). The planned fix
  is pid + process start time (`/proc/<pid>/stat` field 22) beside the pid in the producer word and the gates,
  compared together — a `ring_version` bump, so 2.3.0 (the robust-mutex alternative replaces the CAS protocol and
  is a design note first). The process test `ipc processes: a SIGKILLed producer replaced by the next attach()`
  covers the in-namespace case.

### 5.6 SPSC

- One gate, always gating, even without an owner. Messages wait for a consumer.
- `join()` takes ownership of the position the last consumer left. A second concurrent `join()` is `busy`.
- `evict()` is refused: a queue never drops data.

### 5.7 Validation (never crash)

Before delivering a record at position `p`, the consumer requires:
- `seq == p`;
- `check == header_check(size, seq, type)`;
- `size ≤ max_message()`;
- `record_units(size) ≤ units to the end of the buffer`, and `≤ cursor − p`;
- a skip record must reach the end of the buffer exactly.

On failure it counts `corrupt()`, skips to the cursor (the next record's position is unknown) and publishes that
progress. A record is never read past the end of the buffer or the cursor.

Record-header integrity is a poison check (the `check` mix plus the bounds above), not a sequence lock: four
relaxed loads can observe a header torn by an overwrite, and a tear that still passes the mix and the bounds can
hand wrong bytes to the callback, which the second `seq` check after delivery (§5.8, `intact()` / `lapped()`)
catches in most cases but not all (2.2.6, review B9). The TSan lane compiles the fences out and therefore does not
exercise the overwrite path: a green TSan run says nothing about torn headers; the overwrite path is covered by
the lapped-reader tests and the kill harnesses under Debug and ASan/UBSan.

### 5.8 Lapped readers

Only an evicted or reaped SPMC consumer can be overwritten; the gating protocol protects every active one.
- **Before delivery:** the consumer checks that its gate word is still its own.
- **After delivery** (`fence_acquire`, then the gate word and the record's `seq`): if either changed, `lapped()`
  becomes true, and what `f` saw may be torn. The slot rings' seqlock gives the same guarantee.

The ticket check is the definitive one. The producer can only reach a consumer's records after that consumer's
gate was freed.

### 5.9 Processes, crashes, fork

- **Producer:** the role is a pid CAS. A dead holder is taken over, and the new producer continues at the cursor;
  an unpublished claim of the dead one is simply never published. Copies made by `fork()` are inert, as in es::ipc
  (detach/leave only in the process that attached).
- **Consumers:** see §5.5.

---

### 5.10 Changes after review and testing (2026-10-02; `testing-and-benchmarks.md` §3)

- **Deferred records before a scan.** `refresh()` publishes deferred records before scanning the gates. A reader
  joining at the published cursor must never sit behind records the producer could still overwrite (review B).
  `full_policy::fail` flushes, then rescans, before refusing (F).
- **`pid_ticket`.** A gate stores which ticket its pid belongs to; a reaper acts only when it matches the word's
  ticket (B4). The owner re-asserts the pair after a progress CAS if a late joiner overwrote it (G).
- **`intact()`.** A changed header under a gate the reader still owns is corruption, not a lap. The reader counts
  it, skips to the cursor, and can still free its gate (H). After an eviction, a garbage header is a lap, not
  corruption (E).
- **SPSC geometry.** `create()` lays out an SPSC ring for one gate, whatever `max_consumers` says (B1).
- **Skip records** are released at once by `peek()` as by `poll()` (B2).
- **`max_message()`** is at most 2³² − 1 (the header's `size` field), even in rings above 4 GiB (B3).
- **Performance.** `read_record()` returns a plain struct (P1: a `std::optional` cost a store-forwarding stall per
  record); `record_units` uses masks and shifts (P2); the gate scan stops at the high-water mark (Q6b).
- **Fork.** Copies made by `fork()` are inert in `detach()`/`leave()` only. A child must not *use* a parent's
  producer or consumer handle (two writers, or a reader moving the parent's gate); attach or join again (review I).
- **Misaligned cursor.** A cursor that wrong data left off the granule grid cannot crash a reader; every later
  record reads as corrupt and the reader skips to the cursor. Rounding or refusing such a cursor in `attach()` is
  open (review J).
- **Memory model.** The after-delivery overwrite check relies on multi-copy-atomic hardware (x86-64, ARMv8), as the
  slot rings' seqlock does; TSan cannot check it (review K).

### 5.11 Progress publication and alignment (2026-10-02, `latency-analysis.md`)

- **History:**
  1. The first version published progress with a CAS on the gate word. The CAS's full barrier waits for the
     application's own stores, often the reply it just sent, and cost 40–60 ns of half-RTT.
  2. An intermediate step offered a run-time `progress_mode` (a checked plain store for SPSC, the CAS for SPMC).
  3. **Layout 2** (§4, §5.5) makes the plain store safe for both kinds, so `progress_mode` was removed: there is
     nothing left to choose.
- **Memory alignment.** `create()`/`attach()` require memory aligned to `basic_ring::mem_align` = max(64, Align).
  Before, only 64 was checked, so `Align` > 64 in a caller's buffer gave misaligned records.
- **Payload alignment:** `DataAlign` (Q10): 16 by default, or 64.

## 6. API

```cpp
#include <es/ipc/ipc.hpp>
namespace ipc = es::ipc;

ipc::geometry g = ipc::ring::for_capacity(64 << 20 /*bytes*/, 64 /*unit*/, 4 /*max consumers*/);
auto mem  = ipc::region::create("feed", ipc::ring::bytes_for(g));            // or region::anonymous(...)
auto ring = ipc::ring::create(*mem, g, ipc::ring_kind::spmc);                 // other side: ipc::ring::attach(*region, kind)

auto prod = ipc::producer::attach(*ring);                                      // full_policy::spin by default
if (auto s = prod->claim(n, /*type*/ 7)) { fill(*s); prod->commit(used); }    // zero copy
prod->write(bytes, n, 7);                                                      // copy convenience
prod->emplace<quote>(quote{...});                                              // a trivially copyable T in place

auto cons = ipc::consumer::join(*ring);
cons->poll([](const ipc::message& m) { handle(m.type, m.data); });           // in place, batch-released
cons->poll_as<quote>([](const quote& q) { ... });                              // typed view; other lengths: mismatched()
if (auto m = cons->peek()) { handle(m->type, m->data); cons->release(); }

ipc::sink out{*prod, /*type*/ 2};                                              // es::serreg claiming sink
es::serreg::serialize(out, "fill"_lit, fill);                                  // frame built in the record

using fixed = ipc::basic_ring<64, 64>;                                         // compile-time unit (attach checks it)
using wide  = ipc::basic_ring<64, 64, 64>;                                     // payloads on their own cache line
```

**Errors:**
- `std::errc`, the same vocabulary as es::ipc: `message_size` (n > `max_message()`), `invalid_argument` (a reserved
  type, a bad geometry, a mismatched attach), `resource_unavailable_try_again` (full under `full_policy::fail`, or
  a ring not yet constructed), `device_or_resource_busy` (role or gate taken).
- `commit` returns `bool`.

---

## 7. Compared with the slot rings (es::ipc_old)

| | es::ipc_old (slots) | es::ipc (units) |
|---|---|---|
| Message size | ≤ slot_bytes (every slot as large as the largest message) | ≤ the whole ring; mixed sizes share it |
| Memory for 95 % 64 B + rare 256 KiB | slots × 256 KiB | ≈ the traffic in flight + 2 × the largest |
| Positions | u64 message counts | u32 unit counts (rings to 128 GiB) |
| Per-message overhead | 16-byte slot head; the slot rounded to 64 | 16-byte record header; the record rounded to the granule |
| Consumer progress | release store per batch | CAS of the gate word per batch (ownership-checked) |
| Evicted consumer | can still write its gate (I-02, fixed only by a layout change) | cannot (word CAS) |
| Lap detection | per-slot seqlock stamp | gate ownership + header seq |
| Random access by sequence | yes (`seq & mask`) | no (records are walked) |
| Corrupt peer data | `len > slot` → `corrupt()` | full header validation → `corrupt()`, skip to cursor |

---

## 8. Testing (see `testing-and-benchmarks.md`)

- **Unit and functional** (`tests/unit/test_ipc.cpp`, `serreg_ipc_tests`; `test_ipc_dynamic.cpp` before the switch):
  - geometry and attach validation;
  - every boundary of the skip logic, records of `max_message()`, wrap-arounds;
  - types, policies, deferred commits;
  - SPSC persistence and hand-over; SPMC broadcast, join while running, evict, busy;
  - corrupt headers;
  - threads and processes (fork) with every byte verified, for units 16/32/64;
  - reaping, producer take-over;
  - `sink` + `es::serreg::serialize`.
- **Sanitizers:** the suite under TSan and ASan/UBSan.
- **Adversarial stress** on 14 cores: join/leave/evict churn, near-wrap positions (cursor and gates started near
  2³²), tiny rings for constant wrapping.
- **Header checks:** each header compiles alone (`tests/headers`); include rules (`headers_include_rules`).

## 9. Benchmarks, and the criteria for replacing the slot rings

`tests/bench/ipc_compare_bench.cpp` (`serreg_ipc_compare_bench`, `ipc_dynamic_bench` before the switch; smoke ctest `ipc_compare_bench_smoke`):
- half-RTT ping-pong, slot rings (es::ipc_old) vs variable-size rings (es::ipc), SPSC and SPMC, 32 and 256 B, threads and processes;
- mixed-size and small-only throughput;
- the ring memory each needs.

**Replace es::ipc only if, on the reference box:**
1. p50/p99 half-RTT for small messages within an agreed margin (proposed: 10 %) of the slot rings;
2. throughput at least equal for small messages and clearly better for mixed sizes;
3. memory clearly lower for mixed sizes;
4. every test green under TSan, ASan/UBSan and the 14-core stress;
5. the maintainer agrees that random access by sequence number (slots only) is not needed.

Sandbox numbers (`testing-and-benchmarks.md`) are indicative; the decision uses `scripts/bench-*` on the reference
box.

### 9.1 Status on 2026-10-02, before layout 2 and the switch (sandbox, hyperthreading off; "es::ipc" = the slot rings, "ipc_dynamic" = the variable-size rings)

| # | Criterion | Status | Numbers |
|---|---|---|---|
| 1 | small-message latency within 10 % | **not met** | 32 B p50: es::ipc 70–78 ns, ipc_dynamic 89–99 ns (+20–30 %); p99 118–126 vs 158–166 ns (+30 %). 256 B p50: +15–30 ns |
| 2 | throughput | **met in the sandbox** | mixed sizes 22–32 vs 7–11 Mmsg/s (about 3×); 64 B only 16–28 vs 10–12.5 (es::ipc was bimodal once: 70–74) |
| 3 | memory | **met** | mixed sizes: 4 MiB vs 64 MiB for a comparable rate |
| 4 | tests green | **met in the sandbox** | 51/51 under Debug, TSan, ASan/UBSan; stress and kill harnesses clean; reference box pending |
| 5 | random access by sequence number not needed | **decided: not needed** (maintainer, 2026-10-02) | |

**The price of switching** (first estimate, before layout 2): about 15–25 ns (20–30 %) more p50 latency and 40 ns
(30 %) more p99 latency for small messages, in exchange for about 3× the throughput on mixed sizes at 1/16 of the
memory. SPMC readers still paid a CAS per release.

**Work the switch itself needs** (besides the move to `include/es/ipc_old/`):
- **The reference benchmarks** `tests/bench/hrtt_*` and `scripts/bench-hrtt-*.sh` are built on es::ipc (slot geometry,
  huge pages, `available()`). They must be ported, or kept on ipc_old, which would make them stop measuring the
  library in use.
- **API parity, or documented removal:**
  - `producer` as a `claiming_sink` directly (today: `basic_sink`);
  - `emplace<T>`;
  - `consumer::poll_as<T>`;
  - `available()`.
- **Code ports:** `examples/ipc_ring.cpp`; `tests/unit/test_ipc.cpp` moves with ipc_old; comments in
  `serreg/router.hpp` and `serreg/reflect.hpp`.
- The CMake targets `es::ipc` / `es::ipc_dynamic`, the install layout, `serregIpcTargets`, `ES_SERREG_IPC_ONLY`, and the
  pkg-config file `es-ipc.pc`.

---

### 9.2 The switch (2026-10-02)

**Decision (maintainer):**
- The maintainer has no `perf` on the reference box: do the optimizations possible without it, write
  `ipc-optimization-directions.md`, and switch. The IPC is not the package's main delivery.
- Random access by sequence number is not needed (criterion 5).

**Done before the switch:**
- **Layout 2:** SPMC without the CAS.
- **`DataAlign`.**
- **`emplace<T>` / `poll_as<T>`** (API parity with es::ipc_old).

**The measured price** (project bench `serreg_ipc_compare_bench --cpus 4,6 --repeat 3`, hyperthreading off, two
invocations, sandbox):

| | es::ipc_old | es::ipc (layout 2) |
|---|---|---|
| 32 B half-RTT p50, thread / process | 65–81 / 71–75 ns | 90–102 / 87–97 ns (SPMC = SPSC) |
| 32 B half-RTT p99 | 112–150 ns | 152–167 ns |
| 256 B half-RTT p50 | 103–121 ns | 121–151 ns |
| mixed sizes throughput | 9–11 Mmsg/s (64 MiB of slots) | 23–27 Mmsg/s (4 MiB) |
| 64 B only throughput | 10–11, or 73–80 in its fast mode | 21–27 Mmsg/s |

In the ping-pong microbenchmark (`latency-analysis.md`) es::ipc is at parity: 81–118 ns vs es::ipc_old's 80–114 ns.
In the project bench it is 15–25 ns behind.

**Also not resolved:** es::ipc_old's bimodal 64-byte throughput (10 or 75 Mmsg/s) against es::ipc's steady
21–27 Mmsg/s. Both are the subject of `ipc-optimization-directions.md`.

## 10. Action plan

| # | Step | Status |
|---|---|---|
| 1 | Requirements and design (this file) | done |
| 2 | `layout`, `ring`, `producer`, `consumer`, `sink` headers | done; smoke test 200k msgs × units 16/32/64, SPSC + 3-consumer SPMC, ~80 % messages, clean under TSan |
| 3 | CMake target (`es::ipc_dynamic`, since the switch `es::ipc`), install, `tests/headers`, include rules | done |
| 4 | Full test suite (unit, threads, processes, corruption, sanitizers) | **done**: 48 cases, green under Debug, TSan, ASan/UBSan; whole tree 297/297 |
| 5 | Adversarial review with hardware stress; fixes | **done**: findings B1–B4, A–K, P1–P2 fixed or documented; stress clean (`testing-and-benchmarks.md` §3) |
| 6 | Benchmark vs slot rings (sandbox) | **done** (indicative): mixed sizes ~2× faster with 1/16 memory; 32 B latency +20–40 ns (`testing-and-benchmarks.md` §4) |
| 7 | `examples/ipc_log/` (two processes, in-band dictionary, partial knowledge) and `ce-examples/ce-example5/` (threads) | **done**: ctest `example_ipc_log` (316 frames, 102 formats, byte-identical); ce-example5 ok on gcc and clang-libcxx |
| 8 | clang lanes (Compiler Explorer): es::ipc and ipc_dynamic have never been compiled with clang | **done**: both compile and run under clang-p2996 + libc++ (ce-example5; after the switch https://godbolt.org/z/354a6K4h6) |
| 9 | Small-message latency | **done as far as possible without `perf`** (`latency-analysis.md`): the cause was the CAS's barrier. Layout 2 removes the CAS for SPSC and SPMC (32 B half-RTT in the project bench: 146–156 → 87–106 ns; es::ipc_old 65–81). Further directions: `ipc-optimization-directions.md` |
| 10 | **Done 2026-10-02 (maintainer: the IPC is not the package's main delivery; switch at the measured price, §9.2):** slot rings → `include/es/ipc_old/` (`es::ipc_old`, unused, tests kept), ipc_dynamic → `include/es/ipc/` (`es::ipc`); `tests/bench/hrtt_*`, `examples/ipc_ring.cpp`, `examples/ipc_log`, ce-example5, `run-all-tests.sh`, book chapter 11 ported | done |
| 11 | `ipc_logger`: single/multi-writer, multi-reader, endless file-backed log (Q8) | later |

## 11. Open questions

- **Q1 — names and place: decided (maintainer, 2026-10-02).** Once everything works with ipc_dynamic (§9 criteria
  met), the current slot rings move to an **`ipc_old`** directory and are no longer used, and **ipc_dynamic is renamed
  `ipc`**. Mechanically:
  - `include/es/ipc/` → `include/es/ipc_old/` (namespace `es::ipc_old`);
  - `include/es/ipc_dynamic/` takes the `ipc` name (namespace `es::ipc`);
  - the users of the slot rings (`tests/bench/hrtt_*`, `examples/ipc_ring`, `tests/unit/test_ipc.cpp`, book
    chapter 11, `docs/`) are ported or moved with them;
  - the CMake target `es::ipc` points at the new library, and `es::ipc_old` stays available for one release.

  The shared pieces ipc_dynamic uses from es::ipc today (`region.hpp`, `common.hpp`) move to the new `ipc` and are no
  longer borrowed from the old one.
- **Q2 — payload alignment:** records are 64-aligned but payloads are 16-aligned (after the 16-byte header). Should
  an `Align`-aligned payload be available (header padded to `Align`, i.e. 64 bytes per record overhead), or a
  per-ring option?
- **Q3 — liveness:** `kill(pid, 0)` as in es::ipc, or OFD locks per role (the slot rings' planned I-03 fix), which
  also solves PID reuse and PID namespaces? It is the same change for both libraries.
- **Q4 — messages larger than the ring:** refuse (today: `errc::message_size`), fragment (begin/middle/end types,
  consumer reassembly: loses zero copy), or an out-of-band blob area in the same region?
- **Q5 — SPMC late joiners and in-band dictionaries:** a broadcast ring overwrites. A consumer that joins after the
  dictionary was sent needs a re-send: periodic checkpoint records, or a "request dictionary" side channel? (The
  examples use SPSC, which keeps it.)
- **Q6 — compile-time capacity:** `Unit` and `Align` are template parameters. Should the capacity (units) also be
  one (0 = run time), for a constant mask?
- **Q10 — payload alignment: decided (maintainer, 2026-10-02).** Keep payloads 16-byte aligned for now. A template
  parameter `DataAlign` (default 16, optionally 64) may be added: payload at record + DataAlign, header at the record
  start, `static_assert(Align >= DataAlign)`, the value recorded in the ring header and checked by `attach()`. It is
  planned together with the SPMC eviction-word change, so the layout version (`ring_version` 2) is bumped once.
- **Q6b — gate scan: decided and done (maintainer, 2026-10-02: readers usually fewer than 16).** The ring keeps a
  high-water mark of the gates ever used (`hwm`, next to the ticket counter). Readers take the lowest free gate, and
  the producer scans `[0, hwm)`: a ring created for 16 readers and read by 3 scans 3 cache lines. A joiner raises the
  mark (seq_cst) between its gate CAS and its cursor re-read, so the I-01 ordering argument covers it. The mark never
  decreases, so churn cannot hide a gate (an "active set" bitmask could lose a bit to a concurrent leave/join and was
  rejected for that reason). Test: "3 readers of a 16-reader ring use gates 0..2 and the scan stops there".
- **Q8 — `ipc_logger` (planned later, maintainer 2026-10-02):** single- and multi-writer, multi-reader, an **endless,
  file-backed** memory log (a log that is also the capture on disk). Design notes for when it starts:
  - **Multiple writers** cannot publish through one cursor store, because they finish out of order. Each record
    needs its own commit marker (Aeron's term buffers write the frame length last). Claiming becomes a `fetch_add`
    on the tail.
  - **Header as the commit marker.** This ring deliberately does *not* use the header as the publish signal,
    because stale bytes from an earlier lap could look committed. A file-backed log that appends into **fresh,
    zero-filled pages** (new file segments, never reused in place) does not have that hazard. There a non-zero
    header can be the commit marker, which is what makes multiple writers simple.
  - **Positions.** An endless log outgrows 32-bit unit positions. A position becomes (segment number, 32-bit unit
    offset in the segment): the unit idea of this ring carries over per segment.
  - **Segments.** Rotation, pre-allocation (`fallocate`), retention, and readers that follow the head across
    segments (`inotify` or polling) are new pieces. Readers can start from any segment, so the dictionary needs
    periodic checkpoint records (DECISIONS P-11, Q5 above).
  - **Shared code.** The record header {size, seq, type, check}, the validation rules (§5.7) and the zero-copy API
    shape should be shared with this ring, so one consumer API reads both.
- **Q7 — Windows: decided (maintainer, 2026-10-02):** after everything works and is tested on Linux. The plan, and
  which files depend on Linux (for ipc_dynamic: `region.hpp`, the liveness check and `getpid`/`sched_yield`; the layout
  and protocols are portable), are in `docs/windows-porting.md`.