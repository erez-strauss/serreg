# es::ipc (developed as es::ipc_dynamic) — why small messages were slower, and what changed (2026-10-02)

> **Read §5 first.** §1–§4 describe the state before layout 2, the same day: the names `es::ipc_dynamic`,
> `serreg_ipc_dynamic_bench` and the run-time `progress_mode` existed then. Layout 2 removed `progress_mode` (readers
> always publish with a plain store), and the switch renamed the rest (`es::ipc`, `serreg_ipc_compare_bench`).

**Machine:** the sandbox (i9-13900H). Threads pinned to P-cores 4 and 6 (5.4 GHz). `perf` is not available here
(`perf_event_paranoid` = 4, no root), so every conclusion comes from A/B experiments, not counters. The first half of
the session ran with hyperthreading **on**; the maintainer turned it **off** partway through. Only the HT-off numbers
are quoted below unless marked otherwise; with HT on, everything was 10–40 ns slower and noisier.

**Measurement floor.** Two binaries that differ only in code that a test does not execute can differ by up to ~20 ns at
p50: es::ipc measured 86 ns in one build and 109 ns in another with identical es::ipc code (code placement). Effects
below ~20 ns are therefore not resolved by this method; the reference box with `perf c2c` / `perf stat` must settle
them.

**Tools** (in `review-programs-for-2.1.11/ipc_dynamic/latency/`, private):
- `pingpong.cpp` — a ping-pong between two threads, rdtsc timing, 200 000 round trips × 5–7 rounds. It covers
  es::ipc SPSC/SPMC and ipc_dynamic with units 0 (run time), 64, 32 and 16.
- `pingpong-bench-messages.cpp` — the same with the project bench's message handling (`-DMODE=1`: fill and checksum
  on the writer; `-DMODE=2`: checksum on the reader too).
- `make-variants.py INCLUDE OUT` — writes header copies with one reader change each (the variants below).
- `alignment.cpp` — prints where records and payloads land for several Unit/Align pairs.

## 1. Questions and answers

| Question | Answer |
|---|---|
| Is the slowness the extra CAS, or more cache lines? | **The CAS.** More precisely, the full barrier a locked instruction implies. A plain store followed by `atomic_thread_fence(seq_cst)` costs the same as the CAS (variant V7). The reader's cache-line footprint is the same as es::ipc's: both spin on the cursor line, then read the record line, and the gate line stays in the reader's own cache. |
| Why does a CAS on a line we already own cost ~40–60 ns? | A locked instruction waits until the core's store buffer has drained. In a ping-pong, the reader has just written its reply into the other ring (record line and cursor line, both being read by the peer), so the barrier waits for those transfers, and that wait lands on the round trip. Deferring the CAS to the first idle poll (V5) does not avoid it reliably: the first idle poll comes right after the reply, and the result flips between fast and slow with message size. |
| Can the plain store for SPSC be a run-time decision? | **Yes, and it is implemented.** The ring kind is already known at run time (`spsc_`), and a well-predicted branch costs nothing measurable. `consumer::progress(progress_mode::store / cas)`; the SPSC default is `store`. |
| Can SPMC use it too? | **Not safely with today's layout.** A reader can be preempted between its ownership check and its store. If it is evicted in that gap, the producer overwrites its records, and the late store re-installs the old gate: a record overwritten in the meantime can then pass every check. `progress(store)` returns false for SPMC. The test-only variant **S1** (one reader ever, hwm == 1, skips the CAS) shows the potential: SPMC 32–112 B goes from 129–172 to 113–131 ns. A safe design for SPMC is in §4. |
| Do 16- or 32-byte units help small-message latency? | **No.** With the default `Align = 64` the granule is 64 bytes whatever the unit, so the layout does not change. `Unit 16 / Align 16` is consistently **20–25 ns slower**: packed records straddle cache lines, so 2 lines move per message. `Unit 32 / Align 32` equals 64/64. Small units are a memory-density choice for mixed sizes, not a latency one. |
| Is a compile-time unit faster than `Unit = 0` (run time)? | Measured +0–25 ns for the run-time unit, but inconsistent across builds. Below the measurement floor; recheck on the reference box. For lowest latency, use `basic_ring<64, 64>` anyway: it costs nothing. |
| Are the units allocated with the proper alignment? | **Records: yes, when the memory is aligned to max(64, Align)**, and a `region` (mmap) always is. **Bug found and fixed:** `create()`/`attach()` only checked 64-byte alignment, so with `Align` = 128 … 4096 and a caller's buffer that is only 64-aligned, records were misaligned. Both now require `basic_ring::mem_align` = max(64, Align). **Payloads are 16-byte aligned** (they follow the 16-byte header), not Align-aligned: see §3. |

## 2. Measurements (HT off, 32 B unless noted, half-RTT p50 in ns)

Variants of the reader (`make-variants.py`):

| Variant | 16 B | 32 B | 48 B | 112 B |
|---|---|---|---|---|
| es::ipc SPSC | 77 | 88 | 91 | 102 |
| as shipped: CAS on every release (u64a64) | 141 | 131 | 134 | 150 |
| V7: plain store + seq_cst fence | 148 | 150 | 151 | 161 |
| V1: plain store, no check | 96 | 92 | 102 | 112 |
| **now: SPSC store mode (check + store)** | **91** | **105** | **105** | **116** |
| V5: CAS deferred to the first idle poll | 84 | 133 | 86 | 98 |

V2–V4 also removed the per-record ownership check, the post-read check and header validation. On top of V1 they gained
nothing measurable (HT on), so plan item 1 of the previous session (ownership checked once per batch) is **dropped
for latency**. The ownership load and the pid re-assert in the store path (variants N1–N3) are also below the floor.

Unit size (now, SPSC):

| | 16 B | 32 B | 48 B | 112 B |
|---|---|---|---|---|
| unit 64 / align 64 | 91 | 105 | 105 | 116 |
| unit 32 / align 32 | 92 | 105 | 104 | 117 |
| unit 16 / align 16 | 110 | 133 | 122 | 137 |

Project bench (`serreg_ipc_dynamic_bench (since the switch `serreg_ipc_compare_bench`) --cpus 4,6 --repeat 3`, HT off, two invocations):

| p50 / p99 (ns) | 32 B thread | 32 B process | 256 B thread | 256 B process |
|---|---|---|---|---|
| es::ipc SPSC | 70–78 / 118–126 | 72–76 / 116–122 | 112–117 / 188–190 | 111–122 / 190–192 |
| es::ipc SPMC | 74 / 131 | 75–80 / 135–139 | 113–125 / 207–211 | 120 / 203–207 |
| ipc_dynamic SPSC, unit 0 | 89–97 / 162–166 | 91–97 / 158–166 | 138–140 / 203–206 | 136–147 / 205–210 |
| ipc_dynamic SPSC, unit 64 | 91–92 / 158–161 | 90–99 / 162–163 | 130–131 / 196–198 | 132–134 / 198–211 |
| ipc_dynamic SPSC, unit 32 | 96–109 / 161 | 103–107 / 162–165 | 141–148 / 205 | 150–152 / 211–213 |
| ipc_dynamic SPSC, unit 16 | 95–97 / 180 | 89–95 / 179–184 | 134–137 / 212–215 | — |
| ipc_dynamic SPMC (CAS) | 91–95 / 161–162 | 92–98 / 164–167 | 127–143 / 197–199 | 141–145 / 206–207 |

Before this session (HT on, CAS everywhere) the same bench gave 148–155 ns for ipc_dynamic SPSC 32 B against 97–118 ns
for es::ipc.

## 3. What remains (for the optimization session)

1. **A residual 15–25 ns (32 B) and 15–30 ns (256 B) behind es::ipc in the project bench.** It is not the reader's
   bookkeeping: with the bench's message pattern, even a reader with every check removed (V4) stays behind. It grows
   when the writer fills and checksums the message in place (`pingpong-bench-messages.cpp` MODE 1/2: +10–35 ns for
   ipc_dynamic, ±0 for es::ipc).
   - **Candidates:**
     - The order of the writer's stores. es::ipc stores a "busy" stamp into the slot at claim time, so its
       ownership request for the line starts early; ipc_dynamic writes the header only at commit.
     - Its 4 separate 4-byte header stores and loads, against es::ipc's 8+4 bytes.
     - The 128-byte spatial prefetcher pairing adjacent records.
   - A first try, a store into the header at claim time (P1), was inconclusive with HT on.
   - **This needs `perf c2c` and `perf stat -e mem_load_retired.*` on the reference box.**
2. **SPMC without the CAS (S1 shows 15–45 ns).** The safe design (layout change, `ring_version` 2):
   - Each gate gets an *eviction word*, written only by evict/reap (CAS), and a *position word*, written only by
     the owner (plain release store).
   - The producer ignores an evicted gate whatever its position says.
   - A freed gate becomes reusable only after the evicted owner has acknowledged the eviction. It stops writing as
     soon as it reads the eviction word, and acknowledges after that, so a late position store can never land on a
     new owner.
   - A process that dies is reaped as today.
   - **Cost:** one more load per gate in the producer's scan, which happens about once per ring's worth of data,
     and a gate is lost while its evicted owner stays alive but never reads again.
3. **Payload alignment.** Today the payload is 16-byte aligned, at record + 16. That keeps a message of up to
   48 bytes in one cache line with its header, which is the best case for latency. **Decision for the
   maintainer:** keep it, or add an option that aligns payloads to `Align`. That option would put the header in the
   previous granule and cost one more cache line per small message.
4. **Gate spacing.** Gates are 64 bytes apart, so the spatial prefetcher pairs gate *i* with gate *i+1*, and for SPSC
   gate 0 with record 0. Spacing them 128 bytes apart is a layout change to measure together with item 2.
5. **Run-time unit** (+0–25 ns, below the floor): compare the generated code on the reference box.

## 4. Summary for the maintainer

- **The cause is found and fixed for SPSC.** It was the CAS's full barrier, not cache lines. SPSC readers now publish
  progress with a checked plain store (a run-time default; `progress(progress_mode::cas)` restores the CAS).
- **Small units don't help latency.** 16-byte units cost 20–25 ns; use unit 64, align 64 for low latency.
- **One alignment bug is fixed:** `Align` > 64 with a 64-aligned buffer gave misaligned records. Payloads are
  16-aligned by design; your decision is item 3.
- **SPMC still uses the CAS.** S1 shows what removing it would gain; §3 item 2 is the safe way to get it.
- **A smaller remaining gap** needs `perf` on the reference box (§3 item 1).

## 5. Follow-up the same day: layout 2, and the header-polling prototype (HT off)

**Layout 2** (`ipc-design.md` §4, §5.5) separates each gate's ownership word, changed only by CAS, from its position
word, which the owner writes with a plain release store. An evicted gate stops holding the producer at once, and is
reused only after the evicted reader has acknowledged. This makes the plain store safe for SPMC too, and removes
`progress_mode`. The single-reader experiment S1 (an SPMC reader skipping the CAS, test only) had shown the gain
first.

| 32 B half-RTT p50 (ping-pong microbenchmark) | before layout 2 | layout 2 |
|---|---|---|
| SPMC | 140–160 ns | 83–132 ns (mostly 91–118), now equal to SPSC |
| SPSC | 81–116 ns | 81–116 ns |
| es::ipc_old (same binaries) | 80–114 ns | 80–114 ns |

Project bench (`serreg_ipc_compare_bench`, two invocations), 32 B p50:
- es::ipc_old: 65–81 ns (thread), 71–75 ns (process);
- es::ipc: 90–102 ns (thread), 87–97 ns (process); SPMC = SPSC.

**H1 — the reader polls the next record's header instead of the cursor.** This is Agrona's model: one cache line
per message instead of two. Patch: `review-programs-for-2.1.11/ipc_dynamic/latency/h1-header-polling.patch`.
- **Result:** at unit 64 it gains 0–13 ns (about the noise); at unit 16 a clear 15–20 ns, because packed records
  cost the extra line. With H1, unit-16 latency equals unit 64.
- **Not adopted yet:** the prototype exposed three hazards that need a design pass (see
  `ipc-optimization-directions.md` D1):
  1. Deferred commits become visible early.
  2. A reader ahead of the cursor must treat the cursor as a lower bound. The first prototype re-delivered messages
     until that was fixed.
  3. A false positive at the frontier: old payload bytes looking like a valid header. Agrona avoids it by zeroing
     the next header in advance.
