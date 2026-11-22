#!/bin/bash
# Run the instruction differential test for one ELF: natively and under iSH.
#
# usage: tools/insndiff/run.sh <ish> <fakefs-rootfs> <elf> [workdir]
#   <ish>            an iSH CLI built with -Dguest_arch=arm64
#   <fakefs-rootfs>  a fakefs directory (ish -f) to run the guest side in
#   <elf>            aarch64 binary whose .text supplies the instruction words
# Needs an aarch64 Linux host with gcc, objcopy and objdump. Results:
# <workdir>/cmp_<set>.txt, one per instruction set.
set -euo pipefail
[ $# -ge 3 ] || { sed -n '2,10p' "$0"; exit 2; }
ish=$1 rootfs=$2 elf=$3
here=$(cd "$(dirname "$0")" && pwd)
work=${4:-$(mktemp -d -t insndiff.XXXXXX)}
mkdir -p "$work"

gcc -static -O1 -Wall -o "$work/insndiff" "$here/insndiff.c" "$here/stub.S"
python3 "$here/extract.py" "$elf" "$work"

guest() { "$ish" -f "$rootfs" "$@" < /dev/null; }
put() { "$ish" -f "$rootfs" /bin/sh -c "cat > $2 && chmod +x $2" < "$1"; }
put "$work/insndiff" /tmp/insndiff

# set:mode:seeds. Mode 1 points every X register into a memory window.
for spec in simd_dp:0:4 int_dp:0:4 ldst_simd:1:2; do
    IFS=: read -r set mode seeds <<< "$spec"
    "$work/insndiff" "$work/$set.bin" "$work/native_$set.bin" "$mode" "$seeds"
    put "$work/$set.bin" "/tmp/insndiff_$set.bin"
    guest /tmp/insndiff "/tmp/insndiff_$set.bin" "/tmp/insndiff_out_$set.bin" "$mode" "$seeds" > /dev/null
    # A fakefs keeps file contents as plain files under data/; copy from there
    # rather than through the guest's stdout.
    cp "$rootfs/data/tmp/insndiff_out_$set.bin" "$work/ish_$set.bin"
    python3 "$here/compare.py" "$work/native_$set.bin" "$work/ish_$set.bin" 3 > "$work/cmp_$set.txt"
    echo "== $set: $(head -1 "$work/cmp_$set.txt")"
    grep '^== ' "$work/cmp_$set.txt" | sed 's/^/   /' || true
done
echo "details: $work/cmp_*.txt"
