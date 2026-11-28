// Regression test for translated block ends testing a copy of their chain
// slots (ish -Djit=true translating; run.sh runs it position-independent, as
// on iOS; skips otherwise).
//
// A block chained to its successor goes there by a direct branch. Its native
// code first tests whether the slot is still chained, through a copy of the
// slot that its module context keeps. When the successor's code is written,
// the successor leaves the cache and its predecessors' slots are unchained.
// The copies must follow, or a predecessor's native code goes on into the old
// code.
//
// Callers on one page branch to code on the next. The edges are a b, the
// taken edge and the fall-through edge of a cbz, and a bl. The callers run
// until they are translated and chained, and then the next page is rewritten,
// round after round. Checked: every result is the new code's (from the second
// round on, the old successor's translation also refuses the new code, so the
// slot is marked and its native code enters the new code the generic way),
// and the JIT translated (/proc/ish/jit).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_chain_copy regress_jit_chain_copy.c

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096
#define WORDS (PAGE / 4)
#define ROUNDS 64
#define CALLS 24   // per caller and round: past the JIT's hot count (8 runs)

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

// Whether /proc/ish/jit says the JIT translates
static int jit_translates(void) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL)
        return 0;
    char line[512];
    int jit = 0;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strcmp(line, "mode: JIT\n") == 0 || strcmp(line, "mode: JIT, position-independent\n") == 0)
            jit = 1;
    fclose(f);
    return jit;
}

#define B(from, to) (0x14000000u | ((uint32_t) ((to) - (from)) & 0x3ffffff))
#define BL(from, to) (0x94000000u | ((uint32_t) ((to) - (from)) & 0x3ffffff))
#define CBZ_X0(from, to) (0xB4000000u | ((uint32_t) ((to) - (from)) & 0x7ffff) << 5)
#define MOVZ_X0(imm) (0xD2800000u | (uint32_t) (imm) << 5)
#define RET 0xD65F03C0u
#define PUSH_LR 0xF81F0FFEu   // str x30, [sp, #-16]!
#define POP_LR 0xF84107FEu    // ldr x30, [sp], #16
#define MOV_X2_X0 0xAA0003E2u // mov x2, x0: something translated before the branch

// Callers on page 0, the code they go to on page 1 (word indices of both)
enum { P_B = 0, P_BL = 2, P_CBZ = WORDS - 2 };      // the cbz falls through to page 1's first word
enum { S_FALL = WORDS, S_B = WORDS + 2, S_TAKEN = WORDS + 4, S_BL = WORDS + 6 };

// What the code on page 1 returns in round r, per edge
static unsigned want(int edge, int r) {
    return (unsigned) (edge * 1000 + r);
}

static void write_callees(uint32_t *code, int r) {
    const unsigned at[4] = {S_B, S_TAKEN, S_FALL, S_BL};
    for (int edge = 0; edge < 4; edge++) {
        code[at[edge]] = MOVZ_X0(want(edge, r));
        code[at[edge] + 1] = RET;
    }
    __builtin___clear_cache((char *) (code + WORDS), (char *) (code + 2 * WORDS));
}

int main(void) {
    if (!jit_translates()) {
        printf("SKIP the JIT does not translate here (ish -Djit=true with a code region)\n");
        return 0;
    }
    uint32_t *code = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    code[P_B] = MOV_X2_X0;
    code[P_B + 1] = B(P_B + 1, S_B);
    code[P_BL] = PUSH_LR;
    code[P_BL + 1] = BL(P_BL + 1, S_BL);
    code[P_BL + 2] = POP_LR;
    code[P_BL + 3] = RET;
    code[P_CBZ] = MOV_X2_X0;
    code[P_CBZ + 1] = CBZ_X0(P_CBZ + 1, S_TAKEN);
    __builtin___clear_cache((char *) code, (char *) (code + WORDS));

    uint64_t (*via_b)(uint64_t) = (uint64_t (*)(uint64_t)) (uintptr_t) (code + P_B);
    uint64_t (*via_bl)(uint64_t) = (uint64_t (*)(uint64_t)) (uintptr_t) (code + P_BL);
    uint64_t (*via_cbz)(uint64_t) = (uint64_t (*)(uint64_t)) (uintptr_t) (code + P_CBZ);
    const char *names[4] = {"b", "cbz taken", "cbz fall-through", "bl"};
    int wrong[4] = {0}, first_round[4] = {0};
    uint64_t first_got[4] = {0};
    for (int r = 1; r <= ROUNDS; r++) {
        write_callees(code, r);
        for (int i = 0; i < CALLS; i++) {
            uint64_t got[4] = {via_b(0), via_cbz(0), via_cbz(1), via_bl(0)};
            for (int edge = 0; edge < 4; edge++)
                if (got[edge] != want(edge, r) && wrong[edge]++ == 0) {
                    first_round[edge] = r;
                    first_got[edge] = got[edge];
                }
        }
    }
    for (int edge = 0; edge < 4; edge++) {
        char name[160], detail[200];
        snprintf(name, sizeof(name), "%s: after the code it goes to is rewritten, the new code's result", names[edge]);
        snprintf(detail, sizeof(detail), "%d of %d calls wrong; the first in round %d: %llu, want %u", wrong[edge],
                 ROUNDS * CALLS, first_round[edge], (unsigned long long) first_got[edge],
                 want(edge, first_round[edge]));
        check(name, wrong[edge] == 0, detail);
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
