#!/usr/bin/env python3
"""Turn an ISH_JIT_RECORD recording into an AOT image (.S) for one guest module.

The recording holds the position-independent (ISH_JIT_PIC=1) translations of
one module, with every host address the code depends on named. This script
lays them out as assembly that links into ish:

- code in __TEXT,__ish_aot: each host symbol (movz + 3 movk in the recording)
  becomes adrp/add + 2 nops, each branch to one of the JIT's shared stubs (the
  exit stub, the block-end slow paths) a branch to the image's own copy of it,
  and each direct link the JIT had made a static branch
  whose literal holds the assemble-time offset target - branch;
- tables in __DATA,__const, described by `struct aot_module` (asbestos/guest-
  arm64/aot.h): module identity (path, size, sha256), translations sorted by
  file offset with their validation key, gadget per unit, segments, links and
  self-loop register maps;
- a constructor that hands the image to ish_aot_register() at load time.

That is Mach-O assembly. For an ish built on Linux (an ELF binary) the same
image is written in ELF's: .ish_aot, .data.rel.ro.ish_aot and
.init_array, :lo12: for @PAGEOFF, .L for the local labels, no underscore
before C names. --format macho writes Mach-O from a Linux recording too, for
the iOS app: Linux and Darwin builds of ish share the conventions and layouts
an image depends on (the same abi in /proc/ish/jit), so only the names differ.

An image is not part of ish: a target links it in as an extra object (the
CLI, macOS or Linux, with meson -Dcli_aot=aot_<name>.S, the iOS app from its
own target), and ish must be built with -Djit=true.

usage: gen.py <recording> <ish binary> <rootfs> aot_<name>.S [--name <name>]
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

from tqdm import tqdm

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compact import compact_lines   # noqa: E402

# Recorders from before the compact table layout: their images need compact.py and the
# abi of the ish that loads them, not the one in the recording.
PRE_COMPACT_ABIS = {0xdaf8fcc7}

NO_LINKS = False     # --no-links: every block end takes the generic path (successor native or not)
KEY_HDR = 7          # key words before the units: mod, off lo/hi, page offset, units, 2 slots
KEY_UNIT = 9         # words per unit; words 7 and 8 of a unit are its gadget pointer
LINK_UNSET = 1


def load_recording(path):
    header, trans = None, []
    with open(path) as f:
        for line in f:
            obj = json.loads(line)
            if 'header' in obj:
                header = obj['header']
                continue
            trans.append(obj)
    if header is None:
        sys.exit(f"❌ {path}: no header line (record with the current ish)")
    return header, trans


# Assembly labels declared without NAME() (gadgets-generic.h), whose Mach-O names have no
# underscore before them either: every other global is a C name or a NAME() one, `_x` there.
GADGET_ASM = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'asbestos', 'guest-arm64',
                          'gadgets-aarch64')


def plain_asm_labels():
    labels = set()
    for f in sorted(os.listdir(GADGET_ASM)):
        if f.endswith('.S'):
            for line in open(os.path.join(GADGET_ASM, f)):
                words = line.split()
                if len(words) == 2 and words[0] in ('.global', '.globl') and '(' not in words[1]:
                    labels.add(words[1])
    return labels


def macho_symbols(elf_table):
    """An ELF ish's names (host_symbols) as a Mach-O build of the same source has them."""
    plain = plain_asm_labels()
    return {k: v if v in plain else '_' + v for k, v in elf_table.items()}


def host_symbols(binary):
    """Recorded names are dladdr names (no leading underscore); map them to
    the names the linker sees: `_gadget_x` for C/.gadget symbols, `fiber_ret`
    for plain assembly labels."""
    out = subprocess.run(['nm', '-g', binary], capture_output=True, text=True, check=True).stdout
    names = {line.split()[-1] for line in out.splitlines() if line.strip()}
    table = {}
    for n in names:
        table.setdefault(n[1:] if n.startswith('_') else n, n)
    for n in names:   # an exact (underscore-less) definition wins
        if not n.startswith('_'):
            table[n] = n
    return table


def link_name(syms, name):
    if name == '@region':
        return 'Laot_text_start'
    if '+' in name or name.startswith('?'):
        return None
    return syms.get(name)


def seg_label(t, s):
    return f'Lt{t}s{s}'


def emit_segment(out, syms, t, s, seg, stats, label=None):
    words = seg['words']
    special = {}
    for rel in seg['rel']:
        at, kind = rel[0], rel[1]
        if kind == 'exit':
            special[at] = ('stub', 'Laot_exit_stub')
            continue
        if kind == 'stub':
            special[at] = ('stub', f'Laot_stub_{rel[2]}')
            continue
        target = link_name(syms, rel[2])
        if target is None:
            return False          # a host address we cannot name: leave this translation out
        special[at] = ('sym', target, words[at] & 31)
    for slot, at, tt, ts, tw in seg['links']:
        special[at] = ('link', slot, tt, ts, tw)

    out.append('    .p2align 4')
    out.append(f'{label or seg_label(t, s)}:')
    i = 0
    while i < len(words):
        sp = special.get(i)
        if sp is None:
            # a run of plain words, 8 to a line: one string per word would not fit in memory for
            # the largest modules (claude.exe: 26 million words)
            j = i + 1
            while j < len(words) and j - i < 8 and j not in special:
                j += 1
            out.append('    .inst ' + ', '.join(f'0x{w:08x}' for w in words[i:j]))
            i = j
            continue
        if sp[0] == 'stub':
            out.append(f'    b {sp[1]}')
            stats['exit'] += 1
            i += 1
            continue
        if sp[0] == 'sym':
            _, target, rd = sp
            out.append(f'    adrp x{rd}, {target}@PAGE')
            out.append(f'    add x{rd}, x{rd}, {target}@PAGEOFF')
            out.append('    nop')
            out.append('    nop')
            stats['sym'] += 1
            i += 4
            continue
        _, slot, tt, ts, tw = sp
        site = f'{seg_label(t, s)}_l{slot}'
        if tt >= 0 and (tt, ts) in stats['labels'] and not NO_LINKS:
            dest = f'{seg_label(tt, ts)} + {4 * tw}'
            out.append(f'{site}:')
            out.append(f'    b {dest}')
            out.append(f'    .quad ({dest}) - {site}')
            stats['linked'] += 1
        else:
            out.append(f'{site}:')
            out.append(f'    .inst 0x{words[i]:08x}')                  # unlinked: skips the literal
            out.append(f'    .quad {LINK_UNSET}')
            stats['unlinked'] += 1
        i += 3
    return True


def segment_offsets(t):
    """File offset of the first guest instruction of each segment."""
    key, offs = t['key'], []
    for seg in t['segs']:
        for u in range(key[4]):
            k = KEY_HDR + KEY_UNIT * u
            if key[k] == seg['pos']:
                dpc = key[k + 3] | key[k + 4] << 32
                offs.append(t['off'] + dpc)
                break
    return offs


def build_id_of(path):
    """NT_GNU_BUILD_ID of a 64-bit little-endian ELF file, b'' if none."""
    with open(path, 'rb') as f:
        data = f.read(1 << 16)
        if data[:6] != b'\x7fELF\x02\x01':
            return b''
        phoff, = struct.unpack_from('<Q', data, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            ptype, _, noff, _, _, nsize = struct.unpack('<IIQQQQ', f.read(40))
            if ptype != 4:   # PT_NOTE
                continue
            f.seek(noff)
            notes, p = f.read(nsize), 0
            while p + 12 <= len(notes):
                namesz, descsz, ntype = struct.unpack_from('<III', notes, p)
                name, desc = p + 12, p + 12 + ((namesz + 3) & ~3)
                if ntype == 3 and notes[name:name + namesz] == b'GNU\0' and 0 < descsz <= 20:
                    return notes[desc:desc + descsz]
                p = desc + ((descsz + 3) & ~3)
    return b''


def insn_norm(w):
    """insn_norm() of jit.c: a guest word without the pc-relative immediate the translation does not
    depend on (branch targets come from the block's code stream, adr / adrp targets from dbase)."""
    if (w & 0x7C000000) == 0x14000000: return w & 0xFC000000            # b, bl
    if (w & 0xFF000010) == 0x54000000: return w & 0xFF00001F            # b.cond
    if (w & 0x7E000000) == 0x34000000: return w & 0xFF00001F            # cbz / cbnz
    if (w & 0x7E000000) == 0x36000000: return w & 0xFFF8001F            # tbz / tbnz
    if (w & 0x1F000000) == 0x10000000: return w & 0x9F00001F            # adr / adrp
    return w


def content_hash(key):
    """key_content_hash() of jit.c: FNV-1a over the key words after the module, file offset and page
    offset (instruction words normalized), skipping the gadget words. Finds a block whose code moved
    or whose pc-relative immediates changed in another module version."""
    h = 0xcbf29ce484222325
    for i in range(4, len(key)):
        if i >= KEY_HDR and (i - KEY_HDR) % KEY_UNIT >= 7:
            continue
        w = insn_norm(key[i]) if i >= KEY_HDR and (i - KEY_HDR) % KEY_UNIT in (5, 6) else key[i]
        h = ((h ^ w) * 0x100000001b3) & 0xffffffffffffffff
    return h


def default_family(mod):
    """The file names an image also serves: other versions of a library keep its soname prefix
    (libz.so.1.3.2 -> libz.so.1*), a program its name."""
    name = os.path.basename(mod)
    m = re.match(r'^(.*\.so\.\d+)(\.[\d.]+)?$', name)
    return m.group(1) + '*' if m else name


# The image's Mach-O sections and their ELF counterparts. The code must come after the gadgets,
# as __ish_aot comes after __text on Mach-O: native code takes a code-stream pointer within 128MB
# after its image's start for native code too (emit_native_dispatch_ex), and ld merges a .text.*
# section into .text, before the gadgets of the libraries linked after it; .ish_aot is placed
# after all of .text.
ELF_SECTIONS = {
    '.section __TEXT,__ish_aot,regular,pure_instructions': '.section .ish_aot,"ax",%progbits',
    '.section __DATA,__const': '.section .data.rel.ro.ish_aot,"aw",%progbits',
    '.section __DATA,__mod_init_func,mod_init_funcs': '.section .init_array,"aw",%init_array',
}
# The labels gen.py makes (Lt<t>s<s>, Lk<t>, Laot_..., Ltrans, ...): local on Mach-O by their L, .L on ELF
LOCAL_LABEL = re.compile(r'(?<![\w.])L(?=aot_|trans\b|byhash\b|family\b|path\b|[tkgsl]\d)')


def is_elf(binary):
    with open(binary, 'rb') as f:
        return f.read(4) == b'\x7fELF'


def elf_lines(lines):
    """The image's assembly (Mach-O's) in ELF's, for an ish built on Linux."""
    out = []
    for line in lines:
        t = line.strip()
        if t.startswith('l_ish_aot_'):   # ld64's anchors (ANCHOR_EVERY): ELF has no such limit
            continue
        if t in ELF_SECTIONS:
            out.append('    ' + ELF_SECTIONS[t])
            continue
        if t.startswith('.asciz'):        # a path or a pattern: no labels in it
            out.append(line)
            continue
        line = LOCAL_LABEL.sub('.L', line)
        line = re.sub(r'(?<![\w.])_ish_aot_', 'ish_aot_', line)
        if line.endswith('@PAGEOFF'):
            head, target = line.rsplit(', ', 1)
            line = f'{head}, :lo12:{target[:-len("@PAGEOFF")]}'
        elif line.endswith('@PAGE'):
            line = line[:-len('@PAGE')]
        out.append(line)
    return out


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.digest()


# A linker-private symbol (l...) every ANCHOR_EVERY translations, in the code and in the tables: a
# table's 32-bit pointer into the code (target - field) is relocated against the nearest ones before the
# target and the field, with small addends, not against the two sections with offsets into them. ld
# (ld-27037) fails on an object with too many large addends ("too many large addends", about 136000
# translations of node without the anchors).
ANCHOR_EVERY = 32


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('recording')
    ap.add_argument('ish')
    ap.add_argument('rootfs')
    ap.add_argument('out')
    ap.add_argument('--name', default='musl')
    ap.add_argument('--keep', help='only translations with a segment at one of these file offsets '
                    '(one per line, hex or decimal): the hot code from a profile; the rest runs as gadgets')
    ap.add_argument('--family', help='fnmatch() pattern of the file names this image also serves (other '
                    'versions of the module, matched block by block); default from the file name, '
                    '"" for none')
    ap.add_argument('--abi', help='abi of the ish that recorded, for a recording made before the header had it '
                    '(the "abi" of /proc/ish/jit in that same build)')
    ap.add_argument('--format', choices=['elf', 'macho'], help="the image's object format: the recording "
                    "ish's by default; macho from a Linux recording makes an image for the iOS app")
    ap.add_argument('--no-links', action='store_true', help='no static links between translations: a block '
                    'end enters its successor through the generic path, so it chains to any successor')
    args = ap.parse_args()
    global NO_LINKS
    NO_LINKS = args.no_links

    header, trans = load_recording(args.recording)
    abi = header.get('abi') or (int(args.abi, 16) if args.abi else None)
    if not abi:
        sys.exit(f"❌ {args.recording}: no abi in the header; re-record, or pass --abi of the ish that made it")
    if abi in PRE_COMPACT_ABIS:
        sys.exit(f"❌ {args.recording}: recorded by an ish with the 64-bit table layout (abi {abi:08x}); record "
                 "with a current ish (an image in the compact layout needs its abi)")
    if header.get('region_full'):
        sys.exit(f"❌ {args.recording}: the JIT code region filled up while recording, so the translations of "
                 "what ran after that are missing; record a smaller workload")
    mods = {t['mod'] for t in trans}
    if len(mods) != 1:
        sys.exit(f"❌ expected one module in the recording, got {sorted(mods)}")
    mod = mods.pop()
    # a fakefs root (meta.db, as the app's) keeps its files in data/
    files = os.path.join(args.rootfs, 'data') if os.path.exists(os.path.join(args.rootfs, 'meta.db')) else args.rootfs
    mod_file = os.path.join(files, mod.lstrip('/'))
    if not os.path.isfile(mod_file):
        sys.exit(f"❌ {mod_file}: module not found in the rootfs")
    syms = host_symbols(args.ish)
    elf = is_elf(args.ish)
    out_format = args.format or ('elf' if elf else 'macho')
    if elf and out_format == 'macho':
        syms = macho_symbols(syms)
    elif not elf and out_format == 'elf':
        sys.exit("❌ --format elf needs a recording made by an ish built on Linux")
    print(f"📼 {len(trans)} translations of {mod}, prologue {header['prologue_words']} words")

    kept = range(len(trans))
    if args.keep:
        hot = {int(x, 0) for x in open(args.keep).read().split()}
        kept = [i for i, t in enumerate(trans) if hot & set(segment_offsets(t))]
        print(f"✂️  keeping {len(kept)} of {len(trans)} translations ({len(hot)} hot offsets)")
    # Stable order: by file offset (the loader binary-searches it), then key.
    order = sorted(kept, key=lambda i: (trans[i]['off'], trans[i]['key']))
    new_id = {old: new for new, old in enumerate(order)}
    trans = [trans[i] for i in order]
    for t in trans:
        for seg in t['segs']:
            # a link into a translation left out stays unlinked
            seg['links'] = [[slot, at, new_id.get(tt, -1), ts, tw] for slot, at, tt, ts, tw in seg['links']]

    stats = {'sym': 0, 'exit': 0, 'linked': 0, 'unlinked': 0, 'dropped': 0, 'labels': set()}
    for ti, t in enumerate(trans):
        for si in range(len(t['segs'])):
            stats['labels'].add((ti, si))

    code, keep = [], []
    for ti, t in enumerate(tqdm(trans, desc='🛠  code', unit='tr')):
        body = []
        ok = all(emit_segment(body, syms, ti, si, seg, stats) for si, seg in enumerate(t['segs']))
        for seg in t['segs']:   # the tables need only the count: the words take a gigabyte for claude.exe
            seg['nwords'] = len(seg.pop('words'))
        if not ok:
            stats['dropped'] += 1
            continue
        if ti % ANCHOR_EVERY == 0:
            code.append(f'l_ish_aot_{args.name}_{ti}:')
        code += body
        keep.append(ti)
    # A dropped translation's code is gone: branches into it would dangle.
    dropped = set(range(len(trans))) - set(keep)
    if dropped:
        sys.exit(f"❌ {len(dropped)} translations reference unnamed host addresses (first: {trans[min(dropped)]['off']:#x})")

    exit_stub = header['exit_stub']
    data = []
    for ti, t in enumerate(tqdm(trans, desc='📋 tables', unit='tr')):
        key = list(t['key'])
        key[0] = 0                                     # module word: filled in by the loader
        nunits = key[4]
        gadgets = []
        for u in range(nunits):
            g = KEY_HDR + KEY_UNIT * u + 7
            name = t['keysym'].get(str(g))
            key[g] = key[g + 1] = 0
            gadgets.append(link_name(syms, name) if name else None)
        if ti % ANCHOR_EVERY == 0:
            data.append(f'l_ish_aot_{args.name}_k{ti}:')
        data.append('    .p2align 3')
        data.append(f'Lk{ti}:')
        for i in range(0, len(key), 8):
            data.append('    .long ' + ', '.join(str(x) for x in key[i:i + 8]))
        data.append('    .p2align 3')
        data.append(f'Lg{ti}:')
        for g in gadgets:
            data.append(f'    .quad {g}' if g else '    .quad 0')
        data.append(f'Ls{ti}:')
        for si, seg in enumerate(t['segs']):
            data.append(f'    .quad {seg_label(ti, si)}')
            data.append(f'    .long {seg["pos"]}, {seg["nwords"]}')
        loop = next((seg['loop'] for seg in t['segs'] if seg['loop']), None)
        if loop:
            si = next(i for i, seg in enumerate(t['segs']) if seg['loop'])
            head, end, g, d, h = loop
            pad = lambda xs: xs + [0] * (8 - len(xs))
            data.append('    .p2align 3')
            data.append(f'Ll{ti}:')
            data.append(f'    .quad {seg_label(ti, si)} + {4 * head}, {seg_label(ti, si)} + {4 * end}')
            data.append(f'    .byte {len(g)}, 0, 0, 0, 0, 0, 0, 0')
            data.append('    .byte ' + ', '.join(str(x) for x in pad(g) + pad(d) + pad(h)))
        t['_loop'] = bool(loop)

    lines = [f'// AOT image of {mod}: {len(trans)} translations, generated by tools/jit_aot/gen.py.',
             '// Do not edit.', '',
             '    .section __TEXT,__ish_aot,regular,pure_instructions',
             '    .p2align 14',
             'Laot_text_start:',
             'Laot_exit_stub:']
    lines += [f'    .inst 0x{w:08x}' for w in exit_stub]
    for i, stub in enumerate(header.get('stubs', []), 1):
        if not emit_segment(lines, syms, 0, 0, dict(stub, links=[]), stats, f'Laot_stub_{i}'):
            sys.exit(f"❌ stub {i} of the recording references an unnamed host address")
    lines += code
    lines += ['    .p2align 2', 'Laot_text_end:', '', '    .section __DATA,__const']
    lines += data
    lines.append('    .p2align 3')
    lines.append('Ltrans:')
    for ti, t in enumerate(trans):
        links = []
        for slot in range(2):
            site = next((f'{seg_label(ti, si)}_l{slot}' for si, seg in enumerate(t['segs'])
                         for l in seg['links'] if l[0] == slot), None)
            links.append(site or '0')
        if ti % ANCHOR_EVERY == 0:
            lines.append(f'l_ish_aot_{args.name}_t{ti}:')
        lines.append(f'    .quad {t["off"]}')
        lines.append(f'    .long {t["idx"]}, {len(t["key"])}')
        lines.append(f'    .quad Lk{ti}, Lg{ti}, Ls{ti}')
        lines.append(f'    .long {len(t["segs"])}, {t["key"][4]}')
        lines.append(f'    .quad {links[0]}, {links[1]}, {f"Ll{ti}" if t["_loop"] else "0"}')
    digest = sha256_of(mod_file)
    build_id = build_id_of(mod_file)
    family = default_family(mod) if args.family is None else args.family
    lines.append('    .p2align 3')
    lines.append('Lbyhash:')
    for h, ti in sorted((content_hash(t['key']), ti) for ti, t in enumerate(trans)):
        lines.append(f'    .quad {h}')
        lines.append(f'    .long {ti}, 0')
    if family:
        lines += ['Lfamily:', f'    .asciz "{family}"']
    lines += ['Lpath:', f'    .asciz "{mod}"', '    .p2align 3',
              f'    .globl _ish_aot_module_{args.name}',
              f'_ish_aot_module_{args.name}:',
              '    .quad Lpath',
              f'    .quad {os.path.getsize(mod_file)}',
              '    .byte ' + ', '.join(str(b) for b in digest),
              f'    .long {len(trans)}, {max(t["idx"] for t in trans) + 1}',
              '    .quad Ltrans',
              f'    .long {header["prologue_words"]}, {header["entry_off"]}, {header["n_pinned"]}, {abi}',
              '    .quad Laot_text_start, Laot_text_end',
              f'    .long {len(build_id)}',
              '    .byte ' + ', '.join(str(b) for b in build_id.ljust(20, b'\0')),
              '    .p2align 3',
              f'    .quad {"Lfamily" if family else "0"}',
              '    .quad Lbyhash',
              f'    .long {len(trans)}, 0', '',
              # Register the image when the binary that links it is loaded.
              '    .text',
              '    .p2align 2',
              'Laot_register:',
              f'    adrp x0, _ish_aot_module_{args.name}@PAGE',
              f'    add x0, x0, _ish_aot_module_{args.name}@PAGEOFF',
              '    b _ish_aot_register',
              '    .section __DATA,__mod_init_func,mod_init_funcs',
              '    .p2align 3',
              '    .quad Laot_register', '']
    # The tables go out in the compact layout of aot.h (compact.py; the code is unchanged).
    out, _ = compact_lines(lines, abi)
    if out_format == 'elf':
        out = elf_lines(out)
    with open(args.out, 'w') as f:
        f.writelines(line + '\n' for line in out)
    print(f"✅ {args.out}: {stats['sym']} symbols, {stats['exit']} exits, "
          f"{stats['linked']} static links ({stats['unlinked']} unlinked), family {family or '-'}, "
          f"sha256 {digest.hex()[:16]}…")


if __name__ == '__main__':
    main()
