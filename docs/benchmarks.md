# Benchmarks — the measured numbers, where they come from, and how to read them

One page for every performance figure the project quotes. Each number names the box, the binary and the run it
comes from; anything not listed here is a *ceiling* (a test assertion), not a measurement. Numbers from the
sandbox are never recorded (DECISIONS G-5: the reference box decides).

**Reference box (2026-10-17):** ai-box Fedora 44 container, Intel i9-13900H (6 P-cores with HT + 8 E-cores,
20 logical), 61 GiB, GCC 16.2.1 (Red Hat), release preset (`-O2`, `-fno-gnu-unique`), governor `powersave`,
hyper-threading on, turbo on. A laptop-class part in a container: good for ratios and orders of magnitude, not a
floor for a tuned server.

## 1. Hot path: serialize, deserialize, inspect, dispatch (`serreg_bench`)

`build/gcc16-release/tests/bench/serreg_bench`, 2026-10-13, same box. The hand-loop rows are the measurement. The
Catch2 rows for `serialize`/`deserialize` in that run were hoisted by the optimiser (loop-invariant inputs, "est
run time 0 ns"); since 2.2.4 those rows change the input per sample and consume the output, so the next run of
`serreg_bench` gives comparable Catch2 numbers.

| path | mean | what it does |
|---|---|---|
| `serialize(span_sink, Order{32 B})` | **2.5 ns** | 16-byte header + 32-byte `memcpy`; `o.id` changes per iteration |
| `deserialize(frame, Order&)` | **~2 ns** | header check, call-hash compare, 32-byte copy (L1-hot) |
| `inspect(frame)` → `frame_view` | **28.7 ns** | header validation + one registry lookup (acquire-load the index, binary search) |
| `inspect` + iterate the one record | **61.8 ns** | the lookup plus the record step |
| `dispatch<Order>(frame, handler)` | **60.6 ns** | `inspect` + record walk + typed handler; the typed part is not measurable |

Reading: the per-message cost of the serializer itself is a few nanoseconds; the ~60 ns of the readers that go
through the registry is one lookup, and it is paid once per *frame*, not per argument. Ceilings asserted under the
`bench` label: 50 ns and 500 ns.

## 2. Registry budgets (`registry_measure`, DECISIONS G-5)

Fedora runs of 2026-10-13 and 2026-10-17 (`serreg-tests-20261013-125545`, `serreg-tests-20261017-112856`):

| test | budget | result |
|---|---|---|
| `registry_measure_10k` — 10 000 entries, 10 MB of ENTRY blobs, push + publish | 10 ms | PASS (whole test 0.06 s incl. process start) |
| `registry_measure_100k` — 100 000 entries, 100 MB | 100 ms | PASS (0.46 s incl. start and allocation of the blobs) |
| `registry_measure_elf` — `--elf` over `serreg_bench` | — | PASS |

The 2-vCPU sandbox misses the 100k budget (158 ms) and sometimes the 10k one; those WARN rows in sandbox reports
are the VM, not the library.

## 3. Half round-trip between two processes over `es::ipc` (`scripts/bench-hrtt-*.sh`)

`bench-hrtt-raw-20261017-124029.txt` and `bench-hrtt-serreg-20261017-124045.txt`: two separately launched
processes, two SPMC variable-size rings in `/dev/shm` (4 KiB pages, no `mlock`), one message in flight, busy spin,
1 000 000 round trips after 100 000 warm-up, three runs each, ping on CPU 19 and pong on CPU 18 — **two E-cores**
(on this part CPUs 12–19 are E-cores), governor `powersave`, HT on.

- **raw**: a 64-byte POD built in the claimed record and read in place, checked by length only.
- **serreg**: the same 64 bytes as `hrtt::msg::Tick`, `serialize()`d into the record through `es::ipc::sink`
  (16-byte frame header + record = 80 wire bytes, one 128-byte ring record either way) and `deserialize`d with
  header, call-hash and length validation.

| half RTT, ns | min | p50 | p90 | p99 | p99.9 | mean |
|---|---|---|---|---|---|---|
| raw, runs 1–3 | 123–125 | **142–147** | 150–154 | 153–161 | 986–1165 | 141.6–145.9 |
| serreg, runs 1–3 | 141–149 | **173–176** | 181–183 | 188–189 | 1197–1292 | 171.8–174.2 |
| **difference** | | **≈ 30 ns** | ≈ 30 ns | ≈ 32 ns | — | ≈ 29 ns |

Reading:

- **The serializer costs ≈ 30 ns per half trip, ≈ 60 ns per round trip** — two `serialize()` and two
  `deserialize()` with full validation, on cache lines that are *cold* (they just arrived from the other core),
  plus 16 more bytes crossing the core-to-core path. That is the number to quote beside the hot-cache micro
  figures of §1 (2.5 + 2 ns): the difference between "L1-hot" and "the line just came from another core" is the
  real-world price, and it is still well under the cost of one cross-core cache-line transfer.
- p50 is the comparable number; the p99.9 column (≈1 µs in both) is the machine — timer interrupts and the
  `powersave` governor — not the library (same tail in raw and serreg).
- The legs are asymmetric in the serreg run (ping→pong ≈ +5 ns, pong→ping ≈ +53 ns over raw, pong turnaround
  ≈ 52 ns in both). The harness timestamps are taken at fixed points of the ping and pong loops, so where the
  encode/decode cost lands depends on the point of measurement, not on the direction; the half-RTT row is the
  one to read. Interleaving the runs (raw, serreg, raw, serreg) would make the per-leg comparison fair.
- These are **E-core, powersave, HT-on** numbers. The same scripts with `--cpus 4,6` (two P-cores, different
  physical cores), the `performance` governor and HT off are expected to show lower absolute values and the same
  ≈30 ns delta; `--huge 2m --lock` removes the page-walk component of the tail (hugetlbfs was not writable for
  the user in this run).

Throughput of the same loop: 3.2–3.3 M round trips/s raw, 2.7–2.8 M serreg (one message in flight; a pipeline
would be bounded by the ring, not the serializer).

## 4. What is not measured yet

- `deserialize(dict, …)` projected vs exact (level-1 evolution), first frame (plan build) and steady state.
- `multimap`/`tuple` encode vs `map`/struct of the same bytes.
- Dictionary-driven readers (`srdump`, `walk_values`) in MB/s; the Python reader in MB/s.
- Compile-time cost per serialized type (seconds per type, size of the `es_serreg_dict` section per type per TU
  — about 250 B is the estimate quoted in the talk; measure it).

## 5. Reproducing

```sh
build/gcc16-release/tests/bench/serreg_bench                       # §1 (pin: taskset -c <P-core>)
build/gcc16-release/tests/bench/registry_measure --entries 100000 --bytes 104857600 --assert   # §2
scripts/bench-hrtt-raw.sh    --cpus 4,6 --runs 3                    # §3, then
scripts/bench-hrtt-serreg.sh --cpus 4,6 --runs 3                    #     same CPUs, back to back
```

Reports land in `logs/bench-hrtt-*.txt`; the first lines record the machine state that decides latency (governor,
SMT, load, huge pages). Attach them to the release's test report.

## Known issue in the harness

The `pinning:` line printed by `serreg_hrtt_*` shows the launching process's own CPU for both sides
("ping cpu 19, pong cpu 19") while the script's run header shows the real pinning ("pong: 18, ping: 19"); each
process prints its own options. Cosmetic; the script's header is authoritative. Fix queued for the next patch (print only
this side's CPU).
