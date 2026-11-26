// Regression test for the scalar saturating add and subtract: SQADD, UQADD,
// SQSUB and UQSUB on Bd, Hd, Sd and Dd.
//
// iSH decoded none of them: they were SIGILL. busybox gzip's deflate slides
// its hash chains with UQSUB Hd, and every compression died with "unhandled
// illegal instruction 0x7e7f2fde" (gzip, tar -z): gzip -1 wrote nothing.
//
// Checked, for each instruction and size, with every pair of 8 operands at
// and around the limits: the result against a C reference (the ARM ARM's
// SignedSatQ and UnsignedSatQ), the rest of Vd zeroed (the operands' other
// lanes are not zero, nor was Vd), and FPSR.QC set when the result saturated
// and only then.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_scalar_saturating regress_scalar_saturating.c

#include <stdint.h>
#include <stdio.h>

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

enum { SQADD, UQADD, SQSUB, UQSUB };
static const char *const op_names[] = {"sqadd", "uqadd", "sqsub", "uqsub"};
static const char size_names[] = "bhsd";

#define QC (1ul << 27)

struct vec {
    uint64_t lo, hi;
};

static uint64_t mask(int bits) {
    return bits == 64 ? ~0ul : (1ul << bits) - 1;
}

static __int128 sext(uint64_t v, int bits) {
    return (__int128) ((int64_t) (v << (64 - bits)) >> (64 - bits));
}

// The ARM ARM's result, and whether it saturated
static uint64_t reference(int op, int bits, uint64_t a, uint64_t b, int *sat) {
    __int128 r, max, min;
    if (op == SQADD || op == SQSUB) {
        r = op == SQADD ? sext(a, bits) + sext(b, bits) : sext(a, bits) - sext(b, bits);
        max = ((__int128) 1 << (bits - 1)) - 1;
        min = -((__int128) 1 << (bits - 1));
    } else {
        r = op == UQADD ? (__int128) a + b : (__int128) a - b;
        max = mask(bits);
        min = 0;
    }
    *sat = r > max || r < min;
    r = r > max ? max : r < min ? min : r;
    return (uint64_t) r & mask(bits);
}

// insn on v1 and v2 into v0, which held 0xee bytes; v0 whole and FPSR after
#define RUN(insn)                                                                                  \
    __asm__ volatile("ldr q1, [%1]\n\tldr q2, [%2]\n\tmovi v0.16b, #0xee\n\tmsr fpsr, xzr\n\t" insn \
                     "\n\tstr q0, [%3]\n\tmrs %0, fpsr"                                            \
                     : "=&r"(fpsr)                                                                 \
                     : "r"(&va), "r"(&vb), "r"(&out)                                               \
                     : "v0", "v1", "v2", "memory")

static struct vec run(int op, int size, uint64_t a, uint64_t b, uint64_t *fpsr_out) {
    uint64_t m = mask(8 << size), fpsr;
    // lane 0 the operand, the other lanes not zero
    struct vec va = {(0xa5a5a5a5a5a5a5a5ul & ~m) | a, 0x5a5a5a5a5a5a5a5aul};
    struct vec vb = {(0x3c3c3c3c3c3c3c3cul & ~m) | b, 0xc3c3c3c3c3c3c3c3ul};
    struct vec out;
    switch (op * 4 + size) {
        case SQADD * 4 + 0: RUN("sqadd b0, b1, b2"); break;
        case SQADD * 4 + 1: RUN("sqadd h0, h1, h2"); break;
        case SQADD * 4 + 2: RUN("sqadd s0, s1, s2"); break;
        case SQADD * 4 + 3: RUN("sqadd d0, d1, d2"); break;
        case UQADD * 4 + 0: RUN("uqadd b0, b1, b2"); break;
        case UQADD * 4 + 1: RUN("uqadd h0, h1, h2"); break;
        case UQADD * 4 + 2: RUN("uqadd s0, s1, s2"); break;
        case UQADD * 4 + 3: RUN("uqadd d0, d1, d2"); break;
        case SQSUB * 4 + 0: RUN("sqsub b0, b1, b2"); break;
        case SQSUB * 4 + 1: RUN("sqsub h0, h1, h2"); break;
        case SQSUB * 4 + 2: RUN("sqsub s0, s1, s2"); break;
        case SQSUB * 4 + 3: RUN("sqsub d0, d1, d2"); break;
        case UQSUB * 4 + 0: RUN("uqsub b0, b1, b2"); break;
        case UQSUB * 4 + 1: RUN("uqsub h0, h1, h2"); break;
        case UQSUB * 4 + 2: RUN("uqsub s0, s1, s2"); break;
        default: RUN("uqsub d0, d1, d2"); break;
    }
    *fpsr_out = fpsr;
    return out;
}

int main(void) {
    char name[64], detail[256], line[320];
    for (int op = SQADD; op <= UQSUB; op++) {
        for (int size = 0; size < 4; size++) {
            int bits = 8 << size;
            uint64_t m = mask(bits), top = 1ul << (bits - 1);
            const uint64_t values[] = {0, 1, 2, top - 1, top, m - 1, m, 0x5555555555555555ul & m};
            int pairs = 0, wrong = 0;
            detail[0] = '\0';
            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 8; j++) {
                    uint64_t a = values[i], b = values[j], fpsr;
                    int sat;
                    uint64_t want = reference(op, bits, a, b, &sat);
                    struct vec got = run(op, size, a, b, &fpsr);
                    int qc = (fpsr & QC) != 0;
                    pairs++;
                    if ((got.lo & m) == want && (got.lo & ~m) == 0 && got.hi == 0 && qc == sat)
                        continue;
                    if (wrong++ == 0)
                        snprintf(detail, sizeof(detail),
                                 "%#lx %s %#lx: %#lx (rest %#lx, high %#lx, QC %d), want %#lx (QC %d)", a,
                                 op == SQADD || op == UQADD ? "+" : "-", b, got.lo & m, got.lo & ~m, got.hi, qc,
                                 want, sat);
                }
            }
            snprintf(name, sizeof(name), "%s %c, %d pairs", op_names[op], size_names[size], pairs);
            snprintf(line, sizeof(line), "%d wrong; the first: %s", wrong, detail);
            check(name, wrong == 0, line);
        }
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
