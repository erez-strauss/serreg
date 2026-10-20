#!/usr/bin/env bash
# scripts/bench-hrtt-lib.sh — shared body of scripts/bench-hrtt-raw.sh and scripts/bench-hrtt-serreg.sh (v2.0.4;
# es::ipc ring geometry, huge pages and mlock options since 2.0.5; the variable-size es::ipc rings since 2.1.13a).
# Not run directly: each script sources it and calls `hrtt_main <raw|serreg> "$@"`.
#
# What a run does: builds the benchmark in the release preset (unless --no-build), records the machine state
# that decides latency (CPU model, governor, SMT, isolcpus/nohz_full, load, compiler), then runs the benchmark
# --runs times as TWO SEPARATELY LAUNCHED PROCESSES — pong first (it waits for the rings), then ping, which
# creates the two shared-memory es::ipc rings (variable-size records: a 16-byte record header + the payload, in
# 64-byte units), measures and prints the table — and writes everything to
#
#   logs/bench-hrtt-<kind>-YYYYMMDD-HHMMSS.txt        the full report (also shown on the terminal)
#   logs/bench-hrtt-<kind>-YYYYMMDD-HHMMSS-runN.samples   per-sample half-RTT in ns (with --samples)
#
# options:
#   --iters N        measured round trips per run (default 1000000)
#   --warmup N       unmeasured round trips first (default 100000)
#   --runs N         repetitions (default 3); the summary lists p50/p99/p99.9 of each
#   --cpus A,B       pin ping to CPU A and pong to CPU B (default: two CPUs on different physical cores of one
#                    package, avoiding CPU 0; 'none' to leave both unpinned)
#   --pause          spin with a CPU pause hint (default: pure busy spin)
#   --slots N        with --slot-bytes: size the ring for N messages (default 1024)
#   --slot-bytes B   ... of up to B bytes each (default 256): a ring of at least N x align_up(16 + B, 64)
#                    bytes, rounded up to a power-of-two number of 64-byte units
#   --ring K         spmc (default: broadcast rings with one reader each) or spsc (queues); pong reads it
#   --huge 2m|1g     rings on explicit huge pages (a hugetlbfs mount of that page size, pages reserved in
#                    vm.nr_hugepages / hugepagesz=1G hugepages=N); default normal pages on /dev/shm
#   --lock           mlock() the rings (needs RLIMIT_MEMLOCK, `ulimit -l`, or CAP_IPC_LOCK)
#   --threads        also run once with pong as a thread in the ping process, for reference
#   --samples        keep the per-sample files
#   --preset P       CMake preset to build/run (default gcc16-release)
#   --no-build       use the existing build/<preset> binaries
#   --out DIR        output directory (default <repo>/logs)
# Before the runs, ring files of earlier benchmark runs whose process is gone (serreg-hrtt-* in /dev/shm and in
# every hugetlbfs mount — a crashed run's huge-page file would keep its pages reserved) are removed.
# Environment: CXX as in scripts/run-all-tests.sh (g++ if GCC >= 16, else /opt/gcc-16/bin/g++-16).
set -u

hrtt_usage() { sed -n '2,/^# Environment/p' "$REPO/scripts/bench-hrtt-lib.sh" | sed 's/^# \{0,1\}//'; }

# hugetlbfs mount points (one per line)
hrtt_hugetlbfs() { awk '$3 == "hugetlbfs" { print $2 }' /proc/mounts 2>/dev/null; }

# Removes serreg-hrtt-<kind>-<pid>-<run>-{a2b,b2a} ring files whose <pid> no longer runs.
hrtt_clean_stale() {
    local d f pid
    for d in /dev/shm $(hrtt_hugetlbfs); do
        for f in "$d"/serreg-hrtt-*; do
            [ -e "$f" ] || continue
            pid=$(basename "$f" | sed -n 's/^serreg-hrtt-[a-z]*-\([0-9]*\)-.*/\1/p')
            [ -z "$pid" ] && pid=$(basename "$f" | sed -n 's/^serreg-hrtt-\([0-9]*\)-.*/\1/p')
            if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
                rm -f "$f" && echo "removed stale ring file $f (process $pid is gone)"
            fi
        done
    done
}

# Two CPUs on different physical cores of the same package (never CPU 0, which takes most interrupts), highest
# numbers first. Prints "A,B" or nothing.
hrtt_pick_cpus() {
    local c pkg core first="" fpkg="" fcore=""
    for c in $(ls -d /sys/devices/system/cpu/cpu[0-9]* 2>/dev/null | sed 's/.*cpu//' | sort -rn); do
        [ "$c" = 0 ] && continue
        [ -r /sys/devices/system/cpu/cpu$c/online ] && [ "$(cat /sys/devices/system/cpu/cpu$c/online)" = 0 ] && continue
        pkg=$(cat /sys/devices/system/cpu/cpu$c/topology/physical_package_id 2>/dev/null || echo 0)
        core=$(cat /sys/devices/system/cpu/cpu$c/topology/core_id 2>/dev/null || echo "$c")
        if [ -z "$first" ]; then first=$c; fpkg=$pkg; fcore=$core
        elif [ "$pkg" = "$fpkg" ] && [ "$core" != "$fcore" ]; then echo "$first,$c"; return; fi
    done
    [ -n "$first" ] && [ "$(nproc)" -ge 2 ] && echo "$first,$(( first > 1 ? first - 1 : 0 ))"
}

hrtt_env() { # the facts that decide a latency number
    local c
    echo "date:        $(date -Is)"
    echo "host:        $(hostname 2>/dev/null) / $(uname -srmo)"
    [ -r /etc/os-release ] && (. /etc/os-release && echo "os:          ${PRETTY_NAME:-?}")
    echo "cpu:         $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')  ($(nproc) online)"
    echo "smt:         $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo ?) (1 = hyper-threading on)"
    for c in ${PING_CPU:-} ${PONG_CPU:-}; do
        echo "cpu$c:        governor $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor 2>/dev/null || echo n/a)," \
             "cur $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null || echo ?) kHz," \
             "siblings $(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list 2>/dev/null || echo ?)"
    done
    echo "no_turbo:    $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)"
    echo "cmdline:     $(tr ' ' '\n' </proc/cmdline 2>/dev/null | grep -E '^(isolcpus|nohz_full|rcu_nocbs|intel_idle|processor.max_cstate|idle|mitigations)=' | tr '\n' ' ')"
    echo "load:        $(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null)"
    echo "hugepages:   $(awk '/^HugePages_(Total|Free)|^Hugepagesize/ { printf "%s %s%s  ", $1, $2, $3 }' /proc/meminfo)"
    echo "hugetlbfs:   $(for m in $(hrtt_hugetlbfs); do printf '%s (%s%s) ' "$m" "$(stat -c '%U:%G %a' "$m" 2>/dev/null)" "$([ -w "$m" ] && echo ', writable' || echo ', NOT writable')"; done)"
    echo "memlock:     $(ulimit -l) (ulimit -l, KiB)"
    echo "compiler:    $("$CXX" --version 2>/dev/null | head -1)"
    echo "preset:      $PRESET   binary: $BIN"
    echo "options:     iters=$ITERS warmup=$WARMUP runs=$RUNS cpus=${CPUS} pause=$PAUSE threads=$THREADS" \
         "slots=$SLOTS slot_bytes=$SLOT_BYTES ring=${RING:-spmc} huge=${HUGE:-no} lock=$LOCK"
}

hrtt_main() {
    local kind=$1; shift
    REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    ITERS=1000000 WARMUP=100000 RUNS=3 CPUS="" PAUSE=0 THREADS=0 SAMPLES=0 PRESET=gcc16-release BUILD=1
    SLOTS=1024 SLOT_BYTES=256 RING="" HUGE="" LOCK=0
    OUTDIR=$REPO/logs
    while [ $# -gt 0 ]; do
        case $1 in
        --iters) ITERS=$2; shift 2 ;;
        --warmup) WARMUP=$2; shift 2 ;;
        --runs) RUNS=$2; shift 2 ;;
        --cpus) CPUS=$2; shift 2 ;;
        --pause) PAUSE=1; shift ;;
        --slots) SLOTS=$2; shift 2 ;;
        --slot-bytes) SLOT_BYTES=$2; shift 2 ;;
        --ring) RING=$2; shift 2 ;;
        --huge) HUGE=$2; shift 2 ;;
        --lock) LOCK=1; shift ;;
        --threads) THREADS=1; shift ;;
        --samples) SAMPLES=1; shift ;;
        --preset) PRESET=$2; shift 2 ;;
        --no-build) BUILD=0; shift ;;
        --out) OUTDIR=$2; shift 2 ;;
        -h|--help) hrtt_usage; return 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; return 2 ;;
        esac
    done

    if [ -z "${CXX:-}" ]; then
        if command -v g++ >/dev/null && [ "$(g++ -dumpversion 2>/dev/null | cut -d. -f1)" -ge 16 ] 2>/dev/null; then CXX=g++
        elif [ -x /opt/gcc-16/bin/g++-16 ]; then CXX=/opt/gcc-16/bin/g++-16
        else CXX=g++; fi
    fi
    export CXX
    local libdir
    libdir=$(dirname "$("$CXX" -print-file-name=libstdc++.so 2>/dev/null || echo /usr/lib64/x)")
    case $libdir in /usr/lib64|/usr/lib|/lib64|/lib|.) ;; *) export LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;; esac

    local stamp target out
    stamp=$(date +%Y%m%d-%H%M%S)
    target=serreg_hrtt_$kind
    BIN=$REPO/build/$PRESET/tests/bench/$target
    mkdir -p "$OUTDIR" || return 2
    out=$OUTDIR/bench-hrtt-$kind-$stamp.txt

    if [ $BUILD -eq 1 ]; then
        echo "building $target ($PRESET) ..."
        (cd "$REPO" && { [ -f "build/$PRESET/CMakeCache.txt" ] || cmake --preset "$PRESET" >"$out.build.log" 2>&1; } &&
            cmake --build --preset "$PRESET" --target "$target" >>"$out.build.log" 2>&1) ||
            { echo "build failed — see $out.build.log" >&2; return 1; }
        rm -f "$out.build.log"
    fi
    [ -x "$BIN" ] || { echo "no benchmark binary at $BIN (build it, or drop --no-build)" >&2; return 1; }

    [ -z "$CPUS" ] && CPUS=$(hrtt_pick_cpus)
    [ "$CPUS" = none ] && CPUS=""
    PING_CPU=${CPUS%%,*} PONG_CPU=${CPUS##*,}
    local pin_ping=() pin_pong=() extra=()
    [ -n "$CPUS" ] && { pin_ping=(--cpu "$PING_CPU"); pin_pong=(--cpu "$PONG_CPU"); }
    [ $PAUSE -eq 1 ] && extra+=(--pause)
    [ $LOCK -eq 1 ] && extra+=(--lock)
    local ping_only=(--slots "$SLOTS" --slot-bytes "$SLOT_BYTES")   # the creator's geometry; pong reads it
    [ -n "$RING" ] && ping_only+=(--ring "$RING")
    [ -n "$HUGE" ] && ping_only+=(--huge "$HUGE")

    local rc=0 r name pong_pid pong_rc samples tmpd
    tmpd=$(mktemp -d) || return 2
    {
        echo "==== es::serreg half-RTT benchmark ($kind) — $stamp"
        hrtt_env
        hrtt_clean_stale
        echo
        for r in $(seq 1 "$RUNS"); do
            name=serreg-hrtt-$kind-$$-$r
            samples=()
            [ $SAMPLES -eq 1 ] && samples=(--samples "$OUTDIR/bench-hrtt-$kind-$stamp-run$r.samples")
            echo "---- run $r/$RUNS: two separately launched processes (pong: ${PONG_CPU:-any}, ping: ${PING_CPU:-any})"
            "$BIN" --role pong --name "$name" "${pin_pong[@]}" "${extra[@]}" --timeout 60 &
            pong_pid=$!
            if ! "$BIN" --role ping --name "$name" "${pin_ping[@]}" "${extra[@]}" "${ping_only[@]}" --iters "$ITERS" \
                --warmup "$WARMUP" --timeout 60 "${samples[@]}" >"$tmpd/run$r.txt" 2>&1; then
                rc=1
                kill "$pong_pid" 2>/dev/null # it would otherwise wait for rings that will never come
            fi
            cat "$tmpd/run$r.txt"
            wait "$pong_pid"; pong_rc=$?
            [ $pong_rc -ne 0 ] && { echo "pong exited with $pong_rc"; rc=1; }
            echo
        done
        echo "==== summary: half RTT in ns per run (p50 is the comparable number; tails measure the machine)"
        printf '%-6s %8s %8s %8s %8s %10s\n' run min p50 p99 p99.9 max
        for r in $(seq 1 "$RUNS"); do
            awk -v r="$r" '/^half RTT/ { printf "%-6s %8s %8s %8s %8s %10s\n", r, $4, $5, $7, $8, $10 }' "$tmpd/run$r.txt"
        done
        echo
        if [ $THREADS -eq 1 ]; then
            echo "---- reference: pong as a thread of the ping process"
            "$BIN" --role both --mode thread ${CPUS:+--cpus "$CPUS"} "${extra[@]}" "${ping_only[@]}" --iters "$ITERS" \
                --warmup "$WARMUP" || rc=1
            echo
        fi
        echo "==== result: $([ $rc -eq 0 ] && echo ok || echo FAILED)   report: $out"
    } 2>&1 | tee "$out"
    rm -rf "$tmpd"
    # the block runs in a subshell of the pipeline: its verdict is the result line it printed
    ! grep -q '^==== result: FAILED' "$out"
}
