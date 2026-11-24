#!/bin/sh
# What CI runs on an AArch64 Linux host (.github/workflows/ci.yml): the CLI
# built as a release, a warning an error, and tests/regress run with it on an
# Alpine aarch64 minirootfs, musl-gcc the guest compiler.
#
# usage: tests/regress/ci.sh <work dir>
#   the build, the rootfs and the guest compiler go in <work dir>, new
#
# Needs clang, lld, meson, ninja, pkg-config, the sqlite3 and libarchive headers,
# musl-gcc (musl-tools), python3, curl and the network (the rootfs).
set -eu
work=$1
cd "$(dirname "$0")/../.."

ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/alpine-minirootfs-3.21.3-aarch64.tar.gz
ALPINE_SHA256=ead8a4b37867bd19e7417dd078748e2312c0aea364403d96758d63ea8ff261ea

mkdir "$work"
work=$(cd "$work" && pwd)

CC=clang meson setup "$work/build" --buildtype=release -Dwerror=true
ninja -C "$work/build" ish tools/fakefsify

# The guest compiler: musl-gcc, with the kernel's uapi headers that musl's
# lack (linux/, asm/, asm-generic/: regress_packet, regress_netlink_stub)
mkdir "$work/kinc"
ln -s /usr/include/linux "$work/kinc/linux"
ln -s /usr/include/asm-generic "$work/kinc/asm-generic"
ln -s /usr/include/aarch64-linux-gnu/asm "$work/kinc/asm"
printf '#!/bin/sh\nexec musl-gcc -isystem %s "$@"\n' "$work/kinc" > "$work/cc-guest"
chmod +x "$work/cc-guest"

curl -fsSL -o "$work/alpine.tar.gz" "$ALPINE"
echo "$ALPINE_SHA256  $work/alpine.tar.gz" | sha256sum -c -
mkdir "$work/rootfs"
tar -xzf "$work/alpine.tar.gz" -C "$work/rootfs"

CC_GUEST="$work/cc-guest" tests/regress/run.sh -i "$work/build/ish" -r "$work/rootfs"
