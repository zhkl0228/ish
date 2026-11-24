#!/bin/sh
# AOT images of what the app's users run most: Java and Maven (libjvm.so),
# Kimi Code (node), Claude Code (claude.exe) and musl under them
# (images-app.json), for the app, on a Mac (Apple silicon) or a Linux arm64
# host: Linux and Darwin builds of ish share what an image depends on (the
# same abi), so a Linux recording makes the app's images too, and the Linux
# CLI's for measure_app_images.sh.
#
# usage: benchmark/aojit/record_app_images.sh [image...]   (default: all four)
#
#   1. build-arm64-jit: ish with the run-time JIT (-Djit=true), which records
#   2. build-aot-root: the root the app ships (deps/rootfs/arm64-root.tar.gz),
#      a fakefs as the app's (/dev/null and /dev/urandom are device files only
#      there), with the packages and npm tools of images-app.json, at the
#      versions apk and npm give today, as ish-install would install them on
#      the phone; kept for the next run (remove it to start over, after the
#      app's root or the tools changed)
#   3. benchmark/aojit/images/<abi>/app: the images (.S) and their iOS
#      objects (aot_<name>_ios.o); the workloads' logs in rec/ (the abi is the
#      recording ish's: an app built from the same source accepts them)
#   4. app/AOTImages.local.xcconfig: the objects, which Xcode links in. Then
#      build and install the app from Xcode as always (on Linux, after the
#      repository is the Mac's again: the paths are $(SRCROOT)'s).
#
# AOT_JIT_BUILD and AOT_ROOT put the recording ish and the root elsewhere (a
# root on a share of the Mac's disk, as Docker Desktop's virtiofs, cannot
# chmod a socket). AOT_NPM_PINS, as "<package>@<version> ...", installs those
# versions of the npm packages of images-app.json in a new root (default: the
# latest). AOT_NO_LOCAL_XCCONFIG=1 leaves app/AOTImages.local.xcconfig alone:
# images published with publish_app_images.sh come in by app/AOTImages.xcconfig.
#
# Needs meson, ninja, clang, libarchive (fakefsify; on a Mac brew install
# libarchive), python3 with tqdm, the network; on a Mac Xcode's command line
# tools, on Linux clang 20 for the iOS objects (IOS_CC, default clang-20). JOBS: images recorded at once (default 1: the
# workloads share their ports and /tmp).
set -e
cd "$(dirname "$0")/../.."
repo=$PWD
jit=${AOT_JIT_BUILD:-$repo/build-arm64-jit}
root=${AOT_ROOT:-$repo/build-aot-root}
list=$repo/benchmark/aojit/images-app.json
tarball=$repo/deps/rootfs/arm64-root.tar.gz
if [ "$(uname -s)" != Darwin ]; then
    command -v "${IOS_CC:-clang-20}" > /dev/null ||
        { echo "❌ no ${IOS_CC:-clang-20} for the iOS objects (apt install clang-20, or IOS_CC=<an LLVM clang, 19 or later>)"; exit 1; }
fi

say() { printf '\n== %s\n' "$*"; }

say "1. the recording ish ($jit)"
[ -f "$jit/build.ninja" ] ||
    CC=${CC:-clang} meson setup "$jit" -Djit=true -Djit_emit=true -Dbuildtype=release
ninja -C "$jit" ish
ninja -C "$jit" tools/fakefsify ||
    { echo "❌ no fakefsify: meson found no libarchive (brew install libarchive, then rm -rf $jit)"; exit 1; }
guest() { "$jit/ish" -f "$root" "$@"; }

if [ ! -f "$root.ready" ]; then
    say "2. the root ($root) from $tarball"
    rm -rf "$root"
    "$jit/tools/fakefsify" "$tarball" "$root"
    # the app writes the phone's; the host's serves here
    guest /bin/sh -c 'cat > /etc/resolv.conf' < /etc/resolv.conf
    apk=$(python3 -c "import json; print(' '.join(json.load(open('$list'))['packages']['apk']))")
    guest /sbin/apk add --no-cache $apk
    # an npm package of images-app.json at its version in AOT_NPM_PINS, if there
    pin() {
        python3 -c "
import sys
pins = dict(p.rsplit('@', 1) for p in sys.argv[1].split())
print(' '.join(p + '@' + pins[p] if p in pins else p for p in sys.argv[2:]))" "$AOT_NPM_PINS" "$@"
    }
    # shellcheck disable=SC2046 # the packages, a word each
    npm=$(pin $(python3 -c "import json; print(' '.join(json.load(open('$list'))['packages']['npm']))"))
    guest /bin/sh -c "npm install -g --no-audit --no-fund $npm" < /dev/null
    python3 -c "
import json
for prefix, packages in json.load(open('$list'))['packages']['npm_prefix'].items():
    print(prefix, ' '.join(packages))" | while read -r prefix packages; do
        guest /bin/sh -c "npm install -g --prefix $prefix --no-audit --no-fund $(pin $packages)" < /dev/null
    done
    touch "$root.ready"
else
    say "2. the root ($root): kept"
fi
# the workloads, as the repository has them now
tar -C benchmark/aojit/guest -cf - . | guest /bin/sh -c 'rm -rf /tmp/aojit && mkdir -p /tmp/aojit && tar -xf - -C /tmp/aojit'

abi=$(ISH_JIT_PIC=1 guest /bin/cat /proc/ish/jit | grep -o 'abi [0-9a-f]*' | cut -d' ' -f2)
out=$repo/benchmark/aojit/images/$abi/app
say "3. the images (abi $abi) -> $out"
IMAGES=$list IOS=1 JOBS=${JOBS:-1} sh benchmark/aojit/record_images.sh "$jit/ish" "$root" "$out" "$@"

[ "$AOT_NO_LOCAL_XCCONFIG" != 1 ] || { du -ch "$out"/aot_*_ios.o | tail -1; exit 0; }
say "4. app/AOTImages.local.xcconfig"
{
    printf 'ISH_AOT_OBJECTS ='
    for f in "$out"/aot_*_ios.o; do printf ' $(SRCROOT)/%s' "${f#$repo/}"; done
    echo
} > app/AOTImages.local.xcconfig
cat app/AOTImages.local.xcconfig
du -ch "$out"/aot_*_ios.o | tail -1
