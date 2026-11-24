// Regression test for the ESR a fault's SIGSEGV frame has (arm64).
//
// Linux puts the fault's ESR in the signal frame, an esr_context record in
// uc_mcontext.__reserved after the FPSIMD context; a handler reads whether a
// store faulted from it. iSH put none: its own JIT's host fault handler, run
// in iSH, said "no esr_context in the signal frame".
//
// Checked, in this order (Linux keeps a fault's ESR for a later SIGSEGV, so
// the kill comes first): a SIGSEGV sent by kill has no ESR; a load from an
// unmapped page has one of a data abort (EC 0x24), not a write, a translation
// fault (DFSC 0x07 here; its level is the hardware's natively); a store to a
// read-only page one of a data abort, a write (WnR), a permission fault.
// Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sigsegv_esr regress_sigsegv_esr.c

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define ESR_RECORD_MAGIC 0x45535201

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

static sigjmp_buf back;
static volatile int have_esr;
static volatile uint64_t esr;

// the records in __reserved: magic, size, then their data; magic 0 ends them
static void on_segv(int sig, siginfo_t *info, void *context) {
    (void) sig;
    (void) info;
    ucontext_t *uc = context;
    unsigned char *at = (unsigned char *) uc->uc_mcontext.__reserved;
    unsigned char *end = at + sizeof(uc->uc_mcontext.__reserved);
    have_esr = 0;
    while (at + 8 <= end) {
        uint32_t magic, size;
        memcpy(&magic, at, 4);
        memcpy(&size, at + 4, 4);
        if (magic == 0 || size == 0)
            break;
        if (magic == ESR_RECORD_MAGIC) {
            uint64_t value;
            memcpy(&value, at + 8, 8);
            esr = value;
            have_esr = 1;
        }
        at += size;
    }
    siglongjmp(back, 1);
}

static void describe(char *detail, size_t size) {
    snprintf(detail, size, "esr record %s, esr %#llx: EC %#llx, WnR %llu, FSC %#llx", have_esr ? "there" : "none",
             (unsigned long long) esr, (unsigned long long) (esr >> 26 & 0x3f), (unsigned long long) (esr >> 6 & 1),
             (unsigned long long) (esr & 0x3f));
}

int main(void) {
    char detail[200];
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    if (sigsetjmp(back, 1) == 0)
        kill(getpid(), SIGSEGV);
    describe(detail, sizeof(detail));
    check("a SIGSEGV sent by kill has no ESR", !have_esr, detail);

    char *none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(none, 4096);
    if (sigsetjmp(back, 1) == 0)
        (void) *(volatile char *) none;
    describe(detail, sizeof(detail));
    check("a load from an unmapped page: a data abort, a read, a translation fault",
          have_esr && (esr >> 26 & 0x3f) == 0x24 && !(esr >> 6 & 1) && (esr & 0x3c) == 0x04, detail);

    char *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sigsetjmp(back, 1) == 0)
        *(volatile char *) ro = 1;
    describe(detail, sizeof(detail));
    check("a store to a read-only page: a data abort, a write, a permission fault",
          have_esr && (esr >> 26 & 0x3f) == 0x24 && (esr >> 6 & 1) && (esr & 0x3c) == 0x0c, detail);
    munmap(ro, 4096);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
