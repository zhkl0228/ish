// Regression test for SQABS (vector) in the arm64 guest.
//
// gen.c did not decode SQABS, so it raised SIGILL. ugrep counts lines with it
// (cmeq against '\n', four vectors added, sqabs, addv), and Claude Code runs
// its own ugrep as the shell's grep: `grep -v -i token` died of "Illegal
// instruction" on the word 0x4e207821, sqabs v1.16b, v1.16b.
//
// Covers every arrangement, the most negative element saturating to the most
// positive, the 64-bit forms clearing bits 127:64, FPSR.QC set only when an
// element saturates, ugrep's own word, and the reserved size=11 Q=0 form
// raising SIGILL. The expected values follow from the instruction's
// definition. On an iSH without the fix every SQABS case fails with SIGILL.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sqabs regress_sqabs.c
// Run:   ish -r <rootfs> /tmp/regress_sqabs

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FPSR_QC (1u << 27)

// Each runs one SQABS from in into a destination of all ones (v1 from v0, or
// v1 from v1 for ugrep's word), with FPSR cleared before, and gives FPSR after.
#define SQABS_FN(fn, insn)                                                        \
    static uint64_t fn(const void *in, void *out) {                               \
        uint64_t fpsr;                                                            \
        __asm__ volatile("msr fpsr, xzr\n\t"                                      \
                         "ldr q0, [%1]\n\t"                                       \
                         "movi v1.16b, #0xff\n\t"                                 \
                         insn "\n\t"                                              \
                         "str q1, [%2]\n\t"                                       \
                         "mrs %0, fpsr"                                           \
                         : "=r"(fpsr) : "r"(in), "r"(out) : "v0", "v1", "memory"); \
        return fpsr;                                                              \
    }
SQABS_FN(sqabs_16b, "sqabs v1.16b, v0.16b")
SQABS_FN(sqabs_8b, "sqabs v1.8b, v0.8b")
SQABS_FN(sqabs_8h, "sqabs v1.8h, v0.8h")
SQABS_FN(sqabs_4h, "sqabs v1.4h, v0.4h")
SQABS_FN(sqabs_4s, "sqabs v1.4s, v0.4s")
SQABS_FN(sqabs_2s, "sqabs v1.2s, v0.2s")
SQABS_FN(sqabs_2d, "sqabs v1.2d, v0.2d")
SQABS_FN(ugrep_word, "mov v1.16b, v0.16b\n\t.inst 0x4e207821")   // sqabs v1.16b, v1.16b
SQABS_FN(reserved_1d, ".inst 0x0ee07821")                         // size=11, Q=0: reserved

struct test_case {
    const char *name;
    uint64_t (*fn)(const void *in, void *out);
    uint8_t in[16], expected[16];
    int qc;   // FPSR.QC after it, or -1 for a SIGILL
};

static const struct test_case cases[] = {
    {"16b", sqabs_16b,
     {0x80, 0x81, 0xff, 0x00, 0x01, 0x7f, 0xfc, 0x9c, 0x64, 0x80, 0xfe, 0x02, 0xc0, 0x40, 0x81, 0x7e},
     {0x7f, 0x7f, 0x01, 0x00, 0x01, 0x7f, 0x04, 0x64, 0x64, 0x7f, 0x02, 0x02, 0x40, 0x40, 0x7f, 0x7e}, 1},
    // ugrep's sums of 0 to 4 newlines, each -1 from cmeq: no saturation
    {"ugrep's word", ugrep_word,
     {0xfc, 0xff, 0x00, 0xfd, 0xfe, 0x00, 0x00, 0xff, 0xff, 0xfc, 0x00, 0x00, 0xfe, 0xff, 0xfd, 0x00},
     {0x04, 0x01, 0x00, 0x03, 0x02, 0x00, 0x00, 0x01, 0x01, 0x04, 0x00, 0x00, 0x02, 0x01, 0x03, 0x00}, 0},
    {"8b", sqabs_8b,
     {0x80, 0xff, 0x05, 0xfb, 0x00, 0x7f, 0x81, 0xc8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
     {0x7f, 0x01, 0x05, 0x05, 0x00, 0x7f, 0x7f, 0x38, 0, 0, 0, 0, 0, 0, 0, 0}, 1},
    // halfwords 8000 ffff 7fff 0000 8001 fff6 1234 edcc, little-endian
    {"8h", sqabs_8h,
     {0x00, 0x80, 0xff, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x01, 0x80, 0xf6, 0xff, 0x34, 0x12, 0xcc, 0xed},
     {0xff, 0x7f, 0x01, 0x00, 0xff, 0x7f, 0x00, 0x00, 0xff, 0x7f, 0x0a, 0x00, 0x34, 0x12, 0x34, 0x12}, 1},
    // 8001 fffe 0003 8000
    {"4h", sqabs_4h,
     {0x01, 0x80, 0xfe, 0xff, 0x03, 0x00, 0x00, 0x80, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
     {0xff, 0x7f, 0x02, 0x00, 0x03, 0x00, 0xff, 0x7f, 0, 0, 0, 0, 0, 0, 0, 0}, 1},
    // 80000000 ffffffff 7fffffff fffff000
    {"4s", sqabs_4s,
     {0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x00, 0xf0, 0xff, 0xff},
     {0xff, 0xff, 0xff, 0x7f, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x7f, 0x00, 0x10, 0x00, 0x00}, 1},
    // fffffffb 00000007: no saturation
    {"2s", sqabs_2s,
     {0xfb, 0xff, 0xff, 0xff, 0x07, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
     {0x05, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0}, 0},
    // 8000000000000000 fffffffffffffffe
    {"2d", sqabs_2d,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
     {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 1},
    {"reserved 1d", reserved_1d, {0}, {0}, -1},
};

static sigjmp_buf env;
static void on_signal(int sig) {
    siglongjmp(env, sig);
}

static void print_bytes(const char *label, const uint8_t *b) {
    printf("  %s", label);
    for (int i = 0; i < 16; i++)
        printf(" %02x", b[i]);
    printf("\n");
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);

    int passed = 0, failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct test_case *c = &cases[i];
        uint8_t out[16];
        uint64_t fpsr = 0;
        int sig = sigsetjmp(env, 1);
        if (sig == 0)
            fpsr = c->fn(c->in, out);
        if (c->qc < 0) {
            if (sig == SIGILL) {
                passed++;
            } else {
                failed++;
                printf("FAIL %s: no SIGILL\n", c->name);
            }
            continue;
        }
        if (sig != 0) {
            failed++;
            printf("FAIL %s: signal %d\n", c->name, sig);
            continue;
        }
        int qc = (fpsr & FPSR_QC) != 0;
        if (memcmp(out, c->expected, 16) == 0 && qc == c->qc) {
            passed++;
        } else {
            failed++;
            printf("FAIL %s: QC %d, expected %d\n", c->name, qc, c->qc);
            print_bytes("got:     ", out);
            print_bytes("expected:", c->expected);
        }
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
