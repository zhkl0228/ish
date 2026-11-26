#!/bin/bash
# The native JIT's tests, on the Apple arm64 host it needs (macOS).
#
#   tools/jit-test-mac.sh --prepare <rootfs dir>     on Linux, first
#   tools/jit-test-mac.sh [build dir]                on macOS
#
# macOS has no compiler for aarch64 Linux programs, so --prepare builds the
# guest tests on Linux (with $CC_GUEST, default clang, static) into
# build/jit-test/bin and copies a plain rootfs directory (ish -r) to
# build/jit-test/rootfs; a checkout shared with the Mac takes them over.
#
# On macOS it builds ish with -Djit=true in the build dir (default
# build-mac-jit), then runs under it:
#   - regress_jit_fault: a host fault at a guest access in native code goes on
#     at the access's TLB-miss exit (ISH_JIT_FAULT_TEST=resume), and one
#     anywhere else kills the process with SIGSEGV and logs a sample
#     (ISH_JIT_FAULT_TEST=unknown);
#   - regress_jit_flush: a 1 MB code region (ISH_JIT_REGION_MB=1), flushed
#     over and over as processes start and end, every result right;
#   - regress_jit_fp: scalar floating point, each instruction's results and
#     flags the gadgets' (ISH_JIT_PIC=1, as on iOS);
#   - regression tests the JIT can break: instructions compared against
#     hardware, faults, signals, memory.
# Needs meson, ninja and timeout. Exits 1 if any test failed.

set -u
cd "$(dirname "$0")/.."
DIR=build/jit-test
TESTS="simd_insns ldpsw dc_zva null_fault fenv mmap_spin stack_extent force_signal eintr process_signals sysv_shm"

if [ "${1:-}" = --prepare ]; then
    [ $# -eq 2 ] && [ -d "$2" ] || { echo "usage: $0 --prepare <rootfs dir>" >&2; exit 2; }
    CC_GUEST="${CC_GUEST:-clang}"
    rm -rf "$DIR"
    mkdir -p "$DIR/bin"
    build() {   # name, flags and sources
        local name=$1
        shift
        "$CC_GUEST" -static -Wall -o "$DIR/bin/regress_$name" "$@" || exit 1
    }
    build simd_insns -O1 tests/regress/regress_simd_insns.c tests/regress/regress_simd_insns.S
    build ldpsw -O0 tests/regress/regress_ldpsw.c
    build dc_zva -O0 tests/regress/regress_dc_zva.c
    build null_fault -O1 tests/regress/regress_null_fault.c
    build fenv -O0 tests/regress/regress_fenv.c -lm
    build jit_fault -O0 tests/regress/regress_jit_fault.c
    build jit_flush -O0 tests/regress/regress_jit_flush.c
    build jit_fp -O0 tests/regress/regress_jit_fp.c
    build mmap_spin -O1 tests/regress/regress_mmap_spin.c
    build stack_extent -O0 tests/regress/regress_stack_extent.c
    build force_signal -O0 tests/regress/regress_force_signal.c
    build eintr -O0 tests/regress/regress_eintr.c
    build process_signals -O0 -pthread tests/regress/regress_process_signals.c
    build sysv_shm -O0 tests/regress/regress_sysv_shm.c
    cp -a "$2" "$DIR/rootfs"
    echo "prepared $DIR: $(ls "$DIR/bin" | wc -l | tr -d ' ') tests, rootfs from $2"
    exit 0
fi

BUILD=${1:-build-mac-jit}
if [ "$(uname -s)-$(uname -m)" != Darwin-arm64 ]; then
    echo "the native JIT needs an Apple arm64 host (macOS)" >&2
    exit 2
fi
if [ ! -d "$DIR/bin" ] || [ ! -d "$DIR/rootfs" ]; then
    echo "no $DIR: run $0 --prepare <rootfs dir> on Linux first" >&2
    exit 2
fi
if [ ! -f "$BUILD/build.ninja" ]; then
    meson setup "$BUILD" -Djit=true --buildtype=release || exit 1
fi
ninja -C "$BUILD" ish || exit 1
ISH=$BUILD/ish
ROOT=$DIR/rootfs
cp "$DIR"/bin/* "$ROOT/tmp/"

status=0
fail() {
    echo "FAIL $1"
    status=1
}

echo "== /proc/ish/jit"
timeout 60 "$ISH" -r "$ROOT" /bin/cat /proc/ish/jit | head -3

for mode in resume unknown; do
    echo "== regress_jit_fault $mode"
    start=$(date +%s)
    out=$(ISH_JIT_FAULT_TEST=$mode timeout 120 "$ISH" -r "$ROOT" /tmp/regress_jit_fault $mode 2>&1)
    rc=$?
    echo "$out"
    echo "(ish exited $rc after $(($(date +%s) - start)) s)"
    # a crash of ish itself leaves a report
    report=$(ls -t ~/Library/Logs/DiagnosticReports/ish-*.ips 2>/dev/null | head -1)
    if [ -n "$report" ] && [ "$(stat -f %m "$report")" -ge "$start" ]; then
        echo "ish crashed: $report"
        head -c 3000 "$report"
        echo
    fi
    # here the JIT is on: a skip means the fault was not made
    if [ $mode = resume ]; then
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || fail "regress_jit_fault resume"
    # killed at the fault, with the sample iSH logs for it
    elif echo "$out" | grep -q '^FAIL' ||
            ! echo "$out" | grep -q 'host fault in native code, not at a guest access'; then
        fail "regress_jit_fault unknown: not killed with a sample in the log"
    fi
done

# a 1 MB code region, flushed over and over as processes start and end
echo "== regress_jit_flush"
out=$(ISH_JIT_REGION_MB=1 timeout 300 "$ISH" -r "$ROOT" /tmp/regress_jit_flush 2>&1)
echo "$out" | grep -v '^PASS' | tail -20
echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || fail "regress_jit_flush"

# scalar FP: each instruction run as gadgets and run hot, the JIT's (PIC, as
# on iOS, where a block runs as gadgets till hot)
echo "== regress_jit_fp"
out=$(ISH_JIT_PIC=1 timeout 300 "$ISH" -r "$ROOT" /tmp/regress_jit_fp 2>&1)
echo "$out" | grep -v '^PASS' | tail -20
echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || fail "regress_jit_fp"

for t in $TESTS; do
    echo "== regress_$t"
    out=$(timeout 300 "$ISH" -r "$ROOT" /tmp/regress_$t 2>&1)
    echo "$out" | grep -v '^PASS' | tail -20
    echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || fail "regress_$t"
done

[ $status = 0 ] && echo "all JIT tests passed" || echo "some JIT tests failed"
exit $status
