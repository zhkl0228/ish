#!/bin/sh
# What CI runs on macOS (.github/workflows/ci.yml's mac-regress): the CLI built
# for Darwin, the app's host as iOS is, as a release, a compiler's warning an
# error, and tests/regress run with it on the Alpine minirootfs the linux job
# ran them on.
# What only Darwin does the linux job cannot see (a FIFO's hangup that Darwin's
# poll does not report; HOST_CPU_LOAD_INFO's 32-bit counts, which wrapped on a
# Mac up a month, below its CPUs' in /proc/stat). Darwin has no compiler for
# the guest: the programs are the linux job's, compiled in Alpine and kept by
# their arguments and sources (tests/regress/cc_guest.py).
#
# usage: tests/regress/ci-mac.sh <work dir> <guest programs>
#   the build and the rootfs go in <work dir>, new; <guest programs> is the
#   linux job's <work dir>/guest-programs
#
# Needs Xcode's clang, lld (the vDSO is linked as a Linux ELF), meson, ninja,
# pkg-config, libarchive and coreutils (Homebrew's: timeout, which run.sh runs
# everything under), python3, curl and the network (the rootfs).
set -eu
work=$1
programs=$(cd "$2" && pwd)
cd "$(dirname "$0")/../.."
repo=$(pwd)

# ci.sh's
ALPINE_RELEASE=3.24.4
ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-$ALPINE_RELEASE-aarch64.tar.gz
ALPINE_SHA256=4b14c18f7e90a063b61462ea1462a5732c154bf6adad594974fe6487cd98a8de

mkdir "$work"
work=$(cd "$work" && pwd)

# Homebrew's libarchive is keg-only: pkg-config finds it by its own path. A
# warning an error when compiling (-Werror), not when linking, which
# -Dwerror=true would make so too (ld -fatal_warnings): Homebrew's bottle is
# built on a newer macOS than the image may be (27.2's on xcode-27's 27.0),
# and ld warns of it ("linking with dylib ... built for newer version")
PKG_CONFIG_PATH="$(brew --prefix libarchive)/lib/pkgconfig" CC=clang \
    meson setup "$work/build" --buildtype=release -Dc_args=-Werror
ninja -C "$work/build" ish tools/fakefsify

printf '#!/bin/sh\nexec python3 -I %s/tests/regress/cc_guest.py prebuilt %s "$@"\n' "$repo" "$programs" > "$work/cc-guest"
chmod +x "$work/cc-guest"

curl -fsSL -o "$work/alpine.tar.gz" "$ALPINE"
echo "$ALPINE_SHA256  $work/alpine.tar.gz" | shasum -a 256 -c -
mkdir "$work/rootfs"
tar -xzf "$work/alpine.tar.gz" -C "$work/rootfs"

PATH="$(brew --prefix coreutils)/libexec/gnubin:$PATH" CC_GUEST="$work/cc-guest" \
    tests/regress/run.sh -i "$work/build/ish" -r "$work/rootfs"
