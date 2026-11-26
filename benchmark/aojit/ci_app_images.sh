#!/bin/sh
# Record and publish the app's AOT images again when they no longer serve what
# the app runs (.github/workflows/aot-images.yml; any arm64 Linux host with the
# tools of record_app_images.sh). They do not when:
#
#   - ish's abi changed (the JIT's code, the layouts it loads from): every
#     image is rejected
#   - npm has another version of Claude Code or Kimi Code: ish-install installs
#     the versions the images were recorded on, so recording the new ones and
#     publishing them (with the versions) is what updates the app's
#   - Alpine has another build of nodejs, openjdk25-jdk, maven, python3,
#     openssh-client-default (ssh), git or github-cli (gh), which ish-install
#     and apk install from the repositories as they are that day
#   - what they are recorded from changed: a workload, the list of images,
#     the recording scripts or gen.py (recording_inputs.py), as when Claude
#     Code's workload came to run it with JavaScriptCore's JIT off
#
# The root is the one the lock's set ships (its musl is the app's), fetched
# from the server. A new root comes in by hand: put it at
# deps/rootfs/arm64-root.tar.gz, then record_app_images.sh and
# publish_app_images.py --upload (benchmark/aojit/README.md).
#
# usage: benchmark/aojit/ci_app_images.sh [--force | --check]
#   --force   record anyway
#   --check   say whether the images serve what the app runs, and stop
# Leaves the repository's lock, xcconfig and ish-install versions changed when
# it published a set; the workflow makes the pull request.
set -e
cd "$(dirname "$0")/../.."
repo=$PWD
lock=benchmark/aojit/app-images.lock.json
jit=${AOT_JIT_BUILD:-$repo/build-arm64-jit}
root=${AOT_ROOT:-$repo/build-aot-root}
force='' check=''
case $1 in
    --force) force=1 ;;
    --check) check=1 ;;
esac

say() { printf '\n== %s\n' "$*"; }
field() { python3 -c "import json, sys; d = json.load(open('$lock')); print(eval(sys.argv[1]))" "$1"; }

say "the root of the lock's set ($(field "d['set']"))"
url=$(field "d['base_url'] + '/' + d['set'] + '/' + d['rootfs']['file']")
mkdir -p deps/rootfs
curl -sSf --retry 3 -o deps/rootfs/arm64-root.tar.gz "$url"
[ "$(sha256sum deps/rootfs/arm64-root.tar.gz | cut -d' ' -f1)" = "$(field "d['rootfs']['sha256']")" ] ||
    { echo "❌ $url is not the lock's root (sha256)"; exit 1; }

say "the recording ish ($jit)"
[ -f "$jit/build.ninja" ] ||
    CC=${CC:-clang} meson setup "$jit" -Djit=true -Djit_emit=true -Dbuildtype=release
ninja -C "$jit" ish tools/fakefsify
rm -rf "$root" "$root.ready"
"$jit/tools/fakefsify" deps/rootfs/arm64-root.tar.gz "$root"
guest() { "$jit/ish" -f "$root" "$@"; }
guest /bin/sh -c 'cat > /etc/resolv.conf' < /etc/resolv.conf

say "what the app runs now, and what the images were recorded on"
abi=$(ISH_JIT_PIC=1 guest /bin/cat /proc/ish/jit | grep -o 'abi [0-9a-f]*' | cut -d' ' -f2)
claude=$(npm view @anthropic-ai/claude-code version)
kimi=$(npm view @moonshot-ai/kimi-code version)
guest /sbin/apk update > /dev/null
apk=$(guest /sbin/apk search -x nodejs openjdk25-jdk maven python3 openssh-client-default git github-cli | python3 -c "
import re, sys
want = {'nodejs', 'openjdk25-jdk', 'maven', 'python3', 'openssh-client-default', 'git', 'github-cli'}
found = {}
for line in sys.stdin:
    m = re.match(r'(.+?)-(\d[^-]*-r\d+)$', line.strip())
    if m and m.group(1) in want:
        found[m.group(1)] = m.group(2)
assert set(found) == want, found
print(' '.join('%s=%s' % i for i in sorted(found.items())))")
inputs=$(python3 benchmark/aojit/recording_inputs.py)
now="abi=$abi @anthropic-ai/claude-code=$claude @moonshot-ai/kimi-code=$kimi $apk inputs=$inputs"
# a package a lock of before it had no image of is "none", and so are the
# inputs of a lock from before they were kept: recorded again
recorded=$(python3 -c "
import json
d = json.load(open('$lock'))
r = d['recorded']
print(' '.join(['abi=' + d['abi'], '@anthropic-ai/claude-code=' + r['npm']['@anthropic-ai/claude-code'],
                '@moonshot-ai/kimi-code=' + r['npm']['@moonshot-ai/kimi-code']] +
               ['%s=%s' % (p, r['apk'].get(p, 'none'))
                for p in sorted(['nodejs', 'openjdk25-jdk', 'maven', 'python3', 'openssh-client-default', 'git',
                                 'github-cli'])] +
               ['inputs=' + r.get('inputs', 'none')]))")
echo "now:      $now"
echo "recorded: $recorded"
if [ "$now" = "$recorded" ] && [ -z "$force" ]; then
    echo "✅ the images serve what the app runs: nothing to record"
    exit 0
fi
[ -z "$check" ] || { echo "⚠️  the images no longer serve what the app runs: to record"; exit 0; }

say "recording on $claude (Claude Code), $kimi (Kimi Code), $apk"
rm -rf "$root"
AOT_JIT_BUILD=$jit AOT_ROOT=$root AOT_NO_LOCAL_XCCONFIG=1 \
    AOT_NPM_PINS="@anthropic-ai/claude-code@$claude @moonshot-ai/kimi-code@$kimi" \
    sh benchmark/aojit/record_app_images.sh
say "publishing"
python3 benchmark/aojit/publish_app_images.py "benchmark/aojit/images/$abi/app" "$jit/ish" "$root" --upload
