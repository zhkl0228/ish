// Regression test for the instructions the JIT translates that cut hot
// blocks before (ish -Djit=true, the JIT position-independent, as on iOS;
// skips otherwise).
//
// A block the JIT translates runs natively up to the first instruction it
// leaves to the gadgets, and through the gadgets from there. In common
// programs' hot blocks (claude.exe, node, python, sqlite, xz, gzip, perl)
// those were load-acquire and store-release (ldar/stlr: V8's, 61% of all),
// exclusives, compare and swap and LSE atomics (locks), fixed-point
// conversions, SIMD & FP loads and stores with
// a register offset, AdvSIMD's multiple-structure and replicating loads and
// stores, dc zva (musl's memset), and AdvSIMD's addp, saddw/uaddw, dup by
// element, smov, scalar shifts, add and saturating arithmetic, aese/aesmc;
// and multiply-adds of four registers none pinned, which failed for want of
// a register. claude.exe's start ran 2.3 times as fast with the JIT as with
// the gadgets alone, python's 5.4 times. In java's, javac's and maven's
// (HotSpot's generated code): ldr (literal), 89% of their cuts, isb and dsb,
// msr fpcr, AdvSIMD's floating point and by-element multiplies, ssubw,
// pmull (zip's CRC-32); gh's mrs cntfrq_el0. And accesses through a tagged
// pointer (bits 56..63, as Linux's TBI lets them be): the JIT's TLB check
// leaves those to the gadgets.
//
// Checked: each instruction (or pair: ldxr and stxr), put in a copy of a
// template made at run time, run once in a fresh copy (the gadgets: a block
// is translated once it is hot) and in a copy run hot (the JIT), for 8 sets
// of operands: q0..q7, x1..x17 (x1..x6 pinned in the JIT, x7..x17 not), NZCV,
// FPSR and the 512 bytes the addresses point into, alike. And some results
// against their known values.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_insns regress_jit_insns.c

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int passed, failed;

static void check(const char *name, int ok, const char *detail) {
    if (ok) {
        passed++;
        printf("PASS %s\n", name);
    } else {
        failed++;
        printf("FAIL %s: %s\n", name, detail);
    }
    fflush(stdout);
}

// What the template reads and writes
struct io {
    uint64_t q[8][2];    // q0..q7
    uint64_t x[17];      // x1..x17
    uint64_t f1, f2;     // cmp f1, f2: the NZCV the instruction finds
    uint64_t pad;
    uint64_t oq[8][2];   // after
    uint64_t ox[17];
    uint64_t nzcv, fpsr;
    uint64_t pad2;
    uint8_t mem[512] __attribute__((aligned(64)));
};
_Static_assert(offsetof(struct io, x) == 128 && offsetof(struct io, f1) == 264 && offsetof(struct io, oq) == 288 &&
                   offsetof(struct io, nzcv) == 552 && offsetof(struct io, mem) == 576,
               "the template's offsets");

// The template: the operands in, the instructions under test at its slots
// (nops here), all they can change out. The JIT translates each of its other
// instructions, so in a hot copy those under test run natively.
__asm__(".text\n"
        ".p2align 2\n"
        ".global insn_template\n"
        "insn_template:\n"
        "    ldp x8, x9, [x0, #264]\n"
        "    cmp x8, x9\n"
        "    ldp q0, q1, [x0]\n"
        "    ldp q2, q3, [x0, #32]\n"
        "    ldp q4, q5, [x0, #64]\n"
        "    ldp q6, q7, [x0, #96]\n"
        "    ldp x1, x2, [x0, #128]\n"
        "    ldp x3, x4, [x0, #144]\n"
        "    ldp x5, x6, [x0, #160]\n"
        "    ldp x7, x8, [x0, #176]\n"
        "    ldp x9, x10, [x0, #192]\n"
        "    ldp x11, x12, [x0, #208]\n"
        "    ldp x13, x14, [x0, #224]\n"
        "    ldp x15, x16, [x0, #240]\n"
        "    ldr x17, [x0, #256]\n"
        ".global insn_template_slot\n"
        "insn_template_slot:\n"
        "    nop\n"
        "    nop\n"
        "    add x0, x0, #288\n"
        "    stp q0, q1, [x0]\n"
        "    stp q2, q3, [x0, #32]\n"
        "    stp q4, q5, [x0, #64]\n"
        "    stp q6, q7, [x0, #96]\n"
        "    stp x1, x2, [x0, #128]\n"
        "    stp x3, x4, [x0, #144]\n"
        "    stp x5, x6, [x0, #160]\n"
        "    stp x7, x8, [x0, #176]\n"
        "    stp x9, x10, [x0, #192]\n"
        "    stp x11, x12, [x0, #208]\n"
        "    stp x13, x14, [x0, #224]\n"
        "    stp x15, x16, [x0, #240]\n"
        "    str x17, [x0, #256]\n"
        "    mrs x15, fpsr\n"
        "    cset x8, mi\n"
        "    cset x9, eq\n"
        "    cset x10, hs\n"
        "    cset x11, vs\n"
        "    lsl x8, x8, #3\n"
        "    orr x8, x8, x9, lsl #2\n"
        "    orr x8, x8, x10, lsl #1\n"
        "    orr x8, x8, x11\n"
        "    stp x8, x15, [x0, #264]\n"
        "    ret\n"
        ".global insn_template_end\n"
        "insn_template_end:\n");
extern const uint32_t insn_template[], insn_template_slot[], insn_template_end[];

// The instructions under test: group, the instructions (1 or 2), and the
// registers holding an address into the memory (0: none): p at its set's
// offset, p2 at 8 bytes past it. q0..q7 are the only V registers compared.
#define INSNS(X)                                                                                  \
    X("ldar, stlr", 1, 2, 0, "ldar x9, [x2]")                                                     \
    X("ldar, stlr", 1, 2, 0, "ldar w10, [x2]")                                                    \
    X("ldar, stlr", 1, 16, 0, "ldarb w11, [x16]")                                                 \
    X("ldar, stlr", 1, 16, 0, "ldarh w1, [x16]")                                                  \
    X("ldar, stlr", 1, 2, 0, "stlr x9, [x2]")                                                     \
    X("ldar, stlr", 1, 2, 0, "stlr w10, [x2]")                                                    \
    X("ldar, stlr", 1, 16, 0, "stlrb w11, [x16]")                                                 \
    X("ldar, stlr", 1, 16, 0, "stlrh w3, [x16]")                                                  \
    X("ldar, stlr", 1, 2, 0, "stlr xzr, [x2]")                                                    \
    X("exclusives", 2, 2, 0, "ldaxr w9, [x2]\n stlxr w10, w11, [x2]")                             \
    X("exclusives", 2, 2, 0, "ldxr x9, [x2]\n stxr w10, x11, [x2]")                               \
    X("exclusives", 2, 16, 0, "ldaxrb w12, [x16]\n stlxrb w13, w14, [x16]")                       \
    X("exclusives", 2, 16, 0, "ldxrh w12, [x16]\n stxrh w13, w14, [x16]")                         \
    X("exclusives", 2, 2, 3, "ldxr x9, [x2]\n stxr w10, x11, [x3]")                               \
    X("exclusives", 2, 2, 0, "ldaxr x9, [x2]\n stlxr w10, xzr, [x2]")                             \
    X("compare and swap", 1, 2, 0, "casal w9, w10, [x2]")                                         \
    X("compare and swap", 2, 2, 0, "ldr w9, [x2]\n casal w9, w10, [x2]")                          \
    X("compare and swap", 2, 16, 0, "ldr x12, [x16]\n cas x12, x13, [x16]")                       \
    X("compare and swap", 2, 2, 0, "ldrb w9, [x2]\n casab w9, w10, [x2]")                         \
    X("compare and swap", 1, 16, 0, "caslh w12, w13, [x16]")                                      \
    X("LSE atomics", 1, 2, 0, "ldadd x9, x10, [x2]")                                              \
    X("LSE atomics", 1, 2, 0, "ldclr w9, w10, [x2]")                                              \
    X("LSE atomics", 1, 16, 0, "ldeoral x12, x13, [x16]")                                         \
    X("LSE atomics", 1, 2, 0, "ldseta w9, w10, [x2]")                                             \
    X("LSE atomics", 1, 16, 0, "swpal x12, x13, [x16]")                                           \
    X("LSE atomics", 1, 2, 0, "ldaddl w9, w9, [x2]")                                              \
    X("LSE atomics", 1, 2, 0, "staddl x10, [x2]")                                                 \
    X("LSE atomics", 1, 16, 0, "ldsetb w12, w13, [x16]")                                          \
    X("LSE atomics", 1, 2, 0, "swph w9, w10, [x2]")                                               \
    X("FP and SIMD register offset", 1, 2, 0, "ldr d0, [x2, x3, lsl #3]")                         \
    X("FP and SIMD register offset", 1, 2, 0, "ldr s5, [x2, w17, uxtw #2]")                       \
    X("FP and SIMD register offset", 1, 2, 0, "ldr q4, [x2, x3, lsl #4]")                         \
    X("FP and SIMD register offset", 1, 2, 0, "ldr b1, [x2, x3]")                                 \
    X("FP and SIMD register offset", 1, 2, 0, "ldr h2, [x2, w3, sxtw #1]")                        \
    X("FP and SIMD register offset", 1, 16, 0, "ldr d7, [x16, x17]")                              \
    X("FP and SIMD register offset", 1, 2, 0, "str q1, [x2, x3]")                                 \
    X("FP and SIMD register offset", 1, 2, 0, "str d3, [x2, x3, lsl #3]")                         \
    X("FP and SIMD register offset", 1, 16, 0, "str s6, [x16, w17, uxtw #2]")                     \
    X("FP and SIMD register offset", 1, 2, 0, "str h7, [x2, x3, sxtx]")                           \
    X("multiple structures", 1, 2, 0, "ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [x2]")                   \
    X("multiple structures", 1, 2, 0, "ld1 {v4.4s}, [x2], #16")                                   \
    X("multiple structures", 1, 2, 0, "ld1 {v5.16b, v6.16b}, [x2], x3")                           \
    X("multiple structures", 1, 16, 0, "ld1 {v1.8b, v2.8b, v3.8b}, [x16]")                        \
    X("multiple structures", 1, 2, 0, "st1 {v3.4s}, [x2], #16")                                   \
    X("multiple structures", 1, 2, 0, "st1 {v4.2d, v5.2d, v6.2d, v7.2d}, [x2]")                   \
    X("multiple structures", 1, 2, 0, "ld2 {v0.16b, v1.16b}, [x2]")                               \
    X("multiple structures", 1, 2, 0, "ld3 {v0.16b, v1.16b, v2.16b}, [x2]")                       \
    X("multiple structures", 1, 2, 0, "ld4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x2]")                   \
    X("multiple structures", 1, 16, 0, "ld4 {v4.8b, v5.8b, v6.8b, v7.8b}, [x16], #32")            \
    X("multiple structures", 1, 2, 0, "st2 {v6.4s, v7.4s}, [x2], #32")                            \
    X("replicate", 1, 2, 0, "ld1r {v0.4h}, [x2]")                                                 \
    X("replicate", 1, 2, 0, "ld1r {v1.2d}, [x2], #8")                                             \
    X("replicate", 1, 2, 0, "ld2r {v2.4s, v3.4s}, [x2]")                                          \
    X("replicate", 1, 16, 0, "ld4r {v4.8b, v5.8b, v6.8b, v7.8b}, [x16], x3")                      \
    X("lanes", 1, 2, 0, "ld1 {v0.b}[3], [x2]")                                                    \
    X("lanes", 1, 2, 0, "ld1 {v1.h}[5], [x2], #2")                                                \
    X("lanes", 1, 16, 0, "ld1 {v2.s}[1], [x16]")                                                  \
    X("lanes", 1, 2, 0, "ld1 {v3.d}[1], [x2], x3")                                                \
    X("dc zva, dczid_el0", 1, 2, 0, "dc zva, x2")                                                 \
    X("dc zva, dczid_el0", 1, 16, 0, "dc zva, x16")                                               \
    X("dc zva, dczid_el0", 1, 0, 0, "mrs x9, dczid_el0")                                          \
    X("SIMD", 1, 0, 0, "addp d0, v1.2d")                                                          \
    X("SIMD", 1, 0, 0, "saddw v0.4s, v1.4s, v2.4h")                                               \
    X("SIMD", 1, 0, 0, "uaddw v3.2d, v4.2d, v5.2s")                                               \
    X("SIMD", 1, 0, 0, "uaddw2 v6.2d, v7.2d, v0.4s")                                              \
    X("SIMD", 1, 0, 0, "saddw2 v1.8h, v2.8h, v3.16b")                                             \
    X("SIMD", 1, 0, 0, "dup v0.2d, v1.d[1]")                                                      \
    X("SIMD", 1, 0, 0, "dup v2.4s, v3.s[3]")                                                      \
    X("SIMD", 1, 0, 0, "dup v4.8b, v5.b[9]")                                                      \
    X("SIMD", 1, 0, 0, "dup v6.8h, v7.h[2]")                                                      \
    X("SIMD", 1, 0, 0, "mov d0, v1.d[1]")                                                         \
    X("SIMD", 1, 0, 0, "mov s2, v3.s[2]")                                                         \
    X("SIMD", 1, 0, 0, "smov x9, v0.b[3]")                                                        \
    X("SIMD", 1, 0, 0, "smov w10, v1.h[5]")                                                       \
    X("SIMD", 1, 0, 0, "smov x11, v2.s[1]")                                                       \
    X("SIMD", 1, 0, 0, "smov x1, v3.h[0]")                                                        \
    X("SIMD", 1, 0, 0, "ushr d3, d4, #56")                                                        \
    X("SIMD", 1, 0, 0, "ushr d1, d2, #64")                                                        \
    X("SIMD", 1, 0, 0, "sshr d5, d6, #3")                                                         \
    X("SIMD", 1, 0, 0, "sshr d4, d0, #64")                                                        \
    X("SIMD", 1, 0, 0, "ushr v0.2d, v1.2d, #64")                                                  \
    X("SIMD", 1, 0, 0, "sshr v2.2d, v3.2d, #64")                                                  \
    X("SIMD", 1, 0, 0, "ushr v4.4s, v5.4s, #32")                                                  \
    X("SIMD", 1, 0, 0, "sshr v6.8h, v7.8h, #16")                                                  \
    X("SIMD", 1, 0, 0, "usra v0.2d, v1.2d, #64")                                                  \
    X("SIMD", 1, 0, 0, "ssra v2.2d, v3.2d, #64")                                                  \
    X("SIMD", 1, 0, 0, "urshr v4.2d, v5.2d, #64")                                                 \
    X("SIMD", 1, 0, 0, "srshr v6.2d, v7.2d, #64")                                                 \
    X("SIMD", 1, 0, 0, "ursra v0.2d, v1.2d, #64")                                                 \
    X("SIMD", 1, 0, 0, "srsra v2.2d, v3.2d, #64")                                                 \
    X("SIMD", 1, 0, 0, "sri v4.2d, v5.2d, #64")                                                   \
    X("SIMD", 1, 0, 0, "usra v6.4s, v7.4s, #32")                                                  \
    X("SIMD", 1, 0, 0, "shrn v0.2s, v1.2d, #32")                                                  \
    X("SIMD", 1, 0, 0, "rshrn v2.2s, v3.2d, #32")                                                 \
    X("SIMD", 1, 0, 0, "usra d0, d1, #64")                                                        \
    X("SIMD", 1, 0, 0, "ssra d2, d3, #64")                                                        \
    X("SIMD", 1, 0, 0, "usra d4, d5, #3")                                                         \
    X("SIMD", 1, 0, 0, "ssra d6, d7, #17")                                                        \
    X("SIMD", 1, 0, 0, "shl d7, d0, #18")                                                         \
    X("SIMD", 1, 0, 0, "add d0, d1, d2")                                                          \
    X("SIMD", 1, 0, 0, "sqadd b0, b1, b2")                                                        \
    X("SIMD", 1, 0, 0, "uqadd h3, h4, h5")                                                        \
    X("SIMD", 1, 0, 0, "sqsub s6, s7, s0")                                                        \
    X("SIMD", 1, 0, 0, "uqsub d1, d2, d3")                                                        \
    X("SIMD", 1, 0, 0, "aese v0.16b, v1.16b")                                                     \
    X("SIMD", 1, 0, 0, "aesmc v2.16b, v3.16b")                                                    \
    X("multiply-add, no register pinned", 1, 0, 0, "umaddl x13, w15, w16, x8")                    \
    X("multiply-add, no register pinned", 1, 0, 0, "msub w14, w11, w10, w13")                     \
    X("multiply-add, no register pinned", 1, 0, 0, "madd x8, x9, x10, x11")                       \
    X("multiply-add, no register pinned", 1, 0, 0, "smaddl x12, w13, w14, x12")                   \
    X("multiply-add, no register pinned", 1, 0, 0, "madd x9, x10, x10, x11")                      \
    X("multiply-add, no register pinned", 2, 0, 0, "madd x13, x10, x11, x13\n msub w14, w11, w10, w13") \
    X("fixed point", 1, 0, 0, "ucvtf d0, x1, #20")                                                \
    X("fixed point", 1, 0, 0, "scvtf s1, w9, #7")                                                 \
    X("fixed point", 1, 0, 0, "fcvtzs x9, d2, #12")                                               \
    X("fixed point", 1, 0, 0, "fcvtzu w10, s3, #3")                                               \
    X("fixed point", 1, 0, 0, "scvtf d4, x10, #64")                                               \
    X("fixed point", 1, 0, 0, "fcvtzs w11, d5, #32")                                              \
    X("ldr literal", 1, 0, 0, "ldr w9, . + 8")                                                    \
    X("ldr literal", 1, 0, 0, "ldr x10, . - 8")                                                   \
    X("ldr literal", 1, 0, 0, "ldrsw x11, . + 12")                                                \
    X("ldr literal", 1, 0, 0, "ldr x1, . + 112")                                                  \
    X("ldr literal", 1, 0, 0, "ldr x17, . - 4")                                                   \
    X("ldr literal", 1, 0, 0, "ldr s1, . + 4")                                                    \
    X("ldr literal", 1, 0, 0, "ldr d0, . - 60")                                                   \
    X("ldr literal", 1, 0, 0, "ldr q2, . + 16")                                                   \
    X("barriers, cntfrq_el0, FPCR", 1, 0, 0, "isb")                                               \
    X("barriers, cntfrq_el0, FPCR", 1, 0, 0, "dsb ish")                                           \
    X("barriers, cntfrq_el0, FPCR", 1, 0, 0, "dsb sy")                                            \
    X("barriers, cntfrq_el0, FPCR", 1, 0, 0, "mrs x9, cntfrq_el0")                                \
    X("barriers, cntfrq_el0, FPCR", 2, 0, 0, "msr fpcr, x9\n mrs x10, fpcr")                      \
    X("barriers, cntfrq_el0, FPCR", 2, 0, 0, "msr fpcr, x11\n fadd d0, d1, d2")                   \
    X("barriers, cntfrq_el0, FPCR", 2, 0, 0, "msr fpcr, xzr\n mrs x12, fpcr")                     \
    X("SIMD floating point", 1, 0, 0, "fadd v0.2d, v1.2d, v2.2d")                                 \
    X("SIMD floating point", 1, 0, 0, "fsub v3.4s, v4.4s, v5.4s")                                 \
    X("SIMD floating point", 1, 0, 0, "fmul v6.2s, v7.2s, v0.2s")                                 \
    X("SIMD floating point", 1, 0, 0, "fmla v1.2d, v2.2d, v3.2d")                                 \
    X("SIMD floating point", 1, 0, 0, "fmls v4.4s, v5.4s, v6.4s")                                 \
    X("SIMD floating point", 1, 0, 0, "fdiv v7.2d, v0.2d, v1.2d")                                 \
    X("SIMD floating point", 1, 0, 0, "fmax v0.4s, v1.4s, v2.4s")                                 \
    X("SIMD floating point", 1, 0, 0, "fmin v3.2d, v4.2d, v5.2d")                                 \
    X("SIMD floating point", 1, 0, 0, "fmaxnm v6.2d, v7.2d, v0.2d")                               \
    X("SIMD floating point", 1, 0, 0, "fminnm v1.4s, v2.4s, v3.4s")                               \
    X("SIMD floating point", 1, 0, 0, "faddp v4.4s, v5.4s, v6.4s")                                \
    X("SIMD floating point", 1, 0, 0, "fabd v7.2d, v0.2d, v1.2d")                                 \
    X("SIMD floating point", 1, 0, 0, "fmaxp v2.2d, v3.2d, v4.2d")                                \
    X("SIMD floating point", 1, 0, 0, "fminp v5.2s, v6.2s, v7.2s")                                \
    X("SIMD floating point", 1, 0, 0, "fcmeq v0.2d, v1.2d, v2.2d")                                \
    X("SIMD floating point", 1, 0, 0, "fcmge v3.4s, v4.4s, v5.4s")                                \
    X("SIMD floating point", 1, 0, 0, "fcmgt v6.2d, v7.2d, v0.2d")                                \
    X("SIMD floating point", 1, 0, 0, "facge v1.4s, v2.4s, v3.4s")                                \
    X("SIMD floating point", 1, 0, 0, "facgt v4.2d, v5.2d, v6.2d")                                \
    X("SIMD floating point", 1, 0, 0, "fmulx v7.4s, v0.4s, v1.4s")                                \
    X("SIMD floating point", 1, 0, 0, "frecps v2.2d, v3.2d, v4.2d")                               \
    X("SIMD floating point", 1, 0, 0, "frsqrts v5.4s, v6.4s, v7.4s")                              \
    X("SIMD by element", 1, 0, 0, "mul v0.4s, v1.4s, v2.s[3]")                                    \
    X("SIMD by element", 1, 0, 0, "mla v3.2s, v4.2s, v5.s[1]")                                    \
    X("SIMD by element", 1, 0, 0, "mls v6.4s, v7.4s, v0.s[2]")                                    \
    X("SIMD by element", 1, 0, 0, "smull v1.2d, v2.2s, v3.s[1]")                                  \
    X("SIMD by element", 1, 0, 0, "umull2 v4.2d, v5.4s, v6.s[0]")                                 \
    X("SIMD by element", 1, 0, 0, "smlal v7.2d, v0.2s, v1.s[3]")                                  \
    X("SIMD by element", 1, 0, 0, "umlal2 v2.2d, v3.4s, v4.s[2]")                                 \
    X("SIMD by element", 1, 0, 0, "smlsl2 v5.2d, v6.4s, v7.s[1]")                                 \
    X("SIMD by element", 1, 0, 0, "umlsl v0.2d, v1.2s, v2.s[0]")                                  \
    X("SIMD by element", 1, 0, 0, "fmul v3.2d, v4.2d, v5.d[1]")                                   \
    X("SIMD by element", 1, 0, 0, "fmla v6.4s, v7.4s, v0.s[3]")                                   \
    X("SIMD by element", 1, 0, 0, "fmls v1.2s, v2.2s, v3.s[1]")                                   \
    X("SIMD by element", 1, 0, 0, "fmul v4.4s, v5.4s, v6.s[2]")                                   \
    X("SIMD by element", 1, 0, 0, "fmla v7.2d, v0.2d, v1.d[0]")                                   \
    X("SIMD", 1, 0, 0, "ssubw v0.4s, v1.4s, v2.4h")                                               \
    X("SIMD", 1, 0, 0, "usubw2 v3.2d, v4.2d, v5.4s")                                              \
    X("SIMD", 1, 0, 0, "ssubw2 v6.8h, v7.8h, v0.16b")                                             \
    X("SIMD", 1, 0, 0, "usubw v1.2d, v2.2d, v3.2s")                                               \
    X("SIMD", 1, 0, 0, "pmull v0.8h, v1.8b, v2.8b")                                               \
    X("SIMD", 1, 0, 0, "pmull2 v3.8h, v4.16b, v5.16b")                                            \
    X("SIMD", 1, 0, 0, "pmull v6.1q, v7.1d, v0.1d")                                               \
    X("SIMD", 1, 0, 0, "pmull2 v1.1q, v2.2d, v3.2d")                                              \
    X("tagged pointers", 2, 2, 9, "orr x9, x2, x3, lsl #56\n ldr x10, [x9]")                       \
    X("tagged pointers", 2, 2, 9, "orr x9, x2, x3, lsl #56\n str x11, [x9, #8]")                   \
    X("tagged pointers", 2, 2, 9, "orr x9, x2, x3, lsl #56\n ldp w10, w11, [x9, #4]")              \
    X("multiply-add, no register pinned", 1, 0, 0, "madd x1, x2, x3, x4")

#define ASM_TEXT(group, n, p, p2, text) text "\n"
__asm__(".section .rodata\n"
        ".arch_extension lse\n"
        ".arch_extension crypto\n"
        ".p2align 2\n"
        ".global insn_words\n"
        "insn_words:\n" INSNS(ASM_TEXT) ".global insn_words_end\n"
        "insn_words_end:\n"
        ".text\n");
extern const uint32_t insn_words[], insn_words_end[];

struct test {
    const char *group;
    int n, p, p2;
    const char *text;
};
#define ENTRY(group, n, p, p2, text) {group, n, p, p2, text},
static const struct test tests[] = {INSNS(ENTRY)};
#define NTESTS (int) (sizeof(tests) / sizeof(tests[0]))

static uint32_t *code;
static size_t code_used, code_words;

// A copy of the template with w[0..1] in its slots, at an address of its own
#define COPY_STRIDE 64
static void *copy(const uint32_t *w, int n) {
    size_t len = (size_t) (insn_template_end - insn_template), slot = (size_t) (insn_template_slot - insn_template);
    if (code_used + COPY_STRIDE > code_words) {
        printf("FAIL the code buffer: %zu words, full\n", code_words);
        exit(1);
    }
    uint32_t *p = code + code_used;
    code_used += COPY_STRIDE;
    memcpy(p, insn_template, len * 4);
    for (int i = 0; i < n; i++)
        p[slot + (size_t) i] = w[i];
    __builtin___clear_cache((char *) p, (char *) (p + len));
    return p;
}

// ---- operands
#define NSETS 8
static const uint32_t offsets[NSETS] = {16, 32, 64, 128, 256, 48, 96, 384};
static const uint64_t x3s[NSETS] = {1, 2, 0, 3, 5, (uint64_t) -1, 4, 7};   // lsl, sxtw, sxtx; post-index
static const uint64_t x17s[NSETS] = {1, 2, 0, 3, 5, 6, 4, 7};               // uxtw
static const uint64_t flags[NSETS][2] = {{5, 5}, {1, 2}, {2, 1}, {(uint64_t) INT64_MIN, 1}, {0, 0}, {3, 7}, {9, 9}, {7, 3}};

static uint64_t lcg(uint64_t *s) {
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s ^ (*s >> 29);
}

struct run {
    const struct test *t;
    int set;
};

// Set k's operands, its addresses for t into out's memory; t's run; the
// addresses back to offsets into the memory
static void run(void *fn, const struct test *t, int k, struct io *out) {
    uint64_t s = 0x9E3779B97F4A7C15ull * (uint64_t) (k + 1);
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 8; i++) {
        out->q[i][0] = lcg(&s);
        out->q[i][1] = lcg(&s);
    }
    for (int i = 0; i < 17; i++)
        out->x[i] = lcg(&s);
    out->x[3 - 1] = x3s[k];
    out->x[17 - 1] = x17s[k];
    out->f1 = flags[k][0];
    out->f2 = flags[k][1];
    for (int i = 0; i < 512; i++)
        out->mem[i] = (uint8_t) lcg(&s);
    uint64_t base = (uint64_t) (uintptr_t) out->mem;
    if (t->p)
        out->x[t->p - 1] = base + offsets[k];
    if (t->p2)
        out->x[t->p2 - 1] = base + offsets[k] + 8;
    __asm__ volatile("msr fpsr, xzr");
    ((void (*)(struct io *)) fn)(out);
    __asm__ volatile("msr fpcr, xzr");   // the rounding mode an msr fpcr under test left
    if (t->p)
        out->ox[t->p - 1] -= base;
    if (t->p2)
        out->ox[t->p2 - 1] -= base;
}

static int same(const struct io *a, const struct io *b) {
    return memcmp(a->oq, b->oq, sizeof(a->oq)) == 0 && memcmp(a->ox, b->ox, sizeof(a->ox)) == 0 &&
           a->nzcv == b->nzcv && a->fpsr == b->fpsr && memcmp(a->mem, b->mem, sizeof(a->mem)) == 0;
}

// What differs between a (the gadgets) and b (the JIT)
static void describe(char *buf, size_t size, const struct io *a, const struct io *b) {
    size_t n = 0;
#define ADD(...) do { if (n < size) n += (size_t) snprintf(buf + n, size - n, __VA_ARGS__); } while (0)
    for (int i = 0; i < 8; i++)
        if (memcmp(a->oq[i], b->oq[i], 16) != 0)
            ADD("q%d %016llx:%016llx / %016llx:%016llx; ", i, (unsigned long long) a->oq[i][1],
                (unsigned long long) a->oq[i][0], (unsigned long long) b->oq[i][1], (unsigned long long) b->oq[i][0]);
    for (int i = 0; i < 17; i++)
        if (a->ox[i] != b->ox[i])
            ADD("x%d %llx / %llx; ", i + 1, (unsigned long long) a->ox[i], (unsigned long long) b->ox[i]);
    if (a->nzcv != b->nzcv)
        ADD("nzcv %llx / %llx; ", (unsigned long long) a->nzcv, (unsigned long long) b->nzcv);
    if (a->fpsr != b->fpsr)
        ADD("fpsr %llx / %llx; ", (unsigned long long) a->fpsr, (unsigned long long) b->fpsr);
    for (int i = 0; i < 512; i++)
        if (a->mem[i] != b->mem[i]) {
            ADD("mem[%d] %02x / %02x (and after)", i, a->mem[i], b->mem[i]);
            break;
        }
#undef ADD
}

// The JIT's mode and how many runs make a block hot, from /proc/ish/jit;
// 0 (and why in why) if the gadgets can not be told from the JIT here
static unsigned hot_runs(char *why, size_t size) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL) {
        snprintf(why, size, "no /proc/ish/jit");
        return 0;
    }
    char line[512], mode[512] = "";
    unsigned runs = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "mode: ", 6) == 0)
            snprintf(mode, sizeof(mode), "%s", line + 6);
        sscanf(line, "hot translation: after %u runs", &runs);
    }
    fclose(f);
    mode[strcspn(mode, "\n")] = '\0';
    if (strcmp(mode, "JIT, position-independent") != 0) {
        snprintf(why, size, "the JIT's mode is \"%s\": a block is translated as it is compiled, or never", mode);
        return 0;
    }
    if (runs == 0)
        snprintf(why, size, "blocks are translated as they are compiled (echo hot 0 > /proc/ish/jit)");
    return runs;
}

int main(void) {
    char why[200];
    unsigned hot = hot_runs(why, sizeof(why));
    if (hot == 0) {
        printf("SKIP %s\n", why);
        return 0;
    }
    int nwords = 0;
    for (int i = 0; i < NTESTS; i++)
        nwords += tests[i].n;
    if (nwords != insn_words_end - insn_words) {
        printf("FAIL the table: %d instructions named, %d assembled\n", nwords, (int) (insn_words_end - insn_words));
        return 1;
    }
    code_words = (size_t) (NTESTS * (NSETS + 2) + 16) * COPY_STRIDE;
    code = mmap(NULL, code_words * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    static struct io cold[NSETS], hot_io[NTESTS][NSETS];
    static struct {
        const char *name;
        int runs, wrong;
        char first[1400];
    } groups[24];
    int ngroups = 0;
    const uint32_t *w = insn_words;
    for (int i = 0; i < NTESTS; i++, w += tests[i - 1].n) {
        const struct test *t = &tests[i];
        int g = 0;
        while (g < ngroups && strcmp(groups[g].name, t->group) != 0)
            g++;
        if (g == ngroups)
            groups[ngroups++].name = t->group;
        // the gadgets: each run in a copy run no other time
        for (int k = 0; k < NSETS; k++)
            run(copy(w, t->n), t, k, &cold[k]);
        // the JIT: a copy run till it is hot, then for each set
        void *fn = copy(w, t->n);
        struct io warm;
        for (unsigned r = 0; r < hot + 4; r++)
            run(fn, t, 0, &warm);
        for (int k = 0; k < NSETS; k++) {
            run(fn, t, k, &hot_io[i][k]);
            groups[g].runs++;
            if (same(&cold[k], &hot_io[i][k]))
                continue;
            if (groups[g].wrong++ == 0) {
                char d[1200];
                describe(d, sizeof(d), &cold[k], &hot_io[i][k]);
                snprintf(groups[g].first, sizeof(groups[g].first), "\"%s\", operands %d: gadgets / JIT: %s",
                         t->text, k, d);
            }
        }
    }
    for (int g = 0; g < ngroups; g++) {
        char name[100], detail[1500];
        snprintf(name, sizeof(name), "%s: the gadgets and the JIT alike, %d runs", groups[g].name, groups[g].runs);
        snprintf(detail, sizeof(detail), "%d of %d runs differ; the first: %s", groups[g].wrong, groups[g].runs,
                 groups[g].first);
        check(name, groups[g].wrong == 0, detail);
    }

    // Known values, run hot (operands 0: offset 16, x3 1)
    char detail[200];
    for (int i = 0; i < NTESTS; i++) {
        const struct test *t = &tests[i];
        const struct io *o = &hot_io[i][0];
        uint64_t got, want;
        if (strcmp(t->text, "ldaxr w9, [x2]\n stlxr w10, w11, [x2]") == 0) {
            uint32_t m;
            memcpy(&m, o->mem + 16, 4);
            got = o->ox[10 - 1] << 32 | m;
            want = (uint32_t) o->ox[11 - 1];
            snprintf(detail, sizeof(detail), "status and word %#llx, want 0 and %#llx", (unsigned long long) got,
                     (unsigned long long) want);
            check("stlxr after ldaxr: stored, status 0", got == want, detail);
        } else if (strcmp(t->text, "ldxr x9, [x2]\n stxr w10, x11, [x3]") == 0) {
            got = o->ox[10 - 1];
            snprintf(detail, sizeof(detail), "status %#llx", (unsigned long long) got);
            check("stxr to another address than ldxr's: failed, status 1", got == 1, detail);
        } else if (strcmp(t->text, "ldr w9, [x2]\n casal w9, w10, [x2]") == 0) {
            uint32_t m;
            memcpy(&m, o->mem + 16, 4);
            snprintf(detail, sizeof(detail), "word %#x, want %#x", m, (uint32_t) o->ox[10 - 1]);
            check("casal with the value memory holds: swapped", m == (uint32_t) o->ox[10 - 1], detail);
        } else if (strcmp(t->text, "dc zva, x2") == 0) {
            int zero = 1;
            for (int b = 0; b < 64; b++)
                zero &= o->mem[b] == 0;   // offset 16: the block at 0
            snprintf(detail, sizeof(detail), "the 64 bytes at 0 %szero, the byte at 64 %02x", zero ? "" : "not ",
                     o->mem[64]);
            check("dc zva: 64 bytes zeroed", zero, detail);
        } else if (strcmp(t->text, "mrs x9, dczid_el0") == 0) {
            got = o->ox[9 - 1];
            snprintf(detail, sizeof(detail), "%#llx", (unsigned long long) got);
            check("dczid_el0: 64-byte blocks (4)", got == 4, detail);
        } else if (strcmp(t->text, "orr x9, x2, x3, lsl #56\n ldr x10, [x9]") == 0) {
            memcpy(&want, o->mem + 16, 8);
            got = o->ox[10 - 1];
            snprintf(detail, sizeof(detail), "%#llx, want %#llx", (unsigned long long) got, (unsigned long long) want);
            check("ldr through a tagged pointer (tag 1): the word at its address", got == want, detail);
        } else if (strcmp(t->text, "ldr w9, . + 8") == 0) {
            got = o->ox[9 - 1];
            snprintf(detail, sizeof(detail), "%#llx, want 0x91048000", (unsigned long long) got);
            check("ldr w9, . + 8: the template's add x0, x0, #288", got == 0x91048000u, detail);
        } else if (strcmp(t->text, "ldrsw x11, . + 12") == 0) {
            got = o->ox[11 - 1];
            snprintf(detail, sizeof(detail), "%#llx, want 0xffffffffad000400", (unsigned long long) got);
            check("ldrsw x11, . + 12: the template's stp q0, q1, [x0], sign-extended", got == 0xffffffffad000400ull,
                  detail);
        } else if (strcmp(t->text, "msr fpcr, x9\n mrs x10, fpcr") == 0) {
            got = o->ox[10 - 1];
            want = o->x[9 - 1] & 0x07c80000u;   // FPCR_GUEST_BITS
            snprintf(detail, sizeof(detail), "%#llx, want %#llx", (unsigned long long) got, (unsigned long long) want);
            check("msr fpcr: the bits a guest may set, read back", got == want, detail);
        } else if (strcmp(t->text, "madd x1, x2, x3, x4") == 0 || strcmp(t->text, "madd x8, x9, x10, x11") == 0) {
            const uint64_t *in = o->x;   // the inputs, as run() set them (not overwritten by the run)
            int d = t->text[6] == '1' ? 1 : 8;
            int a = d == 1 ? 2 : 9;
            want = in[a - 1] * in[a] + in[a + 1];
            got = o->ox[d - 1];
            snprintf(detail, sizeof(detail), "%#llx, want %#llx", (unsigned long long) got, (unsigned long long) want);
            check(d == 1 ? "madd x1, x2, x3, x4: the product and sum" : "madd x8, x9, x10, x11: the product and sum",
                  got == want, detail);
        }
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
