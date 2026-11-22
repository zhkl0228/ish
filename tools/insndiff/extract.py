#!/usr/bin/env python3
# Extract distinct aarch64 instruction words from an ELF's .text for insndiff.
#
# usage: extract.py <elf> <outdir>
# Writes <outdir>/simd_dp.bin, int_dp.bin, ldst_simd.bin and ldst_int.bin: little-endian
# uint32 words, sorted. Words that name x27-x30 are left out (insndiff keeps
# its state pointer, branch target, frame pointer and LR there), and so are
# words whose result depends on the PC or that would move the harness stack.
import os, struct, subprocess, sys, tempfile

RESERVED = {27, 28, 29, 30}

def classify(w):
    fields = {w & 31, (w >> 5) & 31, (w >> 10) & 31, (w >> 16) & 31}
    if fields & RESERVED:
        return None
    rd, rn = w & 31, (w >> 5) & 31
    if (w >> 25) & 7 == 7:                  # data processing - SIMD and FP
        return 'simd_dp'
    if (w >> 26) & 7 == 4:                  # data processing - immediate
        if (w >> 23) & 7 in (0, 1):         # ADR/ADRP: PC-relative
            return None
        if rd == 31 or rn == 31:            # SP forms would move the harness stack
            return None
        return 'int_dp'
    if (w >> 25) & 7 == 5:                  # data processing - register
        if (w >> 24) & 0x1f == 0x0b and (w >> 21) & 1 and (rd == 31 or rn == 31):
            return None                     # add/sub extended register: 31 is SP
        return 'int_dp'
    if (w >> 25) & 5 == 4 and (w >> 26) & 1:  # loads and stores, SIMD and FP
        if (w >> 27) & 7 == 3 and not (w >> 24) & 1:
            return None                     # load register (literal): PC-relative
        if rn == 31:                        # SP base
            return None
        return 'ldst_simd'
    if (w >> 25) & 5 == 4:                  # loads and stores, general registers
        if (w >> 27) & 7 == 3 and not (w >> 24) & 1:
            return None                     # load register (literal): PC-relative
        if rn == 31:                        # SP base
            return None
        if constrained_unpredictable(w):
            return None
        return 'ldst_int'
    return None

def constrained_unpredictable(w):
    """Load/store forms whose result the architecture leaves open, so native
    and iSH may both be right and still differ."""
    rt, rn, rt2 = w & 31, (w >> 5) & 31, (w >> 10) & 31
    load = (w >> 22) & 1
    if (w >> 27) & 7 == 5 and not (w >> 26) & 1:   # load/store pair, general registers
        writeback = (w >> 23) & 3 in (1, 3)         # post- or pre-indexed
        if writeback and rn in (rt, rt2):
            return True
        if load and rt == rt2:
            return True
        return False
    if (w >> 27) & 7 == 7 and (w >> 24) & 3 == 0 and not (w >> 21) & 1:
        # load/store register, immediate post- or pre-indexed: writeback into Rt
        if (w >> 10) & 3 in (1, 3) and rn == rt:
            return True
    return False

def main():
    elf, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    with tempfile.NamedTemporaryFile(suffix='.text') as t:
        subprocess.run(['objcopy', '-O', 'binary', '--only-section=.text', elf, t.name], check=True)
        raw = open(t.name, 'rb').read()
    words = set(struct.unpack('<%dI' % (len(raw) // 4), raw[:len(raw) // 4 * 4]))
    sets = {'simd_dp': [], 'int_dp': [], 'ldst_simd': [], 'ldst_int': []}
    for w in words:
        c = classify(w)
        if c:
            sets[c].append(w)
    for name, ws in sets.items():
        ws.sort()
        with open(os.path.join(outdir, name + '.bin'), 'wb') as f:
            f.write(struct.pack('<%dI' % len(ws), *ws))
        print(f'{name}: {len(ws)} words')

main()
