#!/usr/bin/env python3
# Compare insndiff dumps from native and iSH runs; group mismatches by mnemonic.
# usage: compare.py native.bin ish.bin [examples_per_mnemonic]
import collections, struct, subprocess, sys, tempfile

REC = struct.Struct('<IiII27QQQQ' + '16s' * 32)
X0, NZCV, FPSR, MEM, V0 = 4, 31, 32, 33, 34

def records(path):
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(REC.size * 4096)
            if not chunk:
                return
            yield from REC.iter_unpack(chunk)

def disasm(words):
    with tempfile.NamedTemporaryFile(suffix='.bin') as f:
        f.write(struct.pack('<%dI' % len(words), *words))
        f.flush()
        out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'aarch64', f.name],
                             capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        parts = line.split('\t')
        if len(parts) >= 3 and parts[0].strip().endswith(':'):
            res[int(parts[1], 16)] = '\t'.join(parts[2:]).strip()
    return res

def main():
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    bad = collections.OrderedDict()
    runs = 0
    words = set()
    for n, i in zip(records(sys.argv[1]), records(sys.argv[2])):
        runs += 1
        words.add(n[0])
        assert n[0] == i[0] and n[2] == i[2]
        diffs = []
        if n[1] != i[1]:
            diffs.append(f'status native={n[1]} ish={i[1]}')
        else:
            for r in range(27):
                if n[X0 + r] != i[X0 + r]:
                    diffs.append(f'x{r} {n[X0 + r]:#x} vs {i[X0 + r]:#x}')
            if n[NZCV] != i[NZCV]:
                diffs.append(f'nzcv {n[NZCV]:#x} vs {i[NZCV]:#x}')
            if n[FPSR] != i[FPSR]:
                diffs.append(f'fpsr {n[FPSR]:#x} vs {i[FPSR]:#x}')
            if n[MEM] != i[MEM]:
                diffs.append('memory window differs')
            for r in range(32):
                if n[V0 + r] != i[V0 + r]:
                    diffs.append(f'v{r} {n[V0 + r][::-1].hex()} vs {i[V0 + r][::-1].hex()}')
        if diffs:
            bad.setdefault(n[0], []).append((n[2], diffs))
    print(f'{len(words)} instructions, {runs} runs; {len(bad)} instructions differ '
          f'(registers/flags/FPSR/memory/status)')
    names = disasm(list(bad))
    groups = collections.defaultdict(list)
    for w, runs in bad.items():
        groups[names[w].split()[0] if w in names else '?'].append((w, runs))
    for mn, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f'\n== {mn}: {len(items)}')
        for w, runs in items[:per]:
            print(f'  {w:08x}  {names.get(w, "?")}   (seeds {",".join(str(s) for s, _ in runs)})')
            for x in runs[0][1][:3]:
                print(f'      {x}')

main()
