// Regression test for the JIT's TLB check of the last stack access's entry
// (ish -Djit=true translating; run.sh runs it position-independent, as on
// iOS; skips otherwise).
//
// A translated segment's first access through sp (or one after another
// register's) checks the page of the TLB entry the last stack access left,
// across segments: the frame's sp_entry, which the full lookup of an sp
// access stores when that check fails. Before, it hashed its address to an
// entry every time.
//
// Checked, the probes on stacks of their own, run over and over (hot:
// translated):
// - pushes and pops each the first access of its block, sp going down across
//   pages and back, a store through another register between: every value
//   popped is the one pushed, and every word stored;
// - two threads doing it at once, each on its own stack;
// - a push onto a stack page made read-only once hot: SIGSEGV at the push
//   (on the signal stack), the page as it was; a pop from one made
//   PROT_NONE: SIGSEGV there;
// and that the JIT made such checks (/proc/ish/jit).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_sp_entry regress_jit_sp_entry.c -lpthread

#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096
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

// probe_stack(top, n, out): on the stack at top, push n pairs {k, k ^ key}
// (k = n..1), then pop them, storing each k through x2 (out[0..n-1]: 1..n)
// and summing k ^ key: the sum. Every push and pop is the first access of
// its block (cbz xzr ends one: the gadgets follow a b within its page).
// probe_push(top) and probe_pop(top): one push, one pop.
__asm__(".text\n"
        ".p2align 2\n"
        ".global probe_stack\n"
        "probe_stack:\n"
        "    mov x9, sp\n"
        "    mov sp, x0\n"
        "    mov x12, #0x5a5a\n"
        "    mov x10, x1\n"
        "1:  eor x11, x10, x12\n"
        "    stp x10, x11, [sp, #-16]!\n"
        "    cbz xzr, 2f\n"
        "2:  subs x10, x10, #1\n"
        "    b.ne 1b\n"
        "    mov x0, #0\n"
        "3:  ldp x10, x11, [sp], #16\n"
        "    cbz xzr, 4f\n"
        "4:  str x10, [x2], #8\n"
        "    ldr x13, [sp, #-8]\n"
        "    add x0, x0, x13\n"
        "    subs x1, x1, #1\n"
        "    b.ne 3b\n"
        "    mov sp, x9\n"
        "    ret\n"
        ".global probe_push\n"
        "probe_push:\n"
        "    mov x9, sp\n"
        "    mov sp, x0\n"
        "    cbz xzr, 1f\n"
        "1:  stp x9, x9, [sp, #-16]!\n"
        "    mov sp, x9\n"
        "    ret\n"
        ".global probe_pop\n"
        "probe_pop:\n"
        "    mov x9, sp\n"
        "    mov sp, x0\n"
        "    cbz xzr, 1f\n"
        "1:  ldp x0, x1, [sp], #16\n"
        "    mov sp, x9\n"
        "    ret\n");
uint64_t probe_stack(void *top, uint64_t n, uint64_t *out);
void probe_push(void *top);
uint64_t probe_pop(void *top);

// /proc/ish/jit: whether the JIT translates, and its count of checks of the
// last stack access's entry; -1 if it does not translate, -2 if it does and
// says no count
static long long sp_checks(void) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL)
        return -1;
    char line[512];
    int jit = 0;
    long long n = -2;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strcmp(line, "mode: JIT\n") == 0 || strcmp(line, "mode: JIT, position-independent\n") == 0)
            jit = 1;
        sscanf(line, "TLB checks of the last stack access's entry (an access through sp, from another segment), "
                     "translated: %lld", &n);
    }
    fclose(f);
    return jit ? n : -1;
}

#define STACK_PAGES 4
#define DEPTH 600   // 9600 bytes of pushes: down across two pages and back

// One run of probe_stack on the stack at top: whether it was right
static int stack_right(uint8_t *top, char *wrong, size_t size, int round) {
    static __thread uint64_t out[DEPTH];
    memset(out, 0, sizeof(out));
    uint64_t sum = probe_stack(top, DEPTH, out), want = 0;
    for (uint64_t k = 1; k <= DEPTH; k++)
        want += k ^ 0x5a5a;
    for (uint64_t i = 0; i < DEPTH; i++)
        if (out[i] != i + 1) {
            snprintf(wrong, size, "round %d: pop %llu gave %llu, want %llu", round, (unsigned long long) i,
                     (unsigned long long) out[i], (unsigned long long) (i + 1));
            return 0;
        }
    if (sum != want) {
        snprintf(wrong, size, "round %d: sum %llu, want %llu", round, (unsigned long long) sum,
                 (unsigned long long) want);
        return 0;
    }
    return 1;
}

static uint8_t *new_stack(void) {
    uint8_t *s = mmap(NULL, STACK_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return s == MAP_FAILED ? NULL : s;
}

struct worker {
    uint8_t *stack;
    int ok;
    char wrong[200];
};
static void *worker(void *arg) {
    struct worker *w = arg;
    w->ok = 1;
    // a top at a different offset into its page each round
    for (int r = 0; r < ROUNDS && w->ok; r++)
        w->ok = stack_right(w->stack + STACK_PAGES * PAGE - 16 * (r % 200), w->wrong, sizeof(w->wrong), r);
    return NULL;
}

static sigjmp_buf fault_env;
static void *volatile fault_addr;
static void on_segv(int sig, siginfo_t *info, void *uc) {
    (void) sig;
    (void) uc;
    fault_addr = info->si_addr;
    siglongjmp(fault_env, 1);
}

int main(void) {
    long long before = sp_checks();
    if (before == -1) {
        printf("SKIP the JIT does not translate here (ish -Djit=true with a code region)\n");
        return 0;
    }
    char wrong[420] = "";

    struct worker one = {new_stack()};
    if (one.stack == NULL) {
        perror("mmap");
        return 1;
    }
    worker(&one);
    check("pushes and pops each first in its block, across pages and back: every value right", one.ok, one.wrong);

    struct worker two[2] = {{new_stack()}, {new_stack()}};
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        if (two[i].stack == NULL || pthread_create(&t[i], NULL, worker, &two[i]) != 0) {
            perror("thread");
            return 1;
        }
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    snprintf(wrong, sizeof(wrong), "%s / %s", two[0].ok ? "right" : two[0].wrong, two[1].ok ? "right" : two[1].wrong);
    check("two threads at once, each on its own stack: every value right", two[0].ok && two[1].ok, wrong);

    // The faults: the handler on a signal stack of its own (sp is on the page)
    stack_t ss = {.ss_sp = malloc(64 * 1024), .ss_size = 64 * 1024};
    if (ss.ss_sp == NULL || sigaltstack(&ss, NULL) != 0) {
        perror("sigaltstack");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    uint8_t *ro = new_stack();
    if (ro == NULL) {
        perror("mmap");
        return 1;
    }
    uint8_t *top = ro + 2 * PAGE + 64;   // pushes 16 below: on page 2
    for (int i = 0; i < ROUNDS; i++)
        probe_push(top);
    memset(ro + 2 * PAGE, 0xa5, PAGE);
    mprotect(ro + 2 * PAGE, PAGE, PROT_READ);
    fault_addr = NULL;
    int faulted = 0;
    if (sigsetjmp(fault_env, 1) == 0)
        probe_push(top);
    else
        faulted = 1;
    int kept = 1;
    for (int i = 0; i < PAGE; i++)
        kept &= ro[2 * PAGE + i] == 0xa5;
    snprintf(wrong, sizeof(wrong), "%s at %p (want %p), the page %s", faulted ? "SIGSEGV" : "no fault", fault_addr,
             (void *) (top - 16), kept ? "as it was" : "written");
    check("a push onto a stack page made read-only once hot: SIGSEGV there, nothing written",
          faulted && fault_addr == top - 16 && kept, wrong);

    uint8_t *none = new_stack();
    if (none == NULL) {
        perror("mmap");
        return 1;
    }
    top = none + 2 * PAGE + 64;
    for (int i = 0; i < ROUNDS; i++)
        probe_pop(top);
    mprotect(none + 2 * PAGE, PAGE, PROT_NONE);
    fault_addr = NULL;
    faulted = 0;
    if (sigsetjmp(fault_env, 1) == 0)
        probe_pop(top);
    else
        faulted = 1;
    snprintf(wrong, sizeof(wrong), "%s at %p (want %p)", faulted ? "SIGSEGV" : "no fault", fault_addr, (void *) top);
    check("a pop from a stack page made PROT_NONE once hot: SIGSEGV there", faulted && fault_addr == top, wrong);

    long long after = sp_checks();
    if (after == -2)
        snprintf(wrong, sizeof(wrong), "/proc/ish/jit says no count of them");
    else
        snprintf(wrong, sizeof(wrong), "%lld before, %lld after", before, after);
    check("the JIT checked sp accesses against the last stack access's entry", before >= 0 && after > before, wrong);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
