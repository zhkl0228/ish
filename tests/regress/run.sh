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

if [ "$MOUNT_FLAG" = "-f" ]; then
    echo
    echo "########## syscall regressions ##########"
    echo "  SKIP  staging a static binary into a fakefs is impractical;"
    echo "        run these with -r <rootfs> (assertions are mount-independent)."
else
    echo
    echo "########## syscall regressions ($ISH $MOUNT_FLAG $ROOTFS) ##########"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_syscall ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

    echo
    echo "########## FMOV immediate decode ##########"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fmov_imm" \
            tests/regress/regress_fmov_imm.c tests/regress/regress_fmov_imm.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fmov_imm || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fmov_imm"
    else
        echo "failed to build regress_fmov_imm" >&2
        status=1
    fi

    echo
    echo "########## SMOV (to general) decode ##########"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_smov" \
            tests/regress/regress_smov.c tests/regress/regress_smov.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_smov || status=1
        rm -f "$GUEST_ROOT/tmp/regress_smov"
    else
        echo "failed to build regress_smov" >&2
        status=1
    fi

    echo
    echo "########## SIMD/FP instruction semantics ##########"
    if "$CC_GUEST" -static -O1 -Wall -o "$GUEST_ROOT/tmp/regress_simd_insns" \
            tests/regress/regress_simd_insns.c tests/regress/regress_simd_insns.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_simd_insns || status=1
        rm -f "$GUEST_ROOT/tmp/regress_simd_insns"
    else
        echo "failed to build regress_simd_insns" >&2
        status=1
    fi

    echo
    echo "########## per-thread sigaltstack ##########"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_sigaltstack" \
            tests/regress/regress_sigaltstack.c; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_sigaltstack || status=1
        rm -f "$GUEST_ROOT/tmp/regress_sigaltstack"
    else
        echo "failed to build regress_sigaltstack" >&2
        status=1
    fi

    echo
    echo "########## getdents keeps no seekdir position ##########"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_getdents" \
            tests/regress/regress_getdents.c; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_getdents || status=1
        rm -f "$GUEST_ROOT/tmp/regress_getdents"
    else
        echo "failed to build regress_getdents" >&2
        status=1
    fi

    echo
    echo "########## preadv/pwritev(2) offset on a 64-bit guest ##########"
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
    echo "########## DC ZVA and DCZID_EL0 ##########"
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
    echo "########## /proc/self/smaps (redis' arm64 COW bug check) ##########"
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
    echo "########## SIGSEGV on unmapped reads and writes near 0 ##########"
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
    echo "########## LDPSW sign-extends ##########"
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
    echo "########## kill() reaches a thread in sigwait ##########"
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
    echo "########## *at() with an absolute path ignores dirfd ##########"
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
    echo "########## 128 KB stack mapping at exec ##########"
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
    echo "########## exit status of init ##########"
    exit_passed=0 exit_failed=0
    check_exit() { # want, then the command to run as init
        local want=$1
        shift
        timeout 20 "$ISH" "$MOUNT_FLAG" "$ROOTFS" "$@" > /dev/null 2>&1
        local got=$?
        if [ "$got" = "$want" ]; then
            exit_passed=$((exit_passed + 1))
        else
            exit_failed=$((exit_failed + 1))
            echo "FAIL $*: iSH exited $got, expected $want"
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
    echo "########## signals init acts on ##########"
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
    echo "########## signals a task brings on itself ##########"
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
    echo "########## unix connections accepted after connect returns ##########"
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
    echo "########## unix sockets bound to a path on the rootfs; fchmodat2 ##########"
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
    echo "########## SO_PEERCRED, listen twice, abstract names freed ##########"
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
    echo "########## signals to a process: any thread takes them ##########"
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
    echo "########## POSIX timers deleted by exec ##########"
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
    echo "########## /dev/shm: shm_open and sem_open ##########"
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
    echo "########## mmap while threads spin in guest code ##########"
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
    echo "########## socket removed from an epoll set during a wait ##########"
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
    echo "########## device nodes from a Darwin import ##########"
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
    echo "########## /proc/stat: cpu line and btime ##########"
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
    echo "########## /proc/<pid>/stat and status, meminfo, loadavg, vmstat ##########"
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
    echo "########## /proc/net/tcp, udp and socket:[inode] links ##########"
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
    echo "########## netlink stub ##########"
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
    echo "########## leaked thread exit ##########"
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
echo "########## cp regressions ##########"
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
