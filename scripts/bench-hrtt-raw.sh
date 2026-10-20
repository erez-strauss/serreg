#!/usr/bin/env bash
# scripts/bench-hrtt-raw.sh — half-RTT between two processes over two es::ipc variable-size SPMC rings, raw 64-byte
# packets with timestamps (built in the ring record, read in place): the floor bench-hrtt-serreg.sh is compared with.
# Report: logs/bench-hrtt-raw-YYYYMMDD-HHMMSS.txt. Options: see scripts/bench-hrtt-lib.sh or --help.
. "$(dirname "$0")/bench-hrtt-lib.sh"
hrtt_main raw "$@"
