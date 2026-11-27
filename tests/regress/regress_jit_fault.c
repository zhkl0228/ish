// Regression test for host faults in translated guest code.
//
// A guest access through a TLB entry whose host memory has gone faults on the
// host. In a gadget the run loop retries the access. In native code (ish built
// with -Djit=true) such a fault retried the block the run loop had entered:
// what ran before the access ran again, so a post-index load before it
// advanced its base twice. It now goes on at the access's TLB-miss exit, which
// runs the access again as a gadget; also when the access's check reused the
// last access's TLB entry, failed, and took the full lookup out of line,
// whose miss goes to that exit. A fault anywhere else in native code has
// nothing to retry from: the process is killed with SIGSEGV and iSH logs a
// sample.
//
//   ISH_JIT_FAULT_TEST=resume  regress_jit_fault resume
//   ISH_JIT_FAULT_TEST=unknown regress_jit_fault unknown
//
// madvise with the advice HOST_FAULT_TEST_ADVICE takes the host memory behind
// a page away and leaves the TLB as it is (asbestos/guest-arm64/host_fault.c).
// resume checks that the probe's result is right, and, where ish translates
// to native code ("mode: JIT" in /proc/ish/jit), that the fault was there.
// unknown needs native code. Without the madvise (natively, on x86, without
// ISH_JIT_FAULT_TEST) or native code for unknown it prints "SKIP ..." and
// exits 0.
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

#define HOST_FAULT_TEST_ADVICE 0x4a4954
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

// /proc/ish/jit: whether ish translates to native code, and its counts of
// host faults there (0 without them)
static int native;
static unsigned long long resumed, elsewhere;
static void read_jit(void) {
    resumed = elsewhere = 0;
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "mode: JIT", 9) == 0)
            native = 1;
        sscanf(line, "host faults in native code: %llu went on at the access's TLB-miss exit, %llu elsewhere",
               &resumed, &elsewhere);
    }
    fclose(f);
}

// A post-index load, then a load from trap: the one that faults.
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

// Through one register: a load of the page after the trap, then of the
// trap's last word. Its check of the first's TLB entry fails, and the full
// lookup out of line finds the trap's entry (the TLB still holds it): the
// access faults after a check whose branch goes to that lookup.
__attribute__((noinline)) static uint64_t probe_reused(volatile uint64_t *after) {
    uint64_t a, b;
    __asm__ volatile("ldr %0, [%2]\n\t"
                     "ldur %1, [%2, #-8]\n\t"
                     : "=&r"(a), "=&r"(b)
                     : "r"(after)
                     : "memory");
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
    return madvise((void *) trap, TRAP_SIZE, HOST_FAULT_TEST_ADVICE) == 0 ? 0 : errno;
}

// The same for probe_reused
static int arm_reused(volatile uint64_t *trap) {
    for (int i = 0; i < 100; i++)
        probe_reused(trap + TRAP_SIZE / 8);
    return madvise((void *) trap, TRAP_SIZE, HOST_FAULT_TEST_ADVICE) == 0 ? 0 : errno;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "resume";
    char detail[200];
    read_jit();
    unsigned long long resumed0 = resumed, elsewhere0 = elsewhere;
    volatile uint64_t *trap = mmap(NULL, 4 * TRAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    trap[0] = 0x1122334455667788;
    trap[TRAP_SIZE / 8 - 1] = 0x0102030405060708;   // the trap's last word
    trap[TRAP_SIZE / 8] = 0x1000;                    // the next page's first
    uint64_t *cursor = src;

    if (strcmp(mode, "resume") == 0) {
        int err = arm(trap);
        if (err != 0) {
            printf("SKIP madvise(HOST_FAULT_TEST_ADVICE): %s (not iSH on arm64, or ISH_JIT_FAULT_TEST not set)\n",
                   strerror(err));
            return 0;
        }
        uint64_t sum = probe(&cursor, trap);
        read_jit();
        snprintf(detail, sizeof(detail), "base advanced %ld bytes, sum %#llx; native code %s: faults went on %llu, "
                 "elsewhere %llu", (long) ((char *) cursor - (char *) src), (unsigned long long) sum,
                 native ? "yes" : "no", resumed - resumed0, elsewhere - elsewhere0);
        // with native code the fault is its own; else a gadget's
        check(native ? "a fault at an access in native code goes on there: the load before it runs once"
                     : "a fault at an access in a gadget is retried there",
              cursor == src + 1 && sum == 0x100 + 0x1122334455667788 && resumed - resumed0 == (native ? 1u : 0u) &&
              elsewhere == elsewhere0, detail);

        resumed0 = resumed;
        elsewhere0 = elsewhere;
        err = arm_reused(trap);
        if (err != 0) {
            snprintf(detail, sizeof(detail), "madvise again: %s", strerror(err));
            check("a fault after a check of the last access's entry and the full lookup out of line", 0, detail);
        } else {
            sum = probe_reused(trap + TRAP_SIZE / 8);
            read_jit();
            snprintf(detail, sizeof(detail), "sum %#llx; native code %s: faults went on %llu, elsewhere %llu",
                     (unsigned long long) sum, native ? "yes" : "no", resumed - resumed0, elsewhere - elsewhere0);
            check(native ? "a fault after a check of the last access's entry and the full lookup out of line goes "
                           "on at the access's exit"
                         : "a fault at an access in a gadget is retried there, again",
                  sum == 0x1000 + 0x0102030405060708 && resumed - resumed0 == (native ? 1u : 0u) &&
                      elsewhere == elsewhere0, detail);
        }
    } else {
        if (!native) {
            printf("SKIP unknown needs native code (ish -Djit=true translating: \"mode: JIT\" in /proc/ish/jit)\n");
            return 0;
        }
        printf("a fault elsewhere in native code: the process is killed at it\n");
        fflush(stdout);
        signal(SIGSEGV, on_segv);
        int err = arm(trap);
        if (err != 0) {
            printf("SKIP madvise(HOST_FAULT_TEST_ADVICE): %s (ISH_JIT_FAULT_TEST=unknown not set?)\n", strerror(err));
            return 0;
        }
        probe(&cursor, trap);
        read_jit();
        snprintf(detail, sizeof(detail), "the process went on: faults went on %llu, elsewhere %llu",
                 resumed - resumed0, elsewhere - elsewhere0);
        check("a fault elsewhere in native code: SIGSEGV, no retry", 0, detail);
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
