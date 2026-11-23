#!/bin/bash
# Run the guest smoke test (guest-smoke.sh) in an arm64 rootfs.
#
# Usage:
#   tests/smoke/run.sh -i <ish binary> -f <fakefs rootfs>
#
# The rootfs is one tools/build-rootfs.sh made, imported with fakefsify; the
# checks need the network. The tools they use (python3, node and npm, Claude
# Code, go) are installed first with ish-install, copied in from the app's
# RootfsPatch.bundle as the app would; one already installed is skipped. Then
# the script goes to the rootfs's /tmp and runs in a login shell.
#
# Given an iSH built with AddressSanitizer (tools/build-asan.sh), the run
# fails on any ASan report, which it prints at the end: a corrupted host
# heap does not always fail a check.

set -u
cd "$(dirname "$0")/../.."

ISH=""
ROOTFS=""
while getopts "i:f:h" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        f) ROOTFS="$OPTARG" ;;
        h) sed -n '2,/^$/p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done
if [ -z "$ISH" ] || [ ! -x "$ISH" ] || [ -z "$ROOTFS" ] || [ ! -d "$ROOTFS" ]; then
    sed -n '2,/^$/p' "$0" >&2
    exit 2
fi

ASAN_REPORTS=""
if grep -qa __asan_init "$ISH"; then
    ASAN_REPORTS=$(mktemp -d)
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$ASAN_REPORTS/asan"
    echo "$ISH is built with AddressSanitizer: reports go to $ASAN_REPORTS"
fi

"$ISH" -f "$ROOTFS" /bin/sh -c 'cat > /usr/local/bin/ish-install && chmod 755 /usr/local/bin/ish-install' \
    < app/RootfsPatch.bundle/files/usr/local/bin/ish-install || exit 1
"$ISH" -f "$ROOTFS" /bin/sh -l -c 'ish-install python claude go' || exit 1
"$ISH" -f "$ROOTFS" /bin/sh -c 'cat > /tmp/guest-smoke.sh' < tests/smoke/guest-smoke.sh || exit 1
out=$("$ISH" -f "$ROOTFS" /bin/sh -l -c 'sh /tmp/guest-smoke.sh' 2>&1)
rc=$?
echo "$out"
status=0
echo "$out" | grep -q '^SUMMARY pass=[0-9]* fail=0$' || status=1
if [ $rc != 0 ]; then
    echo "iSH exited $rc"
    status=1
fi

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
exit $status
