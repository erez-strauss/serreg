// es/ipc/ipc.hpp — umbrella header of es::ipc: shared-memory regions (normal or explicit huge pages) and the
// variable-size rings — an SPSC queue and an SPMC broadcast (up to 256 readers) — with 32-bit unit positions, a
// 16-byte record header {size, seq, type, check}, skip records at the end of the buffer, zero-copy claim/commit for
// the writer and peek/poll for readers, and wrong data never crashing a reader. Header-only, C++23, Linux, no
// reflection; es::ipc::sink is an es::serreg claiming sink, so es::serreg::serialize() builds frames in place.
// Design: docs/ipc/ipc-design.md. CMake target: es::ipc. (Until 2.1.13 es::ipc was the fixed-slot ring, now es::ipc_old.)
// One PID namespace: liveness of a producer or consumer is kill(pid, 0); processes in other PID namespaces that
// share the mapping neither reap a dead owner reliably nor are reliably seen as alive (ipc-design.md §5.5;
// pid + start time planned for 2.3.0 with a ring_version bump).
#pragma once

#include "common.hpp"
#include "consumer.hpp"
#include "layout.hpp"
#include "producer.hpp"
#include "region.hpp"
#include "ring.hpp"
#include "sink.hpp"
