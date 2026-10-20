#!/usr/bin/env bash
# scripts/run-all-tests.sh — the full es::serreg verification run, with every log captured.
#
# Runs, in order: environment capture → clang-format check → book sync check → one fresh configure/build/ctest per preset
# (gcc16-debug, gcc16-release, gcc16-static, gcc16-static-none, gcc16-ubsan-trap, gcc16-asan-ubsan, gcc16-tsan) → srdump / file(1) /
# golden-print checks on the debug build → fuzz targets under ASan/UBSan (else under UBSan trap mode, else with
# _GLIBCXX_ASSERTIONS) → repeated concurrency tests under TSan →
# release bench (non-gating) → install + find_package/pkg-config consumer check. Every stage's stdout+stderr
# goes to its own file under a dated/timed directory, plus a summary, and the directory is tarred:
#
#   logs/serreg-tests-YYYYMMDD-HHMMSS/00-summary.txt       PASS/FAIL/WARN/SKIP per stage, with the reason
#   logs/serreg-tests-YYYYMMDD-HHMMSS/00-environment.txt   toolchain versions, host facts, script options
#   logs/serreg-tests-YYYYMMDD-HHMMSS/<preset>/{configure,build,ctest}.log + LastTest.log, LastTestsFailed.log
#   logs/serreg-tests-YYYYMMDD-HHMMSS/checks/*.log         srdump, file(1) magic, golden-print.txt, format check
#   logs/serreg-tests-YYYYMMDD-HHMMSS/fuzz/<target>.log    corpus + mutation runs; seed printed on the first line
#   logs/serreg-tests-YYYYMMDD-HHMMSS/install/*.log        cmake --install, consumer configure/build/run, pkg-config
#   logs/serreg-tests-YYYYMMDD-HHMMSS.tar.gz               the same directory, ready to attach to a review
#
# Exit status: 0 when every gating stage passed or was skipped (SKIP = the toolchain cannot run it here, the
# reason is in the summary); 1 otherwise. WARN stages (bench ceilings) never fail the run.
# What an agent must do with the output is written in docs/test-and-log-protocol.md.
#
# usage: scripts/run-all-tests.sh [options]
#   --presets a,b,c      presets to run (default: gcc16-debug,gcc16-release,gcc16-static,gcc16-static-none,
#                        gcc16-ubsan-trap,gcc16-asan-ubsan,gcc16-tsan; add clang-p2996 only when a P2996 clang is
#                        CXX). gcc16-static-none (2.1.11) is the static library without the ELF backend
#                        (ES_SERREG_IMAGE_FORMAT=none). gcc16-ubsan-trap (2.1.6) needs no sanitizer runtime: UB
#                        executes a trap instruction, so it runs on any box. By default it skips where
#                        gcc16-asan-ubsan runs (the same UBSan checks plus vptr and ASan); name it in --presets to
#                        run it anyway.
#   --quick              gcc16-debug only, 5000 fuzz iterations, no install check — a smoke run
#   --fuzz-iters N       mutated runs per fuzz target (default 200000)
#   --fuzz-seed N        seed for the fuzz targets (default: derived from the timestamp; printed in the log)
#   --tsan-repeat N      repetitions of the dlopen/signal/concurrency subset under TSan (default 5)
#   --jobs N             parallel build/test jobs (default: nproc)
#   --keep-build         reconfigure existing build/<preset> dirs (--fresh) instead of deleting them
#   --no-install         skip the install + consumer stage
#   --out DIR            log root (default: <repo>/logs)
#   -h, --help
# Environment: CXX selects the compiler (default: g++ if it is GCC >= 16, else /opt/gcc-16/bin/g++-16);
# LD_LIBRARY_PATH is extended with that compiler's libstdc++ directory so binaries run without an installed GCC 16.
set -u

# ---------------------------------------------------------------- options
REPO=$(cd "$(dirname "$0")/.." && pwd)
STAMP=$(date +%Y%m%d-%H%M%S)
PRESETS=gcc16-debug,gcc16-release,gcc16-static,gcc16-static-none,gcc16-ubsan-trap,gcc16-asan-ubsan,gcc16-tsan
PRESETS_DEFAULT=1
FUZZ_ITERS=200000
FUZZ_SEED=$(( $(date +%s) % 1000003 ))
TSAN_REPEAT=5
JOBS=$(nproc 2>/dev/null || echo 2)
KEEP_BUILD=0
DO_INSTALL=1
OUT_ROOT=$REPO/logs
while [ $# -gt 0 ]; do
    case $1 in
    --presets) PRESETS=$2; PRESETS_DEFAULT=0; shift 2 ;;
    --quick) PRESETS=gcc16-debug; PRESETS_DEFAULT=0; FUZZ_ITERS=5000; DO_INSTALL=0; shift ;;
    --fuzz-iters) FUZZ_ITERS=$2; shift 2 ;;
    --fuzz-seed) FUZZ_SEED=$2; shift 2 ;;
    --tsan-repeat) TSAN_REPEAT=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --keep-build) KEEP_BUILD=1; shift ;;
    --no-install) DO_INSTALL=0; shift ;;
    --out) OUT_ROOT=$2; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

OUT=$OUT_ROOT/serreg-tests-$STAMP
mkdir -p "$OUT/checks" || exit 2
SUMMARY=$OUT/00-summary.txt
ENVF=$OUT/00-environment.txt
: >"$SUMMARY"
FAILED=0
START_ALL=$(date +%s)

# ---------------------------------------------------------------- compiler
if [ -z "${CXX:-}" ]; then
    if command -v g++ >/dev/null && [ "$(g++ -dumpversion 2>/dev/null | cut -d. -f1)" -ge 16 ] 2>/dev/null; then
        CXX=g++
    elif [ -x /opt/gcc-16/bin/g++-16 ]; then
        CXX=/opt/gcc-16/bin/g++-16
    else
        CXX=g++
    fi
fi
export CXX
CXX_LIBDIR=$(dirname "$("$CXX" -print-file-name=libstdc++.so 2>/dev/null || echo /usr/lib64/x)")
case $CXX_LIBDIR in
/usr/lib64|/usr/lib|/lib64|/lib|.) ;;
*) export LD_LIBRARY_PATH="$CXX_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:allocator_may_return_null=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
export TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}

# ---------------------------------------------------------------- helpers
log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }
# record NAME STATUS SECONDS LOGFILE [note]
record() {
    printf '%-42s %-5s %6ss  %s%s\n' "$1" "$2" "$3" "${4#"$OUT"/}" "${5:+  -- $5}" >>"$SUMMARY"
    case $2 in FAIL) FAILED=1 ;; esac
    log "$2  $1${5:+ ($5)}"
}
# stage NAME LOGFILE CMD... — runs CMD with all output in LOGFILE; returns its status
stage() {
    local name=$1 logf=$2 t0 rc
    shift 2
    mkdir -p "$(dirname "$logf")"
    t0=$(date +%s)
    log "run   $name"
    { printf '$ %s\n' "$*"; "$@"; } >"$logf" 2>&1
    rc=$?
    printf '\n[exit %s after %ss]\n' "$rc" "$(( $(date +%s) - t0 ))" >>"$logf"
    if [ $rc -eq 0 ]; then record "$name" PASS "$(( $(date +%s) - t0 ))" "$logf"
    else record "$name" FAIL "$(( $(date +%s) - t0 ))" "$logf" "exit $rc"; fi
    return $rc
}
# warn_stage — like stage, but a failure is WARN (non-gating)
warn_stage() {
    local name=$1 logf=$2 t0 rc
    shift 2
    mkdir -p "$(dirname "$logf")"
    t0=$(date +%s)
    log "run   $name"
    { printf '$ %s\n' "$*"; "$@"; } >"$logf" 2>&1
    rc=$?
    printf '\n[exit %s after %ss]\n' "$rc" "$(( $(date +%s) - t0 ))" >>"$logf"
    if [ $rc -eq 0 ]; then record "$name" PASS "$(( $(date +%s) - t0 ))" "$logf"
    else record "$name" WARN "$(( $(date +%s) - t0 ))" "$logf" "exit $rc (non-gating)"; fi
    return $rc
}
skip() { record "$1" SKIP 0 "$2" "$3"; }
# sanitizer_available FLAG — can this toolchain link and run a -fsanitize=FLAG binary?
sanitizer_available() {
    local p=$OUT/checks/probe-$1
    printf 'int main(){return 0;}\n' | "$CXX" -x c++ -fsanitize="$1" - -o "$p" >"$p.log" 2>&1 && "$p" >>"$p.log" 2>&1
}
# tsan_probe — can TSan binaries run here, reliably? Sets TSAN_WRAP (a command prefix for every TSan stage) or
# TSAN_WHY. TSan maps a fixed shadow; with high ASLR entropy (vm.mmap_rnd_bits=32, the default on recent kernels)
# some processes land in an incompatible layout and TSan re-execs itself with personality(ADDR_NO_RANDOMIZE). A
# container's default seccomp profile blocks that, so a random ~half of all TSan processes die with "incompatible
# memory layout" — an environment failure, not a race. One trivial run proves nothing: run it 40 times.
TSAN_WRAP=()
TSAN_WHY=
tsan_probe() {
    local p=$OUT/checks/probe-thread i ok=1 arch
    arch=$(uname -m)
    printf 'int main(){return 0;}\n' | "$CXX" -x c++ -fsanitize=thread - -o "$p" >"$p.log" 2>&1 ||
        { TSAN_WHY="libtsan not available for $CXX"; return 1; }
    for i in $(seq 1 40); do "$p" >>"$p.log" 2>&1 || { ok=0; break; }; done
    [ $ok -eq 1 ] && { echo "tsan probe: 40/40 plain runs ok" >>"$p.log"; return 0; }
    if grep -q -e 'incompatible memory layout' -e 'unable to disable ASLR' "$p.log"; then
        if command -v setarch >/dev/null && setarch "$arch" -R true 2>>"$p.log"; then
            ok=1
            for i in $(seq 1 40); do setarch "$arch" -R "$p" >>"$p.log" 2>&1 || { ok=0; break; }; done
            if [ $ok -eq 1 ]; then
                TSAN_WRAP=(setarch "$arch" -R)
                echo "tsan probe: plain runs hit the ASLR layout problem; 40/40 ok under 'setarch $arch -R' — TSan stages run under it" >>"$p.log"
                return 0
            fi
        fi
        TSAN_WHY="ENV: TSan vs ASLR (vm.mmap_rnd_bits=$(cat /proc/sys/vm/mmap_rnd_bits 2>/dev/null || echo ?)) and ADDR_NO_RANDOMIZE is blocked; on the host: sudo sysctl vm.mmap_rnd_bits=28, or run the container with --security-opt seccomp=unconfined"
        return 1
    fi
    TSAN_WHY="TSan probe binary failed (see log)"
    return 1
}
tail_into_summary() { # tail_into_summary LOGFILE N — appends the last N lines of a log to the summary
    { printf '\n--- tail of %s ---\n' "${1#"$OUT"/}"; tail -n "$2" "$1"; } >>"$SUMMARY"
}

# ---------------------------------------------------------------- environment
{
    echo "es::serreg full test run  $STAMP"
    echo "repo:      $REPO"
    echo "options:   presets=$PRESETS fuzz_iters=$FUZZ_ITERS fuzz_seed=$FUZZ_SEED tsan_repeat=$TSAN_REPEAT jobs=$JOBS keep_build=$KEEP_BUILD install=$DO_INSTALL"
    echo "host:      $(hostname 2>/dev/null) / $(uname -srmo)"
    echo "cpus:      $(nproc 2>/dev/null)   mem: $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo 2>/dev/null)"
    [ -r /etc/os-release ] && . /etc/os-release && echo "os:        ${PRETTY_NAME:-?}"
    echo "CXX:       $CXX -> $(command -v "$CXX")"
    "$CXX" --version 2>&1 | head -1 | sed 's/^/           /'
    echo "libdir:    $CXX_LIBDIR   LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}"
    echo "cmake:     $(cmake --version 2>/dev/null | head -1)"
    echo "ninja:     $(ninja --version 2>/dev/null)"
    echo "binutils:  $(ld --version 2>/dev/null | head -1)"
    echo "glibc:     $(ldd --version 2>/dev/null | head -1)"
    echo "clang-fmt: $( (command -v clang-format >/dev/null && clang-format --version) 2>/dev/null)"
    echo "file(1):   $(file --version 2>/dev/null | head -1)"
    echo "tshark:    $( (command -v tshark >/dev/null && tshark --version 2>/dev/null | head -1) || echo "not found: wireshark_dissector_parity is disabled")"
    echo "lua:       $(for l in lua5.2 lua52 lua5.4 lua54 lua; do command -v $l >/dev/null && echo -n "$l=$($l -e 'io.write(_VERSION)' 2>/dev/null) "; done; echo "(wireshark_dissector_lua52/_lua54 need 5.2/5.4)")"
    echo "git:       $(git -C "$REPO" describe --always --dirty 2>/dev/null || echo 'not a git checkout')"
    echo "project:   $(grep -m1 'project(serreg VERSION' "$REPO/CMakeLists.txt")"
    echo "user:      $(id -un 2>/dev/null) ($(id -u 2>/dev/null))   memlock: $(ulimit -l) KiB"
    echo "hugepages: $(awk '/^HugePages_(Total|Free)|^Hugepagesize/ { printf "%s %s%s  ", $1, $2, $3 }' /proc/meminfo 2>/dev/null)"
    awk '$3 == "hugetlbfs" { print $2, $4 }' /proc/mounts 2>/dev/null | while read -r m o; do
        echo "hugetlbfs: $m ($o) $(stat -c '%U:%G %a' "$m" 2>/dev/null) $([ -w "$m" ] && echo writable || echo NOT-writable-by-this-user)"
    done
    echo "sanitizers: ASAN_OPTIONS=$ASAN_OPTIONS UBSAN_OPTIONS=$UBSAN_OPTIONS TSAN_OPTIONS=$TSAN_OPTIONS"
} >"$ENVF" 2>&1
cat "$ENVF"
{
    echo "es::serreg test summary  $STAMP   (environment: 00-environment.txt)"
    echo
    printf '%-42s %-5s %7s  %s\n' stage status time log
    printf '%-42s %-5s %7s  %s\n' ----- ------ ---- ---
} >>"$SUMMARY"

cd "$REPO" || exit 2

# ---------------------------------------------------------------- format check
if command -v clang-format >/dev/null; then
    stage "format/clang-format --dry-run --Werror" "$OUT/checks/format.log" \
        sh -c 'find include src tools tests examples -type f \( -name "*.cpp" -o -name "*.hpp" \) -print0 | xargs -0 clang-format --style=file --dry-run --Werror'
else
    skip "format/clang-format" "$OUT/checks/format.log" "clang-format not installed"
fi
# book/ in sync with the sources (AGENTS.md §5b): pages per source file, line citations, removed names
if command -v python3 >/dev/null 2>&1; then
    stage "book/check-book.py (pages, citations)" "$OUT/checks/book.log" python3 scripts/check-book.py
else
    skip "book/check-book.py" "$OUT/checks/book.log" "python3 not installed"
fi

# run_fuzz BUILD_DIR LABEL — every fuzz target over its corpus, $FUZZ_ITERS mutations each, seed $FUZZ_SEED
run_fuzz() {
    local F=$1/tests/fuzz/serreg_fuzz t
    if [ ! -x "$F" ]; then skip "fuzz ($2)" "$SUMMARY" "serreg_fuzz not built"; return; fi
    for t in rri capture elf config; do
        stage "fuzz/$t ($FUZZ_ITERS runs, seed $FUZZ_SEED, $2)" "$OUT/fuzz/$t.log" \
            "$F" "$t" "tests/fixtures/corpus/$t" "$FUZZ_ITERS" "$FUZZ_SEED"
    done
}

# ---------------------------------------------------------------- per-preset build + test
IFS=, read -r -a PRESET_LIST <<<"$PRESETS"
declare -A BUILT=()
for P in "${PRESET_LIST[@]}"; do
    D=$OUT/$P
    mkdir -p "$D"
    B=$REPO/build/$P
    CTEST_EXTRA=()
    WRAP=()
    case $P in
    gcc16-asan-ubsan)
        if ! sanitizer_available address; then skip "$P" "$OUT/checks/probe-address.log" "libasan not available for $CXX"; continue; fi
        CTEST_EXTRA=(-LE bench) ;;
    gcc16-ubsan-trap)
        # 2.1.6: UBSan without a runtime (trap mode). Redundant where the ASan/UBSan preset runs, so by default it
        # runs only where that one cannot; the compile-time probe is skipped (its wall-clock limit is not a UB test).
        if [ "$PRESETS_DEFAULT" -eq 1 ] && [[ ",$PRESETS," == *",gcc16-asan-ubsan,"* ]] && sanitizer_available address; then
            skip "$P" "$OUT/checks/probe-address.log" "UBSan runs inside gcc16-asan-ubsan here; name it in --presets to run it too"; continue
        fi
        CTEST_EXTRA=(-LE bench -E compile_probe_depth4) ;;
    gcc16-tsan)
        if ! tsan_probe; then skip "$P" "$OUT/checks/probe-thread.log" "$TSAN_WHY"; continue; fi
        WRAP=("${TSAN_WRAP[@]}")
        CTEST_EXTRA=(-LE bench) ;;
    gcc16-release)
        CTEST_EXTRA=(-LE bench) ;; # bench runs as its own non-gating stage below
    gcc16-static | gcc16-static-none)
        CTEST_EXTRA=(-LE bench) ;; # 2.2.1: registry_measure's budgets are the release bench stage's, not a gate here
    clang-p2996)
        if ! "$CXX" --version 2>/dev/null | grep -qi clang; then skip "$P" "$ENVF" "CXX is not clang"; continue; fi ;;
    esac
    if [ $KEEP_BUILD -eq 1 ] && [ -d "$B" ]; then
        stage "$P/configure (--fresh)" "$D/configure.log" cmake --preset "$P" --fresh || continue
    else
        rm -rf "$B"
        stage "$P/configure" "$D/configure.log" cmake --preset "$P" || continue
    fi
    stage "$P/build" "$D/build.log" cmake --build --preset "$P" -j"$JOBS" || continue
    BUILT[$P]=1
    stage "$P/ctest" "$D/ctest.log" "${WRAP[@]}" ctest --preset "$P" -j"$JOBS" --output-on-failure --timeout 600 "${CTEST_EXTRA[@]}"
    cp -f "$B"/Testing/Temporary/LastTest.log "$D/LastTest.log" 2>/dev/null
    cp -f "$B"/Testing/Temporary/LastTestsFailed.log "$D/LastTestsFailed.log" 2>/dev/null
    grep -E 'tests passed|tests failed|Total Test time' "$D/ctest.log" | sed "s|^|$P: |" >>"$SUMMARY.tails"

    case $P in
    gcc16-debug)
        T=$B/tests/unit/serreg_tests
        SR=$B/srdump
        [ -x "$T" ] && stage "$P/golden-print" "$OUT/checks/golden-print.txt" "$T" "[golden-print]"
        if [ -x "$SR" ]; then
            stage "$P/srdump --version" "$OUT/checks/srdump-version.log" "$SR" --version
            stage "$P/srdump --verify golden capture" "$OUT/checks/srdump-verify.log" "$SR" tests/fixtures/golden/orders.ser --verify
            stage "$P/srdump --elf --program-hash" "$OUT/checks/srdump-elf.log" "$SR" --elf "$T" --program-hash
            stage "$P/srdump --elf --json (unit binary)" "$OUT/checks/srdump-elf-json.log" "$SR" --elf "$T" --json
        fi
        if command -v file >/dev/null; then
            stage "$P/file(1) magic" "$OUT/checks/file-magic.log" \
                sh -c 'file -m share/serreg.magic tests/fixtures/golden/orders.ser tests/fixtures/golden/orders.rri && file -m share/serreg.magic tests/fixtures/golden/orders.ser | grep -q "es::serreg"'
        fi
        # gcc16-debug has -D_GLIBCXX_ASSERTIONS (2.0.3): when the ASan/UBSan preset will not run the fuzzers,
        # they run here, so out-of-range span/vector accesses still abort (the 2.0.1 decoder bug would have).
        if { [[ ",$PRESETS," != *",gcc16-asan-ubsan,"* ]] || ! sanitizer_available address; } &&
            [[ ",$PRESETS," != *",gcc16-ubsan-trap,"* ]]; then
            run_fuzz "$B" "_GLIBCXX_ASSERTIONS"
        fi ;;
    gcc16-ubsan-trap)
        run_fuzz "$B" "UBSan trap mode, _GLIBCXX_ASSERTIONS" ;;
    gcc16-release)
        warn_stage "$P/bench (label bench, non-gating)" "$D/bench.log" ctest --preset "$P" -L bench --output-on-failure --timeout 600
        [ -x "$B/examples/file_store" ] && stage "$P/example file_store + dump registry (toml)" "$OUT/checks/example-file_store.log" \
            env ES_SERREG_DUMP_REGISTRY=- ES_SERREG_DUMP_FORMAT=toml "$B/examples/file_store" "$OUT/checks/file_store.ser" ;;
    gcc16-asan-ubsan)
        run_fuzz "$B" "ASan/UBSan" ;;
    gcc16-tsan)
        stage "$P/concurrency x$TSAN_REPEAT (dlopen_[ABDEFJL]|signal_dump|concurrent|hrtt_.*_thread)" "$D/concurrency-repeat.log" \
            "${WRAP[@]}" ctest --preset "$P" --repeat until-fail:"$TSAN_REPEAT" -R 'dlopen_[ABDEFJL]|signal_dump|concurrent|hrtt_.*_thread' --output-on-failure --timeout 600 ;;
    esac
done

# ---------------------------------------------------------------- install + consumer
if [ $DO_INSTALL -eq 1 ]; then
    IP=
    for cand in gcc16-release gcc16-debug; do [ -n "${BUILT[$cand]:-}" ] && { IP=$cand; break; }; done
    if [ -z "$IP" ]; then
        skip "install" "$SUMMARY" "no release/debug build succeeded"
    else
        ID=$OUT/install
        PFX=$REPO/build/_install-$STAMP
        CD=$REPO/build/_consumer-$STAMP
        mkdir -p "$ID" "$CD"
        cat >"$CD/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 4.0)
project(serreg_consumer LANGUAGES CXX)
find_package(serreg CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE es::serreg)
# 2.1.13 (decision G): three libraries in one package; es::serreg does not bring es::ipc
foreach(_t es::serreg es::serreg_tools es::ipc)
  if(NOT TARGET ${_t})
    message(FATAL_ERROR "find_package(serreg) without components must provide ${_t}")
  endif()
endforeach()
get_target_property(_serreg_links es::serreg INTERFACE_LINK_LIBRARIES)
if(_serreg_links MATCHES "ipc")
  message(FATAL_ERROR "es::serreg must not link es::ipc (2.1.13): ${_serreg_links}")
endif()
# 2.2.1 (DECISIONS G-2): the minor is the compatibility unit until 3.0 — the same minor is accepted, the next
# minor is not, and the SONAME carries major.minor
find_package(serreg ${serreg_VERSION_MAJOR}.${serreg_VERSION_MINOR} CONFIG REQUIRED)
math(EXPR _next_minor "${serreg_VERSION_MINOR} + 1")
find_package(serreg ${serreg_VERSION_MAJOR}.${_next_minor} CONFIG QUIET)
if(serreg_FOUND AND serreg_VERSION VERSION_LESS "${serreg_VERSION_MAJOR}.${_next_minor}")
  message(FATAL_ERROR "find_package(serreg ${serreg_VERSION_MAJOR}.${_next_minor}) accepted ${serreg_VERSION}: SameMinorVersion expected")
endif()
find_package(serreg CONFIG REQUIRED)
get_target_property(_serreg_soname es::serreg IMPORTED_SONAME_RELEASE)
if(NOT _serreg_soname)
  get_target_property(_serreg_soname es::serreg IMPORTED_SONAME_DEBUG)
endif()
if(NOT _serreg_soname)
  get_target_property(_serreg_soname es::serreg IMPORTED_SONAME_NOCONFIG)
endif()
if(_serreg_soname AND NOT _serreg_soname MATCHES "\\.so\\.${serreg_VERSION_MAJOR}\\.${serreg_VERSION_MINOR}$")
  message(FATAL_ERROR "libserreg SONAME is ${_serreg_soname}; expected major.minor (2.2.1, G-2)")
endif()
# es::ipc alone (2.0.5): header-only, C++23, no reflection flags must leak into this target
add_executable(ipc_consumer ipc_main.cpp)
target_link_libraries(ipc_consumer PRIVATE es::ipc)
set_target_properties(ipc_consumer PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
EOF
        # 2.1.13: an ES_SERREG_IPC_ONLY installation, and a consumer that asks for the ipc component only
        mkdir -p "$CD/ipc_only"
        cat >"$CD/ipc_only/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 4.0)
project(serreg_ipc_only_consumer LANGUAGES CXX)
find_package(serreg CONFIG REQUIRED COMPONENTS ipc)
if(TARGET es::serreg OR TARGET es::serreg_tools)
  message(FATAL_ERROR "COMPONENTS ipc must not load the reflection libraries")
endif()
add_executable(ipc_consumer ipc_main.cpp)
target_link_libraries(ipc_consumer PRIVATE es::ipc)
set_target_properties(ipc_consumer PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
EOF
        cat >"$CD/ipc_main.cpp" <<'EOF'
// find_package consumer of es::ipc only: an SPSC queue in an anonymous region, one typed message, zero copy.
#include <cstdint>
#include <print>
#include <es/ipc/ipc.hpp>
namespace ipc_consumer
{
class app
{
public:
    int run() const
    {
        const auto g = es::ipc::ring::for_capacity(4096, 64, 1);
        auto m = es::ipc::region::anonymous(es::ipc::ring::bytes_for(g));
        auto q = m ? es::ipc::ring::create(*m, g, es::ipc::ring_kind::spsc) : std::unexpected(m.error());
        auto p = q ? es::ipc::producer::attach(*q) : std::unexpected(q.error());
        auto c = q ? es::ipc::consumer::join(*q) : std::unexpected(q.error());
        if (!p || !c || !p->emplace<std::uint64_t>(std::uint64_t{42}))
            return 2;
        std::uint64_t got = 0;
        c->poll_as<std::uint64_t>([&](const std::uint64_t v) { got = v; });
        std::println("ipc consumer ok: {}", got);
        return got == 42 ? 0 : 3;
    }
};
} // namespace ipc_consumer
int main()
{
    return ipc_consumer::app{}.run();
}
EOF
        cat >"$CD/main.cpp" <<'EOF'
// find_package consumer: registers one type before main(), serialises it, verifies the registry saw it.
#include <print>
#include <vector>
#include <es/serreg/serreg.hpp>
struct probe_msg { int a; double b; };
namespace consumer
{
class app
{
public:
    int run() const
    {
        std::vector<std::byte> out;
        es::serreg::vector_sink s{out};
        if (!es::serreg::serialize(s, probe_msg{1, 2.0})) return 2;
        const auto& reg = es::serreg::registry::instance();
        std::println("consumer ok: {} bytes, {} registry entries, program hash {:016x}", out.size(),
                     reg.size(), reg.program_hash());
        return reg.find_type_by_name("probe_msg") ? 0 : 3;
    }
};
} // namespace consumer
int main()
{
    return consumer::app{}.run();
}
EOF
        stage "install/cmake --install ($IP)" "$ID/install.log" cmake --install "build/$IP" --prefix "$PFX" &&
            stage "install/consumer configure" "$ID/consumer-configure.log" cmake -S "$CD" -B "$CD/build" -G Ninja -DCMAKE_PREFIX_PATH="$PFX" &&
            stage "install/consumer build" "$ID/consumer-build.log" cmake --build "$CD/build" &&
            stage "install/consumer run" "$ID/consumer-run.log" "$CD/build/consumer" &&
            stage "install/es::ipc consumer run (no reflection flags)" "$ID/ipc-consumer-run.log" "$CD/build/ipc_consumer" &&
            stage "install/srdump --version (installed)" "$ID/srdump-installed.log" "$PFX/bin/srdump" --version &&
            stage "install/es::ipc only (ES_SERREG_IPC_ONLY configure + install)" "$ID/ipc-only-install.log" \
                sh -c "cmake -S . -B '$CD/ipc_only_build' -G Ninja -DES_SERREG_IPC_ONLY=ON -DCMAKE_INSTALL_PREFIX='$CD/ipc_pfx' && cmake --install '$CD/ipc_only_build'" &&
            stage "install/es::ipc only consumer (COMPONENTS ipc, C++23)" "$ID/ipc-only-consumer.log" \
                sh -c "cp '$CD/ipc_main.cpp' '$CD/ipc_only/' && cmake -S '$CD/ipc_only' -B '$CD/ipc_only/build' -G Ninja -DCMAKE_PREFIX_PATH='$CD/ipc_pfx' && cmake --build '$CD/ipc_only/build' && '$CD/ipc_only/build/ipc_consumer'" &&
            stage "install/user_example1 standalone build" "$ID/user_example1-build.log" \
                sh -c "cmake -S examples/user_example1 -B '$CD/ue1' -G Ninja -DCMAKE_PREFIX_PATH='$PFX' && cmake --build '$CD/ue1'" &&
            stage "install/user_example1 writer + readers" "$ID/user_example1-run.log" \
                bash -o pipefail -c "cd '$CD/ue1' && ./writer && ./reader1 && ./reader1 --blocks && ./reader2 >/dev/null && ./reader2 --blocks | tail -1 && ./reader3 | tail -1"
        if command -v pkg-config >/dev/null; then
            stage "install/pkg-config --cflags --libs serreg serreg-tools es-ipc" "$ID/pkg-config.log" \
                env PKG_CONFIG_PATH="$PFX/lib64/pkgconfig:$PFX/lib/pkgconfig" pkg-config --cflags --libs serreg serreg-tools es-ipc
        fi
        rm -rf "$PFX" "$CD"
    fi
fi

# ---------------------------------------------------------------- summary + tarball
{
    echo
    echo "total time: $(( $(date +%s) - START_ALL ))s"
    echo "result:     $([ $FAILED -eq 0 ] && echo 'ALL GATING STAGES PASSED (or SKIPPED with reason)' || echo 'FAILURES — see FAIL rows and their logs')"
    if [ -s "$SUMMARY.tails" ]; then echo; echo "ctest totals:"; sed 's/^/  /' "$SUMMARY.tails"; fi
    for P in "${PRESET_LIST[@]}"; do
        [ -s "$OUT/$P/LastTestsFailed.log" ] && { echo; echo "failed tests ($P):"; sed 's/^/  /' "$OUT/$P/LastTestsFailed.log"; }
        # 2.0.7: tests that skipped themselves (environment: no huge-page pool, a non-writable hugetlbfs, ...) are
        # listed with their reason — a SKIP is not a pass, it says what this box could not verify
        if [ -f "$OUT/$P/ctest.log" ] && grep -q '\*\*\*Skipped' "$OUT/$P/ctest.log"; then
            echo
            echo "skipped tests ($P) — not verified on this box:"
            grep '\*\*\*Skipped' "$OUT/$P/ctest.log" | sed 's/^.*Test *#[0-9]*: \(.*\) \.\.\.*\*\*\*Skipped.*$/  \1/' | sort -u
            # Catch2 wraps the SKIP message over several lines: join them up to the blank line
            [ -f "$OUT/$P/LastTest.log" ] && awk '/explicitly with message/ { m = 1; t = ""; next }
                m && /^[[:space:]]*$/ { if (t != "") print "    reason:" t; m = 0; next }
                m { sub(/^[[:space:]]+/, " "); t = t $0 }' "$OUT/$P/LastTest.log" | sort -u
        fi
    done
} >>"$SUMMARY"
rm -f "$SUMMARY.tails"
# tails of failed logs, so the summary alone is usually enough to start triage
grep -E ' (FAIL|WARN) ' "$SUMMARY" | awk '{for(i=1;i<=NF;i++) if ($i ~ /\.(log|txt)$/) print $i}' | sort -u |
    while read -r rel; do [ -f "$OUT/$rel" ] && tail_into_summary "$OUT/$rel" 40; done
# sanitizer reports, if any, verbatim into the summary
grep -lE 'ERROR: (Address|Leak|Thread)Sanitizer|runtime error:' "$OUT"/*/*.log "$OUT"/fuzz/*.log 2>/dev/null |
    while read -r f; do { printf '\n--- sanitizer report in %s ---\n' "${f#"$OUT"/}"; grep -nE -A25 'ERROR: (Address|Leak|Thread)Sanitizer|runtime error:' "$f" | head -200; } >>"$SUMMARY"; done

( cd "$OUT_ROOT" && tar -czf "serreg-tests-$STAMP.tar.gz" "serreg-tests-$STAMP" )
echo
cat "$SUMMARY"
echo
echo "logs:    $OUT"
echo "tarball: $OUT_ROOT/serreg-tests-$STAMP.tar.gz"
exit $FAILED
