#ifndef ASBESTOS_H
#define ASBESTOS_H
#include "misc.h"
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"

#define FIBER_INITIAL_HASH_SIZE (1 << 10)
#define FIBER_CACHE_SIZE (1 << 12)  // 4096 entries
#define FIBER_PAGE_HASH_SIZE (1 << 14)  // ~4-5 blocks per bucket on big programs; invalidation walks a bucket

struct asbestos {
    // there is one asbestos per address space
    struct mmu *mmu;
    size_t mem_used;
    size_t num_blocks;

    struct list *hash;
    size_t hash_size;

    // list of fiber_blocks that should be freed soon (at the next RCU grace
    // period, if we had such a thing)
    struct list jetsam;

    // A way to look up blocks in a page
    struct {
        struct list blocks[2];
    } *page_hash;

    // A new value (asbestos_next_gen) on every block invalidation: a thread
    // that sees it empties its ret_cache, whose entries point into blocks'
    // code. Its block cache stays: an entry of a block invalidated since
    // (is_jetsam) is passed over where it is read, the run loop's lookup and
    // native code's alike.
    unsigned invalidate_gen;

    // A new value whenever jetsam'd blocks are freed (jetsam_lock held to
    // write, so no thread is running): a thread that held no read lock
    // meanwhile may have them in its block cache, and empties it. Never
    // another asbestos's value, so a cache of another's is emptied too.
    unsigned free_gen;

    // Number of threads currently inside cpu_run_to_interrupt.
    // When 1, we can skip jetsam_lock (no other thread to synchronize with).
    unsigned active_threads;

    // === RCU-like Optimization ===
    // Atomic counter: number of threads currently executing JIT code.
    // Jetsam cleanup waits until this reaches 0 before freeing blocks.
    // This avoids read lock overhead on every JIT enter/exit.
    _Atomic unsigned jit_active_threads;

    // Generation number for jetsam cleanup. Incremented when jetsam list
    // becomes non-empty. Threads check this to know when cleanup is needed.
    _Atomic unsigned jetsam_gen;

    lock_t lock;
    wrlock_t jetsam_lock;
#ifdef ISH_JIT
    void *jit_ctxs;             // guest-arm64/jit.c: per-module contexts of this address space
    struct list jit_all;        // in asbestos.c's list of every asbestos, for a flush of the code region
#endif
    // When a thread last left translated code (cpu_run_to_interrupt), in ms
    // of CLOCK_MONOTONIC: how long the address space has been idle, for
    // asbestos_reclaim_idle
    _Atomic uint64_t last_active_ms;

    // The address spaces using it. A forked child uses its parent's (mm_copy)
    // while the code both run is the same; one whose code comes to differ
    // takes a new one of its own (mem_asbestos_detach). Freed with the last.
    _Atomic unsigned refs;
    // It has had a block of a page that is writable or anonymous (code a JIT
    // wrote, or that may change): not given to a child, whose such pages may
    // come to differ unseen, and whose parent would then have to leave it
    bool private_content;
    // It has been shared: its blocks may be of other code than the address
    // space looking one up has there (a page another mapped since), so each
    // is looked up checked (fiber_block->tag)
    bool ever_shared;
    // The run loop looks blocks up without the lock after a block cache miss
    // (fiber_lookup_unlocked): odd while hash is being resized, and the
    // arrays a resize replaced, freed where jetsam blocks are (no thread
    // running then, so none reading them)
    _Atomic unsigned hash_seq;
    struct list *retired_hash;
};

// this is roughly the average number of instructions in a basic block according to anonymous sources
// times 4, roughly the average number of gadgets/parameters in an instruction, according to anonymous sources
#define FIBER_BLOCK_INITIAL_CAPACITY 16

struct fiber_block {
#ifdef ISH_JIT
    // Hot translation (guest-arm64/jit.c, jit_hot_runs): a block no image and
    // no translation of another's gave native code as it was compiled runs as
    // gadgets, code[0]'s gadget gadget_jit_count's. That counts its runs
    // (jit_runs) and goes on to the gadget it stands in for (jit_first); at
    // jit_hot_runs the run loop translates the block (INT_JIT_HOT,
    // asbestos.c's fiber_block_hot), and code[0] is native code or jit_first
    // again. jit_first is kept: a thread may be in gadget_jit_count still.
    // First, before addr: code's distance from addr is what the gadgets'
    // loads of addr through _pc reach (ldur, at most 256 bytes back).
    unsigned long jit_first;
    uint32_t jit_runs;
#endif
    addr_t addr;
    addr_t end_addr;
    size_t used;

    // pointers to the ip values in the last gadget
    unsigned long *jump_ip[2];
    // original values of *jump_ip[]
    unsigned long old_jump_ip[2];
    // blocks that jump to this block
    struct list jumps_from[2];

    // hashtable bucket links
    struct list chain;
    // list of blocks in a page
    struct list page[2];
    // links for jumps_from
    struct list jumps_from_links[2];
    // links for free list
    struct list jetsam;
    bool is_jetsam;
    // What code it was translated from (mmu_code_tag, of both its pages): 0,
    // the address space's own; CODE_TAG_EARLY, what all using its asbestos
    // have; else bytes of a file only some may have there
    uint64_t tag;

#ifdef ISH_JIT
    // Native code (guest-arm64/jit.c). native_link[i]: the patchable
    // instruction in the native exit for jump_ip[i] (nop, or a direct branch
    // into the chained successor); native_link_orig[i]: what a link replaced.
    uint32_t *native_link[2];
    uint32_t native_link_orig[2];
    uint32_t *native_loop;      // self-loop block: head of the promoted loop body
    uintptr_t native_entry;     // hot entry of the native code at code[0], 0 if none
    void *jit_ctx;              // PIC: module context the native code runs with (x29)
    unsigned jit_idx;           // PIC: its translation's slot in that context
#endif

    unsigned long code[];
};

// High-bit tracing: detect emulation bugs that leave bits 32+ dirty in guest regs
extern volatile bool g_trace_highbits;

// Create a new asbestos
struct asbestos *asbestos_new(struct mmu *mmu);
void asbestos_free(struct asbestos *asbestos);
// Another address space uses it (a forked child), or one leaves it; the last
// frees it, when none of its threads runs its code (their mem locks held to
// write, or they are gone)
struct asbestos *asbestos_retain(struct asbestos *asbestos);
void asbestos_release(struct asbestos *asbestos);
// More than one address space uses it
bool asbestos_shared(struct asbestos *asbestos);
// An address space leaves it if another uses it still: never the last, so
// never freeing it (a thread leaving it may be running its code, holding its
// jetsam_lock, which freeing takes). False, and nothing done, if none other
// does.
bool asbestos_leave_shared(struct asbestos *asbestos);
// Frees every translated block of an address space none of whose threads is
// running translated code, nor has for idle_ms: what memory pressure can take
// back from an idle process, which translates its blocks again when it runs.
// Nothing if a thread is in its code, or came in meanwhile. Says how many
// blocks, and the bytes malloc held for them.
void asbestos_reclaim_idle(struct asbestos *asbestos, uint64_t now_ms, uint64_t idle_ms, size_t *blocks,
                           size_t *bytes);
// Every address space's translated blocks, and the bytes malloc holds for them
extern _Atomic uint64_t st_fiber_blocks, st_fiber_bytes;

#ifdef ISH_JIT
// The JIT's code region full (guest-arm64/jit.h's jit_flush_*): every block of
// every address space invalidated, and the region taken from its start once
// no thread can be running what was in it. Called by a thread between its run
// loop's passes (task_run_current), holding no lock; one caller does it, the
// others find nothing to do.
void asbestos_jit_flush(void);
#endif

// Invalidate all fiber blocks in pages start (inclusive) to end (exclusive).
// Locks the asbestos. Should only be called by memory.c in conjunction with
// mem_changed.
void asbestos_invalidate_range(struct asbestos *asbestos, page_t start, page_t end);
void asbestos_invalidate_page(struct asbestos *asbestos, page_t page);
void asbestos_invalidate_all(struct asbestos *asbestos);

#endif
