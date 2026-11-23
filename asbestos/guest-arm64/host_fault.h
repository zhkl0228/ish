#ifndef ASBESTOS_ARM64_HOST_FAULT_H
#define ASBESTOS_ARM64_HOST_FAULT_H

// Host faults (SIGSEGV, SIGBUS) in translated guest code, the gadgets' and
// native code's (asbestos/guest-arm64/host_fault.c).

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

// From a SIGSEGV or SIGBUS handler: true if the fault was in guest code and
// is taken care of, the context set to go on (the handler then returns);
// false if it is a crash of iSH's own.
bool asbestos_host_fault(int sig, siginfo_t *info, void *ucontext);

// For a process with no handler of its own (the app; the command line ish
// calls asbestos_host_fault() from its crash handler): a SIGSEGV and SIGBUS
// handler for faults in guest code, which hands any other on to the handler
// installed before it (by default the system's, which writes a crash report).
// Call it before the first guest process runs.
void asbestos_install_host_fault_handler(void);

// ISH_JIT_FAULT_TEST=resume|unknown (tests/regress/regress_jit_fault.c):
// madvise with this advice takes the host memory behind a guest range away,
// the TLB left as it is, until an access faults on it. false: the test is
// off, or already armed.
#define HOST_FAULT_TEST_ADVICE 0x4a4954
bool host_fault_test_arm(void *host, size_t len);

#endif
