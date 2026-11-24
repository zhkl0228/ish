#!/bin/sh
# Are the app's images (record_app_images.sh) worth it: their workloads timed on
# an ish CLI (Mac or Linux arm64) that, as the app, translates nothing at run
# time and runs the images it links in (-Djit_emit=false), with them and
# without them (ISH_JIT=0), and
# Kimi Code's node in either mode: the interpreter (which node's image runs as
# native code) and V8's tiers (whose code runs through the gadgets either way).
#
# usage: benchmark/aojit/measure_app_images.sh   (after record_app_images.sh)
set -e
cd "$(dirname "$0")/../.."
repo=$PWD
root=${AOT_ROOT:-$repo/build-aot-root}  # record_app_images.sh's
aot=$repo/build-arm64-aot
abi=$(ISH_JIT_PIC=1 build-arm64-jit/ish -f "$root" /bin/cat /proc/ish/jit | grep -o 'abi [0-9a-f]*' | cut -d' ' -f2)
images=$(ls "$repo"/benchmark/aojit/images/$abi/app/aot_*.S | grep -v '_ios\.S$' | paste -sd, -)
if [ "$(uname -s)" = Linux ]; then
    # GNU as, which takes seconds: clang 18's assembler is quadratic in the
    # differences of labels in one section, of which an image has millions
    # (claude.exe's: unfinished after 8 minutes)
    for f in $(echo "$images" | tr , ' '); do
        [ "${f%.S}.o" -nt "$f" ] || as -o "${f%.S}.o" "$f"
    done
    images=$(echo "$images" | sed 's/\.S\(,\|$\)/.o\1/g')
fi

if [ -f "$aot/build.ninja" ]; then
    meson configure "$aot" -Dcli_aot="$images"
else
    CC=${CC:-clang} meson setup "$aot" -Dguest_arch=arm64 -Djit=true -Djit_emit=false -Dbuildtype=release -Dcli_aot="$images"
fi
ninja -C "$aot" ish
"$aot/ish" -f "$root" /bin/cat /proc/ish/jit | grep -E 'images:|  ok|rejected'

# seconds a workload took, as `label: N s`, its output in $aot/measure/; a run
# whose output shows a step that failed (record.sh's markers) is no timing
mkdir -p "$aot/measure"
run() {
    label=$1 env=$2 command=$3
    log=$aot/measure/$(echo "$label" | tr -s ' ,' '-' | sed 's/-$//').log
    t0=$(date +%s)
    env $env "$aot/ish" -f "$root" /bin/sh -c "$command" < /dev/null > "$log" 2>&1
    t=$(( $(date +%s) - t0 ))
    if grep -q -E 'FAIL|Traceback|unrecognized option|^Usage:|skipped|: not found|No such file or directory|Segmentation fault|Killed|^\[ERROR\]|❌' "$log"; then
        echo "$label: $t s ❌ a step failed: $log"
    else
        echo "$label: $t s"
    fi
}
echo
run "java and maven, images   " "" "sh /tmp/aojit/java/java_train.sh"
run "java and maven, none     " "ISH_JIT=0" "sh /tmp/aojit/java/java_train.sh"
run "claude, images           " "" "sh /tmp/aojit/claude/claude_train.sh"
run "claude, none             " "ISH_JIT=0" "sh /tmp/aojit/claude/claude_train.sh"
run "kimi, images, interpreter" "" "ISH_NODE_MODE=interpreter sh /tmp/aojit/kimi/kimi_train.sh"
run "kimi, images, tiers      " "" "ISH_NODE_MODE=tiers sh /tmp/aojit/kimi/kimi_train.sh"
run "kimi, none, tiers        " "ISH_JIT=0" "ISH_NODE_MODE=tiers sh /tmp/aojit/kimi/kimi_train.sh"
run "kimi, none, interpreter  " "ISH_JIT=0" "ISH_NODE_MODE=interpreter sh /tmp/aojit/kimi/kimi_train.sh"
