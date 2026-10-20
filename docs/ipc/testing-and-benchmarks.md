# es::ipc (developed as es::ipc_dynamic) — test, review and benchmark results

**Date:** 2026-10-02 · **Tree:** `include/es/ipc_dynamic/` (after the switch `include/es/ipc/`) · **Machine:** the review sandbox (13th Gen Intel
Core i9-13900H, 14 cores online, shared with other jobs). **These numbers are indicative**; replacement decisions use
the reference box (`ipc-design.md` §9).

> **After layout 2 and the switch (2026-10-02, the same day):**
> - **Tests:** `serreg_ipc_tests` (`tests/unit/test_ipc.cpp`) has 55 cases, green under Debug, TSan and ASan/UBSan.
>   Under TSan the eviction stress runs without the evictor; its read-while-overwritten race is by design. (The old
>   slot rings' 26 cases left the build with G-17.)
> - **Whole tree:** 304/304 ctest.
> - **Harnesses:** every one in `review-programs-for-2.1.11/ipc_dynamic/` rerun on layout 2, including SPMC stress
>   with constant evictions and 2 × 102 kill take-overs: corrupt 0, bad 0, stall 0.
> - **Benchmarks:** current numbers in `latency-analysis.md` §5 and `ipc-design.md` §9.2. The sections below
>   describe the first version (layout 1).

## 1. How the work was split

| Part | Who | Result |
|---|---|---|
| Design and headers | lead | `ipc-design.md`; layout, ring, producer, consumer, sink |
| Test suite | test agent | `tests/unit/test_ipc_dynamic.cpp`: 46 cases (the lead added 2 for Q6b and the deferred/join fix) |
| Adversarial review with hardware stress | review agent | findings A–K (§3); repro and stress programs, patched headers |
| Benchmark | benchmark agent | `tests/bench/ipc_dynamic_bench.cpp` (`serreg_ipc_dynamic_bench (since the switch `serreg_ipc_compare_bench`)`); findings P1–P2 (§3) |
| Examples | examples agent | `examples/ipc_log/` (two processes), `ce-examples/ce-example5/` (two threads) |
| Fixes, integration, reruns | lead | every finding below fixed in the headers and verified with the agents' own repros |

## 2. Test results (final headers)

| Build | Result |
|---|---|
| Debug (`-D_GLIBCXX_ASSERTIONS`) | **51/51 test cases, 100 886 assertions** (48 before the progress-mode/alignment change) |
| ThreadSanitizer (gcc16-tsan flags, `setarch -R`) | **51/51**, no reports |
| ASan + UBSan (gcc16-asan-ubsan flags) | **51/51**, no reports |
| Whole tree, Debug build, `ctest` | **300/300** (the two Lua-stub Wireshark tests are disabled here: no interpreter) |
| Formatting / include rules / book check | clean / 0 problems / 0 problems |

What the suite covers:
- geometry, `create` and `attach` validation (every header field), the 256-reader limit;
- every boundary of the skip logic, records of `max_message()`, rings above 4 GiB;
- wrap across 2³²;
- types, policies, deferred commits, cancel and abandon;
- SPSC persistence and hand-over;
- SPMC broadcast, join while running, evict (the evicted reader can never move the new owner's gate);
- 11 forged-header variants;
- threads (200 000 mixed messages up to ~80 % of the ring, every byte checked, units 16/32/64);
- processes (fork, SPSC and SPMC, named and anonymous regions);
- dead-reader reaping and producer take-over;
- `sink` with `es::serreg::serialize`;
- the Q6b scan bound;
- deferred records versus a joining reader.

**Hardware stress** (the review agent's harnesses, kept in `review-programs-for-2.1.11/ipc_dynamic/`, rerun on the final headers):

| Harness | Result |
|---|---|
| SPMC, 128 units, 8 readers joining/leaving + evictor, cursor started at 0xFFFFF000 | 14.4 M produced, 110 049 joins, 51 116 evictions, **corrupt 0, bad 0, stall 0** (9 torn deliveries, all to evicted readers, all reported as lapped) |
| SPMC, 16 units, 12 readers | 7.8 M produced, 94 M deliveries, **0 / 0 / 0** |
| SPSC hand-over between readers | 57 M, 19 504 hand-overs, **0 / 0 / 0** |
| Deferred commits, 1 and 6 joining readers (previously: corruption and a stall) | **0 / 0 / 0** |
| 200 random SIGKILLs of producer and reader processes | 102 producer take-overs, 50 M deliveries, **bad payload 0, corrupt 0** |
| 2³¹-unit ring, 1 GiB record across the 2³² wrap | delivered intact |

## 3. Findings and fixes

| ID | Found by | Severity | What | Fix |
|---|---|---|---|---|
| B1 | tests | high | an SPSC ring created with `max_consumers` ≠ 1 could not be attached (offsets computed for the requested count, header says 1) | `create()` normalises an SPSC geometry to one gate |
| B2 / A | tests, review | high | `peek()`/`wait()` did not release a trailing skip record, so a producer waiting for a large record deadlocked | release the skip at once |
| B3 / C | tests, review | medium | rings above 4 GiB: sizes above 2³² − 1 were truncated by the u32 header field | `max_message()` ≤ 2³² − 1 (`message_size` above) |
| B4 / D | tests, review | medium | a reaper could pair a stale dead pid with a new owner's ticket | `gate::pid_ticket`: the pid is valid only for the ticket it was stored with |
| B (review) | review | high | deferred commits with no gating reader let the producer run a ring ahead of the published cursor, so a joining reader lost records | publish deferred records before every free-space scan |
| H | review | high | a changed header under a gate the reader still owned was taken for a lap, and `leave()` then never freed the gate (an SPSC queue wedged for ever) | such a header counts as corruption; the reader skips to the cursor and still frees its gate |
| E | review | low | after an eviction, a garbage header was counted as corruption, not as a lap | check ownership before counting |
| F | review | low | `full_policy::fail` with deferred records never flushed | flush, then rescan, before refusing |
| G | review | low (plausible) | a joiner evicted mid-join could overwrite the next owner's pid entry late | the owner re-asserts its entry after a progress CAS |
| P1 | benchmark | perf | `read_record()` returned `std::optional<record>`: GCC built it with partial stores and reloaded it whole, a store-forwarding stall on every record (~8 ns/message) | a plain struct, `units == 0` = refused (consume cost 10–12 → ~3.7 ns/message in the agent's microbenchmark) |
| P2 | benchmark | perf (minor) | two integer divisions per record | masks and shifts (unit and granule are powers of two) |
| Q6b | maintainer | perf | readers usually < 16; a ring created for 16 but read by 3 scanned 16 gates | high-water mark of gates ever used; readers take the lowest free gate |
| I, J, K | review | info | fork copies can still be *used* (only detach/leave are inert); a misaligned cursor from wrong data makes every later skip count as corrupt (no crash); the overwrite detection relies on multi-copy-atomic hardware (x86, ARMv8), as the slot rings do | documented (design §5.10) |

## 4. Benchmark (Release, pinned to two P-cores, 2 invocations × 3 repetitions)

> **Superseded for latency** by `latency-analysis.md` (2026-10-02, hyperthreading off, SPSC store mode). Those numbers
> are lower for every library. Most of the gap below came from the reader's CAS, which SPSC no longer uses. The
> tables in this section were taken with hyperthreading on, before that change.

`serreg_ipc_dynamic_bench --cpus 4,6 --repeat 3`. Every run verified (sequence, length, checksum, no lap, no
corruption).

**Half-RTT, p50 / p99 in ns** (two invocations; the machine was shared, so invocation-level shifts of 30–50 % occur):

| | 32 B thread | 32 B process | 256 B thread | 256 B process |
|---|---|---|---|---|
| es::ipc SPSC | 108/137 · 52/130 | 118/233 · 49/121 | 171/372 · 88/181 | 189/385 · 89/259 |
| es::ipc SPMC | 114/139 · 54/129 | 116/167 · 52/123 | 170/356 · 86/186 | 175/273 · 88/263 |
| ipc_dynamic SPSC | 148/290 · 78/161 | 155/289 · 93/165 | 169/222 · 104/183 | 164/196 · 103/190 |
| ipc_dynamic SPMC | 152/167 · 91/166 | 156/286 · 71/168 | 171/201 · 106/188 | 170/204 · 106/266 |

**Throughput, Mmsg/s (median of 3), two invocations:**

| Workload / ring | es::ipc | ipc_dynamic (unit 64, align 64) | ipc_dynamic (unit 16, align 16) |
|---|---|---|---|
| Mixed 95 % 64 B / 4.9 % 1 KiB / 0.1 % 256 KiB, **4 MiB** | 14.3 · 14.9 (16 slots × 256 KiB) | **28.9 · 27.0** | 27.0 · 24.1 |
| Same, es::ipc with **64 MiB** (256 slots) | 19.1 · 14.3 | (4 MiB) | (4 MiB) |
| 64 B only, 0.5 MiB | 65.6 · 12.5 (bimodal) | 28.8 · 35.0 | 25.7 · 24.6 |

**Reading:**
- **Mixed sizes, the case ipc_dynamic exists for:** about **1.8–2× the throughput of the slot ring, with 1/16 of the
  memory**. It is stable across invocations.
- **256-byte messages:** latency equal at p50; p99 equal or better.
- **32-byte messages:** ipc_dynamic is **20–40 ns slower at p50**.
- **64-byte-only throughput:** the slot ring is bimodal on this shared machine (12–77 Mmsg/s); ipc_dynamic is stable
  at 25–35. The comparison needs the reference box.

**Where the small-message cost is:** the reader checks gate ownership before each record and again after it
(`intact()`), validates the header, and publishes progress with a CAS instead of a plain store. Candidates:
1. check ownership once per **batch**, before and after, instead of per record. The guarantee stays: nothing is
   delivered from a gate that was lost before the batch, and a lap during the batch is reported after it;
2. move the header check of `seq`/`check` to a debug or "paranoid" policy for trusted peers, if the maintainer
   accepts that trade;
3. a plain release store of progress for SPSC rings, whose gate can only change hands by reaping a dead process.
   The CAS is only needed against eviction, which SPSC refuses.

**Replacement criteria (design §9) today:**
- **Met:** memory and mixed-size throughput.
- **Not met:** small-message latency (+20–40 ns).
- **Unknown:** 64-byte-only throughput, until the reference box is run.
