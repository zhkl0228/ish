// Regression test for translated calls and returns through the frame's
// shadow stack (ish -Djit=true translating; run.sh runs it position-
// independent, as on iOS; skips otherwise).
//
// A translated bl or blr is a host bl, its return a host ret, which the CPU
// predicts. The frame's shadow stack keeps an entry per call: the guest
// return address, where the host bl returns to (the block end for the code
// after the call), and x29. A ret whose target is the last entry's address
// returns there with a host ret. Any other ret goes the way it went before
// (ret_cache, the run loop).
//
// Checked, each probe run over and over (hot: translated), every result
// against C's:
// - recursion 100 deep, more than the shadow stack's 64 entries;
// - calls through function pointers (blr), in a loop;
// - a longjmp out of three calls, leaving their entries, then calls again;
// - a function that returns elsewhere than its caller (x30 changed);
// - the code a call returns to rewritten during the call: the new code runs;
// - two threads at once;
// and that the JIT made such calls (/proc/ish/jit).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_shadow regress_jit_shadow.c -lpthread

#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ROUNDS 40   // calls per probe: well past the JIT's hot count (8 runs)

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

// rec(n, k): n + rec(n - 1, k) ... + k at the bottom, by bl to itself.
// warp(): its ret goes to warp_alt (x30 changed, the return address kept in
// x9), which returns 9 to warp's caller.
__asm__(".text\n"
        ".p2align 2\n"
        ".global rec\n"
        "rec:\n"
        "    cbz x0, 1f\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    str x0, [sp, #16]\n"
        "    sub x0, x0, #1\n"
        "    bl rec\n"
        "    ldr x2, [sp, #16]\n"
        "    add x0, x0, x2\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        "1:  mov x0, x1\n"
        "    ret\n"
        ".global warp\n"
        "warp:\n"
        "    mov x9, x30\n"
        "    mov x0, #7\n"
        "    adr x30, warp_alt\n"
        "    ret\n"
        "warp_alt:\n"
        "    add x0, x0, #2\n"
        "    mov x30, x9\n"
        "    ret\n"
        // warp_caller(): warp's result through its own return, plus 100
        ".global warp_caller\n"
        "warp_caller:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    bl warp\n"
        "    add x0, x0, #100\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n");
uint64_t rec(uint64_t n, uint64_t k);
uint64_t warp_caller(void);

// /proc/ish/jit: whether the JIT translates, and its count of calls by a
// host bl; -1 if it does not translate, -2 if it does and says no count
static long long host_calls(void) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL)
        return -1;
    char line[512];
    int jit = 0;
    long long n = -2, r;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strcmp(line, "mode: JIT\n") == 0 || strcmp(line, "mode: JIT, position-independent\n") == 0)
            jit = 1;
        sscanf(line, "calls by a host bl, translated: %lld; rets by a host ret, translated: %lld", &n, &r);
    }
    fclose(f);
    return jit ? n : -1;
}

static int recursion_right(char *wrong, size_t size) {
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t k = (uint64_t) r * 1000, got = rec(100, k), want = 100 * 101 / 2 + k;
        if (got != want) {
            snprintf(wrong, size, "round %d: rec(100, %llu) = %llu, want %llu", r, (unsigned long long) k,
                     (unsigned long long) got, (unsigned long long) want);
            return 0;
        }
    }
    return 1;
}

static uint64_t __attribute__((noinline)) f1(uint64_t x) { return x * 3 + 1; }
static uint64_t __attribute__((noinline)) f2(uint64_t x) { return x ^ 0x55; }
static uint64_t __attribute__((noinline)) f3(uint64_t x) { return f1(x) + f2(x); }
static uint64_t (*const fns[3])(uint64_t) = {f1, f2, f3};

static jmp_buf jb;
static void __attribute__((noinline)) deep3(int n) {
    if (n == 3)
        longjmp(jb, 1);
    if (n < 3)
        deep3(n + 1);
    __asm__ volatile("" ::: "memory");   // not a tail call
}

struct worker {
    int ok;
    char wrong[200];
};
static void *worker(void *arg) {
    struct worker *w = arg;
    w->ok = recursion_right(w->wrong, sizeof(w->wrong));
    return NULL;
}

#define MOVZ_X0(imm) (0xD2800000u | (uint32_t) (imm) << 5)
#define BLR_X1 0xD63F0020u
#define PUSH_FP_LR 0xA9BF7BFDu   // stp x29, x30, [sp, #-16]!
#define POP_FP_LR 0xA8C17BFDu    // ldp x29, x30, [sp], #16
#define RET 0xD65F03C0u
static uint32_t *code;   // the caller: blr x1, then movz x0, #K
static int rewrites;
static void rewrite_after_call(void) {
    code[2] = MOVZ_X0(1000 + rewrites);
    __builtin___clear_cache((char *) code, (char *) (code + 8));
}

int main(void) {
    long long before = host_calls();
    if (before == -1) {
        printf("SKIP the JIT does not translate here (ish -Djit=true with a code region)\n");
        return 0;
    }
    char wrong[420] = "";

    check("recursion 100 deep, more than the shadow stack holds: every sum right", recursion_right(wrong, sizeof(wrong)),
          wrong);

    int bad = 0;
    for (int r = 0; r < ROUNDS && !bad; r++) {
        uint64_t sum = 0, want = 0;
        for (int i = 0; i < 30; i++) {
            sum += fns[i % 3]((uint64_t) (r * 30 + i));
            uint64_t x = (uint64_t) (r * 30 + i);
            want += i % 3 == 0 ? x * 3 + 1 : i % 3 == 1 ? x ^ 0x55 : (x * 3 + 1) + (x ^ 0x55);
        }
        if (sum != want) {
            bad = 1;
            snprintf(wrong, sizeof(wrong), "round %d: %llu, want %llu", r, (unsigned long long) sum,
                     (unsigned long long) want);
        }
    }
    check("calls through function pointers (blr): every result right", !bad, wrong);

    bad = 0;
    for (int r = 0; r < ROUNDS && !bad; r++) {
        if (setjmp(jb) == 0)
            deep3(0);
        uint64_t got = rec(10, (uint64_t) r);
        if (got != 55 + (uint64_t) r) {
            bad = 1;
            snprintf(wrong, sizeof(wrong), "round %d: after the longjmp rec(10) = %llu, want %llu", r,
                     (unsigned long long) got, (unsigned long long) (55 + r));
        }
    }
    check("a longjmp out of three calls, then calls again: every result right", !bad, wrong);

    bad = 0;
    for (int r = 0; r < ROUNDS && !bad; r++) {
        uint64_t got = warp_caller();
        if (got != 109) {
            bad = 1;
            snprintf(wrong, sizeof(wrong), "round %d: %llu, want 109", r, (unsigned long long) got);
        }
    }
    check("a function returning elsewhere than its caller (x30 changed): the code there runs", !bad, wrong);

    // caller(fn): stp; blr x1 (fn); movz x0, #K; ldp; ret, on a page of its own
    code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    code[0] = PUSH_FP_LR;
    code[1] = BLR_X1;
    code[2] = MOVZ_X0(1000);
    code[3] = POP_FP_LR;
    code[4] = RET;
    __builtin___clear_cache((char *) code, (char *) (code + 8));
    uint64_t (*caller)(uint64_t, void (*)(void)) = (uint64_t (*)(uint64_t, void (*)(void))) (uintptr_t) code;
    bad = 0;
    for (int r = 0; r < ROUNDS && !bad; r++) {
        rewrites = r + 1;
        uint64_t got = caller(0, rewrite_after_call);
        if (got != 1000u + (uint64_t) rewrites) {
            bad = 1;
            snprintf(wrong, sizeof(wrong), "round %d: %llu, want %llu (the code before the call: %llu)", r,
                     (unsigned long long) got, (unsigned long long) (1000 + rewrites),
                     (unsigned long long) (1000 + r));
        }
    }
    check("the code a call returns to rewritten during the call: the new code runs", !bad, wrong);

    struct worker two[2];
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        if (pthread_create(&t[i], NULL, worker, &two[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    snprintf(wrong, sizeof(wrong), "%s / %s", two[0].ok ? "right" : two[0].wrong, two[1].ok ? "right" : two[1].wrong);
    check("two threads at once: every sum right", two[0].ok && two[1].ok, wrong);

    long long after = host_calls();
    if (after == -2)
        snprintf(wrong, sizeof(wrong), "/proc/ish/jit says no count of them");
    else
        snprintf(wrong, sizeof(wrong), "%lld before, %lld after", before, after);
    check("the JIT made calls by a host bl", before >= 0 && after > before, wrong);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
