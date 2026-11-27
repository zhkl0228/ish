// Regression test for a TLB check that reuses the last access's entry (ish
// -Djit=true translating; run.sh runs it position-independent, as on iOS;
// skips otherwise).
//
// In a translated block, an access through the same register as the access
// before it (the register not moved far since) first checks the page of the
// TLB entry that access left, without the hash. Off that page it takes the
// full lookup, placed out of line after the block, which comes back, or on a
// miss goes to the unit's exit (its gadget).
//
// Checked, each probe run over and over (hot: translated), every result
// against what the memory holds:
// - the same page: loads, a store and a pair through x0 after the first;
// - the next page: the entry's check fails and the full lookup out of line
//   hits, then x0's page again; a next page not in the TLB yet (never
//   touched): the full lookup misses too, and the gadget reads its zeroes;
// - accesses across the end of a page: the check fails, and so does the
//   full lookup;
// - a post-index load, then the base's new address, and the next page;
// - a store after a load of a page made read-only: a write's check takes
//   the entry's page_if_writable, and the guest gets SIGSEGV at the store,
//   the memory as it was;
// - the next page made PROT_NONE: SIGSEGV at the access there;
// and that the JIT made such checks (/proc/ish/jit).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_tlb_reuse regress_jit_tlb_reuse.c

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096

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

// The probes: every access through x0, so all after the first check the
// entry of the one before
__asm__(".text\n"
        ".p2align 2\n"
        // [p] + [p+8] + [p+8] + [p+16], [p+8] stored at p+16 first
        "probe_same:\n"
        "    ldr x1, [x0]\n"
        "    ldr x2, [x0, #8]\n"
        "    str x2, [x0, #16]\n"
        "    ldp x3, x4, [x0, #8]\n"
        "    add x0, x1, x2\n"
        "    add x0, x0, x3\n"
        "    add x0, x0, x4\n"
        "    ret\n"
        // [p] + [p+4096] + [p+8]: the next page, then p's again
        "probe_next:\n"
        "    ldr x1, [x0]\n"
        "    ldr x2, [x0, #4096]\n"
        "    ldr x3, [x0, #8]\n"
        "    add x0, x1, x2\n"
        "    add x0, x0, x3\n"
        "    ret\n"
        // [p] + [p+8] + [p+4096], after a post-index load
        "probe_post:\n"
        "    ldr x1, [x0], #8\n"
        "    ldr x2, [x0]\n"
        "    ldr x3, [x0, #4088]\n"
        "    add x0, x1, x2\n"
        "    add x0, x0, x3\n"
        "    ret\n"
        // [p] + 1, stored at p+8
        "probe_store:\n"
        "    ldr x1, [x0]\n"
        "    add x1, x1, #1\n"
        "    str x1, [x0, #8]\n"
        "    mov x0, x1\n"
        "    ret\n");
uint64_t probe_same(void *p), probe_next(void *p), probe_post(void *p), probe_store(void *p);
__asm__(".global probe_same\n.global probe_next\n.global probe_post\n.global probe_store\n");

static uint64_t at(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void fill(uint8_t *p, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++)
        p[i] = (uint8_t) (i * 131 + seed * 29 + 7);
}

// /proc/ish/jit: whether the JIT translates, and its count of checks of the
// last access's entry; -1 if it does not translate, -2 if it does and says
// no count
static long long reused_checks(void) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL)
        return -1;
    char line[512];
    int jit = 0;
    long long n = -2;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strcmp(line, "mode: JIT\n") == 0 || strcmp(line, "mode: JIT, position-independent\n") == 0)
            jit = 1;
        sscanf(line, "TLB checks of the last access's entry (an access through the same register), translated: %lld",
               &n);
    }
    fclose(f);
    return jit ? n : -1;
}

static sigjmp_buf fault_env;
static void *volatile fault_addr;
static void on_segv(int sig, siginfo_t *info, void *uc) {
    (void) sig;
    (void) uc;
    fault_addr = info->si_addr;
    siglongjmp(fault_env, 1);
}

// p's probe, n calls; the address it faulted at, NULL if none
static void *faulting(uint64_t (*probe)(void *), void *p, int n) {
    fault_addr = NULL;
    for (volatile int i = 0; i < n; i++)
        if (sigsetjmp(fault_env, 1) == 0)
            probe(p);
        else if (fault_addr == NULL)
            return (void *) 1;
    return fault_addr;
}

// The probe at each 4-byte offset into the first page of the area, rounds
// times; wrong: the first wrong result
static int sweep(const char *name, uint64_t (*probe)(void *), uint64_t (*want)(uint8_t *), uint8_t *area, int rounds,
                 char *wrong, size_t size) {
    int runs = 0, bad = 0;
    for (int r = 0; r < rounds; r++)
        for (int off = 0; off < PAGE; off += 4) {
            uint8_t *p = area + off;
            uint64_t w = want(p), got = probe(p);
            runs++;
            if (got != w && bad++ == 0)
                snprintf(wrong, size, "%s at page offset %d: %#llx, want %#llx", name, off, (unsigned long long) got,
                         (unsigned long long) w);
        }
    if (bad && strlen(wrong) + 40 < size)
        snprintf(wrong + strlen(wrong), size - strlen(wrong), " (%d of %d runs wrong)", bad, runs);
    return bad == 0;
}

static uint64_t want_same(uint8_t *p) {
    uint64_t a = at(p), b = at(p + 8);
    return a + b + b + b;   // [p+16] is b once stored
}
static uint64_t want_next(uint8_t *p) {
    return at(p) + at(p + PAGE) + at(p + 8);
}
static uint64_t want_post(uint8_t *p) {
    return at(p) + at(p + 8) + at(p + PAGE);
}

int main(void) {
    long long before = reused_checks();
    if (before == -1) {
        printf("SKIP the JIT does not translate here (ish -Djit=true with a code region)\n");
        return 0;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    char wrong[300] = "";
    uint8_t *area = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    fill(area, 3 * PAGE, 1);
    check("the same page, and across its end: every result the memory's",
          sweep("probe_same", probe_same, want_same, area, 4, wrong, sizeof(wrong)), wrong);
    wrong[0] = '\0';
    check("the next page through the full lookup out of line, then the first page's: every result the memory's",
          sweep("probe_next", probe_next, want_next, area, 4, wrong, sizeof(wrong)), wrong);
    wrong[0] = '\0';
    check("after a post-index load, its new address and the next page: every result the memory's",
          sweep("probe_post", probe_post, want_post, area, 4, wrong, sizeof(wrong)), wrong);

    // Pairs of pages, the second never touched: not in the TLB as the full
    // lookup out of line looks it up (the first is, read just before)
    enum { PAIRS = 128 };
    uint8_t *fresh = mmap(NULL, 2 * PAIRS * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fresh == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    int bad = 0;
    for (int i = 0; i < PAIRS; i++) {
        uint8_t *p = fresh + 2 * i * PAGE + 64;
        fill(p - 64, PAGE, (unsigned) i);
        uint64_t w = at(p) + at(p + 8), got = probe_next(p);   // the second page's zeroes
        if (got != w && bad++ == 0)
            snprintf(wrong, sizeof(wrong), "pair %d: %#llx, want %#llx", i, (unsigned long long) got,
                     (unsigned long long) w);
    }
    check("the next page not in the TLB yet: the full lookup misses, its zeroes read", bad == 0, wrong);

    // A store after a load, hot on a writable page, then on it read-only
    uint8_t *ro = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (int i = 0; i < 64; i++)
        probe_store(ro + 128);
    uint64_t kept = at(ro + 136);
    mprotect(ro, PAGE, PROT_READ);
    void *addr = faulting(probe_store, ro + 128, 3);
    snprintf(wrong, sizeof(wrong), "fault at %p (want %p), the word there %#llx (was %#llx)", addr,
             (void *) (ro + 136), (unsigned long long) at(ro + 136), (unsigned long long) kept);
    check("a store after a load of a read-only page: SIGSEGV at the store, nothing written",
          addr == ro + 136 && at(ro + 136) == kept, wrong);

    // The next page PROT_NONE once hot
    uint8_t *two = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (two == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    fill(two, 2 * PAGE, 3);
    for (int i = 0; i < 64; i++)
        probe_next(two + 256);
    mprotect(two + PAGE, PAGE, PROT_NONE);
    addr = faulting(probe_next, two + 256, 3);
    snprintf(wrong, sizeof(wrong), "fault at %p, want %p", addr, (void *) (two + 256 + PAGE));
    check("the next page PROT_NONE: SIGSEGV at the access there", addr == two + 256 + PAGE, wrong);

    long long after = reused_checks();
    if (after == -2)
        snprintf(wrong, sizeof(wrong), "/proc/ish/jit says no count of them");
    else
        snprintf(wrong, sizeof(wrong), "%lld before, %lld after", before, after);
    check("the JIT checked accesses against the last access's entry", before >= 0 && after > before, wrong);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
