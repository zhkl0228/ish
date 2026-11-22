// Regression test for SMOV (to general) in the arm64 guest.
//
// gen.c decoded UMOV but not SMOV (0Q001110 000 imm5 0 0101 1 Rn Rd), so the
// instruction raised SIGILL. Bun 1.4 (Claude Code's runtime) executes
// `smov x12, v0.b[0]` during startup and crashed with "Illegal instruction".
//
// Covers byte/halfword to W and X and word to X, at the lowest, highest and
// both half-boundary lanes, with negative and positive elements. The expected
// values are the element sign-extended to the destination width, with a W
// destination zeroing bits 63:32. Running this natively on aarch64 hardware
// passes, which pins the expectations to the real instruction.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_smov
//            regress_smov.c regress_smov.S
// Run:   ish -r <rootfs> /tmp/regress_smov

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void smov_table(const uint8_t vec[16], uint64_t *out);

struct smov_case {
    const char *name;
    char dest;  // 'w' or 'x'
    char elem;  // 'b', 'h' or 's'
    int index;
};

static const struct smov_case cases[] = {
    {"smov w, v.b[0]", 'w', 'b', 0},
    {"smov w, v.b[7]", 'w', 'b', 7},
    {"smov w, v.b[8]", 'w', 'b', 8},
    {"smov w, v.b[15]", 'w', 'b', 15},
    {"smov x, v.b[0]", 'x', 'b', 0},
    {"smov x, v.b[7]", 'x', 'b', 7},
    {"smov x, v.b[8]", 'x', 'b', 8},
    {"smov x, v.b[15]", 'x', 'b', 15},
    {"smov w, v.h[0]", 'w', 'h', 0},
    {"smov w, v.h[3]", 'w', 'h', 3},
    {"smov w, v.h[4]", 'w', 'h', 4},
    {"smov w, v.h[7]", 'w', 'h', 7},
    {"smov x, v.h[0]", 'x', 'h', 0},
    {"smov x, v.h[3]", 'x', 'h', 3},
    {"smov x, v.h[4]", 'x', 'h', 4},
    {"smov x, v.h[7]", 'x', 'h', 7},
    {"smov x, v.s[0]", 'x', 's', 0},
    {"smov x, v.s[1]", 'x', 's', 1},
    {"smov x, v.s[2]", 'x', 's', 2},
    {"smov x, v.s[3]", 'x', 's', 3}
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

static const uint8_t vec[16] = {
    0x81, 0x02, 0x83, 0x7f, 0xf5, 0x06, 0x87, 0x08,
    0x99, 0x1a, 0xab, 0x3c, 0xcd, 0x5e, 0xef, 0x70,
};

static uint64_t expected(const struct smov_case *c) {
    int64_t v;
    if (c->elem == 'b') {
        v = (int8_t) vec[c->index];
    } else if (c->elem == 'h') {
        int16_t h;
        memcpy(&h, &vec[c->index * 2], sizeof(h));
        v = h;
    } else {
        int32_t s;
        memcpy(&s, &vec[c->index * 4], sizeof(s));
        v = s;
    }
    if (c->dest == 'w')
        return (uint32_t) v;
    return (uint64_t) v;
}

int main(void) {
    uint64_t out[NCASES];
    smov_table(vec, out);
    int failed = 0;
    for (size_t i = 0; i < NCASES; i++) {
        uint64_t want = expected(&cases[i]);
        if (out[i] != want) {
            printf("FAIL %-20s got %#018llx want %#018llx\n", cases[i].name,
                   (unsigned long long) out[i], (unsigned long long) want);
            failed++;
        }
    }
    printf("%zu passed, %d failed\n", NCASES - failed, failed);
    return failed != 0;
}
