// Regression test for host faults in the ARM64 JIT's native code.
//
// A host fault at a guest access in native code (its TLB entry valid, the host
// memory behind it not) retried the block the run loop had entered: what ran
// before the access ran again, so a post-index load before it advanced its
// base twice. It now goes on at the access's TLB-miss exit, which runs the
// access again as a gadget. A fault anywhere else in native code has nothing
// to retry from: the process is killed with SIGSEGV and iSH logs a sample.
//
//   ISH_JIT_FAULT_TEST=resume  regress_jit_fault resume
//   ISH_JIT_FAULT_TEST=unknown regress_jit_fault unknown
//
// The fault is made by ish built with -Djit=true (an Apple arm64 host):
// madvise with the advice JIT_FAULT_TEST_ADVICE takes the host memory behind a
// page away and leaves the TLB as it is, and /proc/ish/jit counts the faults.
// Without them (natively, without the JIT, or without ISH_JIT_FAULT_TEST) it
// prints "SKIP ..." and exits 0.
//
// resume prints one line per case and "N passed, M failed". unknown is killed
// at the fault: run.sh and tools/jit-test-mac.sh look for iSH's sample in the
// log, and it prints FAIL if it goes on past it.
//
// Between arming and the probe nothing may change the mappings: a change (a
// copy-on-write copy, as a forked child's first store makes; a page mapped on
// first use) flushes the TLB, the access then misses it and faults in its
// gadget instead, and native code never sees the fault. So everything else is
// done first.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_fault regress_jit_fault.c

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define JIT_FAULT_TEST_ADVICE 0x4a4954
#define TRAP_SIZE 16384 // a host page on Apple arm64, 4 guest pages

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

// /proc/ish/jit's counts of host faults in native code; -1 without them
static int jit_faults(unsigned long long *resumed, unsigned long long *elsewhere) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL)
        return -1;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "host faults in native code: %llu went on at the access's TLB-miss exit, %llu elsewhere",
                   resumed, elsewhere) == 2)
            found = 0;
    fclose(f);
    return found;
}

// A post-index load, then a load from trap: the one native code faults on.
__attribute__((noinline)) static uint64_t probe(uint64_t **cursor, volatile uint64_t *trap) {
    uint64_t *p = *cursor, a, b;
    __asm__ volatile("ldr %0, [%2], #8\n\t"
                     "ldr %1, [%3]\n\t"
                     : "=&r"(a), "=&r"(b), "+&r"(p)
                     : "r"(trap)
                     : "memory");
    *cursor = p;
    return a + b;
}

static uint64_t src[2] = {0x100, 0x200};

// iSH ends the process itself, which no handler sees: this one runs only if
// the fault came to the guest as a SIGSEGV of its own instead
static void on_segv(int sig) {
    (void) sig;
    static const char msg[] = "FAIL a fault elsewhere in native code: delivered to the guest as SIGSEGV\n";
    write(1, msg, sizeof(msg) - 1);
    _exit(3);
}

// Translated and in the TLB, then the trap's host memory taken away; 0 armed,
// else errno. The probe must follow at once (see the top).
static int arm(volatile uint64_t *trap) {
    for (int i = 0; i < 100; i++) {
        uint64_t *cursor = src;
        probe(&cursor, trap);
    }
    return madvise((void *) trap, TRAP_SIZE, JIT_FAULT_TEST_ADVICE) == 0 ? 0 : errno;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "resume";
    char detail[200];
    unsigned long long resumed0, elsewhere0, resumed1, elsewhere1;
    if (jit_faults(&resumed0, &elsewhere0) < 0) {
        printf("SKIP no /proc/ish/jit fault counts: not iSH with the native JIT\n");
        return 0;
    }
    volatile uint64_t *trap = mmap(NULL, 4 * TRAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    trap[0] = 0x1122334455667788;

    uint64_t *cursor = src;
    if (strcmp(mode, "resume") == 0) {
        int err = arm(trap);
        if (err != 0) {
            printf("SKIP madvise(JIT_FAULT_TEST_ADVICE): %s (ISH_JIT_FAULT_TEST=resume not set?)\n", strerror(err));
            return 0;
        }
        uint64_t sum = probe(&cursor, trap);
        jit_faults(&resumed1, &elsewhere1);
        snprintf(detail, sizeof(detail), "base advanced %ld bytes, sum %#llx, faults went on %llu, elsewhere %llu",
                 (long) ((char *) cursor - (char *) src), (unsigned long long) sum, resumed1 - resumed0,
                 elsewhere1 - elsewhere0);
        check("a fault at an access goes on there: the load before it runs once",
              cursor == src + 1 && sum == 0x100 + 0x1122334455667788 && resumed1 - resumed0 == 1 &&
              elsewhere1 == elsewhere0, detail);
    } else {
        printf("a fault elsewhere in native code: the process is killed at it\n");
        fflush(stdout);
        signal(SIGSEGV, on_segv);
        int err = arm(trap);
        if (err != 0) {
            printf("SKIP madvise(JIT_FAULT_TEST_ADVICE): %s (ISH_JIT_FAULT_TEST=unknown not set?)\n", strerror(err));
            return 0;
        }
        probe(&cursor, trap);
        jit_faults(&resumed1, &elsewhere1);
        snprintf(detail, sizeof(detail), "the process went on: faults went on %llu, elsewhere %llu",
                 resumed1 - resumed0, elsewhere1 - elsewhere0);
        check("a fault elsewhere in native code: SIGSEGV, no retry", 0, detail);
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
