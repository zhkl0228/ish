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
        h) sed -n '2,26p' "$0"; exit 0 ;;
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

    echo
    echo "########## /proc/net/tcp, udp and socket:[inode] links ##########"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_proc_net" \
            tests/regress/regress_proc_net.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_proc_net 2>&1)
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

echo
if [ "$status" = "0" ]; then
    echo "regress: all cases passed"
else
    echo "regress: FAILURES (see above)"
fi
exit $status
