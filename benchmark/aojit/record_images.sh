#!/bin/sh
# Record the AOT images of images.json with a JIT build of ish, on a rootfs that
# has the suite installed (install.sh), and optionally assemble them for iOS.
#
# usage: record_images.sh <ish built with -Djit=true> <rootfs> <out dir> [image...]
#   out dir   gets aot_<name>.S (and rec/<name>.jsonl); with IOS=1 also aot_<name>_ios.o,
#             for the app (on Linux from aot_<name>_ios.S, the image in Mach-O, made by
#             IOS_CC, default clang-20: clang 18's assembler is quadratic in an image's
#             label differences, which a large image has millions of)
#   image     names from images.json, or the list IMAGES names (images-app.json: the
#             app's tools), by default all of them. Images are recorded in parallel
#             (JOBS, default 3). Link them with -Dcli_aot=<.S list> (Mac CLI) or
#             ISH_AOT_OBJECTS=<.o list> (app); they are not part of the repo.
set -e
[ $# -ge 3 ] || { sed -n '2,9p' "$0"; exit 1; }
ish=$1 rootfs=$2 out=$3; shift 3
here=$(cd "$(dirname "$0")" && pwd)
list=${IMAGES:-$here/images.json}
record=$here/../../tools/jit_aot/record.sh
mkdir -p "$out/rec"
names=${*:-$(python3 -c "import json; print(' '.join(json.load(open('$list'))['images']))")}
rec() {
    name=$1
    set -- $(python3 -c "import json; i = json.load(open('$list'))['images']['$name']; print(i['module'], i['workload'])")
    RECORDING="$out/rec/$name.jsonl" sh "$record" "$ish" "$rootfs" "/tmp/aojit/$2" "$out/aot_$name.S" "$1" 2>&1 |
        grep -E "✅|❌|⚠️|^   " | sed "s/^/[$name] /" | cut -c1-230
    [ "$IOS" = 1 ] || return 0
    # not when record.sh made no image (its status is lost in the pipe)
    [ "$out/aot_$name.S" -nt "$out/rec/$name.jsonl" ] || return 0
    if [ "$(uname -s)" = Darwin ]; then
        xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -c -o "$out/aot_${name}_ios.o" "$out/aot_$name.S" &&
            echo "📱 $out/aot_${name}_ios.o"
        return 0
    fi
    python3 "$here/../../tools/jit_aot/gen.py" "$out/rec/$name.jsonl" "$ish" "$rootfs" "$out/aot_${name}_ios.S" \
            --name "$(echo "$name" | tr '.-' '__')" --format macho > "$out/rec/$name.ios.log" 2>&1 ||
        { echo "[$name] ❌ the Mach-O image failed (gen.py exited $?): $out/rec/$name.ios.log"; return 0; }
    ${IOS_CC:-clang-20} -target arm64-apple-ios16.0 -c -o "$out/aot_${name}_ios.o" "$out/aot_${name}_ios.S" &&
        echo "📱 $out/aot_${name}_ios.o"
}
n=0
for name in $names; do
    rec "$name" &
    n=$((n + 1)); [ $((n % ${JOBS:-3})) -ne 0 ] || wait
done
wait
ls -la "$out"/aot_*
