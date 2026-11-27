#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "asbestos/asbestos.h"
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "kernel/memory.h"
#include "util/verbosetrace.h"
#include "kernel/mm.h"
#include "asbestos/guest-arm64/host_fault.h"

#if ANON_MMAP_LIMIT_PAGES > 0
_Atomic long anon_page_count;

// [T-ish-anon-cap-dynamic] Effective limit, host-tunable at boot. Defaults to
// the compile-time ceiling so builds that never call the setter keep the old
// fixed-cap behaviour.
_Atomic long anon_page_limit = ANON_MMAP_LIMIT_PAGES;

static void anon_count_check(long count);

void ish_set_anon_page_limit(long pages) {
    if (pages <= 0)
        return; // host couldn't measure (e.g. unlimited) — keep the default
    if (pages > ANON_MMAP_LIMIT_PAGES)
        pages = ANON_MMAP_LIMIT_PAGES;
    atomic_store(&anon_page_limit, pages);
}

// [T-ish-footprint-brake] Live memory status. stamp==0 means the host never
// fed us — legacy ledger mode. See the design note in mm.h.
static _Atomic int ish_mem_state_v = ISH_MEM_OK;
static _Atomic uint64_t ish_mem_limit_v;
static _Atomic uint64_t ish_mem_avail_v;
static _Atomic uint64_t ish_mem_stamp_ms;

// Feed staleness window. A sampler that stops updating for this long is
// treated as dead and the brake engages — the governor must fail closed.
#define ISH_MEM_STALE_MS 2000

static uint64_t ish_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

bool ish_footprint_mode(void) {
    return atomic_load(&ish_mem_stamp_ms) != 0;
}

void ish_set_memory_status(uint64_t limit_bytes, uint64_t avail_bytes, bool pressure_critical) {
    if (limit_bytes == 0)
        return; // host couldn't measure — don't flip modes on garbage
    int prev = atomic_load(&ish_mem_state_v);
    int next;
    if (pressure_critical) {
        // The OS itself says memory is critical — believe it over our math.
        next = ISH_MEM_BRAKE;
    } else if (avail_bytes * 10 < limit_bytes) {
        next = ISH_MEM_BRAKE;                       // < 10% headroom
    } else if (prev == ISH_MEM_BRAKE && avail_bytes * 100 < limit_bytes * 15) {
        next = ISH_MEM_BRAKE;                       // hysteresis: exit above 15%
    } else {
        next = ISH_MEM_OK;
    }
    atomic_store(&ish_mem_limit_v, limit_bytes);
    atomic_store(&ish_mem_avail_v, avail_bytes);
    atomic_store(&ish_mem_state_v, next);
    atomic_store(&ish_mem_stamp_ms, ish_monotonic_ms());
    if (next != prev)
        printk("mem: brake %s — footprint %lu MB / limit %lu MB (avail %lu MB)%s\n",
               next == ISH_MEM_BRAKE ? "ENGAGED" : "released",
               (unsigned long)((limit_bytes - avail_bytes) >> 20),
               (unsigned long)(limit_bytes >> 20),
               (unsigned long)(avail_bytes >> 20),
               pressure_critical ? " [OS pressure critical]" : "");
    // What can be given back before an allocation is refused: idle
    // processes' translations
    if (next == ISH_MEM_BRAKE)
        ish_reclaim_translations_async();
}

// ---- Translations of idle address spaces, given back under memory pressure
//
// Every address space has blocks of its own: a forked child translates again
// what its parent had, and a multiprocessing.Pool(200) of python took 24.8 MB
// a worker, 13.5 MB of it translated blocks (35.8K), where Linux shares the
// code. A worker waiting for work keeps them all. Under memory pressure (the
// brake) the blocks of every address space whose threads have run no
// translated code for TRANSLATIONS_IDLE_MS are freed (asbestos_reclaim_idle);
// it translates them again when it runs. /proc/ish/translations says what was
// freed, and "reclaim" written to it frees them at once.
#define TRANSLATIONS_IDLE_MS 2000
// Between two reclaims the brake runs on: those freed are freed, and a
// reclaim walks every address space's blocks
#define TRANSLATIONS_RECLAIM_EVERY_MS 5000

static _Atomic uint64_t st_reclaims, st_reclaim_spaces, st_reclaim_blocks, st_reclaim_bytes;
static _Atomic bool reclaim_running;
static _Atomic uint64_t reclaim_last_ms;

void ish_reclaim_translations(void) {
    // The address spaces of the tasks, each once and retained, under the
    // locks that keep them (pids_lock, then the task's general_lock, as
    // /proc's readers take them); reclaimed with neither held
    size_t n = 0, cap = 64;
    struct mm **mms = malloc(cap * sizeof(*mms));
    lock(&pids_lock);
    for (pid_t_ pid = 1; pid <= MAX_PID && mms != NULL; pid++) {
        struct task *task = pid_get_task(pid);
        if (task == NULL)
            continue;
        lock(&task->general_lock);
        struct mm *mm = task->mm;
        bool seen = mm == NULL;
        for (size_t i = 0; i < n && !seen; i++)
            seen = mms[i] == mm;
        if (!seen) {
            if (n == cap) {
                cap *= 2;
                struct mm **grown = realloc(mms, cap * sizeof(*mms));
                if (grown == NULL) {
                    unlock(&task->general_lock);
                    break;
                }
                mms = grown;
            }
            mm_retain(mm);
            mms[n++] = mm;
        }
        unlock(&task->general_lock);
    }
    unlock(&pids_lock);

    // Their asbestoses, each once (forked address spaces share one,
    // mem_asbestos_share), retained: an address space may leave its own
    // meanwhile (mem_asbestos_detach, which holds its mem's lock to write),
    // and the others using it go
    struct asbestos **spaces_of = malloc(n * sizeof(*spaces_of) + 1);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (spaces_of != NULL) {
            read_wrlock(&mms[i]->mem.lock);
            struct asbestos *asbestos = mms[i]->mem.mmu.asbestos;
            bool seen = asbestos == NULL;
            for (size_t j = 0; j < m && !seen; j++)
                seen = spaces_of[j] == asbestos;
            if (!seen)
                spaces_of[m++] = asbestos_retain(asbestos);
            read_wrunlock(&mms[i]->mem.lock);
        }
        mm_release_from(mms[i], "ish_reclaim_translations");
    }
    free(mms);

    uint64_t now = ish_monotonic_ms();
    size_t spaces = 0, blocks = 0, bytes = 0;
    for (size_t i = 0; i < m; i++) {
        size_t b = 0, by = 0;
        asbestos_reclaim_idle(spaces_of[i], now, TRANSLATIONS_IDLE_MS, &b, &by);
        if (b != 0) {
            spaces++;
            blocks += b;
            bytes += by;
        }
        asbestos_release(spaces_of[i]);
    }
    free(spaces_of);
    atomic_fetch_add(&st_reclaims, 1);
    atomic_fetch_add(&st_reclaim_spaces, spaces);
    atomic_fetch_add(&st_reclaim_blocks, blocks);
    atomic_fetch_add(&st_reclaim_bytes, bytes);
    if (blocks != 0)
        printk("mem: freed %zu translated blocks (%.1f MB) of %zu idle address spaces\n", blocks,
               bytes / 1048576.0, spaces);
}

static void *reclaim_thread(void *UNUSED(arg)) {
    ish_reclaim_translations();
    atomic_store(&reclaim_last_ms, ish_monotonic_ms());
    atomic_store(&reclaim_running, false);
    return NULL;
}

void ish_reclaim_translations_async(void) {
    uint64_t last = atomic_load(&reclaim_last_ms);
    if (last != 0 && ish_monotonic_ms() - last < TRANSLATIONS_RECLAIM_EVERY_MS)
        return;
    if (atomic_exchange(&reclaim_running, true))
        return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, reclaim_thread, NULL) != 0) {
        atomic_store(&reclaim_running, false);
        return;
    }
    pthread_detach(thread);
}

void ish_reclaim_describe(uint64_t *runs, uint64_t *spaces, uint64_t *blocks, uint64_t *bytes) {
    *runs = atomic_load(&st_reclaims);
    *spaces = atomic_load(&st_reclaim_spaces);
    *blocks = atomic_load(&st_reclaim_blocks);
    *bytes = atomic_load(&st_reclaim_bytes);
}

bool ish_mem_commit_ok(uint64_t bytes) {
    uint64_t stamp = atomic_load(&ish_mem_stamp_ms);
    if (stamp == 0)
        return true;   // legacy mode: the ledger check in anon_pages_reserve decides
    uint64_t limit = atomic_load(&ish_mem_limit_v);
    // Sanity gate: one request larger than the entire allowance can never be
    // dirtied safely — give it a clean upfront ENOMEM (malloc returns NULL)
    // instead of admitting it and killing the process mid-memset later.
    if (limit != 0 && bytes > limit)
        return false;
    if (ish_monotonic_ms() - stamp > ISH_MEM_STALE_MS)
        return false;  // dead sampler fails closed
    return atomic_load(&ish_mem_state_v) == ISH_MEM_OK;
}

// What a commitment of nothing more is told: refused only by the brake or a
// stale feed, as every other one is then
bool ish_mem_braked(void) {
    return !ish_mem_commit_ok(0);
}

// Check-and-add under the runtime limit. CAS loop rather than fetch_add so a
// refusal adds nothing — the old blind fetch_add sites both leaked count on
// the failure path and (worse) let paths that "only account" sail past the
// limit entirely, which is how the 2026-08-25 jsonnet compile grew a 2GB+
// footprint with the cap nominally in place.
//
// [T-ish-footprint-brake] Two admission regimes share this single choke
// point. In footprint mode the decision comes from ish_mem_commit_ok()
// (live jetsam headroom) and the counter is accounting only — a plain add,
// no limit comparison, because the whole point of the redesign is that
// COMMITMENT is not what kills the app, dirty footprint is. Legacy mode
// (host never installed a feed: tests, Linux CLI) keeps the ledger check
// bit-for-bit as before.
bool anon_pages_reserve(long pages) {
    if (ish_footprint_mode()) {
        if (!ish_mem_commit_ok((uint64_t)pages * PAGE_SIZE))
            return false;
        atomic_fetch_add(&anon_page_count, pages);
        anon_pages_precharged(pages);
        return true;
    }
    long limit = atomic_load(&anon_page_limit);
    long count = atomic_load(&anon_page_count);
    anon_count_check(count);
    do {
        if (count + pages > limit)
            return false;
    } while (!atomic_compare_exchange_weak(&anon_page_count, &count, count + pages));
    // Tell pt_map_nothing this much is already accounted for, so the pages it
    // is about to map are not charged a second time.
    anon_pages_precharged(pages);
    return true;
}

void anon_pages_unreserve(long pages) {
    atomic_fetch_sub(&anon_page_count, pages);
}

// [T-ish-anon-count-negative] Handoff between "reserved by a caller enforcing
// the cap" and "charged because pages were actually mapped".
//
// pt_map_nothing charges every page it maps. Callers that reserve FIRST (to
// refuse the allocation before doing any work) would otherwise be counted
// twice, so they park the reservation here and pt_map_nothing consumes it.
// Thread-local because the reserve and the map always happen on the same
// thread, back to back, under mem->lock — a global would let two concurrent
// mappers steal each other's credit.
static __thread long anon_precharged_pages;

void anon_pages_precharged(long pages) {
    anon_precharged_pages = pages;
}

long anon_pages_precharge_peek(void) {
    return anon_precharged_pages;
}

void anon_pages_charge_mapped(long pages) {
    long credit = anon_precharged_pages;
    anon_precharged_pages = 0;
    if (credit >= pages)
        return;              // fully covered by the caller's reservation
    atomic_fetch_add(&anon_page_count, pages - credit);
}

// Fail loudly when the invariant breaks instead of silently granting memory.
// Called from the cap check, which is the point where a negative count would
// start handing out free headroom.
static void anon_count_check(long count) {
    if (count >= 0)
        return;
    static _Atomic bool reported;
    bool expected = false;
    if (atomic_compare_exchange_strong(&reported, &expected, true)) {
        printk("mmap: BUG anon_page_count went NEGATIVE (%ld pages) — "
               "charge/uncharge asymmetry; the cap is now granting free "
               "headroom. Clamping to 0.\n", count);
        assert(count >= 0);
    }
    // Release builds (NDEBUG) keep running: clamp so the cap still holds a
    // line rather than authorising unbounded memory.
    long expected_count = count;
    atomic_compare_exchange_strong(&anon_page_count, &expected_count, 0);
}

// [T-ish-anon-cap-above-jetsam] Announce the moment the cap actually bites.
//
// Without this the guest just sees ENOMEM and the app log says nothing, so a
// runaway allocation is indistinguishable from a bug in the user's script —
// which is exactly how the 2026-08-24 report was first misread. Rate-limited
// because a process hitting the ceiling typically retries in a tight loop.
static void anon_limit_report(const char *where, long requested_pages) {
    // [T-ish-footprint-brake] In footprint mode the refusal has nothing to do
    // with the ledger, so the "cap/ceiling" line below would be misinformation
    // — report the actual reason (brake / stale feed / oversized ask), rate
    // limited by time since the counter no longer tracks the trigger.
    if (ish_footprint_mode()) {
        static _Atomic uint64_t last_ms;
        uint64_t now = ish_monotonic_ms();
        uint64_t prev_ms = atomic_load(&last_ms);
        if (now - prev_ms < 2000)
            return;
        atomic_store(&last_ms, now);
        uint64_t limit = atomic_load(&ish_mem_limit_v);
        uint64_t avail = atomic_load(&ish_mem_avail_v);
        bool stale = now - atomic_load(&ish_mem_stamp_ms) > ISH_MEM_STALE_MS;
        printk("mmap: memory brake refused %s — requested %ld pages, app footprint "
               "%lu MB / limit %lu MB (avail %lu MB)%s. Failing the guest allocation "
               "to keep the app below the jetsam line.\n",
               where, requested_pages,
               (unsigned long)((limit - avail) >> 20), (unsigned long)(limit >> 20),
               (unsigned long)(avail >> 20),
               stale ? " [sampler stale — failing closed]" : "");
        return;
    }
    static _Atomic long last_report_pages;
    long count = atomic_load(&anon_page_count);
    long prev = atomic_load(&last_report_pages);
    // One line per 16MB of movement in the high-water mark, so a retry storm
    // does not itself become the thing that floods the log.
    if (count > prev - 4096 && count < prev + 4096)
        return;
    atomic_store(&last_report_pages, count);
    long limit = atomic_load(&anon_page_limit);
    // [T-ish-anon-cap-page-units] Report HOST megabytes: each counted guest
    // page occupies a full host page, so `pages / 256` (pages x 4KB) understates
    // the real footprint by 4x on a 16KB-page device and made the previous log
    // line agree with a cap that was itself wrong.
    long kb_per_page = (long)getpagesize() / 1024;
    printk("mmap: anonymous page cap reached in %s — in use %ld pages (%ld MB host), "
           "requested %ld pages (%ld MB host), cap %ld pages (%ld MB host, "
           "ceiling %ld MB host). Failing the guest allocation instead of "
           "letting the host app be killed by jetsam.\n",
           where, count, count * kb_per_page / 1024,
           requested_pages, requested_pages * kb_per_page / 1024,
           limit, limit * kb_per_page / 1024,
           (long)ANON_MMAP_LIMIT_PAGES * kb_per_page / 1024);
}
#endif

// [T-ish-mm-diag] Source of `struct mm` identities. Starts at 1 so that 0 is
// never a live seq — a log line with seq=0 means the object was already
// released (mm_release_from zeroes it) or never initialised.
static _Atomic uint64_t g_mm_seq = 1;

struct mm *mm_new(void) {
    struct mm *mm = malloc(sizeof(struct mm));
    if (mm == NULL)
        return NULL;
    mm->seq = atomic_fetch_add(&g_mm_seq, 1);
    mem_init(&mm->mem);
    mm->start_brk = mm->brk = 0; // should get overwritten by exec
    mm->exefile = NULL;
    mm->refcount = 1;
    return mm;
}

struct mm *mm_copy(struct mm *mm) {
    struct mm *new_mm = malloc(sizeof(struct mm));
    if (new_mm == NULL)
        return NULL;
    *new_mm = *mm;
    // Fix wrlock_init failing because it thinks it's reinitializing the same lock
    memset(&new_mm->mem.lock, 0, sizeof(new_mm->mem.lock));
    new_mm->refcount = 1;
    // [T-ish-mm-diag] A COW clone is its own object: it must NOT inherit the
    // parent's seq, or a fork would make two live mms share one identity and
    // the "same seq twice" rule would fire on every fork+exit pair.
    new_mm->seq = atomic_fetch_add(&g_mm_seq, 1);
    mem_init(&new_mm->mem);
    fd_retain(new_mm->exefile);
    write_wrlock(&mm->mem.lock);
    // The parent's translated blocks are the child's too, while their code
    // is the same (mem_asbestos_share; before the copy, which keeps P_CODE
    // where the asbestos is shared). The child's mem is no one else's yet.
    mem_asbestos_share(&mm->mem, &new_mm->mem);
    pt_copy_on_write(&mm->mem, &new_mm->mem, 0, MEM_PAGES);
    write_wrunlock(&mm->mem.lock);
    return new_mm;
}

void mm_retain(struct mm *mm) {
    mm->refcount++;
}

// [T-ish-mm-diag] Same as mm_release, but names the release path so the
// log can tell do_exit / exec / cleanup_handler apart when two of them race
// on one mm (the mem_destroy brk #1 investigation, analysis §10).
void mm_release_from(struct mm *mm, const char *caller) {
    // [T-ish-mm-diag] Unconditional entry line, BEFORE the decrement, so a
    // second release on an already-freed object is visible without waiting
    // for a crash: it shows up as the same seq twice, the second with
    // refcount_before=0 (or a wrapped UINT_MAX after the unsigned decrement).
    uint64_t seq = mm->seq;
    unsigned before = atomic_load(&mm->refcount);
    // [T-ios-log-verbose-tier] 3 lines per process exit is too much for the
    // logger under a fork storm (LoggingManager dropped chunks and ish_vprintk
    // showed up as a mutex waiter); the rare SAFETY-VALVE/DEFERRED-RELEASE
    // lines stay unconditional.
    if (ish_verbose_trace_enabled)
    printk("[iSH][MM-RELEASE-ENTER] pid=%d mm=%p seq=%llu refcount_before=%u, caller=%s\n",
           current ? current->pid : -1, (void *) mm,
           (unsigned long long) seq, before, caller);
    unsigned left = --mm->refcount;
    if (left == 0) {
        if (ish_verbose_trace_enabled)
        printk("[iSH][MM-RELEASE] pid=%d mm=%p seq=%llu refcount→0, caller=%s\n",
               current ? current->pid : -1, (void *) mm,
               (unsigned long long) seq, caller);
        if (mm->exefile != NULL)
            fd_close(mm->exefile);
        mem_destroy(&mm->mem);
        // Poison the identity before handing the memory back to malloc: if
        // anything releases this object again, its seq reads 0 rather than a
        // plausible live value.
        mm->seq = 0;
        free(mm);
    }
}

void mm_release(struct mm *mm) {
    mm_release_from(mm, "unknown");
}

static addr_t do_mmap(addr_t addr, uint64_t len, dword_t prot, dword_t flags, fd_t fd_no, uint64_t offset) {
    int err;
    pages_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!pages) return _EINVAL;
    extern bool ish_exec_trace(void);
    if (ish_exec_trace() && len >= 0x200000ULL) {
        fprintf(stderr, "MMAP: pid=%d addr=0x%llx len=0x%llx prot=0x%x flags=0x%x fd=%d\n",
                current->pid,
                (unsigned long long)addr,
                (unsigned long long)len,
                prot, flags, fd_no);
    }
    page_t page;
    if (addr != 0) {
        if (PGOFFSET(addr) != 0)
            return _EINVAL;
        page = PAGE(addr);
        // Reject hints that would overlap the stack region in low 4GB
        // or exceed the 48-bit user address limit.
        // Hints within low 4GB (up to the stack) are unchanged — V8
        // Wasm guard regions use those. Hints above 4GB are now
        // honoured (Go's arena hints at 0x4000000000, 0x14000000000…
        // need to be placed where asked so the runtime's scavengeIndex
        // metadata is consistent with the actual arena layout).
        bool low_hint = (page < 0x100000);  // < 4GB
        if (low_hint) {
            if (page + pages > STACK_TOP_PAGE) {
                if (flags & MMAP_FIXED)
                    return _ENOMEM;
                addr = 0;
                page = 0;
            }
        } else {
            if (page + pages > USER_ADDR_MAX_PAGE) {
                if (flags & MMAP_FIXED)
                    return _ENOMEM;
                addr = 0;
                page = 0;
            }
        }
        if (addr != 0 && !(flags & MMAP_FIXED) && !pt_is_hole(current->mem, page, pages))
            addr = 0;
    }
    if (addr == 0) {
        // V8 reserves its heap cage with:
        //     mmap(NULL, chunk_size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
        //     (typical chunk_size: 128MB, reserved before mprotect'ing
        //      sub-regions RW as it allocates).
        // Node 22 is built without pointer compression, so V8 stores
        // full 64-bit tagged pointers into heap slots. If the cage
        // lives in low 4GB (0xc0000000..0xd0000000), a slot that
        // accidentally holds a small integer 0x00c36a73 looks like a
        // valid heap pointer and V8 derefs it into unmapped memory.
        // Placing the cage above 4GB makes such stray values obviously
        // non-canonical so V8's own `ptr & kHeapObjectTagMask` checks
        // catch them before deref.
        //
        // Match criteria (conservative, only V8-cage shape):
        //  * PROT_NONE reservation
        //  * private + anonymous
        //  * ≥ 128MB (0x8000 pages). V8 reserves 128MB+ chunks; Go
        //    arenas default to 64MB and need to stay in low 4GB
        //    because the Go runtime's arenaIndex / scavengeIndex
        //    metadata is laid out with that assumption.
        bool is_v8_cage_reservation =
            prot == 0 &&
            (flags & (MMAP_PRIVATE | MMAP_ANONYMOUS))
                == (MMAP_PRIVATE | MMAP_ANONYMOUS) &&
            !(flags & MMAP_SHARED) &&
            pages >= 0x8000;  // 128MB
        if (is_v8_cage_reservation)
            page = pt_find_hole_for_reservation(current->mem, pages);
        else
            page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE)
            return _ENOMEM;
    }

    if (flags & MMAP_SHARED)
        prot |= P_SHARED;

    if (flags & MMAP_ANONYMOUS) {
        // PROT_NONE mappings (guard regions) don't consume real memory,
        // so don't count them against the anonymous page limit.
        bool is_prot_none = !(prot & P_READ) && !(prot & P_WRITE) && !(prot & P_EXEC);
        // Three kinds of mapping are address space, not memory, and become a
        // lazy mem_reservation: no host memory until a page is mprotect'ed or
        // touched (mem_ptr's demand-map path, pt_set_flags' commit).
        //
        // MAP_NORESERVE over 256MB: JSC's 128GB cage, the JVM's heap.
        bool big_noreserve = (flags & MMAP_NORESERVE) && pages > 0x10000;
        // [T-ish-noreserve-lazy] PROT_NONE with MAP_NORESERVE from 1MB: what
        // the JVM reserves (os::reserve_memory), its 240MB code cache, 64MB
        // metaspace nodes, G1's side tables, and what it uncommits. A host
        // PROT_NONE mapping costs no memory, but it costs address space, and
        // every guest process shares the app's one, which iOS caps: a few JVMs
        // at once (Maven, the javadoc it forks, another build) and one could no
        // longer start, "Failed to reserve memory for metaspace". Smaller ones
        // (V8's 256KB pages, guard pages) keep their host mapping, so the
        // reservation list, searched linearly, stays short.
        bool reserve_only = is_prot_none && (flags & MMAP_NORESERVE) &&
            pages >= NORESERVE_LAZY_MIN_PAGES;
        // PROT_NONE from 128MB, even without MAP_NORESERVE: V8 reserves its
        // heap cage in 128MB chunks and mprotect's sub-regions RW as it
        // allocates. Allocating all of it up front would waste ~640MB per
        // node process.
        bool cage_chunk = is_prot_none && pages >= 0x8000;
        if (big_noreserve || reserve_only || cage_chunk) {
            // Where we placed it ourselves, a big NORESERVE reservation goes
            // on its natural alignment when the hole allows. Never where the
            // guest asked for an address: MAP_FIXED must be at it, and a hint
            // we honoured too, or the JVM, which reserves at chosen addresses,
            // takes it for a miss.
            if (big_noreserve && addr == 0) {
                pages_t align_pages = pages;
                if (align_pages > 0x40000) align_pages = 0x40000;
                page_t aligned = (page / align_pages) * align_pages;
                if (aligned >= MMAP_HOLE_END && pt_is_hole(current->mem, aligned, pages))
                    page = aligned;
            }
            // MAP_FIXED replaces what was there, as the eager path's pt_map
            // does: the JVM uncommits with mmap(MAP_FIXED | MAP_NORESERVE,
            // PROT_NONE), and V8 decommits with MAP_FIXED PROT_NONE. Without
            // this the old pages stayed, readable and writable, their memory
            // held, their contents there again at the next commit.
            if (flags & MMAP_FIXED)
                pt_unmap_always(current->mem, page, pages);
            if ((err = pt_map_lazy(current->mem, page, pages, prot)) < 0)
                return err;
            return page << PAGE_BITS;
        }
#if ANON_MMAP_LIMIT_PAGES > 0
        if (!is_prot_none && !anon_pages_reserve((long)pages)) {
            anon_limit_report("mmap", (long)pages);
            return _ENOMEM;
        }
#endif
        if ((err = pt_map_nothing(current->mem, page, pages, prot)) < 0) {
#if ANON_MMAP_LIMIT_PAGES > 0
            if (!is_prot_none)
                anon_pages_unreserve((long)pages);
#endif
            return err;
        }
    } else {
        // fd must be valid
        struct fd *fd = f_get(fd_no);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops->mmap == NULL)
            return _ENODEV;
        if ((err = fd->ops->mmap(fd, current->mem, page, pages, offset, prot, flags)) < 0)
            return err;
        pt_set_file(current->mem, page, pages, fd, offset);
        // its code may be translated for others sharing the asbestos that
        // map the same (dlopen in each worker of a pool): what bytes it has
        if (!(prot & P_WRITE) && !(flags & MMAP_SHARED))
            mem_set_code_tags(current->mem, page, pages, fd, offset);
    }
    return page << PAGE_BITS;
}

static addr_t mmap_common(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    STRACE("mmap(0x%x, 0x%x, 0x%x, 0x%x, %d, %d)", addr, len, prot, flags, fd_no, offset);
    if (len == 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;

    write_wrlock(&current->mem->lock);
    addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    write_wrunlock(&current->mem->lock);
    return res;
}

addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    return mmap_common(addr, len, prot, flags, fd_no, offset << PAGE_BITS);
}

// ARM64 mmap syscall: offset is passed directly (not shifted like mmap2)
// and takes 6 direct arguments (not a pointer to a struct like x86 mmap)
addr_t sys_mmap64(addr_t addr, addr_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    STRACE("mmap64(0x%llx, 0x%llx, 0x%x, 0x%x, %d, 0x%llx)", (unsigned long long)addr, (unsigned long long)len, prot, flags, fd_no, (unsigned long long)offset);
    if (len == 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;

    write_wrlock(&current->mem->lock);
    // all 64 bits of it: cut to 32, a file mapped at 5GB was mapped at 1GB
    // (CPython's test_mmap test_large_offset)
    addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    write_wrunlock(&current->mem->lock);
    return res;
}

struct mmap_arg_struct {
    dword_t addr, len, prot, flags, fd, offset;
};

addr_t sys_mmap(addr_t args_addr) {
    struct mmap_arg_struct args;
    if (user_get(args_addr, args))
        return _EFAULT;
    return mmap_common(args.addr, args.len, args.prot, args.flags, args.fd, args.offset);
}

// Diagnostic (ISH_VMA_TRACE=1): log munmap/madvise/mprotect that touch a
// file-backed mapping. Used to track how guest runtimes reclaim file-backed
// regions (e.g. bun MADV_DONTNEED'ing its embedded bytecode cache).
static int vma_trace_hits_file(addr_t addr, addr_t len) {
    extern char *getenv(const char *);
    if (!getenv("ISH_VMA_TRACE")) return 0;
    addr_t end = addr + len;
    int found = 0;
    read_wrlock(&current->mem->lock);
    for (addr_t p = addr; p < end && !found; p += PAGE_SIZE) {
        struct pt_entry *pt = mem_pt(current->mem, PAGE(p));
        if (pt != NULL && !(pt->flags & P_ANONYMOUS) &&
                pt->data != NULL && pt->data->fd != NULL)
            found = 1;
    }
    read_wrunlock(&current->mem->lock);
    return found;
}

int_t sys_munmap(addr_t addr, addr_t len) {
    STRACE("munmap(0x%llx, 0x%llx)", (unsigned long long)addr, (unsigned long long)len);
    if (vma_trace_hits_file(addr, len))
        fprintf(stderr, "[vma] munmap(0x%llx, 0x%llx) [file-backed] pid=%d\n",
                (unsigned long long)addr, (unsigned long long)len, current->pid);
    if (getenv("ISH_PROT_TRACE")) {
        addr_t end = addr + len;
        if (end > 0xed000000ULL && addr < 0xf0000000ULL) {
            fprintf(stderr, "[PROT_TRACE] munmap(0x%llx, 0x%llx)\n",
                    (unsigned long long)addr, (unsigned long long)len);
        }
    }
    pages_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    write_wrlock(&current->mem->lock);
    int err = pt_unmap_always(current->mem, PAGE(addr), pages);
    write_wrunlock(&current->mem->lock);
    if (err < 0)
        return _EINVAL;
    return 0;
}

#define MREMAP_MAYMOVE_ 1
#define MREMAP_FIXED_ 2

static addr_t mremap_locked(addr_t addr, addr_t old_len, addr_t new_len, dword_t flags);

// Under the mem's lock, as every other change of its page table: it was
// taken without, another thread faulting or mapping meanwhile. Lengths are
// 64-bit, as arm64's registers give them.
addr_t sys_mremap(addr_t addr, addr_t old_len, addr_t new_len, dword_t flags) {
    write_wrlock(&current->mem->lock);
    addr_t res = mremap_locked(addr, old_len, new_len, flags);
    write_wrunlock(&current->mem->lock);
    return res;
}

static addr_t mremap_locked(addr_t addr, addr_t old_len, addr_t new_len, dword_t flags) {
    STRACE("mremap(%#llx, %#llx, %#llx, %d)", (unsigned long long) addr, (unsigned long long) old_len,
           (unsigned long long) new_len, flags);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (flags & ~(MREMAP_MAYMOVE_ | MREMAP_FIXED_))
        return _EINVAL;
    if (flags & MREMAP_FIXED_) {
        FIXME("missing MREMAP_FIXED");
        return _EINVAL;
    }
    // Whole pages, as Linux's PAGE_ALIGN: rounded down, a mapping shrunk to
    // less than a page was taken for none, and its last page unmapped (CPython's
    // mmap.resize(512) of a page at an offset, test_mmap's test_offset: the
    // read after was a SIGSEGV). A length of 0 is EINVAL.
    if (new_len == 0)
        return _EINVAL;
    pages_t old_pages = PAGE_ROUND_UP(old_len);
    pages_t new_pages = PAGE_ROUND_UP(new_len);

    // shrinking always works
    if (new_pages <= old_pages) {
        int err = pt_unmap(current->mem, PAGE(addr) + new_pages, old_pages - new_pages);
        if (err < 0)
            return _EFAULT;
        return addr;
    }

    struct pt_entry *entry = mem_pt(current->mem, PAGE(addr));
    if (entry == NULL)
        return _EFAULT;
    dword_t pt_flags = entry->flags;
    // all of it one mapping, as Linux's vma_to_resize: a page not mapped was
    // read through a NULL entry (&& for ||), and iSH crashed
    for (page_t page = PAGE(addr); page < PAGE(addr) + old_pages; page++) {
        entry = mem_pt(current->mem, page);
        if (entry == NULL || entry->flags != pt_flags)
            return _EFAULT;
    }
    if (!(pt_flags & P_ANONYMOUS)) {
        FIXME("mremap grow on file mappings");
        return _EFAULT;
    }
    page_t extra_start = PAGE(addr) + old_pages;
    pages_t extra_pages = new_pages - old_pages;
    if (pt_is_hole(current->mem, extra_start, extra_pages)) {
        int err = pt_map_nothing(current->mem, extra_start, extra_pages, pt_flags);
        if (err < 0)
            return err;
        return addr;
    }

    // No room where it is: with MREMAP_MAYMOVE it moves, as Linux's
    // move_vma, where it was ENOMEM (CPython's mmap.resize of an anonymous
    // mapping, test_resize_up_private_anonymous_mapping; musl's realloc of
    // a large block copies instead). The growth is mapped first, where it
    // can fail; then each old page's data at the new place, as it was (its
    // flags, a fork's copy-on-write and a shared mapping's sharing with it),
    // and the old range unmapped.
    if (!(flags & MREMAP_MAYMOVE_))
        return _ENOMEM;
    page_t dest = pt_find_hole(current->mem, new_pages);
    if (dest == BAD_PAGE)
        return _ENOMEM;
    int err = pt_map_nothing(current->mem, dest + old_pages, extra_pages, pt_flags);
    if (err < 0)
        return err;
    for (pages_t i = 0; i < old_pages; i++) {
        struct pt_entry *pt = mem_pt(current->mem, PAGE(addr) + i);
        struct data *data = pt->data;
        size_t offset = pt->offset;
        unsigned page_flags = pt->flags;
        pt_map_data(current->mem, dest + i, 1, data, offset, page_flags);
    }
    pt_unmap(current->mem, PAGE(addr), old_pages);
    return (addr_t) dest << PAGE_BITS;
}

int_t sys_mprotect(addr_t addr, addr_t len, int_t prot) {
    STRACE("mprotect(0x%llx, 0x%llx, 0x%x)", (unsigned long long)addr, (unsigned long long)len, prot);
    if (getenv("ISH_PROT_TRACE")) {
        // Trace mprotect that touches the node binary range (V8 codespace candidate).
        addr_t end = addr + len;
        if (end > 0xed000000ULL && addr < 0xf0000000ULL) {
            fprintf(stderr, "[PROT_TRACE] mprotect(0x%llx, 0x%llx, prot=0x%x)\n",
                    (unsigned long long)addr, (unsigned long long)len, prot);
        }
    }
    if (vma_trace_hits_file(addr, len))
        fprintf(stderr, "[vma] mprotect(0x%llx, 0x%llx, prot=0x%x) [file-backed] pid=%d\n",
                (unsigned long long)addr, (unsigned long long)len, prot, current->pid);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    write_wrlock(&current->mem->lock);
    int err = pt_set_flags(current->mem, PAGE(addr), pages, prot);
    write_wrunlock(&current->mem->lock);
    return err;
}

// Pages MADV_DONTNEED zeroed in place, for /proc/ish/host: a guest gives them
// back (JSC's scavenger), and they stay the app's, written with zeros
_Atomic uint64_t st_dontneed_zeroed;

dword_t sys_madvise(addr_t addr, dword_t len, dword_t advice) {
    STRACE("madvise(0x%llx, 0x%x, %d)", (unsigned long long)addr, len, advice);
    // tests/regress/regress_jit_fault.c, with ISH_JIT_FAULT_TEST set: the host
    // memory behind [addr, addr + len) goes away until an access faults on
    // it, the TLB left as it is. len is whole host pages.
    if (advice == HOST_FAULT_TEST_ADVICE) {
        read_wrlock(&current->mem->lock);
        void *host = mem_ptr(current->mem, addr, MEM_READ);
        read_wrunlock(&current->mem->lock);
        if (host == NULL || (uintptr_t) host % real_page_size != 0 || len % real_page_size != 0)
            return _EINVAL;
        return host_fault_test_arm(host, len) ? 0 : _EINVAL;
    }
    if (advice == 4 && vma_trace_hits_file(addr, len))
        fprintf(stderr, "[vma] madvise(0x%llx, 0x%x, DONTNEED) [file-backed] pid=%d\n",
                (unsigned long long)addr, len, current->pid);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    // Linux returns ENOMEM if any page in the range is not part of a mapping.
    // This is load-bearing, not pedantry: musl's pthread_getattr_np probes the
    // main-thread stack extent one page at a time with madvise(MADV_NORMAL)
    // and relies on ENOMEM at the first unmapped page. Always returning
    // success made it report the stack as RLIMIT_STACK-sized (128MB); bun then
    // moves SP to that "bottom" and zeroes the whole range, faulting at the
    // rlimit boundary (deterministic startup SIGSEGV at stack_top - 128MB).
    // Lazy reservations count as mapped — natively they'd be PROT_NONE VMAs.
    {
        read_wrlock(&current->mem->lock);
        for (page_t page = PAGE(addr); page < PAGE(addr) + PAGE_ROUND_UP(len); page++) {
            if (mem_pt(current->mem, page) == NULL &&
                    mem_find_reservation(current->mem, page) == NULL) {
                read_wrunlock(&current->mem->lock);
                return _ENOMEM;
            }
        }
        read_wrunlock(&current->mem->lock);
    }
    // MADV_FREE (8) is purely advisory: the kernel MAY reclaim the pages under
    // memory pressure, but until then reads still return the old contents.
    // Eagerly zeroing them is both wrong (a subsequent read that races the
    // reclaim must see either old-or-zero, never a torn mix) and dangerous
    // under threads — another thread aliasing the page via a live TLB entry
    // would observe it turn to zero mid-computation. JSC's scavenger uses
    // MADV_FREE heavily; treat it as a no-op (safe: we just keep the memory).
    if (advice == 8 /* MADV_FREE */)
        return 0;

    // Diagnostic: ISH_NO_DONTNEED=1 treats MADV_DONTNEED as a no-op (like
    // MADV_FREE) to test whether the in-place zeroing loses concurrent writes.
    {
        static int no_dontneed = -1;
        if (no_dontneed == -1) {
            const char *e = getenv("ISH_NO_DONTNEED");
            no_dontneed = (e && e[0] == '1') ? 1 : 0;
        }
        if (no_dontneed && advice == 4)
            return 0;
    }
    if (advice == 4 /* MADV_DONTNEED */) {
        // MADV_DONTNEED semantics differ by mapping type:
        //   - ANONYMOUS private: the next access must see zero-fill. We zero
        //     the backing in place.
        //   - FILE-BACKED private (MAP_PRIVATE over a file): DONTNEED discards
        //     the private (CoW) copy so the NEXT access re-reads the ORIGINAL
        //     FILE CONTENTS — NOT zero. bun does exactly this to reclaim the
        //     physical pages of its embedded bytecode cache (.bun section, a
        //     164MB MAP_PRIVATE file mapping) after decoding, expecting the
        //     bytes to transparently page back in. Zeroing them here corrupted
        //     the cache mid-decode → UnlinkedCodeBlock numCalleeLocals read as
        //     0 → llint prologue stack-frame underflow → SIGSEGV (the
        //     claude-cli crash). For a private file mapping whose pages we
        //     haven't diverged from the file (the common read-only case), the
        //     file contents are already present in the backing, so restoring
        //     original file content is a no-op — safest is to leave them.
        // MADV_DONTNEED is issued on HUGE mappings (bun MADV_DONTNEEDs its
        // ~248MB embedded .bun bytecode cache on exit = ~60k pages), so the
        // per-page cost dominates. Two rules keep it cheap, both established by
        // history and re-confirmed here:
        //   1. Walk under a READ lock, not a write lock. Only the rare page that
        //      actually needs zeroing takes a brief write lock. A per-page write
        //      lock over 60k pages contends hard with the JIT reader threads and,
        //      on-device, shows up as a multi-second exit stall.
        //   2. Skip pages that don't need work:
        //      - not in the page table (lazy/unmaterialized) — a demand-faulted
        //        page is already zero (fix 5ab5c2d7).
        //      - file-backed private — MUST NOT be zeroed (zeroing bun's cache
        //        is the original ad9cdf74 crash); preserving it already yields
        //        the correct (clean) contents. We deliberately do NOT pread each
        //        page to emulate the dirty-page revert: that reintroduced a
        //        ~60k-synchronous-read exit hang (claude printed its version
        //        then never returned, main thread parked in sys_madvise→pread).
        addr_t end = addr + len;
        bool any = false;
        for (addr_t p = addr; p < end; p += PAGE_SIZE) {
            read_wrlock(&current->mem->lock);
            // A shared mapping's pages keep their contents, as on Linux: a
            // later access reads them again from what they share (MAP_SHARED
            // anonymous memory, a System V shared memory segment).
            struct pt_entry *pt = mem_pt(current->mem, PAGE(p));
            bool skip = pt == NULL || (pt->flags & P_SHARED) ||
                        (!(pt->flags & P_ANONYMOUS) &&
                         pt->data != NULL && pt->data->fd != NULL);
            if (skip) {
                read_wrunlock(&current->mem->lock);
                continue;
            }
            void *ptr = mem_ptr(current->mem, p, MEM_WRITE);
            if (ptr != NULL) {
                memset(ptr, 0, PAGE_SIZE);
                atomic_fetch_add_explicit(&st_dontneed_zeroed, 1, memory_order_relaxed);
                any = true;
            }
            read_wrunlock(&current->mem->lock);
        }
        if (any)
            mem_changed_pub(current->mem, PAGE(addr), PAGE(end - 1) - PAGE(addr) + 1);
    }
    return 0;
}

dword_t sys_mbind(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(mode),
        addr_t UNUSED(nodemask), dword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

int_t sys_mlock(addr_t UNUSED(addr), dword_t UNUSED(len)) {
    return 0;
}

// A shared file mapping is the host's (MAP_SHARED), its writes the file's as
// they are made, so there is nothing to write back. What Linux refuses is
// refused: an address not page-aligned and flags it has not, or both
// MS_ASYNC and MS_SYNC, are EINVAL (CPython's mmap.flush at an offset not
// page-aligned, test_flush_return_value); a range with a page not mapped
// ENOMEM, a page reserved and not yet touched being mapped. It said success
// of anything.
#define MS_ASYNC_ 1
#define MS_INVALIDATE_ 2
#define MS_SYNC_ 4
int_t sys_msync(addr_t addr, addr_t len, int_t flags) {
    STRACE("msync(%#llx, %#llx, %#x)", (unsigned long long) addr, (unsigned long long) len, flags);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (flags & ~(MS_ASYNC_ | MS_INVALIDATE_ | MS_SYNC_))
        return _EINVAL;
    if ((flags & MS_ASYNC_) && (flags & MS_SYNC_))
        return _EINVAL;
    int err = 0;
    read_wrlock(&current->mem->lock);
    for (page_t page = PAGE(addr); page < PAGE(addr) + PAGE_ROUND_UP(len); page++) {
        if (mem_pt(current->mem, page) == NULL && mem_find_reservation(current->mem, page) == NULL) {
            err = _ENOMEM;
            break;
        }
    }
    read_wrunlock(&current->mem->lock);
    return err;
}

// membarrier(2). JSC's concurrent JIT and WTF's parking-lot use this to make
// their sequentially-consistent-fence-free fast paths safe: they omit the
// per-thread barrier on the common path and issue a process-wide barrier via
// membarrier on the slow path (RegisterState publish, GC handshake). Without
// it (our old ENOSYS stub) JSC has no fallback that works, and a compiler
// thread can publish a half-decoded UnlinkedCodeBlock that the main thread
// then reads as zeros — the claude-cli numCalleeLocals=0 crash.
//
// In iSH every guest thread is a native host thread over shared native
// memory, so a real host barrier here plus the ordering already present on
// the OTHER threads' next memory op gives the required guarantee. The
// EXPEDITED variants are synchronous: the caller must observe all other
// threads' prior accesses. We approximate this by a strong host fence; on the
// ARM64/x86 hosts iSH targets this is a full DMB ISH / MFENCE, which — paired
// with the fact that reader threads re-load through the TLB with acquire
// semantics — is sufficient. QUERY reports the commands we support.
#define MEMBARRIER_CMD_QUERY                     0
#define MEMBARRIER_CMD_GLOBAL                    (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED          (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED         (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (1 << 4)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE (1 << 5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1 << 6)

int_t sys_membarrier(int_t cmd, uint_t flags, int_t UNUSED(cpu_id)) {
    STRACE("membarrier(%d, 0x%x)", cmd, flags);
    if (getenv("ISH_MEMBARRIER_TRACE"))
        fprintf(stderr, "[membarrier] pid=%d cmd=%d flags=0x%x\n",
                current->pid, cmd, flags);
    // The only defined flag is MEMBARRIER_CMD_FLAG_CPU (1), valid solely with
    // the CPU-targeted variants we don't implement. For every command we do
    // handle, a non-zero flags argument is invalid — match Linux and reject it.
    if (flags != 0)
        return _EINVAL;
    switch (cmd) {
        case MEMBARRIER_CMD_QUERY:
            // Advertise the private/global expedited commands + registration.
            return MEMBARRIER_CMD_GLOBAL
                 | MEMBARRIER_CMD_GLOBAL_EXPEDITED
                 | MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED
                 | MEMBARRIER_CMD_PRIVATE_EXPEDITED
                 | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED
                 | MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE
                 | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE;

        // Registration commands: nothing to set up (any thread may issue the
        // expedited barrier). Native Linux requires prior registration, so
        // JSC always calls these first; return success.
        case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
        case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
        case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
            return 0;

        // Barrier commands: issue a real process-wide-strength host fence.
        case MEMBARRIER_CMD_GLOBAL:
        case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
        case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
        case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            __sync_synchronize();
            return 0;

        default:
            return _EINVAL;
    }
}

addr_t sys_brk(addr_t new_brk) {
    STRACE("brk(0x%x)", new_brk);
    struct mm *mm = current->mm;
    write_wrlock(&mm->mem.lock);
    if (new_brk < mm->start_brk)
        goto out;
    addr_t old_brk = mm->brk;

    if (new_brk > old_brk) {
        // expand heap: map region from old_brk to new_brk
        // round up because of the definition of brk: "the first location after the end of the uninitialized data segment." (brk(2))
        // if the brk is 0x2000, page 0x2000 shouldn't be mapped, but it should be if the brk is 0x2001.
        page_t start = PAGE_ROUND_UP(old_brk);
        pages_t size = PAGE_ROUND_UP(new_brk) - PAGE_ROUND_UP(old_brk);
        if (!pt_is_hole(&mm->mem, start, size))
            goto out;
#if ANON_MMAP_LIMIT_PAGES > 0
        if (!anon_pages_reserve((long)size)) {
            anon_limit_report("brk", (long)size);
            goto out;
        }
#endif
        // readable and writable, as Linux's heap (maps said -w-p of it)
        int err = pt_map_nothing(&mm->mem, start, size, P_READ | P_WRITE);
        if (err < 0) {
#if ANON_MMAP_LIMIT_PAGES > 0
            anon_pages_unreserve((long)size);
#endif
            goto out;
        }
    } else if (new_brk < old_brk) {
        // shrink heap: unmap pages that are entirely above new_brk
        // PAGE_ROUND_UP(new_brk) is the first page we can safely unmap
        // (the page containing new_brk may still have live data below new_brk)
        page_t first_unmap = PAGE_ROUND_UP(new_brk);
        page_t last_unmap = PAGE_ROUND_UP(old_brk);
        if (first_unmap < last_unmap)
            pt_unmap_always(&mm->mem, first_unmap, last_unmap - first_unmap);
    }

    mm->brk = new_brk;
out:;
    addr_t brk = mm->brk;
    write_wrunlock(&mm->mem.lock);
    return brk;
}
