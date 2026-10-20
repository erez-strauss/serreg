# 11. `es::ipc`: shared-memory rings and half-RTT benchmarks

`es::ipc` is the library's inter-process and inter-thread transport. It is the **variable-size ring** library
(maintainer decision Q1), developed as `es::ipc_dynamic`. It provides:
- **regions**: named shared memory on tmpfs or hugetlbfs, or an anonymous shared mapping, with normal pages or
  explicit 2 MiB/1 GiB huge pages, optionally `mlock`ed;
- **one ring layout, two kinds**: `ring_kind::spmc`, a single-producer / multi-consumer *broadcast* where every
  consumer reads every message (up to 256 consumers), and `ring_kind::spsc`, a single-producer / single-consumer
  *queue* that keeps messages while no consumer is attached and resumes where the last consumer stopped;
- **variable-size records**: a 16-byte header `{size, seq, type, check}` and the payload, rounded up to units of 16,
  32 or 64 bytes; one message may be as large as the whole ring, and small and large messages share it;
- **zero-copy access** on both sides: `claim`/`commit` a record in place, `peek`/`poll` it in place;
- **never crashing on wrong data**: every record header is validated before a reader forms a span.

The geometry (unit size, capacity, consumer count) can be chosen **at run time** and is stored in the ring, so a
process that attaches needs no compile-time knowledge of it; the unit can also be fixed at compile time. `es::ipc`
is header-only C++23 for Linux. It does not depend on es::serreg and needs no reflection flags (CMake target
`es::ipc`). `es::ipc::sink` adapts a producer to es::serreg's `claiming_sink` concept, so `serialize()` writes frames
straight into ring records.

The fixed-slot rings that were `es::ipc` earlier (and before them `es::serreg::disruptor`), **`es::ipc_old`**, are
out of the build (G-17; §11.7 says why they were replaced). Design and results of the new library:
`docs/ipc/ipc-design.md`, `docs/ipc/latency-analysis.md`, `docs/ipc/testing-and-benchmarks.md`.

`es::ipc` is a library of its own in the serreg package (decision G), and `es::serreg` does not bring it: a program
that uses both links both (`target_link_libraries(app PRIVATE es::serreg es::ipc)`, as `examples/ipc_ring` does).
Ways to get it:

```cmake
find_package(serreg CONFIG REQUIRED COMPONENTS ipc)   # es::ipc: header-only, C++23
target_link_libraries(app PRIVATE es::ipc)
```

- **`COMPONENTS ipc`** loads only `serregIpcTargets.cmake`, the export set of `es::ipc`; without
  components, `find_package(serreg)` provides every library the installation has.
- **`-DES_SERREG_IPC_ONLY=ON`** configures and installs only `es::ipc` (its headers, its export
  set, the package config files and `es-ipc.pc`) and stops before the compiler check, so no reflection compiler is
  needed.
- **pkg-config:** `es-ipc` (`Cflags: -I… -std=c++23`, `Libs: -lpthread`).

`files/build-system.md` has the package layout. Per-file pages: [`ipc.hpp`](files/include-es-ipc-ipc.hpp.md) lists
them all.

## 11.1 Regions: where the ring lives

```cpp
#include <es/ipc/ipc.hpp>
namespace ipc = es::ipc;

auto a = ipc::region::create("fills", bytes);                                        // /dev/shm/fills
auto b = ipc::region::create("fills", bytes, {.pages = ipc::pages::huge_2m, .lock = true}); // <hugetlbfs>/fills
auto o = ipc::region::open("fills");            // another process: /dev/shm, then every hugetlbfs mount
auto t = ipc::region::anonymous(bytes, {.pages = ipc::pages::huge_2m});   // threads / fork children
```

| | named region (between unrelated processes) | anonymous region (threads, `fork` children) |
|---|---|---|
| `pages::normal` | a file in `/dev/shm` (tmpfs): `open(O_CREAT\|O_EXCL\|O_NOFOLLOW\|O_CLOEXEC)`, `ftruncate`, `mmap(MAP_SHARED\|MAP_POPULATE)` | `mmap(MAP_SHARED\|MAP_ANONYMOUS\|MAP_POPULATE)` |
| `pages::huge_2m` / `huge_1g` | a file on the hugetlbfs mount whose page size matches (found in `/proc/mounts`, or `.dir`) | `… \| MAP_HUGETLB \| MAP_HUGE_2MB/1GB` |

**Why not `shm_open()`.** On Linux `shm_open("/x")` is `open("/dev/shm/x")` and nothing more, and it cannot
reach hugetlbfs, which shared huge pages require. `shm_open` does guarantee one useful thing: the memory is RAM,
not a disk file subject to writeback. `region` keeps that guarantee with `fstatfs()`. A named region is accepted
only on tmpfs (normal pages) or on hugetlbfs with exactly the requested page size. Any other directory, such as
an ext4 path passed as `.dir`, gives `errc::invalid_argument`, and the file just created is unlinked.

**Huge pages are explicit.** They come from the pool the administrator reserved: `vm.nr_hugepages` for 2 MiB
pages, or `hugepagesz=1G hugepages=N` on the kernel command line. hugetlbfs reserves the pages at `mmap()`, so a
pool that is too small shows up as `errc::not_enough_memory` from `create()` or `anonymous()`, never as a silent
fallback to normal pages. Transparent huge pages are not used. `page_size()` reports what the mapping really
has, and the benchmark report prints it.

**Sizes and faults.** The size is rounded up to the page size. Every mapping is `MAP_POPULATE`, so the first
touch never takes a page fault, and `.lock = true` also `mlock()`s it (which needs `ulimit -l`/`RLIMIT_MEMLOCK`
or `CAP_IPC_LOCK`). If the lock fails, the error is returned. On a size-limited tmpfs (Docker's 64 MiB `/dev/shm`)
`ftruncate` and `MAP_POPULATE` succeed beyond the limit and a later touch is a `SIGBUS`, so (I-05) `create()` also
calls `posix_fallocate()` for normal pages: a shortage of space is an error of
`create()`, not a signal at run time.

**Names and cleanup.** Names may carry a leading `/` (the `shm_open` style). An inner `/`, `.`, `..` or an empty
name is refused. The creator owns the name and unlinks it on `close()` or destruction, unless `keep()` is called.
Only the creating process unlinks (I-06): a `fork`ed child that inherited the region object and exits
normally leaves the name in place.
`region::remove(name)` performs the same search as `open()`. A creator that crashes leaves its file behind, and
on hugetlbfs such a file keeps its huge pages reserved until someone removes it. For that reason the benchmark
scripts delete stale benchmark ring files before each run.

## 11.2 The rings

```cpp
#include <es/ipc/ipc.hpp>
namespace ipc = es::ipc;

const ipc::geometry g = ipc::ring::for_capacity(64 << 20, 64, 4);   // 64 MiB of records, 64-byte units, 4 readers
auto mem  = ipc::region::create("feed", ipc::ring::bytes_for(g));
auto ring = ipc::ring::create(*mem, g, ipc::ring_kind::spmc);        // std::expected<ring, std::errc>
auto prod = ipc::producer::attach(*ring);
auto cons = ipc::consumer::join(*ring);

// another process:
auto m  = ipc::region::open("feed");
auto r2 = ipc::ring::attach(*m, ipc::ring_kind::spmc);              // the geometry is read from the ring
```

`ring` is `basic_ring<>`, a cheap, copyable, non-owning handle to a ring in some memory. That memory can be a region
or any buffer aligned to `ring::mem_align` = max(64, `Align`), and it must outlive every handle, producer and
consumer made from it. `producer` and `consumer` are move-only and the same types for both ring kinds
([`ring.hpp`](files/include-es-ipc-ring.hpp.md)).

**Template parameters** (`basic_ring<Unit, Align, DataAlign>`, and the same three on `basic_producer`,
`basic_consumer` and `basic_sink`):

| parameter | default | meaning |
|---|---|---|
| `Unit` | 0 (`dynamic`) | the unit of positions and record sizes: 0 = taken from the geometry at run time; 16, 32 or 64 = fixed at compile time (`attach()` refuses a ring of another unit) |
| `Align` | 64 | record alignment, a power of two in 16 … 4096; records start on a `granule` = max(unit, Align) boundary |
| `DataAlign` | 16 | the payload's offset in a record: 16 (the header and up to 48 payload bytes share a cache line) or 64 (the payload starts its own line; needs `Align ≥ 64`) |

All three are layout properties recorded in the ring header and checked by `attach()`.

**Geometry.** `geometry{unit_bytes, units, max_consumers}`: a unit of 16, 32 or 64 bytes; `units` a power of two
up to 2³¹; 1 … 256 consumers. Positions are 32-bit counts of units that wrap modulo 2³², so a ring can hold up to
128 GiB with 64-byte units. `for_capacity(bytes, unit, consumers)` rounds a byte capacity up to a valid geometry.
`max_message()` is the whole ring minus `DataAlign` (at most 2³² − 1). **Sizing rule** (design §5.3): make the
capacity at least twice the largest message — for an application that sends its dictionary in band, twice
`max(largest message, dictionary)` — so that a large record never waits for more than one ring's worth of drained
data.

**Memory layout** (layout 2, `ring_version` 2; details on the [`layout.hpp`](files/include-es-ipc-layout.hpp.md)
page):

```
offset 0      ring_header   magic "IPCRING2" (stored last), version 2, kind, unit, align, units, max_consumers,
                            data_offset, total_bytes, data_align
       64     cursor        u32: position after the last published record                         own cache line
       128    producer      i32: pid holding the producer role, 0 = none                          own cache line
       192    tickets       u32 ownership ticket counter; +4: u32 hwm, the gates ever used        own cache line
       256    gates         max_consumers × {u64 own = ticket << 32 | state, u32 pos, i32 pid, u32 pid_ticket}
       data   records       units × unit; a record = {u32 size, u32 seq, u32 type, u32 check} + payload at +DataAlign
```

Everything in the ring is a plain integer at a fixed offset, accessed only through `std::atomic_ref`; the memory
holds no C++ object owned by one process and no pointers, so any number of processes can map it at different
addresses. `attach()` copies the header, validates the copy (magic with acquire — 0 means "still being built",
`errc::resource_unavailable_try_again`; version, `errc::not_supported`; kind, unit, alignments, geometry and derived
offsets, `errc::invalid_argument`; region size, `errc::no_buffer_space`) and builds the handle from it.

### SPSC and SPMC semantics

| | `ring_kind::spmc` (broadcast) | `ring_kind::spsc` (queue) |
|---|---|---|
| consumers | up to `max_consumers` (≤ 256); **each reads every message** published after it joined | exactly one at a time; `join()` of a second one fails with `errc::device_or_resource_busy` |
| no consumer attached | the producer runs free: messages go to nobody | messages are **kept**; the producer blocks (or fails, with `full_policy::fail`) when the queue is full |
| the producer is gated by | the slowest *active* consumer | the queue's one gate, owned or not |
| a consumer leaves / restarts | its gate is freed; a new consumer starts at the cursor | the position stays; the next consumer continues **exactly** after the last released message |
| a consumer process dies (`kill -9`, no `leave()`) | its gate is reaped: by a blocked producer, by `reap_dead_consumers()`, and by a `join()` that found every gate taken | ownership is cleared, the position kept; a message the dead consumer had peeked but not released is delivered again (at-least-once) |
| the producer process dies | the next `producer::attach()` takes the role over and continues at the cursor; a record it had claimed but not committed was never visible | the same |
| `ring.evict(id)` | the producer stops waiting for that consumer **at once**; the consumer sees `lapped()` and frees the gate | refused: a queue never drops messages |

## 11.3 Writing: claim, commit, sink

```cpp
if (auto s = prod->claim(n, /*type*/ 7)) {      // std::expected<std::span<std::byte>, std::errc>
    build_into(*s);                              // exactly n bytes, in the ring record
    prod->commit(used);                          // used <= n; one release store of the cursor
}
prod->write(bytes, n, 7);                        // copy convenience
prod->publish(max, 7, [&](std::span<std::byte> r) { return fill(r); });   // or return ipc::abandon
prod->emplace<quote>(quote{...});                // a trivially copyable T built in place, type_data

ipc::sink out{*prod, /*type*/ 2};                // es::serreg claiming sink
es::serreg::serialize(out, "fill"_lit, fill);    // the frame is built in the record
```

([`producer.hpp`](files/include-es-ipc-producer.hpp.md), [`sink.hpp`](files/include-es-ipc-sink.hpp.md).)

- **Message types.** Every record carries a `u32` type. Applications use `0 … 0xFFFFFEFF` (`type_data` = 0 is the
  default); `0xFFFFFF00` and above are the library's, and a claim with one is `errc::invalid_argument`.
- **`claim(n, type)`** reserves one contiguous record and returns a span of exactly `n` payload bytes. Errors:
  `errc::message_size` (`n > max_message()`), `errc::resource_unavailable_try_again` (no room under
  `full_policy::fail`, or always for `try_claim`). A claim that is not committed is abandoned by the next `claim()`
  or by `cancel()`.
- **`commit(n, publish = true)`** writes the header and publishes with one release store of the cursor. It returns
  `false`, publishing nothing, without an open claim or for `n` above the claim. `commit(n, false)` defers the store;
  `flush()` publishes every deferred record with one store (batching).
- **Skip records.** When a record does not fit before the end of the buffer, the producer pads the rest of the
  buffer with a record of type `type_skip`, publishes it at once, and starts the record at the beginning. Readers
  never see skips.
- **Full ring.** Under `full_policy::spin` (default) the producer spins with a pause, a `sched_yield()` every
  `yield_interval` (1024) spins and a check for dead consumers every `reap_interval` (2²²) spins; under
  `full_policy::fail` it returns the error, and reaps dead consumers on every 1024th refusal. Through `sink`, a
  failed claim is `nullptr`, which `serialize()` reports as `error::sink_full` with nothing written.
- The producer caches a limit and rescans the gates only when it reaches it, so in steady state it reads no shared
  line but its own. Deferred records are published before every rescan.

## 11.4 Reading: peek, poll, message

```cpp
cons->poll([](const ipc::message& m) { handle(m.type, m.data); });    // in place; one gate store per batch
cons->poll_as<quote>([](const quote& q) { ... });                     // typed view; other lengths: mismatched()
if (auto m = cons->peek()) { handle(m->type, m->data); cons->release(); }
auto m = cons->wait();                                                 // spin until a message (or lapped)
```

([`consumer.hpp`](files/include-es-ipc-consumer.hpp.md).) A `message` is `{std::span<const std::byte> data,
std::uint32_t type, std::uint32_t position}`: the payload in place, valid until it is released. es::serreg's readers
(`deserialize`, `dispatch`, `frame_router::route`, `view<T>`) take `m.data` directly.

- **`peek()`** returns the next message or `nullopt`; **`release()`** hands it back (`false` when there was none, or
  when it was overwritten while held). **`poll(f, max)`** calls `f` for up to `max` messages and releases them with
  one gate update; `f` may return `bool` to stop early. **`poll_as<T>(f)`** gives a typed view
  (`std::start_lifetime_as` when available) of each message of exactly `sizeof(T)` bytes; `T` must be trivially
  copyable with `alignof(T) ≤ DataAlign`.
- **Progress is a plain release store** of the gate's position — no locked instruction (§11.5). Ownership lives in a
  separate word that only compare-and-swap changes (*free → joining → active*, *evicted*, back to *free*). An evicted
  gate stops holding the producer at once, whatever its owner still stores, and is reused only after its owner has
  acknowledged the eviction, which it does after its last store.
- **Lapped.** A consumer checks that it still owns its gate before each record and again after reading it. A
  consumer that was evicted or reaped stops for good: `lapped()` is true, it never moves the gate again, and an
  evicted one frees its gate as it notices. `join()` again to continue.
- **Validation.** Before a record is handed out, its header must carry the expected position (`seq`) and a matching
  `check`, its size must fit the ring, and it must end before the end of the buffer and before the cursor; a skip
  must reach the end of the buffer exactly. A record that fails is counted in `corrupt()` and the consumer skips to
  the cursor (the next record's position is unknown). Nothing is read past the buffer or the cursor.
- **Joining an SPMC ring** keeps the store-buffering fix of the slot rings (their review finding I-01): the joiner
  activates its gate (`seq_cst`) and then re-reads the cursor; the producer's gate scan begins with a `seq_cst`
  read-modify-write of the cursor. The producer cannot be a whole ring ahead of a new consumer.
- **Crashes, pid reuse, fork.** Roles are pids in shared memory; `attach()`/`join()` take over a role whose pid no
  longer exists (`kill(pid, 0)` → `ESRCH`). A gate's `pid_ticket` ties its pid to the owner's ticket, so a reaper
  never frees a new owner's gate because of an earlier owner's dead pid. A pid reused by an unrelated process keeps
  the role until that process exits. A handle copied into a child by `fork()` is inert in `detach()`/`leave()`, but
  the child must not *use* it: fork first, then `attach`/`join` in each process (as `examples/ipc_ring.cpp` does).

## 11.5 Latency: what the reader's progress costs

`docs/ipc/latency-analysis.md` is the investigation of 2026-10-02 into why small messages were slower on the new
rings than on the slot rings. Its findings, which shaped layout 2:

- **The CAS's full barrier.** The first version published a reader's progress with a CAS of the gate word. The
  cost was not an extra cache line but the full barrier every locked instruction implies: it waits until the
  core's store buffer has drained. In a ping-pong the reader has just written its reply into the other ring, so the
  barrier waits for those transfers, and the wait lands on the round trip — 40–60 ns of half-RTT on small messages.
  A plain store followed by `atomic_thread_fence(seq_cst)` cost the same as the CAS (its variant V7). Layout 2 (§11.4)
  makes a plain release store safe for both kinds, so neither SPSC nor SPMC readers pay it any more.
- **Units.** 16- or 32-byte units do not help small-message latency. With the default `Align = 64` the granule is 64
  bytes whatever the unit, so the layout does not change; `Unit 16 / Align 16` was consistently 20–25 ns slower,
  because packed records straddle cache lines and two lines move per message. Small units are a memory-density
  choice for mixed sizes; for the lowest latency use `basic_ring<64, 64>`. A compile-time unit against `Unit = 0`
  measured +0–25 ns, inconsistently: below the measurement floor.
- **HT off.** Only the numbers taken with hyperthreading off are quoted (threads pinned to two P-cores of the
  sandbox's i9-13900H); with hyperthreading on everything was 10–40 ns slower and noisier.
- **The measurement floor.** Two binaries that differ only in code a test does not execute differed by up to
  ~20 ns at p50 (86 vs 109 ns with identical ring code: code placement). Effects below ~20 ns are not resolved by
  these A/B experiments, which were timed without `perf`; `perf stat` counters on the reference box must settle them.
- **Alignment.** One bug was found and fixed: `create()`/`attach()` checked only 64-byte alignment, so `Align` > 64 in
  a 64-aligned buffer gave misaligned records; both now require `mem_align`. Payloads are 16-byte aligned by design
  (`DataAlign` 16), or 64 on request.

Consumer-side detail from the benchmark work (`docs/ipc/testing-and-benchmarks.md` §3, finding P1): returning
`std::optional<record>` from the header check made GCC build it with partial stores and reload it whole, a
store-forwarding stall of about 8 ns per message; the reader returns a plain struct.

## 11.6 The half-RTT benchmarks

```
      ping ──[ ring <name>-a2b ]──▶ pong        ping: t0, encode in the record, commit
      ping ◀──[ ring <name>-b2a ]── pong        pong: t1 when seen → decode → t2 → encode the reply in the other ring
                                                ping: decode the reply → t3
half RTT = (t3 - t0) / 2      one-way ping→pong = t1 - t0      one-way pong→ping = t3 - t2      turnaround = t2 - t1
```

The setup is two processes and two es::ipc rings, SPMC with one
consumer each by default, or SPSC queues with `--ring spsc`. Exactly one message is in flight: ping sends message
`i+1` only after reply `i` has been decoded. `CLOCK_MONOTONIC` is machine-wide, so the one-way times are real
measurements; the report prints the cost of one clock read. Warm-up round trips are discarded, and the samples are
sorted once at the end. Readers `peek()` the record in place and `release()` it after the reply was built (pong) or
decoded (ping).

| | `serreg_hrtt_raw` | `serreg_hrtt_serreg` |
|---|---|---|
| content | 64 bytes: `seq, t0, t1, t2, order_id, px, qty, sym[12]` | the same fields as the reflected struct `hrtt::msg::Tick` |
| writing | `claim(sizeof(raw_packet))`, the packet constructed in place, `commit` | `serialize(sink, tick)` through `es::ipc::sink`: the 16-byte header and the 64-byte record built in the ring record (80 B) |
| reading | a typed view of the record (`start_lifetime_as`); length check only | `deserialize(frame, tick)`: header, format version, call hash and length validated, then the record copied out |
| the difference measures | — | es::serreg's typed framing and validation, on both sides of both legs |

Options (both programs):

| option | meaning |
|---|---|
| `--role both\|ping\|pong` | `both` (default): ping here, and pong forked (`--mode process`, default) or on a thread (`--mode thread`, anonymous regions). `ping` creates the two named regions and rings; `pong` opens them by name and reads the geometry and kind from the rings |
| `--slots N`, `--slot-bytes B` | ring room for `N` messages of up to `B` bytes: a capacity of at least `N × align_up(16 + B, 64)` bytes, rounded up by `ring::for_capacity` (default 1024 × 256, i.e. 512 KiB of records). Only ping, the creator, needs them |
| `--ring spmc\|spsc` | SPMC rings with one reader (default) or SPSC queues; pong learns the kind from the ring header |
| `--huge 2m\|1g` | rings on explicit huge pages: a hugetlbfs file for processes, `MAP_HUGETLB` for threads. The pool must be reserved |
| `--lock` | `mlock()` the rings |
| `--iters N`, `--warmup N` | measured round trips (1 000 000) and discarded warm-up (100 000) |
| `--cpus A,B`, `--cpu N`, `--pause`, `--timeout SEC`, `--samples FILE`, `--quiet` | pinning, spin with a pause hint, how long to wait for the peer, per-sample output, `ok: N round trips` only |

Exit codes: 0 ok, 2 usage (including a message that does not fit the ring), 3 cannot attach or join, 4 a reply
failed to decode or carried the wrong sequence or timestamp, 5 lapped, 6 rings or peer not available in time. If
ping fails in fork mode, it stops its pong child.

For orientation only (sandbox, Release, pinned to two P-cores, hyperthreading off, 1 000 000 iterations, default
SPMC ring; reported by the port of 2026-10-02, not a reference-box result): half-RTT p50/p99 raw 115/136 ns
(thread) and 117/140 ns (process), serreg 141/175 ns (thread) and 140/174 ns (process).

### Scripts

`scripts/bench-hrtt-raw.sh` and `scripts/bench-hrtt-serreg.sh` (shared body `scripts/bench-hrtt-lib.sh`) build
the benchmark in `gcc16-release`. They pick two CPUs on different physical cores of one package (never CPU 0),
and record the machine state that decides latency: CPU model, SMT, the governor and frequency of the two CPUs,
`no_turbo`, `isolcpus`/`nohz_full` from the kernel command line, load, the huge-page pool
(`HugePages_Total/Free`), the hugetlbfs mounts and `ulimit -l`. They remove stale ring files, then run `--runs`
times (default 3) as **two separately launched processes**: pong first, then ping. Output goes to
`logs/bench-hrtt-<raw|serreg>-YYYYMMDD-HHMMSS.txt` and ends with a per-run summary.

```sh
scripts/bench-hrtt-raw.sh                                     # 3 × 1 000 000 round trips, normal pages
scripts/bench-hrtt-serreg.sh --huge 2m --lock --runs 5        # 2 MiB huge pages, locked
scripts/bench-hrtt-serreg.sh --slots 64 --slot-bytes 128 --threads --samples
```

Huge pages on a Docker box: reserve pages on the host
(`echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`). A benchmark run needs 2 pages
(one per ring at the default geometry) and the huge-page unit test (`ipc region: explicit 2 MiB huge pages …` in `serreg_ipc_tests`) needs 2. Then give the container a hugetlbfs **the
benchmark's user can write to**. A hugetlbfs mount is `root:root 0755` by default, so a non-root user gets
`permission_denied`. Either `sudo chmod 1777 /dev/hugepages` (sticky, like `/tmp`), or mount it for that user:
`mount -t hugetlbfs -o pagesize=2M,uid=$(id -u),gid=$(id -g),mode=0770 none /dev/hugepages`. For a container,
`-v /dev/hugepages:/dev/hugepages` with the same permissions on the host, or a mount inside it (which needs
privileges). With several 2M mounts, `region::create` uses the first one the user can write to. Without any of
this, `--huge 2m` fails with the error rather than running on normal pages.

### Reading a report

- **p50 half RTT** is the number to compare between the two programs, and between normal and huge pages. On an
  idle, pinned machine it is dominated by cache-line transfer between the two cores.
- **serreg − raw at p50**, and more directly the **turnaround** row (pong's decode plus encode), is es::serreg's
  per-message cost over raw zero-copy access.
- **p99.9 and above** measure the machine: interrupts, SMT siblings, frequency changes, TLB misses and page faults.
  Huge pages and `--lock` target the last two. The environment block of the report records the rest.

The ctests `hrtt_{raw,serreg}_smoke_{process,thread}` run 2 000 round trips of each program in each mode. They
check only that every reply arrives in order, with the right timestamp, and that nobody is lapped. Timing is
never checked in ctest.

### The old-vs-new benchmark

The comparison benchmark (`ipc_compare_bench.cpp`, built while `es::ipc_old` was in the tree; now under
`remove-me/tests/bench/`, deleted with the headers in 2.3.0) measured es::ipc_old against es::ipc in
one binary, every message verified (sequence, length, checksum): a half-RTT ping-pong over two rings (es::ipc_old
`spsc_ring` / `spmc_ring` with one consumer, es::ipc SPSC / SPMC; 32- and 256-byte messages; pong as a thread or a
`fork()`ed process), and producer → consumer throughput for a mixed size distribution (95 % 64 B, 4.9 % 1 KiB,
0.1 % 256 KiB) and for 64-byte messages only, with the ring memory each library needs. Options: `--quick`,
`--iters N`, `--warmup N`, `--msgs N`, `--repeat N`, `--cpus A,B`, `--mode thread|process|both`,
`--only FILTER[,…]`. Its last line was `ipc compare bench: ok` only when every run verified. The numbers below come from release
builds on an idle machine, pinned to two cores of one socket.

## 11.7 es::ipc_old, and why it was replaced

`es::ipc_old` was the fixed-slot SPMC broadcast ring and SPSC queue that were `es::ipc` before the variable-size
rings: run-time geometry
`{slot_bytes, slots, max_consumers}`, u64 message sequence numbers, a 16-byte slot head with a seqlock stamp, and
random access by sequence number. It stayed in the tree unused for a few releases (maintainer decision Q1) and
left the build with G-17: its headers, its 26-case suite and the comparison benchmark wait under `remove-me/` until
2.3.0. Its region tests — tmpfs, huge pages, I-06, I-08 — exercised es::ipc's `region` and moved into
`serreg_ipc_tests` ("ipc region: …").

**Why it was replaced.** Every slot is as large as the largest message. A feed of 64-byte ticks with an occasional
large snapshot needs slots of the snapshot's size, so the ring is almost all waste; a whole es::serreg dictionary
(tens of KB, for the in-band dictionary of `examples/ipc_log`) cannot travel through a 256-byte slot at all
(`docs/ipc/ipc-design.md` §1). The variable-size ring lets small and large messages share one ring.

**The measured price** (`docs/ipc/ipc-design.md` §9.1–§9.2; sandbox, hyperthreading off, the project bench pinned
to CPUs 4 and 6, three repetitions, two invocations — §9.2 with today's `serreg_ipc_compare_bench`; indicative, not
reference-box numbers):

- **Mixed sizes:** about **3× the throughput at 1/16 of the memory**: 22–32 against 7–11 Mmsg/s in the first
  comparison (§9.1), 23–27 Mmsg/s from a 4 MiB ring against 9–11 Mmsg/s from 64 MiB of slots after layout 2 (§9.2).
- **64-byte messages only:** es::ipc a steady 21–27 Mmsg/s; es::ipc_old 10–11, or 73–80 in a fast mode it enters
  only sometimes (bimodal, not resolved).
- **Small-message latency:** es::ipc is still about **15–25 ns behind** in the project bench — 32-byte half-RTT p50
  90–102 ns (thread) and 87–97 ns (process) against es::ipc_old's 65–81 and 71–75 ns; p99 152–167 against
  112–150 ns; 256 bytes 121–151 against 103–121 ns at p50. In the two-thread ping-pong microbenchmark of
  `latency-analysis.md` the two are at parity (81–118 against 80–114 ns, design §9.2). The residual is below what the
  sandbox can attribute without `perf` (§11.5, the measurement floor).

The maintainer switched at that price (design §9.2: the IPC is not the package's main delivery; random access by
sequence number is not needed).

| | es::ipc_old (slots) | es::ipc (variable-size records) |
|---|---|---|
| message size | ≤ `slot_bytes`; every slot as large as the largest message | ≤ the whole ring; mixed sizes share it |
| positions | u64 message counts | u32 unit counts (rings up to 128 GiB) |
| per-message overhead | 16-byte slot head; the slot rounded to 64 | 16-byte record header; the record rounded to the granule |
| consumer progress | release store per batch | release store per batch (layout 2) |
| an evicted consumer | can still store into its gate (review finding I-02) | cannot hold the producer; its gate is reused only after it acknowledged |
| lap detection | per-slot seqlock stamp | gate ownership + the header's `seq` |
| corrupt peer data | `len > slot` → `corrupt()` | full header validation → `corrupt()`, skip to the cursor |
| random access by sequence | yes | no (records are walked) |

## 11.8 Maintaining it

- The ring layout is an ABI between processes built at different times. A layout change bumps
  `detail::ring_version` (now 2), and `attach` then refuses rings of other versions. es::ipc_old has its own
  `ring_version` (1) and magic (`"IPCRING1"`), so neither library attaches the other's rings.
- `cache_line` is fixed at 64. `std::hardware_destructive_interference_size` may differ between the compilers
  that built two processes.
- Only `std::atomic_ref` touches shared integers. Gate ownership changes only by CAS; a reader's position only by a
  plain release store of its owner. Nothing on the claim/commit/peek/release/poll path allocates, locks or makes a
  system call, except `sched_yield()` and `kill(pid, 0)` while *blocked*.
- Tests: `tests/unit/test_ipc.cpp` (executable `serreg_ipc_tests`, kept separate so its types stay out of
  `serreg_tests`' golden registry; test names "ipc: …", "ipc SPSC: …", "ipc SPMC: …", "ipc threads: …",
  "ipc processes: …"). It covers geometry and attach validation (every header field, the 256-reader limit, the
  memory alignment); every boundary of the skip logic, records of `max_message()` and rings above 4 GiB; the wrap
  across 2³²; types, policies, deferred commits, cancel and abandon; SPSC persistence and hand-over; SPMC broadcast,
  joins while running, eviction and layout 2's plain-store progress; forged headers; threads and `fork`ed processes
  with every byte verified for units 16, 32 and 64; dead-reader reaping and producer take-over; `sink` with
  `es::serreg::serialize`; `DataAlign` 64; `emplace<T>`/`poll_as<T>`. `docs/ipc/testing-and-benchmarks.md` has the
  results, including the many-core stress and kill harnesses. `serreg_ipc_tests` links `es::ipc` only.
- The region cases are the "ipc region: …" tests of the same executable: "ipc region: create/open/remove on tmpfs,
  and only on tmpfs or hugetlbfs", "ipc region: only the creating process unlinks a named region" (I-06),
  "ipc region: open() refuses a region file writable by others unless foreign_owner" (I-08) and "ipc region:
  explicit 2 MiB huge pages — named (hugetlbfs) and anonymous (MAP_HUGETLB)" (SKIP where the machine has no
  hugetlbfs mount, no free huge pages or no writable mount). es::ipc_old's own 26 cases — among them the review
  fixes I-01, I-04, I-07, I-13 — left the build with it (G-17).
- Keep `es::ipc` free of es::serreg includes and of reflection: `tests/headers` compiles every `include/es/ipc` and
  `include/es/ipc_old` header alone as C++23 against `es::ipc`/`es::ipc_old` only (`serreg_headers_ipc`), and the
  install stage of `scripts/run-all-tests.sh` installs with `ES_SERREG_IPC_ONLY=ON` and builds a `COMPONENTS ipc`
  consumer.
