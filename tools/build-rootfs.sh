#!/bin/bash
# Build the arm64 guest's root filesystem: Alpine's aarch64 minirootfs, brought
# up to date, with the packages the app preinstalls. The result is the tarball
# the app imports on first launch (root.tar.gz in the app bundle).
#
# usage: tools/build-rootfs.sh <meson build dir> <out.tar.gz>
#
# The ARM64 app targets copy deps/rootfs/arm64-root.tar.gz (ROOTFS_FILE in
# app/GuestARM64.xcconfig), so the usual run is
#   tools/build-rootfs.sh build-arm64 deps/rootfs/arm64-root.tar.gz
#
# The build dir must be an arm64-guest build (meson setup -Dguest_arch=arm64)
# with ish, tools/fakefsify and tools/unfakefsify built:
#   ninja -C <build dir> ish tools/fakefsify tools/unfakefsify
# Packages are installed by running apk inside iSH, so this needs network
# access. It runs wherever that build runs (macOS, aarch64 Linux).
set -euo pipefail

ALPINE_BRANCH=v3.24
ALPINE_RELEASE=3.24.4
# openjdk25-jdk comes before maven so it is the java-jdk maven depends on.
PACKAGES=(
    python3 py3-pip py3-pysocks py3-cryptography py3-yaml
    nodejs npm go openjdk25-jdk maven build-base clang21 git github-cli
    redis mariadb mariadb-client
    curl wget vim jq shellcheck 7zip xz openssh iputils bind-tools iproute2 net-tools procps-ng
    # telnet: busybox-extras has one too, but only as `busybox-extras telnet`
    inetutils-telnet
)
# Installed with npm -g, the way Claude Code's own updater expects to update it.
NPM_PACKAGES=(@anthropic-ai/claude-code)

[ $# -eq 2 ] || { sed -n '2,16p' "$0"; exit 2; }
build=$(cd "$1" && pwd)
out=$2
mkdir -p "$(dirname "$out")"
for tool in ish tools/fakefsify tools/unfakefsify; do
    [ -x "$build/$tool" ] || { echo "missing $build/$tool; build it first (see usage)" >&2; exit 1; }
done

work=$(mktemp -d -t ish-rootfs.XXXXXX)
trap 'rm -rf "$work"' EXIT
name=alpine-minirootfs-$ALPINE_RELEASE-aarch64.tar.gz
url=https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/releases/aarch64/$name

echo "downloading $name"
curl -fsSL -o "$work/$name" "$url"
curl -fsSL -o "$work/$name.sha256" "$url.sha256"
(cd "$work" && shasum -a 256 -c "$name.sha256")

"$build/tools/fakefsify" "$work/$name" "$work/fs"
guest() { "$build/ish" -f "$work/fs" "$@" < /dev/null; }

# apk needs DNS. The app writes its own /etc/resolv.conf at runtime, so the
# build host's is removed again below; the minirootfs ships none.
grep '^nameserver' /etc/resolv.conf | "$build/ish" -f "$work/fs" /bin/sh -c 'cat > /etc/resolv.conf'
guest /sbin/apk upgrade --no-cache
guest /sbin/apk add --no-cache "${PACKAGES[@]}"
guest /bin/sh -l -c "npm install -g --no-audit --no-fund ${NPM_PACKAGES[*]} && npm cache clean --force"
guest /bin/rm /etc/resolv.conf
# A fakefs keeps regular files' contents under data/, so the package database
# can be read from here.
awk '/^P:/ { name = substr($0, 3) } /^V:/ { print name "-" substr($0, 3) }' \
    "$work/fs/data/lib/apk/db/installed" | sort > "$work/packages.txt"

"$build/tools/unfakefsify" "$work/fs" "$out"
echo "wrote $out ($(du -h "$out" | cut -f1)), $(wc -l < "$work/packages.txt") packages:"
grep -E "^($(IFS='|'; echo "${PACKAGES[*]}"))-[0-9]" "$work/packages.txt" | sed 's/^/  /'
"$build/ish" -f "$work/fs" /bin/sh -l -c 'npm ls -g --depth=0' < /dev/null | sed 's/^/  /'
