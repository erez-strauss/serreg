# `include/es/ipc/common.hpp`

The vocabulary shared by every `es::ipc` header. It pulls in only `<atomic>`, `<cerrno>`,
`<cstddef>`, `<cstdint>`, `<signal.h>` and `<sys/types.h>` (the last three for `process_gone`). It is included by `region.hpp`, `layout.hpp`, `ring.hpp`, `producer.hpp` and `consumer.hpp`, and so by everything in `es/ipc/`; the old slot rings in `es/ipc_old/` include it as `<es/ipc/common.hpp>` and take its names into `es::ipc_old` with using-declarations.

| name | kind | meaning |
|---|---|---|
| `cache_line` | constant 64 | alignment of every independently written shared field. Deliberately fixed rather than `std::hardware_destructive_interference_size`, because it is part of the cross-process layout |
| `yield_interval` | constant 1024 | blocked spins between `sched_yield()` calls (producer `claim`, consumer `wait`) |
| `reap_interval` | constant 2²² | blocked producer spins between checks for dead consumer processes |
| `full_policy` | enum `spin`, `fail` | what `producer::claim` does on a full ring (both libraries) |
| `cpu_relax()` | inline function | `pause` on x86, `yield` on AArch64, nothing elsewhere |
| `fence_release()`, `fence_acquire()` | inline functions | the fences of the overwrite checks (es::ipc: `consumer`'s `intact()` and `read_record()` order their loads before the ownership check; es::ipc_old: the seqlock). They compile to nothing under `__SANITIZE_THREAD__`, because GCC rejects `atomic_thread_fence` with `-fsanitize=thread` (`-Wtsan`, an error under `-Werror`) |
| `detail::load/store/cas` | function templates | the only way shared integers are touched: `std::atomic_ref<T>` with the given order (`cas` is acq_rel). A `static_assert` requires lock-free `atomic_ref` for 64-bit, 32-bit and signed 32-bit integers |
| `detail::process_gone(pid)` | inline function | `kill(pid, 0)` failed with `ESRCH`: no such process. `EPERM` (exists, other user) is *not* gone. Used by `reap_dead_consumers`, `producer::attach` and `consumer::join` to take over roles of crashed processes (G2-3) |
| `detail::align_up(n, a)` | constexpr | rounds `n` up to a multiple of `a` |

Why `std::atomic_ref` over plain integers rather than `std::atomic<T>` members: the memory is created by one
process and used by others, so no process "constructs" objects in it. `atomic_ref` gives atomic access to
suitably aligned integers without depending on object lifetime across processes.
