// Intel standard interrupts
// Any interrupt not handled specially becomes a SIGSEGV
#define INT_NONE -1
#define INT_DIV 0
#define INT_DEBUG 1
#define INT_NMI 2
#define INT_BREAKPOINT 3
#define INT_OVERFLOW 4
#define INT_BOUND 5
#define INT_UNDEFINED 6
#define INT_FPU 7 // do not try to use the fpu. instead, try to realize the truth: there is no fpu.
#define INT_DOUBLE 8 // interrupt during interrupt, i.e. interruptception
#define INT_GPF 13
#define INT_TIMER 32
#define INT_SYSCALL 0x80
#define INT_JIT_CRASH 0x100  // JIT SIGSEGV recovery (stale TLB after CoW)
// Its address space is to leave the asbestos it shares with others before it
// runs or makes more blocks (handle_interrupt: mem_asbestos_detach)
#define INT_ASBESTOS_DETACH 0x102
// So, for a block it found was translated from other code than its address
// space has there (another's mapping)
#define INT_ASBESTOS_DIFFERS 0x103
// A block left to be translated when hot has run jit_hot_runs times as gadgets
// (gadget_jit_count): the run loop translates it at cpu->pc, its start, before
// it runs again (asbestos.c, fiber_block_hot). gadgets.h has it too.
#define INT_JIT_HOT 0x104
// A host fault in native code with no position to retry from: the process is
// killed (asbestos/guest-arm64/jit.c, jit_native_fault())
#define INT_JIT_NATIVE_FAULT 0x101
#define INT_PREBUILT_RET 0x200  // nested guest call from a prebuilt-gadget spec_fn
                                 // returned to its sentinel LR (ARM64 only)
