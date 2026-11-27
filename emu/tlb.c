#include "emu/cpu.h"
#include "emu/tlb.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "kernel/fs.h"
#include "util/sync.h"

static bool tlb_catch_up(struct tlb *tlb, struct mmu *mmu);

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    if (tlb->mmu == mmu && tlb->mem_changes == mmu->changes)
        return;
    // The changes it missed while not attached, if the log has them all:
    // their pages' entries go, the others stand
    if (tlb->mmu == mmu && tlb_catch_up(tlb, mmu))
        return;
    if (tlb->mmu != mmu) {
        // Address space changed (execve); block cache and ret_cache are invalid
        memset(tlb->block_cache, 0, sizeof(tlb->block_cache));
        tlb->block_cache_gen = 0;
        if (tlb->frame != NULL) {
            free(tlb->frame);
            tlb->frame = NULL;
        }
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = mmu->changes;
    tlb_flush(tlb);
}

void tlb_attach(struct tlb *tlb, struct mmu *mmu) {
    lock(&mmu->tlbs_lock);
    list_add(&mmu->tlbs, &tlb->tlbs_link);
    unlock(&mmu->tlbs_lock);
    tlb->attached = mmu;
    // On the list before the caller compares mmu->changes (tlb_refresh()): a
    // change that compare misses comes after the list_add, so it takes its
    // pages out of this TLB.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void tlb_detach(struct tlb *tlb) {
    struct mmu *mmu = tlb->attached;
    if (mmu == NULL)
        return;
    lock(&mmu->tlbs_lock);
    list_remove(&tlb->tlbs_link);
    unlock(&mmu->tlbs_lock);
    tlb->attached = NULL;
}

void mmu_poke_tlbs(struct mmu *mmu) {
    lock(&mmu->tlbs_lock);
    struct tlb *tlb;
    list_for_each_entry(&mmu->tlbs, tlb, tlbs_link) {
        struct cpu_state *cpu = tlb->cpu;
        // Attached just before its first quantum: cpu_run_to_interrupt has
        // not pointed poked_ptr at _poked yet, but will.
        if (cpu->poked_ptr != NULL)
            cpu_poke(cpu);
        else
            __atomic_store_n(&cpu->_poked, true, __ATOMIC_SEQ_CST);
    }
    unlock(&mmu->tlbs_lock);
}

// An entry of a page in [start, start + pages), read or write, emptied
static void tlb_entry_drop(struct tlb_entry *entry, page_t start, pages_t pages);

// Its entries of pages [start, start + pages) emptied
static void tlb_drop_pages(struct tlb *tlb, page_t start, pages_t pages) {
    if (pages >= TLB_SIZE) {
        for (unsigned i = 0; i < TLB_SIZE; i++)
            tlb_entry_drop(&tlb->entries[i], start, pages);
        return;
    }
    for (pages_t i = 0; i < pages; i++)
        tlb_entry_drop(&tlb->entries[TLB_INDEX((addr_t) (start + i) << PAGE_BITS)], start + i, 1);
}

static void tlb_entry_drop(struct tlb_entry *entry, page_t start, pages_t pages) {
    page_t r = __atomic_load_n(&entry->page, __ATOMIC_RELAXED);
    page_t w = __atomic_load_n(&entry->page_if_writable, __ATOMIC_RELAXED);
    // TLB_PAGE_EMPTY's page is above every 48-bit address's: never in range
    if ((r >> PAGE_BITS) - start >= pages && (w >> PAGE_BITS) - start >= pages)
        return;
    __atomic_store_n(&entry->page, TLB_PAGE_EMPTY, __ATOMIC_RELAXED);
    __atomic_store_n(&entry->page_if_writable, TLB_PAGE_EMPTY, __ATOMIC_RELAXED);
}

// After mmu->changes moved for pages [start, start + pages): the TLBs of the
// threads running guest code here lose their entries of those pages before
// the caller goes on, so that no access of theirs after one that saw what the
// caller stores next goes through the old mapping (a page made code: its
// writes come back through the miss path, W^X). Their inline lookups check
// nothing else. A TLB not attached is flushed as it comes back (tlb_refresh).
// A thread filling an entry meanwhile reads mmu->changes again after
// (tlb_handle_miss): of the two, one sees the other's.
static void mmu_tlbs_invalidate(struct mmu *mmu, page_t start, pages_t pages) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   // the caller's bump before the entries are read
    lock(&mmu->tlbs_lock);
    struct tlb *tlb;
    list_for_each_entry(&mmu->tlbs, tlb, tlbs_link)
        tlb_drop_pages(tlb, start, pages);
    unlock(&mmu->tlbs_lock);
}

// The count moves, the change is logged (its slot's seq marked busy with it
// while written, so that a reader takes it whole or not at all, and tells
// this change being written from a later one taking the slot), then the
// attached TLBs lose its pages: a TLB attached after its log slot is read
// gets them from there or from here
void mmu_changed(struct mmu *mmu, page_t start, pages_t pages) {
    uint64_t seq = __atomic_add_fetch(&mmu->changes, 1, __ATOMIC_SEQ_CST);
    struct mmu_change *c = &mmu->change_log[seq % MMU_CHANGE_LOG];
    __atomic_store_n(&c->seq, seq | MMU_CHANGE_BUSY, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    c->start = start;
    c->pages = pages;
    __atomic_store_n(&c->seq, seq, __ATOMIC_RELEASE);
    mmu_tlbs_invalidate(mmu, start, pages);
}

// A TLB attached again (on the list already, tlb_attach) after missing the
// changes since mem_changes: their pages' entries go, if the log still has
// them all, and true. A change not logged yet, or being logged, is one whose
// invalidation comes after this TLB was on the list: it reaches it there. A
// slot taken by a later change (too many since, or one taking it as it is
// read), or a change of more pages than the TLB has entries (a sweep each,
// as dear as a flush): false, flushed.
static bool tlb_catch_up(struct tlb *tlb, struct mmu *mmu) {
    uint64_t to = __atomic_load_n(&mmu->changes, __ATOMIC_ACQUIRE);
    uint64_t from = tlb->mem_changes;
    if (to - from > MMU_CHANGE_LOG)
        return false;
    for (uint64_t s = from + 1; s <= to; s++) {
        struct mmu_change *c = &mmu->change_log[s % MMU_CHANGE_LOG];
        uint64_t s1 = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
        page_t start = c->start;
        pages_t pages = c->pages;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint64_t s2 = __atomic_load_n(&c->seq, __ATOMIC_RELAXED);
        if ((s1 & ~MMU_CHANGE_BUSY) > s || (s2 & ~MMU_CHANGE_BUSY) > s)
            return false;   // taken by a later change
        if (s1 != s || s2 != s)
            continue;       // not logged yet, or being logged: its invalidation reaches this TLB
        if (pages >= TLB_SIZE)
            return false;
        tlb_drop_pages(tlb, start, pages);
    }
    tlb->mem_changes = (unsigned) to;
    return true;
}

void tlb_flush(struct tlb *tlb) {
    tlb->mem_changes = tlb->mmu->changes;
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = TLB_PAGE_EMPTY, .page_if_writable = TLB_PAGE_EMPTY};
}

void tlb_free(struct tlb *tlb) {
    if (tlb->frame != NULL)
        free(tlb->frame);
    free(tlb);
}

bool __tlb_read_cross_page(struct tlb *tlb, addr_t addr, char *value, unsigned size) {
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL)
        return false;
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL)
        return false;
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, addr_t addr, const char *value, unsigned size) {
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL)
        return false;
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL)
        return false;
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    return true;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, addr_t addr, int type) {
    // Seqlock-style translate: snapshot mmu->changes BEFORE translating, then
    // verify it did not advance across the translation. If it did, another
    // thread remapped a page concurrently and `ptr` may reference the stale
    // host backing (a partial munmap / CoW leaves the old page readable via
    // its lingering refcount, so the stale ptr silently reads wrong data).
    // Loop until we get a ptr and a generation that are mutually consistent,
    // so the gen we stamp on the entry truly matches the backing it points at.
    uint64_t gen;
    char *ptr;
    // A write miss on a CoW page performs the copy inside mmu_translate, which
    // itself bumps mmu->changes; the retry then sees a stable generation once
    // the page is resolved. Bound the loop so a thread continuously remapping
    // can't livelock us — after the cap we accept the latest consistent-enough
    // translation (the per-block coherence check will still catch a later
    // change).
    for (int attempt = 0; ; attempt++) {
        gen = __atomic_load_n(&tlb->mmu->changes, __ATOMIC_SEQ_CST);
        // Attached, the TLB has lost its entries of every page changed since
        // mem_changes: a change made with the address space's lock held to
        // read takes them out of the attached TLBs (mmu_tlbs_invalidate), one
        // held to write has none attached, and an attach after it takes its
        // pages out or flushes (tlb_refresh). Its other entries stand: flushing it whole at each
        // change cost java's threads, as it maps and protects, a 256 KB
        // TLB's refill. Not attached (a C access in a syscall), it may have
        // missed some: flushed.
        if (gen != tlb->mem_changes && tlb->attached == NULL)
            tlb_flush(tlb);
        tlb->mem_changes = gen;
        ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
        // Re-read after translate; if unchanged, ptr is consistent with `gen`.
        if (__atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE) == gen)
            break;
        if (attempt >= 8) {
            // Refresh gen to the post-translate value so the entry we stamp is
            // at least self-consistent with this last translation.
            gen = __atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE);
            tlb->mem_changes = gen;
            break;
        }
        // Raced with a concurrent remap — retry with the fresh generation.
    }
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }

    tlb->dirty_page = TLB_PAGE(addr);

    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    tlb_ent->page = TLB_PAGE(addr);
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);
    // Record the coherence generation this host pointer is valid for. A hit on
    // this entry is only trustworthy while mmu->changes still equals this; if
    // another thread remapped the page (CoW/mmap/munmap) mmu->changes advances
    // and the cached data_minus_addr points at the stale host backing.
    tlb_ent->gen = (uintptr_t) gen;

    if (type == MEM_WRITE) {
        tlb_ent->page_if_writable = TLB_PAGE(addr);
    } else {
        // On read miss, speculatively check if the page is also writable.
        if (tlb->mmu->ops->translate_write_nofault) {
            char *wptr = tlb->mmu->ops->translate_write_nofault(tlb->mmu, TLB_PAGE(addr));
            tlb_ent->page_if_writable = wptr ? TLB_PAGE(addr) : TLB_PAGE_EMPTY;
        } else {
            tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
        }
    }
    // Another thread that changed the page's mapping since gen may have taken
    // the entry out before it was written (mmu_tlbs_invalidate): read after
    // it, mmu->changes says so, and the entry goes (this access is done with
    // what was translated, as one already under way when the change came).
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&tlb->mmu->changes, __ATOMIC_RELAXED) != gen) {
        __atomic_store_n(&tlb_ent->page, TLB_PAGE_EMPTY, __ATOMIC_RELAXED);
        __atomic_store_n(&tlb_ent->page_if_writable, TLB_PAGE_EMPTY, __ATOMIC_RELAXED);
    }

    return (void *) ((uintptr_t) ptr - TLB_PAGE(addr) + addr);
}

/*
 * C-based memory access functions for ARM64 guest gadgets.
 * These provide reliable memory access by using the proven C-based TLB code.
 *
 * Return value convention for loads:
 * - Returns the loaded value through the 'out' pointer
 * - Returns 0 on success, -1 on segfault (tlb->segfault_addr is set)
 *
 * Return value convention for stores:
 * - Returns 0 on success, -1 on segfault (tlb->segfault_addr is set)
 */



// Load functions - return 0 on success, -1 on segfault
__no_instrument int c_load64(struct tlb *tlb, addr_t addr, uint64_t *out) {
    // Handle unaligned access by reading byte-by-byte
    if ((addr & 7) != 0) {
        // Unaligned - read each byte separately (little-endian)
        uint64_t result = 0;
        for (int i = 0; i < 8; i++) {
            void *ptr = __tlb_read_ptr(tlb, addr + i);
            if (ptr == NULL) {
                return -1;
            }
            result |= ((uint64_t)*(uint8_t *)ptr) << (i * 8);
        }
        *out = result;
        return 0;
    }

    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;  // segfault_addr already set by tlb_handle_miss
    }
    *out = *(uint64_t *)ptr;
    return 0;
}

__no_instrument int c_load32(struct tlb *tlb, addr_t addr, uint32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint32_t *)ptr;
    return 0;
}

__no_instrument int c_load16(struct tlb *tlb, addr_t addr, uint16_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load8(struct tlb *tlb, addr_t addr, uint8_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint8_t *)ptr;
    return 0;
}

// Sign-extending load functions
__no_instrument int c_load32_sx(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int32_t)*(uint32_t *)ptr;
    return 0;
}

__no_instrument int c_load16_sx64(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int16_t)*(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load16_sx32(struct tlb *tlb, addr_t addr, int32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int32_t)(int16_t)*(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load8_sx64(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int8_t)*(uint8_t *)ptr;
    return 0;
}

__no_instrument int c_load8_sx32(struct tlb *tlb, addr_t addr, int32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int32_t)(int8_t)*(uint8_t *)ptr;
    return 0;
}

// Store functions - return 0 on success, -1 on segfault
__no_instrument int c_store64(struct tlb *tlb, addr_t addr, uint64_t value) {
    // Handle unaligned access by writing byte-by-byte
    if ((addr & 7) != 0) {
        // Unaligned - write each byte separately (little-endian)
        for (int i = 0; i < 8; i++) {
            void *ptr = __tlb_write_ptr(tlb, addr + i);
            if (ptr == NULL) {
                return -1;
            }
            *(uint8_t *)ptr = (value >> (i * 8)) & 0xFF;
        }
        return 0;
    }

    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;  // segfault_addr already set
    }
    *(uint64_t *)ptr = value;
    return 0;
}

__no_instrument int c_store32(struct tlb *tlb, addr_t addr, uint32_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint32_t *)ptr = value;
    return 0;
}

__no_instrument int c_store16(struct tlb *tlb, addr_t addr, uint16_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint16_t *)ptr = value;
    return 0;
}

__no_instrument int c_store8(struct tlb *tlb, addr_t addr, uint8_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint8_t *)ptr = value;
    return 0;
}

// IC IVAU: the guest modified code at `addr` and is invalidating its icache.
// Drop any translated blocks for that page so the new code gets retranslated.
// Without this, a store through an already-writable TLB entry (which never
// takes the mem_ptr write-miss path that calls asbestos_invalidate_page)
// leaves stale translations running — JSC's in-place JIT repatching breaks.
// A shared asbestos's code written so is its address space's to leave too
// (mmu_code_written).
__no_instrument void c_ic_ivau(struct tlb *tlb, addr_t addr) {
    mmu_code_written(tlb->mmu, PAGE(addr));
}

// Atomic memory operations (LSE): LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP
// Return: 0 on success, -1 on segfault or unsupported op
// Helper macros for atomic RMW with CAS loop (for min/max that lack atomic builtins)
#define ATOMIC_RMW_CAS_LOOP(type, ptr, val, op_expr) do { \
    type old = __atomic_load_n((type *)(ptr), __ATOMIC_SEQ_CST); \
    type newv; \
    do { \
        newv = (op_expr); \
    } while (!__atomic_compare_exchange_n((type *)(ptr), &old, newv, \
             true, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
    *old_out = old; \
} while(0)

__no_instrument int c_atomic_rmw(struct tlb *tlb, addr_t addr, uint64_t value,
                                 uint32_t size, uint32_t op, uint64_t *old_out) {
    if (old_out == NULL)
        return -1;

    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t val = (uint8_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint8_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, ((int8_t)old > (int8_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, ((int8_t)old < (int8_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 1: { // 16-bit
            uint16_t val = (uint16_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint16_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, ((int16_t)old > (int16_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, ((int16_t)old < (int16_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 2: { // 32-bit
            uint32_t val = (uint32_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint32_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, ((int32_t)old > (int32_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, ((int32_t)old < (int32_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 3: { // 64-bit
            uint64_t val = value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint64_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, ((int64_t)old > (int64_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, ((int64_t)old < (int64_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        default:
            return -1;
    }
}

#undef ATOMIC_RMW_CAS_LOOP

// Atomic compare-and-swap (CAS/CASA/CASL/CASAL)
// Uses host atomic CAS for thread safety.
// Return: 0 on success, -1 on segfault or unsupported size
__no_instrument int c_atomic_cas(struct tlb *tlb, addr_t addr, uint64_t expected,
                                 uint64_t desired, uint32_t size, uint64_t *old_out) {
    if (old_out == NULL)
        return -1;

    // Get writable host pointer (CAS needs both read and write)
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t exp = (uint8_t)expected;
            __atomic_compare_exchange_n((uint8_t *)ptr, &exp, (uint8_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;  // exp is updated to actual value on failure
            return 0;
        }
        case 1: { // 16-bit
            uint16_t exp = (uint16_t)expected;
            __atomic_compare_exchange_n((uint16_t *)ptr, &exp, (uint16_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        case 2: { // 32-bit
            uint32_t exp = (uint32_t)expected;
            __atomic_compare_exchange_n((uint32_t *)ptr, &exp, (uint32_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        case 3: { // 64-bit
            uint64_t exp = expected;
            __atomic_compare_exchange_n((uint64_t *)ptr, &exp, desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        default:
            return -1;
    }
}

// 128-bit atomic compare-and-swap for the CASP (Compare-And-Swap Pair)
// instruction. CASP atomically compares/swaps a 16-byte pair — this MUST be
// a single atomic 128-bit operation, not two 64-bit CAS, otherwise a torn
// update can break lock-free algorithms that rely on the pair being updated
// atomically (e.g. libpas's pas_versioned_field {value, version}).
//
// exp_lo/exp_hi = expected pair (Rs, Rs+1); des_lo/des_hi = desired pair
// (Rt, Rt+1). On return, *old_lo/*old_hi hold the actual old pair (the CAS
// writes the old value back into the expected registers, success or fail).
// Returns 0 on success (host ptr valid), -1 on segfault.
__no_instrument int c_atomic_cas128(struct tlb *tlb, addr_t addr,
                                    uint64_t exp_lo, uint64_t exp_hi,
                                    uint64_t des_lo, uint64_t des_hi,
                                    uint64_t *old_lo, uint64_t *old_hi) {
    if (old_lo == NULL || old_hi == NULL)
        return -1;
    // CASP requires 16-byte alignment; real hardware raises an alignment fault
    // otherwise. Enforce it: an unaligned __atomic_compare_exchange_16 on a
    // misaligned pointer is not lock-free (it takes libatomic's global lock),
    // so it would silently lose atomicity against a correctly-aligned CASP from
    // another thread. Return -1 so the caller raises INT_GPF like the HW fault.
    if (addr & 0xf)
        return -1;
    // 16-byte access must not straddle a page boundary for a single ptr.
    if (PGOFFSET(addr) > PAGE_SIZE - 16)
        return -1;
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;
    __uint128_t exp = ((__uint128_t)exp_hi << 64) | (__uint128_t)exp_lo;
    __uint128_t des = ((__uint128_t)des_hi << 64) | (__uint128_t)des_lo;
    __atomic_compare_exchange_n((__uint128_t *)ptr, &exp, des,
                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    // exp is updated to the actual old value on failure (and unchanged =
    // old value on success).
    *old_lo = (uint64_t)exp;
    *old_hi = (uint64_t)(exp >> 64);
    return 0;
}

// STXR atomic compare-and-swap helper for LDXR/STXR emulation.
// Compares memory at addr with expected_val, if equal stores new_val.
// Returns 0 on success (CAS succeeded), 1 on failure (CAS lost race),
// or -1 on segfault.
//
// To prevent CoW from invalidating the host pointer between getting it
// and the CAS, we snapshot mmu->changes before and after. If it changed,
// another thread did CoW/mmap, so we retry with a fresh TLB lookup.
__no_instrument int c_stxr_cas(struct tlb *tlb, addr_t addr,
                               uint64_t expected_val, uint64_t new_val,
                               uint32_t size) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t exp = (uint8_t)expected_val;
            uint8_t des = (uint8_t)new_val;
            return __atomic_compare_exchange_n((uint8_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 1: { // 16-bit
            uint16_t exp = (uint16_t)expected_val;
            uint16_t des = (uint16_t)new_val;
            return __atomic_compare_exchange_n((uint16_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 2: { // 32-bit
            uint32_t exp = (uint32_t)expected_val;
            uint32_t des = (uint32_t)new_val;
            return __atomic_compare_exchange_n((uint32_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 3: { // 64-bit
            uint64_t exp = expected_val;
            uint64_t des = new_val;
            return __atomic_compare_exchange_n((uint64_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        default:
            return -1;
    }
}

// STXP atomic compare-and-swap helper for LDXP/STXP pair-exclusive emulation.
// A pair-exclusive store must atomically write BOTH registers only if memory
// still holds the pair loaded by LDXP — emulating it as a plain store pair
// that always "succeeds" tears lock-free algorithms (e.g. libpas versioned
// fields via __atomic_compare_exchange_16 outline fallbacks) under threads.
// size: 2 = 32-bit pair (one 64-bit CAS), 3 = 64-bit pair (one 128-bit CAS).
// Returns 0 on CAS success, 1 on CAS failure, -1 on segfault/misaligned.
__no_instrument int c_stxp_cas(struct tlb *tlb, addr_t addr,
                               uint64_t exp1, uint64_t exp2,
                               uint64_t new1, uint64_t new2, uint32_t size) {
    if (size == 2) {
        uint64_t exp = (uint32_t)exp1 | ((uint64_t)(uint32_t)exp2 << 32);
        uint64_t des = (uint32_t)new1 | ((uint64_t)(uint32_t)new2 << 32);
        void *ptr = __tlb_write_ptr(tlb, addr);
        if (ptr == NULL)
            return -1;
        return __atomic_compare_exchange_n((uint64_t *)ptr, &exp, des,
                                           false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
    }
    if (size != 3)
        return -1;
    // STXP requires 16-byte alignment; unaligned pair-exclusives fault on real
    // hardware. Enforce it — an unaligned __atomic_compare_exchange_16 is not
    // lock-free (libatomic global lock), silently losing atomicity. Return -1
    // so the caller raises INT_GPF like the HW alignment fault.
    if (addr & 0xf)
        return -1;
    // 16-byte access must not straddle a page.
    if (PGOFFSET(addr) > PAGE_SIZE - 16)
        return -1;
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;
    __uint128_t exp = ((__uint128_t)exp2 << 64) | (__uint128_t)exp1;
    __uint128_t des = ((__uint128_t)new2 << 64) | (__uint128_t)new1;
    return __atomic_compare_exchange_n((__uint128_t *)ptr, &exp, des,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}

// LDP/STP helper functions for pair loads/stores
// Return: 0 on success, -1 on segfault
__no_instrument int c_ldp64(struct tlb *tlb, addr_t addr, uint64_t *val1, uint64_t *val2) {
    void *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_read_ptr(tlb, addr + 8);
    if (ptr2 == NULL) {
        return -1;
    }
    *val1 = *(uint64_t *)ptr1;
    *val2 = *(uint64_t *)ptr2;
    (void)0;
    return 0;
}

__no_instrument int c_ldp32(struct tlb *tlb, addr_t addr, uint32_t *val1, uint32_t *val2) {
    void *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_read_ptr(tlb, addr + 4);
    if (ptr2 == NULL) {
        return -1;
    }
    *val1 = *(uint32_t *)ptr1;
    *val2 = *(uint32_t *)ptr2;
    return 0;
}

__no_instrument int c_stp64(struct tlb *tlb, addr_t addr, uint64_t val1, uint64_t val2) {
    void *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_write_ptr(tlb, addr + 8);
    if (ptr2 == NULL) {
        return -1;
    }
    *(uint64_t *)ptr1 = val1;
    *(uint64_t *)ptr2 = val2;
    return 0;
}

__no_instrument int c_stp32(struct tlb *tlb, addr_t addr, uint32_t val1, uint32_t val2) {
    void *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_write_ptr(tlb, addr + 4);
    if (ptr2 == NULL) {
        return -1;
    }
    *(uint32_t *)ptr1 = val1;
    *(uint32_t *)ptr2 = val2;
    return 0;
}

// Interleaved SIMD load (LD2/LD3/LD4)
// Reads interleaved elements from memory and deinterleaves into consecutive Vt registers.
// num_regs: 2 (LD2), 3 (LD3), or 4 (LD4)
// elem_size: 0=byte, 1=halfword, 2=word, 3=doubleword
// Q: 0=64-bit (lower half), 1=128-bit (full register)
// rt: first destination register (Vt, Vt+1, ..., Vt+num_regs-1 mod 32)
__no_instrument int c_simd_load_interleaved(struct cpu_state *cpu, struct tlb *tlb,
                                             addr_t addr, uint32_t rt, uint32_t num_regs,
                                             uint32_t elem_size, uint32_t Q) {
    unsigned elem_bytes = 1u << elem_size;
    unsigned elems_per_reg = (Q ? 16 : 8) / elem_bytes;
    unsigned total_bytes = num_regs * elems_per_reg * elem_bytes;
    if (total_bytes > 64 || num_regs > 4 || elem_size > 3) return -1;

    // Read all raw bytes from guest memory (bulk read via host pointer)
    uint8_t buf[64];
    if (PGOFFSET(addr) + total_bytes <= PAGE_SIZE) {
        // Fast path: entire access fits within one page
        char *ptr = __tlb_read_ptr(tlb, addr);
        if (ptr == NULL) return -1;
        memcpy(buf, ptr, total_bytes);
    } else {
        // Slow path: crosses page boundary
        if (!__tlb_read_cross_page(tlb, addr, (char *)buf, total_bytes))
            return -1;
    }

    // Deinterleave: memory has [elem0_reg0, elem0_reg1, ..., elem0_regN-1, elem1_reg0, ...]
    for (unsigned r = 0; r < num_regs; r++) {
        uint32_t vt = (rt + r) & 0x1f;
        cpu->fp[vt].q = 0;
        for (unsigned e = 0; e < elems_per_reg; e++) {
            unsigned src_offset = (e * num_regs + r) * elem_bytes;
            unsigned dst_offset = e * elem_bytes;
            memcpy(&cpu->fp[vt].b[dst_offset], &buf[src_offset], elem_bytes);
        }
    }
    return 0;
}

// Interleaved SIMD store (ST2/ST3/ST4)
// Reads consecutive Vt registers and writes interleaved elements to memory.
__no_instrument int c_simd_store_interleaved(struct cpu_state *cpu, struct tlb *tlb,
                                              addr_t addr, uint32_t rt, uint32_t num_regs,
                                              uint32_t elem_size, uint32_t Q) {
    unsigned elem_bytes = 1u << elem_size;
    unsigned elems_per_reg = (Q ? 16 : 8) / elem_bytes;
    unsigned total_bytes = num_regs * elems_per_reg * elem_bytes;
    if (total_bytes > 64 || num_regs > 4 || elem_size > 3) return -1;

    // Interleave register data into buffer
    uint8_t buf[64];
    for (unsigned r = 0; r < num_regs; r++) {
        uint32_t vt = (rt + r) & 0x1f;
        for (unsigned e = 0; e < elems_per_reg; e++) {
            unsigned dst_offset = (e * num_regs + r) * elem_bytes;
            unsigned src_offset = e * elem_bytes;
            memcpy(&buf[dst_offset], &cpu->fp[vt].b[src_offset], elem_bytes);
        }
    }

    // Write all bytes to guest memory (bulk write via host pointer)
    if (PGOFFSET(addr) + total_bytes <= PAGE_SIZE) {
        char *ptr = __tlb_write_ptr(tlb, addr);
        if (ptr == NULL) return -1;
        memcpy(ptr, buf, total_bytes);
    } else {
        if (!__tlb_write_cross_page(tlb, addr, (char *)buf, total_bytes))
            return -1;
    }
    return 0;
}
