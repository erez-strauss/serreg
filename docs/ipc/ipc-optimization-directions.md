# es::ipc — optimization directions and industry comparison (2026-10-02)

**Context.**
- The maintainer switched to the variable-size rings as es::ipc on 2026-10-02 (`ipc-design.md` §9.2). The IPC is
  not the package's main delivery.
- `perf` is available neither in the sandbox nor on the reference box.
- This file records where es::ipc stands against published results and what to try next, ordered by expected gain
  over effort.
- Measurements: `latency-analysis.md` (microbenchmark, variants) and `testing-and-benchmarks.md`. Every sandbox
  number is from an i9-13900H with hyperthreading off, threads pinned to P-cores 4 and 6.

## 1. Where es::ipc stands

| | es::ipc (layout 2) | es::ipc_old (slot rings) |
|---|---|---|
| 32 B half-RTT p50, ping-pong microbenchmark | 81–118 ns (SPSC = SPMC) | 80–114 ns |
| 32 B half-RTT p50, project bench (checksummed messages) | 87–102 ns | 65–81 ns |
| 32 B half-RTT p99, project bench | 152–167 ns | 112–150 ns |
| 64 B-only throughput | 21–27 Mmsg/s | 10–11, or 73–80 in its fast mode |
| Mixed 64 B / 1 KiB / 256 KiB throughput | 23–27 Mmsg/s, about 10 GB/s, in 4 MiB | 9–11 Mmsg/s in 64 MiB |

**Measurement floor.** Two builds that differ only in code a test does not run move by up to ~20 ns at p50 (code
placement). Only effects above that are resolved here.

## 2. Published results (industry)

Sources were checked by a research pass on 2026-10-02 against the primary pages. "Half-RTT" = RTT / 2, the unit our
benchmarks report.

| Source | What | Number | Our unit | Conditions |
|---|---|---|---|---|
| Rigtorp SPSCQueue (github.com/rigtorp/SPSCQueue) | RTT, 2 threads, 2 queues of `int` | 133 ns | **~67 ns half-RTT** | Ryzen 9 3900X, same chiplet; 4-byte items |
| ″ | throughput | 362,723 ops/ms | ~363 M items/s | 4-byte items |
| ″ boost::lockfree::spsc / folly PCQ (same table) | RTT | 222 / 147 ns | ~111 / ~74 ns | same |
| Rigtorp "Optimizing a ring buffer for throughput" (rigtorp.se/ringbuffer, 2021) | throughput, cross-chiplet | 5.5 M → 112 M items/s | — | adding cached remote indices gave the ×20 |
| Rigtorp nanomq | RTT, 100-byte messages | 250 ns | ~125 ns | Core 2 Duo T7500 (old) |
| LMAX Disruptor paper (2011) | latency per hop (one-way), 3-stage pipeline | mean 52 ns, 99 % < 128 ns, 99.99 % < 8,192 ns | ≈ one-way | Java 6, i7-2720QM, events 1 µs apart, no pinning |
| ″ | unicast 1P–1C throughput | 26 M ops/s (2011) → 160 M ops/s (Disruptor 4) | — | Nehalem 2.8 GHz → EPYC 9374F; long events |
| Aeron + Chronicle (Man Group, 2020) | RTT, 100-byte messages, JMH | 0.25 µs | ~125 ns | hardware not given |
| Aeron IPC (third party, wire-gauge, 2026) | RTT, 128 B, 5,000 msg/s | p50 0.53 µs, p99 8.15 µs | ~265 ns p50 | i9-13980HX, isolated P-cores; not a vendor source |
| ″ their raw shared-memory ring | RTT, 128 B | p50 0.35 µs, p99 0.44 µs | ~175 ns p50 | same machine |
| Chronicle Queue README | send/receive between JVMs, 40 B | 99 %: 0.78–1.2 µs | — | hardware not given |
| Core-to-core latency (nviennot, ~2022) | one-way cache-line transfer | Alder Lake P↔P 35 ns; i9-9900K 21 ns; Zen 3 intra-CCD 16–17, cross-CCD 84–85 ns | — | CAS ping-pong / 2; no Raptor Lake entry (UNVERIFIED ~30–33 ns) |

**Reading:**
- **Latency.** A half-RTT can't drop below one cache-line transfer, about 35 ns on this class of Intel P-core (no
  Raptor Lake figure verified), plus the work done.
  - es::ipc and es::ipc_old move **two lines per message**: the cursor, then the record.
  - So ~80–100 ns half-RTT is in the expected range, and roughly 2× the floor.
  - Rigtorp's ~67 ns half-RTT is on a Zen 2 chiplet, where a transfer is about 17 ns.
  - The Disruptor's 52 ns per hop is a mean in Java, with events spaced 1 µs apart.
  - Published Aeron IPC numbers (125–265 ns half-RTT) are above ours.
  - **es::ipc is in the same class as the fast specialised queues.** It does more work per message: the header,
    the check, validation.
- **Throughput.** Rigtorp's and the Disruptor's 100–360 M items/s are for fixed 4–8-byte items with cached indices
  and batch consumption. es::ipc moves variable-size records with a validated 16-byte header, and our bench fills and
  checksums every message. Its 21–27 Mmsg/s for 64 B is well below those figures. The gap to es::ipc_old's fast mode
  (73–80) is the clearest throughput target (D3).

**What these projects do that is relevant here, and where es::ipc stands:**

| Technique | Who | es::ipc |
|---|---|---|
| cached remote index (read the other side only when the cache says full or empty) | Rigtorp, Agrona `headCache`, folly | **yes**: producer `limit_`, consumer `avail_` |
| record header {length, type} plus a padding record at the wrap | Agrona | **yes**: {size, seq, type, check} plus a skip record |
| claim / commit / abort | Agrona, Disruptor | **yes**: claim / commit / cancel |
| batch consumption, one progress store per batch | Disruptor, Agrona | **yes**: `poll()` |
| the reader polls the record (length as the commit flag) instead of a cursor: one line per message | Agrona | **no** (prototype H1: D1) |
| zeroing the next header in advance, so the frontier never shows stale bytes | Agrona | no (part of D1) |
| indices and counters 128 bytes apart (defeats the adjacent-line prefetcher) | Agrona (2 lines) | no: 64 (D4) |
| huge pages, pinning, isolated cores | Rigtorp, all low-latency setups | huge pages yes (`region`); pinning is up to the application |

## 3. Directions, in order

### D1 — The reader polls the next record's header, not the cursor (expected: −1 cache line per message)

Prototype H1 (`review-programs-for-2.1.11/ipc_dynamic/latency/h1-header-polling.patch`, `latency-analysis.md` §5).

**Measured:** 0–13 ns at unit 64 (around the noise); **15–20 ns at unit 16**, where unit 16 then equals unit 64.

**Design work needed** before adopting it:
1. **Deferred commits.** `commit(n, false)` must stay invisible until `flush()`:
   - the producer writes `seq = pos ^ 0x8000'0000` for a deferred record;
   - `flush()` walks `[published, tail)` (bounded by the unit count) and stores the real `seq` (release) before the
     cursor.
2. **Reader ahead of the cursor.** A reader can see a record through its header before the cursor covers it.
   Everywhere the cursor is read, it must act as a lower bound (`cursor_floor()`).
3. **Frontier false positives.** Old payload bytes at the next header position could form a valid {size, seq, type,
   check}. Agrona zeroes the next header when it writes a record. That needs one granule of slack at claim time,
   so the zeroing store never touches unreleased data.
4. **Ordering.** The producer writes `seq` last, with release; the reader acquire-loads `seq` first.
5. **Proof obligations:**
   - the I-01 join argument is unchanged, since a joiner still starts at the cursor;
   - `intact()` keeps detecting laps.

**Tests:** the review harnesses (`e_stress`, `e_stress_defer`, `i_kill`) with deferred commits, plus a frontier test
that plants a forged header in old payload.

### D2 — Fewer header stores and loads per record (expected: a few ns per message, mostly throughput)

- Write the 16-byte header with one 16-byte store (or two 8-byte ones) instead of four 4-byte relaxed atomics. No
  atomicity is needed beyond the publishing store (the cursor today, `seq` under D1).
- Read it the same way, in one load.
- Optionally drop the `check` multiply for trusted peers, as a run-time `validation` policy. The maintainer's O2
  ("never crash on wrong data") still holds without it, since the bounds checks stay.

### D3 — Small-message throughput (64 B: 21–27 vs es::ipc_old's 73–80 Mmsg/s in its fast mode)

Candidates, each easy to A/B in `serreg_ipc_compare_bench --only tput`:
- **One cursor store per message.** The producer publishes on every commit. Measure the deferred path:
  `commit(n, false)` with a flush every N messages or on empty. This is the Disruptor's batching.
- **Per-record validation in the consumer's `poll`** (D2).
- **es::ipc_old's bimodality** (10 or 75 Mmsg/s for the same binary) points at a placement effect: the two
  threads' hot lines in the same cache set, or the adjacent-line prefetcher. Find it before tuning es::ipc's
  throughput against it.

### D4 — 128-byte spacing of shared hot lines (expected: small, removes a known interference)

- Gates are 64 bytes apart, so the adjacent-line (128-byte) prefetcher pairs gate *i* with gate *i+1*. For SPSC it
  also pairs gate 0 with record 0.
- Agrona keeps every index two lines apart.
- Spacing gates, and the cursor, 128 bytes apart is a layout change (`ring_version` 3), so bundle it with D1.

### D5 — Waiting (expected: up to tens of ns for `wait()` users)

- `consumer::wait()` and the producer's full-ring spin issue `pause` on every iteration. On recent Intel cores
  `pause` costs on the order of 100+ cycles, so detection can lag by tens of ns.
- The benchmarks busy-poll without `pause`, so they don't show it.
- **Proposal:** a `wait_policy` with three settings:
  - busy (no pause): for pinned, isolated cores;
  - pause: the default;
  - yield.

### D6 — The run-time unit (expected: 0–25 ns, below the floor; verify first)

- `Unit = 0` measured slower than a compile-time unit in some builds.
- **Check:** the generated code of `peek`/`poll`/`claim` (`objdump -d`), to look for reloads of `unit_`/`mask_`
  after each acquire load.
- **Remedy if real:** cache them in locals, or dispatch once on the unit to `basic_*<16|32|64>` instantiations.

### D7 — Measurement without perf (needed for D1–D6)

- **Settings:** hyperthreading off, a fixed frequency (performance governor), two pinned P-cores on an idle
  machine.
- **Runs:** ≥ 5 interleaved rounds per variant, medians reported.
- **Variants:** `make-variants.py` patches the headers and `pingpong.cpp` measures. Treat < 20 ns as noise unless
  it repeats across builds.
- **To reduce code-placement noise,** try `-falign-functions=64 -falign-loops=64` for benchmark builds.
- **Without `perf`,** counters are still reachable through `rdpmc` if the kernel allows user RDPMC
  (`/sys/bus/event_source/devices/cpu/rdpmc`). A small wrapper could count L2 misses / snoop hits around the
  ping-pong loop.

### D8 — API conveniences found while porting (no performance effect)

Found while porting the reference benchmarks and examples:
- **Attach without knowing the kind:** `ring::kind_of(region)`, or `attach()` accepting either kind. The ported
  half-RTT benchmark tries SPMC, then SPSC.
- **Sizing by messages:** `ring::for_messages(n, max_bytes)` and a static `record_bytes(payload)`. Callers otherwise
  repeat the header and rounding rule. Also: `for_capacity` silently caps at 2^31 units.
- **"Empty" vs "lapped":** `peek()` returns nullopt for both, so callers must check `lapped()`. A tri-state result
  would make the loop harder to get wrong.
- **`publish_as<T>`** (es::ipc_old had it): `emplace<T>` covers construction, and a `fill(T&)` variant would complete
  it.
- **The ipc_log example's gaps:**
  - `frame_router` has no dictionary overload for `otherwise`;
  - `basic_sink` can't report why a claim failed;
  - there is no producer-side "drained" query.

### D9 — Later (not latency)

- **ipc_logger** (Q8): single/multi-writer, multi-reader, an endless file-backed log; it reuses the record format.
- **Windows** (`docs/windows-porting.md`).
