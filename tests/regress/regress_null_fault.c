// Regression test for faults on unmapped memory near address 0.
//
// A read of unmapped memory raised no SIGSEGV in the arm64 guest: a recovery
// in the fault handler set the load's destination to 0 and stepped over it
// (up to 8 times per address). HotSpot's interpreter checks for null by
// loading the class of the receiver and catching the SIGSEGV; reading 0
// instead, it went on with a null class and jumped into the bytes of its own
// code on the second NullPointerException at a call site. Go's nil checks work
// the same way.
//
// Reads and writes at and just above 0 must each raise SIGSEGV with si_addr
// set to the address and the PC at the faulting instruction, every time
// (more often than the recovery allowed). Running natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O1 -o regress_null_fault regress_null_fault.c
// Run:   ish -r <rootfs> /tmp/regress_null_fault

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <ucontext.h>

static int passed, failed;
static sigjmp_buf fault_jmp;
static void *fault_addr;
static uint64_t fault_pc;

static void on_segv(int sig, siginfo_t *info, void *context) {
    (void) sig;
    fault_addr = info->si_addr;
    fault_pc = ((ucontext_t *) context)->uc_mcontext.pc;
    siglongjmp(fault_jmp, 1);
}

extern char load_insn[], store_insn[];

static void load(uintptr_t addr) {
    uint64_t value;
    __asm__ volatile(".globl load_insn\nload_insn: ldr %0, [%1]" : "=r"(value) : "r"(addr) : "memory");
}

static void store(uintptr_t addr) {
    __asm__ volatile(".globl store_insn\nstore_insn: str xzr, [%0]" : : "r"(addr) : "memory");
}

static void check_fault(const char *what, void (*access)(uintptr_t), const char *insn, uintptr_t addr) {
    int ok = 0;
    if (sigsetjmp(fault_jmp, 1) == 0) {
        access(addr);
        printf("FAIL %s %#lx: no SIGSEGV\n", what, (unsigned long) addr);
    } else if (fault_addr != (void *) addr) {
        printf("FAIL %s %#lx: si_addr %p\n", what, (unsigned long) addr, fault_addr);
    } else if (fault_pc != (uint64_t) insn) {
        printf("FAIL %s %#lx: pc %#llx, expected %p\n", what, (unsigned long) addr, (unsigned long long) fault_pc, insn);
    } else {
        ok = 1;
    }
    if (ok)
        passed++;
    else
        failed++;
}

int main(void) {
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO};
    sigaction(SIGSEGV, &sa, NULL);
    static const uintptr_t addrs[] = {0x0, 0x8, 0xff8, 0x1000};
    for (int round = 0; round < 10; round++) {
        for (unsigned i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
            check_fault("read", load, load_insn, addrs[i]);
            check_fault("write", store, store_insn, addrs[i]);
        }
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
