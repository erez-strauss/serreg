#!/usr/bin/env bash
# scripts/bench-hrtt-serreg.sh — half-RTT between two processes over two es::ipc variable-size SPMC rings, the same
# 64 bytes of content as an es::serreg message: serialize() builds the frame in the ring record (es::ipc::sink),
# deserialize() validates and reads it on the other side. Report: logs/bench-hrtt-serreg-YYYYMMDD-HHMMSS.txt.
# Options: bench-hrtt-lib.sh.
. "$(dirname "$0")/bench-hrtt-lib.sh"
hrtt_main serreg "$@"
