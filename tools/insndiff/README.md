# insndiff: arm64 guest instruction differential test

Runs aarch64 instruction words one at a time with a deterministic register
(and memory) state, natively and under iSH, and reports every word whose
result differs. It needs an aarch64 Linux host: the native run is the
reference.

```sh
tools/insndiff/run.sh build-arm64/ish <fakefs-rootfs> <aarch64-elf> [workdir]
```

`run.sh` builds `insndiff` statically, pulls the distinct instruction words
out of the ELF's `.text` (`extract.py`), runs each set natively and in the
guest, and compares the dumps (`compare.py`). Any ELF works as a corpus; a
large one such as a Bun or Node binary covers what real programs execute.

## Instruction sets

| set | what | mode | register states per word |
| --- | --- | --- | --- |
| `simd_dp` | SIMD and FP data processing | registers | 4 |
| `int_dp` | integer data processing (immediate and register) | registers | 4 |
| `ldst_simd` | SIMD and FP loads and stores | memory | 2 |
| `ldst_int` | general-register loads and stores, exclusives and atomics | memory | 2 |

In memory mode every X register points into a 4 KB window of a buffer mapped
at the same fixed address in both runs; the window is restored before and
hashed after each run.

`ldst_int` also leaves out the forms whose result the architecture leaves
open (writeback into the base register, a load pair into one register).

`extract.py` leaves out words that name x27-x30 (the harness keeps its state
pointer, branch target, frame pointer and return address there), PC-relative
words (ADR/ADRP, literal loads) and words that would move or address SP.

## Reading the results

Each `cmp_<set>.txt` groups the differing words by mnemonic with the first
registers that differ, FPSR's exception flags among them. Two kinds of
differences are not emulation bugs:

- In `ldst_simd`, register-offset addressing (`ldr b0, [x11, x9]`) adds two
  pointers and lands outside the buffer; whether that address is mapped
  depends on the process's address space, so the native run can get SIGSEGV
  where iSH reads something, or the other way round.
- `status native=4 ish=0`: the host CPU lacks an instruction iSH implements
  (SM3 and SM4 on Apple cores).

A `status native=0 ish=4` line means iSH raised SIGILL: the instruction is not
implemented.

`compare.py` streams the dumps, so the integer set (about a million runs for a
Bun binary) fits in memory.
