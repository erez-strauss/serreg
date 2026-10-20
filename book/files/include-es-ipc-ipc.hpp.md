# `include/es/ipc/ipc.hpp`

The umbrella header of `es::ipc`: it includes `common.hpp`, `consumer.hpp`,
`layout.hpp`, `producer.hpp`, `region.hpp`, `ring.hpp` and `sink.hpp`. Since the switch of 2026-10-02 (maintainer
decision Q1) `es::ipc` is the **variable-size ring** library — developed as `es::ipc_dynamic` — : shared-memory
regions (normal or explicit huge pages) and an SPSC queue and an SPMC broadcast (up to 256 readers) over one layout,
with 32-bit unit positions, a 16-byte record header `{size, seq, type, check}`, skip records at the end of the
buffer, zero-copy `claim`/`commit` for the writer and `peek`/`poll` for readers, and wrong data never crashing a
reader. The earlier fixed-slot ring is `es::ipc_old`
([`ipc_old.hpp`](include-es-ipc_old-ipc_old.hpp.md)). Chapter 11; design `docs/ipc/ipc-design.md`.

| header | page |
|---|---|
| `common.hpp` | [shared vocabulary](include-es-ipc-common.hpp.md): `cache_line`, `full_policy`, intervals, fences, `detail::load/store/cas` |
| `region.hpp` | [regions](include-es-ipc-region.hpp.md): named (tmpfs, hugetlbfs) and anonymous shared memory |
| `layout.hpp` | [layout](include-es-ipc-layout.hpp.md): geometry, ring header, gates, record header, unit arithmetic |
| `ring.hpp` | [`basic_ring`](include-es-ipc-ring.hpp.md): create, attach, the gate scan, evict, reap |
| `producer.hpp` | [`basic_producer`](include-es-ipc-producer.hpp.md): claim, commit, flush, skip records |
| `consumer.hpp` | [`message`, `basic_consumer`](include-es-ipc-consumer.hpp.md): join, peek, poll, validation |
| `sink.hpp` | [`basic_sink`](include-es-ipc-sink.hpp.md): a producer as an es::serreg claiming sink |

It is header-only, needs C++23 (`std::expected`, `std::atomic_ref`, `std::start_lifetime_as` when available) on
Linux, and needs no reflection. CMake: `target_link_libraries(app PRIVATE es::ipc)`, an INTERFACE target with
`cxx_std_23`; pkg-config `es-ipc`. `es::serreg` does not bring it: a program that uses both links both.
`es/serreg/serreg.hpp` does **not** include it: include `<es/ipc/ipc.hpp>` explicitly. The install check of
`scripts/run-all-tests.sh` builds an `es::ipc`-only consumer to prove that no reflection flag leaks into it.
