#ifndef MEMORY_H
#define MEMORY_H

#include <stdatomic.h>
#include <unistd.h>
#include <stdbool.h>
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"
#include "misc.h"

// Lazy reservation for MAP_NORESERVE regions (JSC heap cage, etc.).
// Records address range and permissions without allocating page table entries.
// Pages are materialized on demand when faulted. Used by both x86 and ARM64
// guests so memory.c can call into the reservation API unconditionally.
struct mem_reservation {
    page_t start;
    pages_t pages;
    unsigned flags;
    struct mem_reservation *next;
};

// ARM64: 4-level page table for 48-bit address space
// L0[512] → L1[512] → L2[512] → L3[512 pt_entry]
// 9+9+9+9 = 36-bit page number + 12-bit offset = 48-bit address
#define PT_LEVELS 4
#define PT_BITS 9
#define PT_ENTRIES (1 << PT_BITS)  // 512

// Extract level index from page number (36-bit page number)
#define PT_INDEX(page, level) (((page) >> (PT_BITS * (3 - (level)))) & (PT_ENTRIES - 1))

// Page table intermediate node (L0, L1, L2)
struct pt_node {
    void *children[PT_ENTRIES]; // pt_node* for L0-L2, pt_entry* array for L3
};

struct mem {
    struct pt_node *pgdir;  // L0 node (512 entries)
    int pgdir_used;

    struct mmu mmu;

    // Hint for pt_find_hole: start searching just below the last successful
    // allocation instead of always from MMAP_HOLE_START. Reset on munmap
    // when freed pages are above the hint. Avoids O(n) rescanning of already-
    // allocated regions on every mmap(addr=0).
    page_t mmap_hint;

    struct mem_reservation *reservations;

    wrlock_t lock;
    lock_t cow_lock;
};

// Address space layout for ARM64 guest.
// 48-bit address space with 4-level page table and 48-bit JIT masks.
// Keep stack and mmap in the low 4GB region (same as x86) so that page table
// walks are shallow (L0[0]→L1[0]→L2→L3 — the first two levels are always
// index 0 and stay hot in cache). The 48-bit infrastructure remains available
// for explicit high-address mmap hints (e.g., V8 CodeRange).
#define STACK_TOP_PAGE    0xffffeULL        // guard page at 0xffffe000
#define STACK_INIT_PAGE   0xffffdULL        // top page of the initial stack (growsdown)
#define STACK_INIT_PAGES  32                // initial stack: 128 KB, as Linux maps at exec
#define STACK_TOP_ADDR    0xffffe000ULL     // SP starts here
#define MMAP_HOLE_START   0xefffdULL        // mmap search starts here (same as x86)
#define MMAP_HOLE_END     0x100ULL          // mmap search ends here (above guard pages)
// Upper bound for valid user addresses (page number, 48-bit / 4K = 36-bit)
#define USER_ADDR_MAX_PAGE  0xFFFFFFFFFULL
// A PROT_NONE MAP_NORESERVE mapping this big or bigger is a lazy reservation,
// no host memory (do_mmap, [T-ish-noreserve-lazy]): 1MB
#define NORESERVE_LAZY_MIN_PAGES 0x100ULL


// Initialize the address space
void mem_init(struct mem *mem);
// Uninitialize the address space
void mem_destroy(struct mem *mem);
// Bump the memory-change generation (for page-backing mutations that bypass
// pt_map/pt_unmap, e.g. madvise MADV_DONTNEED in-place zeroing).
void mem_changed_pub(struct mem *mem, page_t start, pages_t pages);
// Return the pagetable entry for the given page
struct pt_entry *mem_pt(struct mem *mem, page_t page);
// Increment *page, skipping over unallocated page directories. Intended to be
// used as the incremenent in a for loop to traverse mappings.
void mem_next_page(struct mem *mem, page_t *page);

#define BYTES_ROUND_DOWN(bytes) (PAGE(bytes) << PAGE_BITS)
#define BYTES_ROUND_UP(bytes) (PAGE_ROUND_UP(bytes) << PAGE_BITS)

#define LEAK_DEBUG 0

struct data {
    void *data; // immutable
    size_t size; // also immutable
    atomic_uint refcount;

    // for display in /proc/pid/maps
    struct fd *fd;
    size_t file_offset;
    const char *name;
    // Guest module id + 1 for the ARM64 JIT (asbestos/guest-arm64/jit.c), 0
    // until looked up. Not under ISH_JIT: app code sees this struct too.
    int jit_mod;
    // Frees it when the last reference goes, instead of munmap and free: a
    // System V shared memory segment's (kernel/shm.c). NULL for the others.
    void (*release)(struct data *data);
#if LEAK_DEBUG
    int pid;
    addr_t dest;
#endif
};
struct pt_entry {
    struct data *data;
    size_t offset;
    unsigned flags;
    // What bytes the page has, while they are a file's (mapped from it,
    // private, never writable since: mem_set_code_tags): its identity and the
    // page's offset in it, hashed. 0 otherwise. For an asbestos address spaces
    // share (mmu_code_tag).
    uint64_t code_tag;
};
// page flags
// P_READ and P_EXEC are ignored for now
#define P_READ (1 << 0)
#define P_WRITE (1 << 1)
#undef P_EXEC // defined in sys/proc.h on darwin
#define P_EXEC (1 << 2)
#define P_RWX (P_READ | P_WRITE | P_EXEC)
#define P_GROWSDOWN (1 << 3)
#define P_COW (1 << 4)
#define P_WRITABLE(flags) (flags & P_WRITE && !(flags & P_COW))

// mapping was created with pt_map_nothing
#define P_ANONYMOUS (1 << 6)
// mapping was created with MAP_SHARED, should not CoW
#define P_SHARED (1 << 7)
// W^X for the JIT: the page contains guest code that has been compiled into
// fiber blocks. While set, no TLB entry may be stamped writable for the page
// (translate_write_nofault refuses), so every guest store to it takes the
// mem_ptr write-miss path, which invalidates the page's compiled blocks and
// clears this flag (bumping mmu->changes) before the store is allowed to
// land. Set by mmu_mark_code_page when a block is compiled from the page.
#define P_CODE (1 << 8)
// Mapped while the address space shared its asbestos (mem_asbestos_share):
// what others using it have there may differ (mmu_code_tag)
#define P_LATE (1 << 9)

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages);
page_t pt_find_hole(struct mem *mem, pages_t size);

// Reservation API is available for both x86 and ARM64 guests.
struct mem_reservation *mem_find_reservation(struct mem *mem, page_t page);
void mem_remove_reservations(struct mem *mem, page_t start, pages_t pages);

int pt_map_lazy(struct mem *mem, page_t start, pages_t pages, unsigned flags);
// Find a hole for a V8-style large reservation. Prefers addresses
// above 4GB so heap pointers are non-canonical in low 32 bits.
page_t pt_find_hole_for_reservation(struct mem *mem, pages_t size);

// Map memory + offset into fake memory, unmapping existing mappings. Takes
// ownership of memory. It will be freed with:
// munmap(memory, pages * PAGE_SIZE)
int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags);
// Map pages of data, from offset, unmapping existing mappings; takes a
// reference to it for each page
void pt_map_data(struct mem *mem, page_t start, pages_t pages, struct data *data, size_t offset, unsigned flags);
// Drop a reference to data; the last one frees it (data->release if set)
void data_unref(struct data *data);
// Map empty space into fake memory
int pt_map_nothing(struct mem *mem, page_t page, pages_t pages, unsigned flags);
// Map pages from start, the first offset bytes of whose host memory precede
// start, as host mappings of 16MB each, a data object apiece, as adjacent
// mmaps would be: for a mapping one host mmap failed for, when the host has no
// free range that large. map_chunk gives the host memory for size bytes from
// byte from of it, or MAP_FAILED. Returns 0, or _ENOMEM with nothing mapped.
int pt_map_chunked(struct mem *mem, page_t start, pages_t pages, size_t offset, unsigned flags,
                   void *(*map_chunk)(size_t from, size_t size, void *ctx), void *ctx);
// Give the data objects of a mapping of fd from file_offset, at pages from
// start, the file: one object, or one per chunk of pt_map_chunked
void pt_set_file(struct mem *mem, page_t start, pages_t pages, struct fd *fd, size_t file_offset);
// Unmap fake memory, return -1 if any part of the range isn't mapped and 0 otherwise
int pt_unmap(struct mem *mem, page_t start, pages_t pages);
// like pt_unmap but doesn't care if part of the range isn't mapped
int pt_unmap_always(struct mem *mem, page_t start, pages_t pages);

// [T-ish-cluster-commit] Commit the faulting page as part of an aligned,
// host-page-sized cluster so a 4KB guest page doesn't consume a whole 16KB
// host page. Degrades to a single-page commit whenever the cluster's other
// pages are already mapped or `same_flags` rejects them; on a 4KB-page host
// it is exactly pt_map_nothing(page, 1). Reports how many pages were actually
// committed via `committed_out` (NULL if not needed) so callers can keep
// anonymous-page accounting exact. Caller must hold mem->lock for WRITING.
int pt_map_cluster(struct mem *mem, page_t page, unsigned flags,
                   bool (*same_flags)(struct mem *mem, page_t page, void *ctx),
                   void *ctx, pages_t *committed_out);

// [T-ish-cluster-hitrate] Diagnostics: how often the cluster actually fires,
// and when it does not, why. Any out-param may be NULL.
void ish_cluster_stats(uint64_t *calls, uint64_t *full, uint64_t *single,
                       uint64_t *why_mapped, uint64_t *why_flags,
                       uint64_t *why_nocluster, uint64_t *why_enomem,
                       uint64_t *pages_committed);
// The anonymous host mmaps that became guest memory since the app started,
// PROT_NONE ones too, a mapping in 16MB chunks one for each, and the guest
// pages they hold; of them the copies on write, of those the ones of a whole
// host page, and the guest pages they copied. Any out-param may be NULL.
void ish_anon_mmap_stats(uint64_t *mmaps, uint64_t *pages, uint64_t *cow_copies, uint64_t *cow_whole,
                         uint64_t *cow_pages);
// Count mmaps anonymous host mmaps that became guest memory, of pages pages
void anon_mmap_count(uint64_t mmaps, pages_t pages);
// Set the flags on memory
int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags);
// Copy pages from src memory to dst memory using copy-on-write
int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages);

// Must call with mem read-locked.
void *mem_ptr(struct mem *mem, addr_t addr, int type);
int mem_segv_reason(struct mem *mem, addr_t addr);

// W^X code-page protection (see P_CODE). Both must be called with the mem
// read-locked (JIT context). mmu_mark_code_page marks the page as containing
// compiled code; returns 1 if the page transitioned to code (mmu->changes was
// bumped, staling every thread's cached writable TLB entry), 0 if it was
// already marked, -1 if the page isn't mapped (nothing to protect).
// mmu_code_page_intact reports whether the mark is still in place — a store
// that raced with compilation clears it.
int mmu_mark_code_page(struct mmu *mmu, page_t page);
bool mmu_code_page_intact(struct mmu *mmu, page_t page);
// What code the page has, for a block translated from it into an asbestos
// address spaces share (mem_asbestos_share): 0 if it is not to be shared
// (unmapped, not executable, writable or anonymous: the address space's own);
// CODE_TAG_EARLY if it was mapped before the asbestos was shared, and every
// address space using it has it the same; else (P_LATE) its file's bytes
// (code_tag), the same in another only if that one has the same there.
// mem's lock held.
#define CODE_TAG_EARLY 1
uint64_t mmu_code_tag(struct mmu *mmu, page_t page);
// Pages mapped from a file, private and not writable: what bytes of it they
// have (code_tag). mem's lock held to write.
void mem_set_code_tags(struct mem *mem, page_t start, pages_t pages, struct fd *fd, size_t offset);
// A forked child's asbestos is its parent's (mm_copy), if the parent's has
// had no block of a writable or anonymous page; both locks held to write
void mem_asbestos_share(struct mem *src, struct mem *dst);
// The address space leaves the asbestos it shares for one of its own (mem's
// lock held to write), as its threads were to: a block was translated from
// other code than it has there (differs, INT_ASBESTOS_DIFFERS), or of its own
// (INT_ASBESTOS_DETACH), or it wrote code (detach_requested)
void mem_asbestos_detach(struct mem *mem, bool differs);
// A page that goes (pt_unmap): its blocks go, but of an asbestos others share
// too (asbestos_page_changing). mem's lock held to write.
void mem_asbestos_page_unmapping(struct mem *mem, page_t page, struct pt_entry *entry);
// A store to code a thread made through no write fault (IC IVAU): the page's
// blocks go, and a shared asbestos is left
void mmu_code_written(struct mmu *mmu, page_t page);
// Forks that shared their parent's asbestos, and address spaces that left one:
// for a mapping change, a block of their own, a write to code
void mem_asbestos_describe(uint64_t *shared_forks, uint64_t *mapping, uint64_t *private_blocks, uint64_t *written);

extern size_t real_page_size;

#endif
