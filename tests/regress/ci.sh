#!/bin/sh
# What CI runs on an AArch64 Linux host (.github/workflows/ci.yml): the CLI
# built as a release, a warning an error, and tests/regress run with it on the
# Alpine aarch64 minirootfs the app's root is made from (tools/build-rootfs.sh),
# its programs compiled by gcc in a container of that Alpine: they link the
# app's musl, not the host's (Ubuntu 24.04's musl-gcc, musl 1.2.4, whose _Fork
# hung regress_proc_zombie where Alpine's 1.2.6 does not). Each program is kept
# in <work dir>/guest-programs (tests/regress/cc_guest.py) for the macOS job
# (ci-mac.sh), which has no compiler for the guest.
#
# usage: tests/regress/ci.sh <work dir>
#   the build, the rootfs and the programs go in <work dir>, new
#
# Needs clang, lld, meson, ninja, pkg-config, the sqlite3 and libarchive headers,
# python3, curl, Docker and the network (the rootfs, the container's image and
# its packages).
set -eu
work=$1
cd "$(dirname "$0")/../.."
repo=$(pwd)

# tools/build-rootfs.sh's
ALPINE_RELEASE=3.24.4
ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-$ALPINE_RELEASE-aarch64.tar.gz
ALPINE_SHA256=4b14c18f7e90a063b61462ea1462a5732c154bf6adad594974fe6487cd98a8de

mkdir "$work"
work=$(cd "$work" && pwd)

CC=clang meson setup "$work/build" --buildtype=release -Dwerror=true
ninja -C "$work/build" ish tools/fakefsify

# The guest compiler: gcc in alpine:<release>, the repository and the work dir
# at their own paths in it, run as this user (run.sh removes what it writes)
container=ish-guest-cc
docker run -d --rm --name "$container" -v "$repo:$repo" -v "$work:$work" "alpine:$ALPINE_RELEASE" sleep infinity
trap 'docker stop -t 0 "$container" > /dev/null' EXIT
docker exec "$container" apk add --no-cache -q gcc musl-dev linux-headers
printf '#!/bin/sh\nexec docker exec -u %s -w "$PWD" %s gcc "$@"\n' "$(id -u):$(id -g)" "$container" > "$work/cc-alpine"
printf '#!/bin/sh\nexec python3 -I %s/tests/regress/cc_guest.py record %s/guest-programs %s/cc-alpine "$@"\n' \
    "$repo" "$work" "$work" > "$work/cc-guest"
chmod +x "$work/cc-alpine" "$work/cc-guest"

curl -fsSL -o "$work/alpine.tar.gz" "$ALPINE"
echo "$ALPINE_SHA256  $work/alpine.tar.gz" | sha256sum -c -
mkdir "$work/rootfs"
tar -xzf "$work/alpine.tar.gz" -C "$work/rootfs"

# run.sh's temporary directories in the work dir too, where the container sees
# what it compiles into them
mkdir "$work/tmp"
TMPDIR="$work/tmp" CC_GUEST="$work/cc-guest" tests/regress/run.sh -i "$work/build/ish" -r "$work/rootfs"
