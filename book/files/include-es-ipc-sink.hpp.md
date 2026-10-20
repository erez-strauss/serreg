# `include/es/ipc/sink.hpp`

*(Developed as `include/es/ipc_dynamic/sink.hpp` and renamed in the switch of 2026-10-02.)*
`basic_sink<Unit, Align, DataAlign>` (alias `sink` = `basic_sink<>`): a producer seen as a **claiming sink** —
`claim(n) → std::byte*`, `commit(n) → bool`, `write(p, n) → bool` — the shape `es::serreg::serialize()` writes into
in place ([`es/serreg/sink.hpp`](include-es-serreg-sink.hpp.md), concept `claiming_sink`). The header does not
include es::serreg: any code that wants "claim a span, fill it, commit it" can use it. Chapter 11 §11.3.

Includes: `layout.hpp`, `producer.hpp`, `<cstddef>`, `<cstdint>`.

```cpp
template<std::uint32_t Unit = dynamic, std::uint32_t Align = 64, std::uint32_t DataAlign = 16>
class basic_sink
```
(`sink.hpp:19-20`.)

| member | meaning |
|---|---|
| `basic_sink(producer, type = type_data, publish = true)` | every record this sink writes gets message type `type`. `publish = false` defers the cursor store (`producer::commit(n, false)`): call `flush()` on the producer, or let a later publishing commit do it |
| `claim(n)` | `producer.claim(n, type)`'s span as a pointer, or `nullptr` (too large, a reserved type, or no room under `full_policy::fail`) |
| `commit(n)` | `producer.commit(n, publish)` |
| `write(p, n)` | claim, copy, commit: for code that only has bytes |
| `type(t)`, `type()`, `producer()` | change or read the message type; the producer |

A sink holds a pointer to its producer, which must outlive it. es::ipc_old's `producer` was itself a claiming
sink; es::ipc's `producer::claim` returns `std::expected<std::span<std::byte>, std::errc>` and takes a message type,
so the adaptor carries the type and turns the error into `nullptr`, which `serialize()` reports as
`error::sink_full` with nothing written.

```cpp
es::ipc::sink out{*prod, my_frame_type};
es::serreg::serialize(out, "fill"_lit, fill);        // the frame is built directly in the ring record
```

On the reading side `consumer::poll` hands each frame to es::serreg's readers (`dispatch`, `deserialize`,
`frame_router::route`) as `m.data`, in place. Users: `examples/ipc_ring.cpp`, `examples/ipc_log/producer.cpp`,
`ce-examples/ce-example5`. Test: "ipc: basic_sink — serreg frames are built in the ring and read in place".
