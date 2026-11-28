#!/bin/bash
# Build and run the fs/wait syscall regression tests inside iSH.
#
# These pin the behaviour of five fixes so a regression fails here rather than
# in a user's shell:
#   #30  O_DIRECTORY flag value + O_NOFOLLOW honored at path resolution
#   #31  fakefs stat/fstat st_dev (nlink, blksize) agreement
#   #29  waitpid not reporting its internal 1s timeout as EINTR
#        waitid si_status/si_code decoding
#        waitid si_uid
#
# Usage:
#   tests/regress/run.sh [-i <ish binary>] [-r <rootfs>] [-f <fakefs path>]
#
#   -i  iSH binary to test. Default: the first of build-arm64-release/ish,
#       build/ish that exists.
#   -r  rootfs to run against, passed to iSH as -r. Default alpine-arm64-321.
#   -f  a file under a fakefs mount point (e.g. /var/minis/shared/doc.txt) to
#       additionally check stat/fstat agreement on. Only meaningful when the
#       rootfs is mounted with -f and a path-translate hook is installed, so it
#       is optional; without it the #31 assertions still run against the rootfs.
#
# The guest test binary is built with a cross-compiler (aarch64-linux-musl-gcc
# by default, override with $CC_GUEST). If none is available the script skips
# rather than failing, since the toolchain is not part of the repo.
#
# Given an iSH built with AddressSanitizer (tools/build-asan.sh), the run also
# fails on any ASan report, which it prints at the end.

set -u
cd "$(dirname "$0")/../.."

ISH=""
ROOTFS="alpine-arm64-321"
FAKEFS_PATH=""
MOUNT_FLAG="-r"

while getopts "i:r:f:m:h" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        r) ROOTFS="$OPTARG" ;;
        f) FAKEFS_PATH="$OPTARG" ;;
        m) MOUNT_FLAG="$OPTARG" ;;
        h) sed -n '2,/^$/p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done

if [ -z "$ISH" ]; then
    for cand in build-arm64-release/ish build/ish; do
        if [ -x "$cand" ]; then ISH="$cand"; break; fi
    done
fi
if [ -z "$ISH" ] || [ ! -x "$ISH" ]; then
    echo "no iSH binary found; build one or pass -i <path>" >&2
    exit 2
fi
if [ ! -d "$ROOTFS" ]; then
    echo "rootfs $ROOTFS not found; pass -r <path>" >&2
    exit 2
fi

CC_GUEST="${CC_GUEST:-aarch64-linux-musl-gcc}"

# An AddressSanitizer build of iSH (tools/build-asan.sh) writes any report to
# a file here, and a report fails the run whatever the tests printed: a
# corrupted host heap does not always change a guest's output.
ASAN_REPORTS=""
if grep -qa __asan_init "$ISH"; then
    ASAN_REPORTS=$(mktemp -d)
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$ASAN_REPORTS/asan"
    echo "$ISH is built with AddressSanitizer: reports go to $ASAN_REPORTS"
fi
if ! command -v "$CC_GUEST" >/dev/null 2>&1; then
    echo "SKIP: no guest cross-compiler ($CC_GUEST) available." >&2
    echo "      Install one or set CC_GUEST to build tests/regress/regress_syscall.c." >&2
    exit 0
fi

SRC=tests/regress/regress_syscall.c
# With -f the guest's / lives under <rootfs>/data; with -r it is <rootfs> itself.
GUEST_ROOT="$ROOTFS"
if [ "$MOUNT_FLAG" = "-f" ]; then
    GUEST_ROOT="$ROOTFS/data"
fi
BIN="$GUEST_ROOT/tmp/regress_syscall"
mkdir -p "$GUEST_ROOT/tmp"
echo "building $SRC with $CC_GUEST"
if ! "$CC_GUEST" -static -O0 -Wall -o "$BIN" "$SRC"; then
    echo "failed to build $SRC" >&2
    exit 1
fi
cp tests/regress/regress_cp.sh "$GUEST_ROOT/tmp/regress_cp.sh"

# A fakefs keeps its own metadata in meta.db, so files dropped straight into
# data/ are invisible to the guest. Register them by copying through the guest
# itself; the shell script is small enough for this to be cheap, but the test
# binary is not, so -f mode runs only the cp cases and expects the syscall
# binary to be run separately under -r (where every one of these assertions is
# equally valid -- none of them depend on the mount type).
if [ "$MOUNT_FLAG" = "-f" ]; then
    rm -f "$BIN"
    b64_sh=$(base64 < tests/regress/regress_cp.sh | tr -d '\n')
    rm -f "$GUEST_ROOT/tmp/regress_cp.sh"
    if ! "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh -c \
        "printf %s '$b64_sh' | base64 -d > /tmp/regress_cp.sh"; then
        echo "failed to stage regress_cp.sh into the fakefs" >&2
        exit 1
    fi
fi

status=0

# A section's header, once the phone is cool again when the tests run in the
# iSH app (tests/regress/wait_cool.sh; at once anywhere else): before the
# section's tests, so that no test's timeout is spent waiting
section() {
    tests/regress/wait_cool.sh
    echo "########## $1 ##########"
}

if [ "$MOUNT_FLAG" = "-f" ]; then
    echo
    section "syscall regressions"
    echo "  SKIP  staging a static binary into a fakefs is impractical;"
    echo "        run these with -r <rootfs> (assertions are mount-independent)."
else
    echo
    section "syscall regressions ($ISH $MOUNT_FLAG $ROOTFS)"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_syscall ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

    echo
    section "FMOV immediate decode"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fmov_imm" \
            tests/regress/regress_fmov_imm.c tests/regress/regress_fmov_imm.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fmov_imm || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fmov_imm"
    else
        echo "failed to build regress_fmov_imm" >&2
        status=1
    fi

    echo
    section "SMOV (to general) decode"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_smov" \
            tests/regress/regress_smov.c tests/regress/regress_smov.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_smov || status=1
        rm -f "$GUEST_ROOT/tmp/regress_smov"
    else
        echo "failed to build regress_smov" >&2
        status=1
    fi

    echo
    section "SIMD/FP instruction semantics"
    if "$CC_GUEST" -static -O1 -Wall -o "$GUEST_ROOT/tmp/regress_simd_insns" \
            tests/regress/regress_simd_insns.c tests/regress/regress_simd_insns.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_simd_insns || status=1
        rm -f "$GUEST_ROOT/tmp/regress_simd_insns"
    else
        echo "failed to build regress_simd_insns" >&2
        status=1
    fi

    echo
    section "scalar saturating add and subtract (SQADD, UQADD, SQSUB, UQSUB)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_scalar_saturating" \
            tests/regress/regress_scalar_saturating.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_scalar_saturating 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_scalar_saturating"
    else
        echo "failed to build regress_scalar_saturating" >&2
        status=1
    fi

    echo
    section "per-thread sigaltstack"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_sigaltstack" \
            tests/regress/regress_sigaltstack.c; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sigaltstack || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sigaltstack"
    else
        echo "failed to build regress_sigaltstack" >&2
        status=1
    fi

    echo
    section "getdents keeps no seekdir position"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_getdents" \
            tests/regress/regress_getdents.c; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_getdents || status=1
        rm -f "$GUEST_ROOT/tmp/regress_getdents"
    else
        echo "failed to build regress_getdents" >&2
        status=1
    fi

    echo
    section "preadv/pwritev(2) offset on a 64-bit guest"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_preadv2" \
            tests/regress/regress_preadv2.c; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_preadv2 || status=1
        rm -f "$GUEST_ROOT/tmp/regress_preadv2"
    else
        echo "failed to build regress_preadv2" >&2
        status=1
    fi

    # The guest dying on an undecoded instruction still leaves iSH exiting 0,
    # so this checks the summary line rather than the exit status.
    echo
    section "DC ZVA and DCZID_EL0"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_dc_zva" \
            tests/regress/regress_dc_zva.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_dc_zva 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_dc_zva"
    else
        echo "failed to build regress_dc_zva" >&2
        status=1
    fi

    echo
    section "/proc/self/smaps (redis' arm64 COW bug check)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_smaps" \
            tests/regress/regress_smaps.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_smaps 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_smaps"
    else
        echo "failed to build regress_smaps" >&2
        status=1
    fi

    echo
    section "SIGSEGV on unmapped reads and writes near 0"
    if "$CC_GUEST" -static -O1 -Wall -o "$GUEST_ROOT/tmp/regress_null_fault" \
            tests/regress/regress_null_fault.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_null_fault 2>&1)
        echo "$out" | tail -5
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_null_fault"
    else
        echo "failed to build regress_null_fault" >&2
        status=1
    fi

    echo
    section "LDPSW sign-extends"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_ldpsw" \
            tests/regress/regress_ldpsw.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_ldpsw 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_ldpsw"
    else
        echo "failed to build regress_ldpsw" >&2
        status=1
    fi

    echo
    section "SQABS (vector) saturates"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sqabs" \
            tests/regress/regress_sqabs.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sqabs 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sqabs"
    else
        echo "failed to build regress_sqabs" >&2
        status=1
    fi

    echo
    section "kill() reaches a thread in sigwait"
    if "$CC_GUEST" -static -O1 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_sigwait_kill" \
            tests/regress/regress_sigwait_kill.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sigwait_kill 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sigwait_kill"
    else
        echo "failed to build regress_sigwait_kill" >&2
        status=1
    fi

    echo
    section "*at() with an absolute path ignores dirfd"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_at_absolute" \
            tests/regress/regress_at_absolute.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_at_absolute 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_at_absolute"
    else
        echo "failed to build regress_at_absolute" >&2
        status=1
    fi

    echo
    section "128 KB stack mapping at exec"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_stack_extent" \
            tests/regress/regress_stack_extent.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_stack_extent 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_stack_extent"
    else
        echo "failed to build regress_stack_extent" >&2
        status=1
    fi

    # iSH exits with init's status. It used to exit 0 whatever init returned,
    # and when init exited with a process still running (a background job,
    # timeout's watchdog) halt_system deadlocked on pids_lock and iSH ended
    # with a host SIGTERM (143). init, as on Linux, ignores signals it has no
    # handler for, even SIGKILL, but a fault still ends it.
    echo
    section "exit status of init"
    exit_passed=0 exit_failed=0
    check_exit() { # want, then the command to run as init
        local want=$1
        shift
        # what iSH itself says, shown if its exit is not the one wanted (on
        # macOS 27 it died of SIGTRAP, 133, with nothing to say why)
        local said
        said=$(timeout 20 "$ISH" "$MOUNT_FLAG" "$ROOTFS" "$@" 2>&1 > /dev/null)
        local got=$?
        if [ "$got" = "$want" ]; then
            exit_passed=$((exit_passed + 1))
        else
            exit_failed=$((exit_failed + 1))
            echo "FAIL $*: iSH exited $got, expected $want"
            [ -z "$said" ] || printf '%s\n' "$said" | tail -n 20 | sed 's/^/    iSH: /'
        fi
    }
    check_exit 3 /bin/sh -c 'exit 3'
    check_exit 1 /bin/false
    check_exit 0 /bin/true
    check_exit 5 /bin/sh -c 'sleep 30 & exit 5'
    check_exit 7 /bin/sh -c 'kill -9 $$; exit 7'
    check_exit 8 /bin/sh -c 'kill -TERM $$; exit 8'
    check_exit 9 /bin/sh -c 'trap "exit 9" TERM; kill -TERM $$; sleep 1; exit 1'
    check_exit 137 /bin/sh -c 'sh -c "kill -9 \$\$"; exit $?'
    printf 'int main(void) { return *(volatile int *) 0; }\n' > "$GUEST_ROOT/tmp/regress_init_fault.c"
    if "$CC_GUEST" -static -O0 -o "$GUEST_ROOT/tmp/regress_init_fault" "$GUEST_ROOT/tmp/regress_init_fault.c"; then
        check_exit 139 /tmp/regress_init_fault
    else
        echo "failed to build regress_init_fault" >&2
        exit_failed=$((exit_failed + 1))
    fi
    printf 'int main(void) { __builtin_trap(); }\n' > "$GUEST_ROOT/tmp/regress_init_fault.c"
    if "$CC_GUEST" -static -O0 -o "$GUEST_ROOT/tmp/regress_init_fault" "$GUEST_ROOT/tmp/regress_init_fault.c"; then
        # A BRK in init, which has no SIGTRAP handler: forced, so it ends init
        # rather than being ignored as a signal init does not handle.
        check_exit 133 /tmp/regress_init_fault
    else
        echo "failed to build regress_init_fault (BRK)" >&2
        exit_failed=$((exit_failed + 1))
    fi
    rm -f "$GUEST_ROOT/tmp/regress_init_fault" "$GUEST_ROOT/tmp/regress_init_fault.c"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_init_thread_exit" \
            tests/regress/regress_init_thread_exit.c; then
        check_exit 5 /tmp/regress_init_thread_exit exit
        check_exit 0 /tmp/regress_init_thread_exit return
    else
        echo "failed to build regress_init_thread_exit" >&2
        exit_failed=$((exit_failed + 1))
    fi
    rm -f "$GUEST_ROOT/tmp/regress_init_thread_exit"
    echo "$exit_passed passed, $exit_failed failed"
    [ "$exit_failed" = 0 ] || status=1

    echo
    section "signals init acts on"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_init_signals" \
            tests/regress/regress_init_signals.c; then
        out=$(timeout 20 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_init_signals 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_init_signals"
    else
        echo "failed to build regress_init_signals" >&2
        status=1
    fi

    echo
    section "signals a task brings on itself"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_force_signal" \
            tests/regress/regress_force_signal.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_force_signal 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_force_signal"
    else
        echo "failed to build regress_force_signal" >&2
        status=1
    fi
    echo
    section "unix connections accepted after connect returns"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_unix_connect" \
            tests/regress/regress_unix_connect.c; then
        out=$(timeout 150 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_unix_connect 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_unix_connect"
    else
        echo "failed to build regress_unix_connect" >&2
        status=1
    fi
    echo
    section "unix sockets bound to a path on the rootfs; fchmodat2"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_unix_path" \
            tests/regress/regress_unix_path.c; then
        out=$(timeout 30 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_unix_path 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_unix_path"
    else
        echo "failed to build regress_unix_path" >&2
        status=1
    fi
    echo
    section "SO_PEERCRED, listen twice, abstract names freed"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_peercred" \
            tests/regress/regress_peercred.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_peercred 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_peercred"
    else
        echo "failed to build regress_peercred" >&2
        status=1
    fi
    echo
    section "a stuck thread's task, after its process exits"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_leaked_task" \
            tests/regress/regress_leaked_task.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_leaked_task 2>&1)
        echo "$out" | grep -v '^\[iSH\]'
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_leaked_task"
    else
        echo "failed to build regress_leaked_task" >&2
        status=1
    fi
    echo
    section "signals to a process: any thread takes them"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_process_signals" \
            tests/regress/regress_process_signals.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_process_signals 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_process_signals"
    else
        echo "failed to build regress_process_signals" >&2
        status=1
    fi
    echo
    section "a futex wait a signal interrupts, with no handler run, starts again"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_futex_restart" \
            tests/regress/regress_futex_restart.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_futex_restart 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_futex_restart"
    else
        echo "failed to build regress_futex_restart" >&2
        status=1
    fi
    echo
    section "set_tid_address returns the caller's thread id"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_set_tid_address" \
            tests/regress/regress_set_tid_address.c; then
        out=$(timeout 30 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_set_tid_address 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_set_tid_address"
    else
        echo "failed to build regress_set_tid_address" >&2
        status=1
    fi
    echo
    section "POSIX timers deleted by exec"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_exec_timers" \
            tests/regress/regress_exec_timers.c; then
        out=$(timeout 30 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_exec_timers 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_exec_timers"
    else
        echo "failed to build regress_exec_timers" >&2
        status=1
    fi
    echo
    section "/dev/shm: shm_open and sem_open"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_dev_shm" \
            tests/regress/regress_dev_shm.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_dev_shm 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_dev_shm"
    else
        echo "failed to build regress_dev_shm" >&2
        status=1
    fi
    echo
    section "mmap while threads spin in guest code"
    if "$CC_GUEST" -static -O1 -Wall -o "$GUEST_ROOT/tmp/regress_mmap_spin" \
            tests/regress/regress_mmap_spin.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_mmap_spin 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_mmap_spin"
    else
        echo "failed to build regress_mmap_spin" >&2
        status=1
    fi
    echo
    section "a page another thread remaps: made code, copied on write"
    if "$CC_GUEST" -static -O1 -Wall -o "$GUEST_ROOT/tmp/regress_tlb_cross_thread" \
            tests/regress/regress_tlb_cross_thread.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_tlb_cross_thread 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_tlb_cross_thread"
    else
        echo "failed to build regress_tlb_cross_thread" >&2
        status=1
    fi
    echo
    section "socket removed from an epoll set during a wait"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_poll_socket_removed" \
            tests/regress/regress_poll_socket_removed.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_poll_socket_removed 2>&1)
        echo "$out" | grep -v '^sockrestart:'
        n=$(echo "$out" | grep -c '^sockrestart:')
        [ "$n" = 0 ] || echo "FAIL $n tasks exited with socket waits not ended"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' && [ "$n" = 0 ] || status=1
        rm -f "$GUEST_ROOT/tmp/regress_poll_socket_removed"
    else
        echo "failed to build regress_poll_socket_removed" >&2
        status=1
    fi
    echo
    section "device nodes from a Darwin import"
    # A fakefs of its own: a tarball with the test as /init and three device
    # nodes, imported with fakefsify, two of them then rewritten in meta.db to
    # Darwin's makedev (major << 24 | minor), as the app stored them on iOS.
    FAKEFSIFY="$(dirname "$ISH")/tools/fakefsify"
    devdir=$(mktemp -d)
    if [ -x "$FAKEFSIFY" ] && "$CC_GUEST" -static -O0 -Wall -o "$devdir/init" \
            tests/regress/regress_dev_nodes.c &&
        python3 -I - "$devdir" <<'EOF' &&
import sys, tarfile
d = sys.argv[1]
with tarfile.open(d + '/root.tar.gz', 'w:gz') as t:
    for name in ('.', './dev', './tmp'):
        ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; t.addfile(ti)
    ti = t.gettarinfo(d + '/init', './init'); ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
    with open(d + '/init', 'rb') as f:
        t.addfile(ti, f)
    for name, major, minor in (('./dev/null', 1, 3), ('./dev/zero', 1, 5), ('./dev/tty7', 1, 3)):
        ti = tarfile.TarInfo(name); ti.type = tarfile.CHRTYPE; ti.mode = 0o666
        ti.devmajor, ti.devminor = major, minor; t.addfile(ti)
EOF
        "$FAKEFSIFY" "$devdir/root.tar.gz" "$devdir/fs" &&
        python3 -I - "$devdir/fs/meta.db" <<'EOF'
import sqlite3, struct, sys
db = sqlite3.connect(sys.argv[1])
darwin = {b'/dev/null': (1 << 24) | 3, b'/dev/zero': (1 << 24) | 5}
rows = db.execute('select paths.path, paths.inode, stats.stat from paths join stats on paths.inode = stats.inode').fetchall()
for path, inode, stat in rows:
    if bytes(path) in darwin:
        mode, uid, gid, _ = struct.unpack('<4I', stat[:16])
        db.execute('update stats set stat = ? where inode = ?',
                   (struct.pack('<4I', mode, uid, gid, darwin[bytes(path)]) + stat[16:], inode))
assert db.total_changes == 2, db.total_changes
db.commit()
EOF
    then
        out=$(timeout 20 "$ISH" -f "$devdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up the device node test (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$devdir"

    echo
    section "job control on a controlling terminal"
    # It needs /dev/ptmx, which a realfs root cannot have: a fakefs of its
    # own, with the test as /init.
    ttydir=$(mktemp -d)
    if [ -x "$FAKEFSIFY" ] && "$CC_GUEST" -static -O0 -Wall -o "$ttydir/init" \
            tests/regress/regress_tty_jobs.c &&
        python3 -I - "$ttydir" <<'EOF' &&
import sys, tarfile
d = sys.argv[1]
with tarfile.open(d + '/root.tar.gz', 'w:gz') as t:
    for name in ('.', './dev', './dev/pts', './tmp', './proc'):
        ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; t.addfile(ti)
    ti = t.gettarinfo(d + '/init', './init'); ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
    with open(d + '/init', 'rb') as f:
        t.addfile(ti, f)
EOF
        "$FAKEFSIFY" "$ttydir/root.tar.gz" "$ttydir/fs"
    then
        out=$(timeout 60 "$ISH" -f "$ttydir/fs" /init < /dev/null 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up the job control test (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$ttydir"

    echo
    section "a session's last process, orphaned, reaped by init after its leader"
    # It needs /dev/ptmx and to be init: a fakefs of its own, the test /init.
    sessdir=$(mktemp -d)
    if [ -x "$FAKEFSIFY" ] && "$CC_GUEST" -static -O0 -Wall -o "$sessdir/init" \
            tests/regress/regress_session_orphan.c &&
        python3 -I - "$sessdir" <<'EOF' &&
import sys, tarfile
d = sys.argv[1]
with tarfile.open(d + '/root.tar.gz', 'w:gz') as t:
    for name in ('.', './dev', './dev/pts', './tmp', './proc'):
        ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; t.addfile(ti)
    ti = t.gettarinfo(d + '/init', './init'); ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
    with open(d + '/init', 'rb') as f:
        t.addfile(ti, f)
EOF
        "$FAKEFSIFY" "$sessdir/root.tar.gz" "$sessdir/fs"
    then
        out=$(timeout 60 "$ISH" -f "$sessdir/fs" /init < /dev/null 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up the session test (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$sessdir"

    section "/dev/shm on a fakefs root: a fresh fakefs each boot"
    # Beside the rootfs, which must be on a filesystem iSH works on: one that
    # keeps a file unlinked while open usable (ftruncate), which $TMPDIR or
    # the checkout may not be (a macOS folder shared into a Linux VM).
    shmdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-shm.XXXXXX")
    if [ -x "$FAKEFSIFY" ] && "$CC_GUEST" -static -O0 -Wall -pthread -o "$shmdir/init" \
            tests/regress/regress_shm_mount.c &&
        python3 -I - "$shmdir" <<'EOF' &&
import sys, tarfile
d = sys.argv[1]
with tarfile.open(d + '/root.tar.gz', 'w:gz') as t:
    for name in ('.', './dev', './dev/pts', './tmp', './proc'):
        ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; t.addfile(ti)
    ti = t.gettarinfo(d + '/init', './init'); ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
    with open(d + '/init', 'rb') as f:
        t.addfile(ti, f)
EOF
        "$FAKEFSIFY" "$shmdir/root.tar.gz" "$shmdir/fs" &&
        timeout 30 "$ISH" -f "$shmdir/fs" /init leave
    then
        out=$(timeout 60 "$ISH" -f "$shmdir/fs" /init check 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up the /dev/shm test (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$shmdir"

    echo
    section "/dev/netcap: the traffic as a pcap stream (fakefs root)"
    # the device node is made at boot, which a realfs root cannot have
    capdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-netcap.XXXXXX")
    if [ -x "$FAKEFSIFY" ] && "$CC_GUEST" -static -O0 -Wall -o "$capdir/init" tests/regress/regress_netcap.c &&
        python3 -I - "$capdir" <<'EOF' &&
import sys, tarfile
d = sys.argv[1]
with tarfile.open(d + '/root.tar.gz', 'w:gz') as t:
    for name in ('.', './dev', './dev/pts', './tmp', './proc'):
        ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; t.addfile(ti)
    ti = t.gettarinfo(d + '/init', './init'); ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
    with open(d + '/init', 'rb') as f:
        t.addfile(ti, f)
EOF
        "$FAKEFSIFY" "$capdir/root.tar.gz" "$capdir/fs"
    then
        out=$(timeout 60 "$ISH" -f "$capdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up the /dev/netcap test (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$capdir"

    echo
    section "/dev/shm emptied at boot (realfs root)"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh -c 'echo left > /dev/shm/regress_left'
    if "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh -c 'test ! -e /dev/shm/regress_left'; then
        echo "PASS a file left in /dev/shm is gone at the next boot"
    else
        echo "FAIL a file left in /dev/shm is still there at the next boot"
        status=1
    fi
    echo
    section "/proc/stat: cpu line and btime"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_proc_stat" \
            tests/regress/regress_proc_stat.c; then
        out=$(timeout 30 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_stat 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_stat"
    else
        echo "failed to build regress_proc_stat" >&2
        status=1
    fi
    echo
    section "inotify: events of the filesystem operations"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_inotify" tests/regress/regress_inotify.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_inotify 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_inotify"
    else
        echo "failed to build regress_inotify" >&2
        status=1
    fi
    echo
    section "JIT: a host fault in native code (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_fault" tests/regress/regress_jit_fault.c; then
        for mode in resume unknown; do
            out=$(ISH_JIT_FAULT_TEST=$mode timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_fault $mode 2>&1)
            echo "$out"
            if echo "$out" | grep -q '^SKIP'; then
                :
            elif [ "$mode" = resume ]; then
                echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
            # killed at the fault, with the sample iSH logs for it
            elif echo "$out" | grep -q '^FAIL' ||
                    ! echo "$out" | grep -q 'host fault in native code, not at a guest access'; then
                echo "regress_jit_fault unknown: not killed with a sample in the log" >&2
                status=1
            fi
        done
        rm -f "$GUEST_ROOT/tmp/regress_jit_fault"
    else
        echo "failed to build regress_jit_fault" >&2
        status=1
    fi
    echo
    section "JIT: the code region flushed as processes come and go (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_flush" tests/regress/regress_jit_flush.c; then
        out=$(ISH_JIT_REGION_MB=1 timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_flush 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        # four arenas of 1 MB, each with its own stubs, the oldest evicted as the
        # one in use nears its end (not PIC: PIC translations are shared across
        # processes, and this test's do not fill 4 MB)
        out=$(ISH_JIT_REGION_MB=4 ISH_JIT_ARENA_MB=1 timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" \
              /tmp/regress_jit_flush 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_flush"
    else
        echo "failed to build regress_jit_flush" >&2
        status=1
    fi
    echo
    section "JIT: scalar floating point, the gadgets' results and its alike (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_fp" tests/regress/regress_jit_fp.c; then
        # position-independent, as on iOS: a block runs as gadgets till hot
        out=$(ISH_JIT_PIC=1 timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_fp 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_fp"
    else
        echo "failed to build regress_jit_fp" >&2
        status=1
    fi
    echo
    section "JIT: atomics, SIMD loads and stores, literal loads, dc zva, barriers, FPCR, SIMD arithmetic, the gadgets' results and its alike (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_insns" tests/regress/regress_jit_insns.c; then
        out=$(ISH_JIT_PIC=1 timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_insns 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_insns"
    else
        echo "failed to build regress_jit_insns" >&2
        status=1
    fi
    echo
    section "JIT: a TLB check of the last access's entry, the full lookup out of line, every result right (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_tlb_reuse" tests/regress/regress_jit_tlb_reuse.c; then
        out=$(ISH_JIT_PIC=1 timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_tlb_reuse 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_tlb_reuse"
    else
        echo "failed to build regress_jit_tlb_reuse" >&2
        status=1
    fi
    echo
    section "JIT: block ends testing a copy of their chain slots, the code they go to rewritten (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_chain_copy" tests/regress/regress_jit_chain_copy.c; then
        out=$(ISH_JIT_PIC=1 timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_chain_copy 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_chain_copy"
    else
        echo "failed to build regress_jit_chain_copy" >&2
        status=1
    fi
    echo
    section "JIT: guest V registers held in host registers within a segment, written through: every result C's"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_vcache" tests/regress/regress_jit_vcache.c -lm; then
        out=$(ISH_JIT_PIC=1 timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_vcache 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_vcache"
    else
        echo "failed to build regress_jit_vcache" >&2
        status=1
    fi
    echo
    section "JIT: a TLB check of the last stack access's entry, across segments (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_sp_entry" tests/regress/regress_jit_sp_entry.c -lpthread; then
        out=$(ISH_JIT_PIC=1 timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_sp_entry 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_sp_entry"
    else
        echo "failed to build regress_jit_sp_entry" >&2
        status=1
    fi
    echo
    section "JIT: calls by a host bl and returns by a host ret, the frame's shadow stack (ish -Djit=true; skips otherwise)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_jit_shadow" tests/regress/regress_jit_shadow.c -lpthread; then
        out=$(ISH_JIT_PIC=1 timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_jit_shadow 2>&1)
        echo "$out"
        echo "$out" | grep -q '^SKIP' || echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_jit_shadow"
    else
        echo "failed to build regress_jit_shadow" >&2
        status=1
    fi
    echo
    section "System V shared memory: shmget, shmat, shmdt, shmctl"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sysv_shm" tests/regress/regress_sysv_shm.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sysv_shm 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sysv_shm"
    else
        echo "failed to build regress_sysv_shm" >&2
        status=1
    fi
    echo
    section "System V message queues: msgget, msgsnd, msgrcv, msgctl"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sysv_msg" tests/regress/regress_sysv_msg.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sysv_msg 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sysv_msg"
    else
        echo "failed to build regress_sysv_msg" >&2
        status=1
    fi
    echo
    section "System V semaphores: semget, semop, semtimedop, semctl"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_sysv_sem" tests/regress/regress_sysv_sem.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sysv_sem 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sysv_sem"
    else
        echo "failed to build regress_sysv_sem" >&2
        status=1
    fi
    echo
    section "signalfd, and SIGCHLD's siginfo"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_signalfd" tests/regress/regress_signalfd.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_signalfd 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_signalfd"
    else
        echo "failed to build regress_signalfd" >&2
        status=1
    fi
    echo
    section "sigqueue: rt_sigqueueinfo, rt_tgsigqueueinfo"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_sigqueue" tests/regress/regress_sigqueue.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sigqueue 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sigqueue"
    else
        echo "failed to build regress_sigqueue" >&2
        status=1
    fi
    echo
    section "SIGCHLD when a child stops or continues; WCONTINUED"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sigchld_stop" tests/regress/regress_sigchld_stop.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sigchld_stop 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sigchld_stop"
    else
        echo "failed to build regress_sigchld_stop" >&2
        status=1
    fi
    echo
    section "SO_RCVTIMEO and SO_SNDTIMEO as a 64-bit libc names them"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sock_timeout" tests/regress/regress_sock_timeout.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sock_timeout 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sock_timeout"
    else
        echo "failed to build regress_sock_timeout" >&2
        status=1
    fi
    echo
    section "the options Java sets on every UDP socket"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_udp_java_opts" tests/regress/regress_udp_java_opts.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_udp_java_opts 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_udp_java_opts"
    else
        echo "failed to build regress_udp_java_opts" >&2
        status=1
    fi
    echo
    section "a socket option iSH does not know: ENOPROTOOPT, as the JDK probes for"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sockopt_unknown" tests/regress/regress_sockopt_unknown.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sockopt_unknown 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sockopt_unknown"
    else
        echo "failed to build regress_sockopt_unknown" >&2
        status=1
    fi
    echo
    section "AF_PACKET sockets and the interface ioctls, as libpcap uses them"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_packet" tests/regress/regress_packet.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_packet 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_packet"
    else
        echo "failed to build regress_packet" >&2
        status=1
    fi
    echo
    section "a sleep a stop or an ignored signal interrupts goes on (restart_syscall)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sleep_restart" tests/regress/regress_sleep_restart.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sleep_restart 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sleep_restart"
    else
        echo "failed to build regress_sleep_restart" >&2
        status=1
    fi
    echo
    section "/dev/fd/N and /dev/stdin of a pipe (bash process substitution, diff)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_dev_fd" tests/regress/regress_dev_fd.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_dev_fd 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_dev_fd"
    else
        echo "failed to build regress_dev_fd" >&2
        status=1
    fi
    echo
    section "poll, select and epoll semantics (hangups, edges, timeouts, files, latency)"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_poll_semantics" \
            tests/regress/regress_poll_semantics.c; then
        # REGRESS_SLOW_HOST (ci-mac.sh's): the latency cases' wider bound; the
        # guest is given env's, iSH's init having only TERM, HOME and PATH
        out=$(timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" ${REGRESS_SLOW_HOST:+/usr/bin/env REGRESS_SLOW_HOST=1} \
            /tmp/regress_poll_semantics 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_poll_semantics"
    else
        echo "failed to build regress_poll_semantics" >&2
        status=1
    fi
    echo
    section "fcntl record locks: F_SETLKW waits, arm64's struct flock"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fcntl_lock" tests/regress/regress_fcntl_lock.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fcntl_lock 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fcntl_lock"
    else
        echo "failed to build regress_fcntl_lock" >&2
        status=1
    fi
    echo
    section "many fds ready at once: epoll, poll, an edge among them"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_poll_many_ready" tests/regress/regress_poll_many_ready.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_poll_many_ready 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_poll_many_ready"
    else
        echo "failed to build regress_poll_many_ready" >&2
        status=1
    fi
    echo
    section "PROT_NONE: reads fault at their address, syscalls get EFAULT"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_prot_none" tests/regress/regress_prot_none.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_prot_none 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_prot_none"
    else
        echo "failed to build regress_prot_none" >&2
        status=1
    fi
    echo
    section "MAP_NORESERVE: reserve, commit, uncommit the JVM's way"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_noreserve" tests/regress/regress_noreserve.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_noreserve 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_noreserve"
    else
        echo "failed to build regress_noreserve" >&2
        status=1
    fi
    echo
    section "a mapping larger than the host's largest free range"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_big_commit" tests/regress/regress_big_commit.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_big_commit 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_big_commit"
    else
        echo "failed to build regress_big_commit" >&2
        status=1
    fi

    # A fakefs of its own with a test as /init: <dir>/fs, from <source>, built
    # with the flags given after it. It has what a realfs root cannot when ish
    # runs as a user: the device nodes iSH makes at boot (/dev/null,
    # /dev/ptmx), and owners of its own (a realfs file's are the host's, which
    # a user cannot give away).
    fakefs_init() {
        local dir=$1 source=$2
        shift 2
        [ -x "$FAKEFSIFY" ] && "$CC_GUEST" -static -O0 -Wall "$@" -o "$dir/init" "$source" &&
            python3 -I - "$dir" <<'EOF' &&
import sys, tarfile
d = sys.argv[1]
with tarfile.open(d + '/root.tar.gz', 'w:gz') as t:
    for name in ('.', './dev', './dev/pts', './tmp', './proc'):
        ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; t.addfile(ti)
    ti = t.gettarinfo(d + '/init', './init'); ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
    with open(d + '/init', 'rb') as f:
        t.addfile(ti, f)
EOF
            "$FAKEFSIFY" "$dir/root.tar.gz" "$dir/fs"
    }
    echo
    section "the mode of what is made: the umask's, not the host's too"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_create_mode" tests/regress/regress_create_mode.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_create_mode 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_create_mode"
    else
        echo "failed to build regress_create_mode" >&2
        status=1
    fi
    echo
    section "access(X_OK) as root of a file no one may execute: EACCES (0)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_access_exec" tests/regress/regress_access_exec.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_access_exec 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_access_exec"
    else
        echo "failed to build regress_access_exec" >&2
        status=1
    fi
    echo
    section "socket of a protocol the type does not have: EPROTONOSUPPORT (EINVAL)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_socket_protocol" tests/regress/regress_socket_protocol.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_socket_protocol 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_socket_protocol"
    else
        echo "failed to build regress_socket_protocol" >&2
        status=1
    fi
    echo
    section "a thread's name: PR_GET_NAME, /proc/<pid>/comm, /proc/self/task/<tid>"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_thread_name" tests/regress/regress_thread_name.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_thread_name 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_thread_name"
    else
        echo "failed to build regress_thread_name" >&2
        status=1
    fi
    echo
    section "a name ending in a slash: O_CREAT EISDIR, unlink ENOTDIR"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_trailing_slash" tests/regress/regress_trailing_slash.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_trailing_slash 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_trailing_slash"
    else
        echo "failed to build regress_trailing_slash" >&2
        status=1
    fi
    echo
    section "utimensat with AT_SYMLINK_NOFOLLOW: a symlink's own times"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_symlink_utime" tests/regress/regress_symlink_utime.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_symlink_utime 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_symlink_utime"
    else
        echo "failed to build regress_symlink_utime" >&2
        status=1
    fi
    echo
    section "the path walk on a fakefs root: symlinks, ENOTDIR, ELOOP, EACCES (fakefs root)"
    walkdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-fakefs_walk.XXXXXX")
    if fakefs_init "$walkdir" tests/regress/regress_fakefs_walk.c; then
        out=$(timeout 60 "$ISH" -f "$walkdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_fakefs_walk (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$walkdir"
    echo
    section "readdir on a fakefs root: read ahead, every d_ino stat's, seekdir, rewinddir, d_off (fakefs root)"
    readdirdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-fakefs_readdir.XXXXXX")
    if fakefs_init "$readdirdir" tests/regress/regress_fakefs_readdir.c; then
        out=$(timeout 60 "$ISH" -f "$readdirdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_fakefs_readdir (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$readdirdir"
    echo
    section "fakefs's lookup cache: another process's chmod seen (two ish on one fakefs root)"
    shareddir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-fakefs_shared.XXXXXX")
    if fakefs_init "$shareddir" tests/regress/regress_fakefs_shared.c; then
        timeout 60 "$ISH" -f "$shareddir/fs" /init watch > "$shareddir/watch.out" 2>&1 &
        watcher=$!
        for _ in $(seq 100); do
            [ -e "$shareddir/fs/data/tmp/shared/ready" ] && break
            sleep 0.1
        done
        timeout 60 "$ISH" -f "$shareddir/fs" /init change 2>&1
        wait "$watcher"
        out=$(cat "$shareddir/watch.out")
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_fakefs_shared (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$shareddir"
    echo
    section "tcdrain, tcsendbreak, tcflow on a pseudo-terminal (ENOTTY) (fakefs root)"
    # a pseudo-terminal needs /dev/ptmx
    ttyflowdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-tty_flow.XXXXXX")
    if fakefs_init "$ttyflowdir" tests/regress/regress_tty_flow.c -pthread; then
        out=$(timeout 60 "$ISH" -f "$ttyflowdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_tty_flow (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$ttyflowdir"
    echo
    section "prlimit of a process by its pid (EINVAL)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_prlimit" tests/regress/regress_prlimit.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_prlimit 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_prlimit"
    else
        echo "failed to build regress_prlimit" >&2
        status=1
    fi
    echo
    section "a path's components as Linux's walk looks them up; FICLONE EOPNOTSUPP"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_path_walk" tests/regress/regress_path_walk.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_path_walk 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_path_walk"
    else
        echo "failed to build regress_path_walk" >&2
        status=1
    fi
    echo
    section "an IP or IPv6 socket's control messages, received and sent"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_ip_cmsgs" tests/regress/regress_ip_cmsgs.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_ip_cmsgs 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_ip_cmsgs"
    else
        echo "failed to build regress_ip_cmsgs" >&2
        status=1
    fi
    echo
    section "a send with MSG_DONTWAIT: EAGAIN when it does not fit, not blocked"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_send_dontwait" tests/regress/regress_send_dontwait.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_send_dontwait 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_send_dontwait"
    else
        echo "failed to build regress_send_dontwait" >&2
        status=1
    fi
    echo
    section "the end of a FIFO's data: a blocking read says 0 when its writer closes"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fifo_eof" tests/regress/regress_fifo_eof.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fifo_eof 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fifo_eof"
    else
        echo "failed to build regress_fifo_eof" >&2
        status=1
    fi
    echo
    section "a pipe's capacity: F_GETPIPE_SZ, F_SETPIPE_SZ"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_pipe_size" tests/regress/regress_pipe_size.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_pipe_size 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_pipe_size"
    else
        echo "failed to build regress_pipe_size" >&2
        status=1
    fi
    echo
    section "/proc/<pid>/map_files: a link for each mapping of a file, to it"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_map_files" tests/regress/regress_map_files.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_map_files 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_map_files"
    else
        echo "failed to build regress_map_files" >&2
        status=1
    fi
    echo
    section "/proc/<pid>/task/<tid>: a thread's own directory, its own CPU time"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_thread_stat" tests/regress/regress_thread_stat.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_thread_stat 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_thread_stat"
    else
        echo "failed to build regress_thread_stat" >&2
        status=1
    fi
    echo
    section "/proc: zombies, a thread's fd, . and .."
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_proc_zombie" tests/regress/regress_proc_zombie.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_zombie 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_zombie"
    else
        echo "failed to build regress_proc_zombie" >&2
        status=1
    fi
    echo
    section "/proc/<pid>/environ: the environment a process was started with"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_proc_environ" tests/regress/regress_proc_environ.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_environ 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_environ"
    else
        echo "failed to build regress_proc_environ" >&2
        status=1
    fi
    echo
    section "maps: a program's code r-x, its stack and heap rw-, as Linux says"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_maps_perms" tests/regress/regress_maps_perms.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_maps_perms 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_maps_perms"
    else
        echo "failed to build regress_maps_perms" >&2
        status=1
    fi
    echo
    section "struct rusage as arm64 has it, written whole (getrusage, wait4)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_rusage" tests/regress/regress_rusage.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_rusage 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_rusage"
    else
        echo "failed to build regress_rusage" >&2
        status=1
    fi
    echo
    section "getpeername of a TCP connection its peer reset: ENOTCONN, not EINVAL"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_getpeername_reset" tests/regress/regress_getpeername_reset.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_getpeername_reset 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_getpeername_reset"
    else
        echo "failed to build regress_getpeername_reset" >&2
        status=1
    fi
    echo
    section "idle processes' translations given back (/proc/ish/translations), and run again"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_reclaim_translations" tests/regress/regress_reclaim_translations.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_reclaim_translations 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_reclaim_translations"
    else
        echo "failed to build regress_reclaim_translations" >&2
        status=1
    fi
    echo
    section "a forked child uses its parent's translations, and leaves them when its code differs"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fork_translations" tests/regress/regress_fork_translations.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fork_translations 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fork_translations"
    else
        echo "failed to build regress_fork_translations" >&2
        status=1
    fi
    echo
    section "calls CPython's test_os found wrong (EFD_SEMAPHORE, memfd_create, priorities)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_os_calls" tests/regress/regress_os_calls.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_os_calls 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_os_calls"
    else
        echo "failed to build regress_os_calls" >&2
        status=1
    fi
    echo
    section "unix sockets' names (autobind, getpeername), msg_flags, ENOTSOCK"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_unix_names" tests/regress/regress_unix_names.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_unix_names 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_unix_names"
    else
        echo "failed to build regress_unix_names" >&2
        status=1
    fi
    echo
    section "dup2, dup3, F_DUPFD given an fd they may not take (iSH's heap broken)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_dup_bad_fd" tests/regress/regress_dup_bad_fd.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_dup_bad_fd 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        echo "$out" | grep -q 'HOST CRASH' && status=1
        rm -f "$GUEST_ROOT/tmp/regress_dup_bad_fd"
    else
        echo "failed to build regress_dup_bad_fd" >&2
        status=1
    fi
    echo
    section "SCM_RIGHTS of an fd that is not: EBADF (iSH crashed)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_scm_rights_bad_fd" tests/regress/regress_scm_rights_bad_fd.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_scm_rights_bad_fd 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_scm_rights_bad_fd"
    else
        echo "failed to build regress_scm_rights_bad_fd" >&2
        status=1
    fi
    echo
    section "mmap of a file past 4GB; what msync refuses"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_mmap_offset_msync" tests/regress/regress_mmap_offset_msync.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_mmap_offset_msync 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_mmap_offset_msync"
    else
        echo "failed to build regress_mmap_offset_msync" >&2
        status=1
    fi
    echo
    section "mremap: lengths in whole pages, a range not all mapped (iSH crashed)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_mremap" tests/regress/regress_mremap.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_mremap 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_mremap"
    else
        echo "failed to build regress_mremap" >&2
        status=1
    fi
    echo
    section "poll of an fd that is not: POLLNVAL, ready at once (it waited)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_poll_nval" tests/regress/regress_poll_nval.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_poll_nval 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_poll_nval"
    else
        echo "failed to build regress_poll_nval" >&2
        status=1
    fi
    echo
    section "ITIMER_VIRTUAL and ITIMER_PROF"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_cpu_itimers" tests/regress/regress_cpu_itimers.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_cpu_itimers 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_cpu_itimers"
    else
        echo "failed to build regress_cpu_itimers" >&2
        status=1
    fi
    echo
    section "a pipe or FIFO end used the other way: EBADF at once (it waited)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_pipe_wrong_end" tests/regress/regress_pipe_wrong_end.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_pipe_wrong_end 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_pipe_wrong_end"
    else
        echo "failed to build regress_pipe_wrong_end" >&2
        status=1
    fi
    echo
    section "calls CPython's test_posix found wrong (fexecve, chown, linkat, affinity, ...) (fakefs root)"
    # chown's checks need owners of its own
    posixdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-posix_calls.XXXXXX")
    if fakefs_init "$posixdir" tests/regress/regress_posix_calls.c; then
        out=$(timeout 60 "$ISH" -f "$posixdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_posix_calls (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$posixdir"
    echo
    section "*at calls from a directory fd that is no directory: ENOTDIR (iSH aborted) (fakefs root)"
    # /dev/null's fd is one of the cases
    dirfddir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-dirfd_not_dir.XXXXXX")
    if fakefs_init "$dirfddir" tests/regress/regress_dirfd_not_dir.c; then
        out=$(timeout 60 "$ISH" -f "$dirfddir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_dirfd_not_dir (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$dirfddir"
    echo
    section "truncate(2) of a path (iSH aborted)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_truncate_path" tests/regress/regress_truncate_path.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_truncate_path 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_truncate_path"
    else
        echo "failed to build regress_truncate_path" >&2
        status=1
    fi
    echo
    section "paths longer than the host's (Darwin's 1024 bytes): a directory 2.4 KB deep"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_long_path" tests/regress/regress_long_path.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_long_path 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_long_path"
    else
        echo "failed to build regress_long_path" >&2
        status=1
    fi
    echo
    section "paths longer than the host's (fakefs root: the app's)"
    longpathdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-long_path.XXXXXX")
    if fakefs_init "$longpathdir" tests/regress/regress_long_path.c; then
        out=$(timeout 60 "$ISH" -f "$longpathdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_long_path (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$longpathdir"
    echo
    section "open with O_DIRECTORY of no directory: ENOTDIR at once (a FIFO's open waited) (fakefs root)"
    # /dev/null is one of the cases
    opendirdir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-open_directory.XXXXXX")
    if fakefs_init "$opendirdir" tests/regress/regress_open_directory.c; then
        out=$(timeout 60 "$ISH" -f "$opendirdir/fs" /init 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_open_directory (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$opendirdir"
    echo
    section "a directory whose host path is out of its fakefs (a stale bind mount's link): ENOENT, not the root's"
    # its /mnt made at a first boot, then on the host a link out of the fakefs
    outsidedir=$(mktemp -d "$(cd "$(dirname "$ROOTFS")" && pwd)/regress-getpath_outside.XXXXXX")
    if fakefs_init "$outsidedir" tests/regress/regress_getpath_outside.c &&
        timeout 30 "$ISH" -f "$outsidedir/fs" /init setup &&
        mkdir "$outsidedir/outside" && rmdir "$outsidedir/fs/data/mnt" &&
        ln -s "$outsidedir/outside" "$outsidedir/fs/data/mnt"
    then
        out=$(timeout 60 "$ISH" -f "$outsidedir/fs" /init check 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
    else
        echo "failed to set up regress_getpath_outside (needs $FAKEFSIFY and python3)" >&2
        status=1
    fi
    rm -rf "$outsidedir"
    echo
    section "interval timers: getitimer, fired one-shots, bad ids, TIMER_ABSTIME 0, timerfd set again"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_timer_values" tests/regress/regress_timer_values.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_timer_values 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_timer_values"
    else
        echo "failed to build regress_timer_values" >&2
        status=1
    fi
    echo
    section "getsockopt(SO_ACCEPTCONN)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_so_acceptconn" tests/regress/regress_so_acceptconn.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_so_acceptconn 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_so_acceptconn"
    else
        echo "failed to build regress_so_acceptconn" >&2
        status=1
    fi
    echo
    section "a fault's ESR in its SIGSEGV frame (arm64)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sigsegv_esr" tests/regress/regress_sigsegv_esr.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sigsegv_esr 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sigsegv_esr"
    else
        echo "failed to build regress_sigsegv_esr" >&2
        status=1
    fi
    echo
    section "a thread's and a process's CPU-time clock (pthread_getcpuclockid, clock_getcpuclockid)"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_cpu_clock" tests/regress/regress_cpu_clock.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_cpu_clock 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_cpu_clock"
    else
        echo "failed to build regress_cpu_clock" >&2
        status=1
    fi
    echo
    section "times() and SIGCHLD's CPU times (clock_t a long)"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_times" tests/regress/regress_times.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_times 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_times"
    else
        echo "failed to build regress_times" >&2
        status=1
    fi
    echo
    section "a fork or thread whose host thread does not start (ISH_TASK_START_FAIL=3)"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_clone_start_fail" \
            tests/regress/regress_clone_start_fail.c; then
        out=$(ISH_TASK_START_FAIL=3 timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_clone_start_fail 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_clone_start_fail"
    else
        echo "failed to build regress_clone_start_fail" >&2
        status=1
    fi
    echo
    section "read-only file mappings shared between processes"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_shared_map" tests/regress/regress_shared_map.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_shared_map 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_shared_map"
    else
        echo "failed to build regress_shared_map" >&2
        status=1
    fi
    echo
    section "/proc/ish/host: lazy commits, MADV_DONTNEED, translated blocks"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_host_stats" tests/regress/regress_host_stats.c; then
        out=$(timeout 120 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_host_stats 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_host_stats"
    else
        echo "failed to build regress_host_stats" >&2
        status=1
    fi
    echo
    section "a /proc file read in parts while it changes"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_proc_snapshot" tests/regress/regress_proc_snapshot.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_snapshot 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_snapshot"
    else
        echo "failed to build regress_proc_snapshot" >&2
        status=1
    fi
    echo
    section "a signal whose frame cannot be written: SIGSEGV, not a hang"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_sigframe_unwritable" tests/regress/regress_sigframe_unwritable.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sigframe_unwritable 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sigframe_unwritable"
    else
        echo "failed to build regress_sigframe_unwritable" >&2
        status=1
    fi
    echo
    section "/proc/<pid>/syscall: the call a thread waits in, its stack and pc"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_proc_syscall" tests/regress/regress_proc_syscall.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_syscall 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_syscall"
    else
        echo "failed to build regress_proc_syscall" >&2
        status=1
    fi
    echo
    section "floating-point environment: exception flags, rounding mode"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fenv" tests/regress/regress_fenv.c -lm; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fenv 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fenv"
    else
        echo "failed to build regress_fenv" >&2
        status=1
    fi
    echo
    section "blocking calls interrupted: restart or EINTR"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_eintr" tests/regress/regress_eintr.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_eintr 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_eintr"
    else
        echo "failed to build regress_eintr" >&2
        status=1
    fi
    echo
    section "task state and CPU time: /proc/<pid>/stat, getrusage, CPU clocks"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_proc_time" \
            tests/regress/regress_proc_time.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_time 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_time"
    else
        echo "failed to build regress_proc_time" >&2
        status=1
    fi
    echo
    section "/proc/<pid>/stat and status, meminfo, loadavg, vmstat"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_proc_pid" \
            tests/regress/regress_proc_pid.c; then
        out=$(timeout 30 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_pid 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_pid"
    else
        echo "failed to build regress_proc_pid" >&2
        status=1
    fi
    echo
    section "/proc/net/tcp, udp and socket:[inode] links"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_proc_net" \
            tests/regress/regress_proc_net.c; then
        out=$(timeout 60 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_net 2>&1)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_proc_net"
    else
        echo "failed to build regress_proc_net" >&2
        status=1
    fi

    # AF_NETLINK stub: no read blocks without bound; links and addresses are
    # the host's; everything else is refused at once. The second run pins
    # the ISH_NETLINK_ROUTE=empty switch (read from the host environment).
    echo
    section "netlink stub"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_netlink_stub" \
            tests/regress/regress_netlink_stub.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_netlink_stub 2>/dev/null)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$|^SKIP' || status=1
        out=$(ISH_NETLINK_ROUTE=empty "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_netlink_stub route-empty "(d)" 2>/dev/null)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$|^SKIP' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_netlink_stub"
    else
        echo "failed to build regress_netlink_stub" >&2
        status=1
    fi

    # A process exiting while its other threads wait in epoll_wait must leave
    # the kernel able to fork. On a fast idle host the safety valve rarely
    # leaks anything, so this passing here is necessary, not sufficient; the
    # leak path itself is exercised on a loaded device (see README).
    echo
    section "leaked thread exit"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_leaked_thread_exit" \
            tests/regress/regress_leaked_thread_exit.c; then
        out=$(timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_leaked_thread_exit 10 8 16 2>/dev/null | tail -1)
        echo "$out"
        echo "$out" | grep -q '^PASS' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_leaked_thread_exit"
    else
        echo "failed to build regress_leaked_thread_exit" >&2
        status=1
    fi
fi

echo
section "cp regressions"
"$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh /tmp/regress_cp.sh ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

rm -f "$BIN" "$GUEST_ROOT/tmp/regress_cp.sh"

if [ -n "$ASAN_REPORTS" ]; then
    echo
    echo "########## AddressSanitizer reports ##########"
    reports=$(ls "$ASAN_REPORTS"/asan.* 2>/dev/null)
    if [ -n "$reports" ]; then
        cat $reports
        status=1
    else
        echo "none"
    fi
fi

echo
if [ "$status" = "0" ]; then
    echo "regress: all cases passed"
else
    echo "regress: FAILURES (see above)"
fi
exit $status
