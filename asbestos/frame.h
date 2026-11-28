#include <stdatomic.h>
#include "emu/cpu.h"

// keep in sync with asm
#define FIBER_RETURN_CACHE_SIZE 4096
#define FIBER_RETURN_CACHE_HASH(x) ((x) & 0xFFF0) >> 4)

struct fiber_frame {
    struct cpu_state cpu;
    void *bp;
    addr_t value_addr;
    uint64_t value[4]; // buffer for crosspage crap (must hold up to 32 bytes)
    struct fiber_block *last_block;
    uint64_t jit_exit_sp; // host sp value that fiber_exit expects (set by fiber_enter)
    // ARM64: code-stream pointer (_pc) at the last fiber_exit, so an INT_GPF
    // can be mapped back to the faulting guest instruction.
    uint64_t fault_stream;
    // JIT: the TLB entry of the last stack access's page, as its offset from
    // the TLB's entries (0, an entry, when the frame is new): a translated
    // segment's first access through sp checks that page first
    uint64_t sp_entry;
    // JIT: the calls translated code made with a host bl, so that the return
    // is a host ret the CPU predicts. ss[ss_top] the last: the guest return
    // address inverted (all zeroes, an empty entry, are no address's), the
    // host address the call returns to, and x29 there. Made after the
    // flushes' invalidations ss_epoch counts (jit_flush_invalidated()) and
    // the freeing of blocks ss_free_gen (asbestos->free_gen) says: emptied
    // after more (jit_frame_enter()), as the host code it returns to may be
    // other code's or a freed block's then
    uint32_t ss_top;
    unsigned ss_free_gen;
    uint64_t ss_epoch;
    struct { uint64_t ret_inv, host, ctx, pad; } ss[64];
    long ret_cache[FIBER_RETURN_CACHE_SIZE]; // a map of ip to pointer-to-call-gadget-arguments
};
