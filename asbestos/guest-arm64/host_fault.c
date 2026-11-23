// Host faults (SIGSEGV, SIGBUS) in translated guest code.
//
// A guest access through a TLB entry whose host memory has gone faults on the
// host, in a gadget or in native code. The handler sends the thread back to
// the run loop through jit_crash_trampoline (INT_JIT_CRASH), which retries the
// access at the position fiber_exit records; native code at a guest access
// goes on at the access's TLB-miss exit instead (jit_native_fault()). Both
// the command line ish (main.c's crash handler) and the app
// (asbestos_install_host_fault_handler()) take faults here.

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#include <asm/sigcontext.h>
#endif
#include "asbestos/frame.h"
#include "asbestos/guest-arm64/host_fault.h"
#include "emu/cpu.h"
#include "emu/interrupt.h"
#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
#endif

// The fields the handler writes, by the structs' own offsets.
//
// These were once hand-copied integer literals, and three of the four had
// drifted from the structs they describe (segfault_addr 832 vs 840,
// segfault_was_write 840 vs 848, jit_exit_sp 920 vs 936). The jit_exit_sp error
// was the damaging one: offset 920 lands on `fiber_frame.value[3]`, the
// crosspage scratch buffer, so recovery restored SP from leftover guest data
// (or 0) and `fiber_exit`'s first instruction — `ldr lr, [sp, #0x60]` —
// faulted: the `fiber_exit + 0` <- `handle_miss_… + 4` crash signature,
// including the literal `KERN_INVALID_ADDRESS at 0x60` when the buffer was 0.
//
// The gadgets reach the same fields through cpu-offsets.h, which meson
// generates from these structs; the asserts tie the two views together.
#include <assert.h>
#include "cpu-offsets.h"
static_assert(offsetof(struct cpu_state, segfault_addr) == CPU_segfault_addr, "cpu-offsets drift: segfault_addr");
static_assert(offsetof(struct cpu_state, segfault_was_write) == CPU_segfault_was_write,
              "cpu-offsets drift: segfault_was_write");
static_assert(offsetof(struct fiber_frame, jit_exit_sp) == LOCAL_jit_exit_sp, "cpu-offsets drift: jit_exit_sp");

// Set while a thread runs guest code, and the frame it runs on (asbestos.c).
extern __thread volatile sig_atomic_t in_jit;
extern __thread void *volatile jit_frame;
// For handle_interrupt's report of the fault (kernel/calls.c).
extern __thread volatile uint64_t jit_last_host_fault;
extern __thread volatile uint64_t jit_last_x7;
extern __thread volatile uint64_t jit_last_x10;
extern __thread volatile int jit_crash_count;
// Returns INT_JIT_CRASH through fiber_exit (entry.S).
extern void jit_crash_trampoline(void);

// Host registers in a signal context: Darwin keeps a pointer to the thread
// state, Linux embeds the registers in uc_mcontext.
#if __APPLE__
#define UC_X(uc, n) ((uc)->uc_mcontext->__ss.__x[n])
#define UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
static uint64_t uc_esr(ucontext_t *uc) {
    return uc->uc_mcontext->__es.__esr;
}
#else
#define UC_X(uc, n) ((uc)->uc_mcontext.regs[n])
#define UC_SP(uc) ((uc)->uc_mcontext.sp)
#define UC_PC(uc) ((uc)->uc_mcontext.pc)
// The kernel puts the ESR of a data abort in an esr_context record in the
// __reserved area, which is a list of records ending with a zero magic.
static uint64_t uc_esr(ucontext_t *uc) {
    struct _aarch64_ctx *head = (struct _aarch64_ctx *) (void *) uc->uc_mcontext.__reserved;
    while (head->magic != 0) {
        if (head->magic == ESR_MAGIC)
            return ((struct esr_context *) head)->esr;
        head = (struct _aarch64_ctx *) (void *) ((char *) head + head->size);
    }
    static const char msg[] = "crash_handler: no esr_context in the signal frame\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(139);
}
#endif

// ISH_JIT_FAULT_TEST=resume|unknown: madvise(addr, len, HOST_FAULT_TEST_ADVICE)
// takes the host memory behind guest [addr, addr + len) away and leaves the
// TLB as it is, so that the next access through the TLB faults on the host,
// in a gadget or in native code; the fault gives it back. With "unknown" a
// fault in native code is taken for one not at a guest access.
static void *volatile fault_test_host;
static volatile size_t fault_test_len;
static volatile bool fault_test_unknown;

bool host_fault_test_arm(void *host, size_t len) {
    const char *mode = getenv("ISH_JIT_FAULT_TEST");
    if (mode == NULL || (strcmp(mode, "resume") != 0 && strcmp(mode, "unknown") != 0) || fault_test_host != NULL)
        return false;
    if (mprotect(host, len, PROT_NONE) < 0)
        return false;
    fault_test_unknown = strcmp(mode, "unknown") == 0;
    fault_test_len = len;
    fault_test_host = host;
    return true;
}

// Whether the fault at addr was the test's; its memory is back.
static bool fault_test_restore(uintptr_t addr) {
    void *host = fault_test_host;
    if (host == NULL || addr - (uintptr_t) host >= fault_test_len)
        return false;
    mprotect(host, fault_test_len, PROT_READ | PROT_WRITE);
    fault_test_host = NULL;
    return true;
}

bool asbestos_host_fault(int sig, siginfo_t *info, void *ucontext) {
    if ((sig != SIGSEGV && sig != SIGBUS) || !in_jit)
        return false;
    ucontext_t *uc = ucontext;
    // _cpu (x1) is the frame fiber_enter() runs on, in gadgets and native code
    // alike. In a C helper a gadget calls it holds an argument: a fault there
    // is iSH's own, and taking it for the guest's would write through x1.
    if (UC_X(uc, 1) != (uint64_t) (uintptr_t) jit_frame)
        return false;
    bool test = fault_test_restore((uintptr_t) info->si_addr);
#ifdef ISH_JIT
    // In native code: a fault at a guest access goes on at its TLB-miss
    // exit, as a miss there would. Anywhere else there is no position to
    // retry from, and the run loop kills the process; x28 is the code-stream
    // position only at exits, so it is cleared for fiber_exit.
    uintptr_t resume;
    enum jit_fault fault = jit_native_fault(uc, (uintptr_t) info->si_addr, test && fault_test_unknown, &resume);
    if (fault == JIT_FAULT_RESUME) {
        UC_PC(uc) = resume;
        return true;
    }
    if (fault == JIT_FAULT_UNKNOWN)
        UC_X(uc, 28) = 0;
#else
    (void) test;   // in gadgets it is retried as any other
#endif

    // _cpu is in x1 — pointer to cpu_state within fiber_frame
    uint64_t cpu_ptr = UC_X(uc, 1);

    // Reconstruct guest segfault_addr from registers.
    // x7 = _addr (host pointer = data_minus_addr + guest_addr)
    // x10 may hold data_minus_addr from TLB lookup (but only on TLB HIT path)
    uint64_t x7 = UC_X(uc, 7);
    uint64_t x10 = UC_X(uc, 10);
    uint64_t guest_addr = (x7 - x10) & 0xffffffffffffULL;

    // Store diagnostic info for handle_interrupt to read
    jit_last_host_fault = (uint64_t) info->si_addr;
    jit_last_x7 = x7;
    jit_last_x10 = x10;
    jit_crash_count++;

    // Determine read/write from host ESR. Bit 6 (WnR): 0=read, 1=write.
    uint64_t esr = uc_esr(uc);
    int was_write = (esr & 0x40) != 0;

    // Write crash info directly to cpu_state via _cpu pointer.
    // Each store must match the field's real width: segfault_was_write is a
    // `bool`, and the previous `*(int *)` store wrote 4 bytes over it and the
    // padding before `trapno`. The generated gadgets use `strb` for the same
    // field, so 1 byte is the correct width on both sides.
    *(addr_t *) (cpu_ptr + offsetof(struct cpu_state, segfault_addr)) = guest_addr;
    *(bool *) (cpu_ptr + offsetof(struct cpu_state, segfault_was_write)) = (bool) was_write;
    // cpu->pc stays the current block's start. x28 (_pc) is left as the
    // code-stream position of the fault, which fiber_exit records, so the
    // run loop retries from the faulting instruction.

    // Restore SP to the value saved by fiber_enter, so fiber_exit
    // can correctly pop the callee-saved register frame.
    UC_SP(uc) = *(uint64_t *) (cpu_ptr + offsetof(struct fiber_frame, jit_exit_sp));

    // Redirect execution to crash trampoline (returns INT_JIT_CRASH)
    UC_PC(uc) = (uint64_t) jit_crash_trampoline;

    // Unblock signal so it can fire again on next crash
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, sig);
    sigprocmask(SIG_UNBLOCK, &unblock, NULL);

    // Signal handler returns; execution resumes at jit_crash_trampoline
    return true;
}

static struct sigaction previous_segv, previous_bus;

static void host_fault_handler(int sig, siginfo_t *info, void *ucontext) {
    if (asbestos_host_fault(sig, info, ucontext))
        return;
    // Not guest code: the handler from before takes it. Put back, it gets the
    // fault when the access runs again on return, as if this one were never
    // there (by default: the crash and its report, at the faulting
    // instruction). A signal sent rather than a fault is raised again.
    sigaction(sig, sig == SIGBUS ? &previous_bus : &previous_segv, NULL);
    if (info->si_code <= 0)
        raise(sig);
}

void asbestos_install_host_fault_handler(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = host_fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &previous_segv);
    sigaction(SIGBUS, &sa, &previous_bus);
}
