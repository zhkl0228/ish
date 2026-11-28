#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/futex.h"

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_REQUEUE_ 3
#define FUTEX_CMP_REQUEUE_ 4
#define FUTEX_WAKE_OP_ 5
#define FUTEX_WAIT_BITSET_ 9
#define FUTEX_WAKE_BITSET_ 10
#define FUTEX_PRIVATE_FLAG_ 128
#define FUTEX_CLOCK_REALTIME_ 256
#define FUTEX_CMD_MASK_ ~(FUTEX_PRIVATE_FLAG_ | FUTEX_CLOCK_REALTIME_)

// FUTEX_WAKE_OP val3 encoding (see Linux kernel futex.h)
#define FUTEX_OP_SET   0
#define FUTEX_OP_ADD   1
#define FUTEX_OP_OR    2
#define FUTEX_OP_ANDN  3
#define FUTEX_OP_XOR   4
#define FUTEX_OP_OPARG_SHIFT 8

#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP_CMP_NE 1
#define FUTEX_OP_CMP_LT 2
#define FUTEX_OP_CMP_LE 3
#define FUTEX_OP_CMP_GT 4
#define FUTEX_OP_CMP_GE 5

// What a futex is known by, as Linux's futex keys. A private one (with
// FUTEX_PRIVATE_FLAG, or on a page no other address space shares) by its
// address space and address. A process-shared one on a shared page by what
// the page is, so that a wake in one process finds a wait in another: for a
// shared mapping of a file (sem_open's in /dev/shm, which each process maps
// for itself) the host file and the word's offset in it; for shared memory
// of no file (MAP_SHARED | MAP_ANONYMOUS across fork, System V shm) the
// word's host address, which is the same in every address space it is in.
// They were all known by address space and address: a wake never reached a
// waiter in another process, which went on only at the 100 ms re-check (a
// round trip of Python's multiprocessing.Semaphore took 100-117 ms).
enum futex_key_kind { FUTEX_KEY_PRIVATE, FUTEX_KEY_FILE, FUTEX_KEY_MEMORY };
struct futex_key {
    enum futex_key_kind kind;
    const void *space;  // private: the address space
    uint64_t a, b, c;   // private: the address; file: dev, inode, offset; memory: the host address
};

struct futex {
    atomic_uint refcount;
    struct futex_key key;
    struct list queue;
    struct list chain; // locked by futex_hash_lock
};

struct futex_wait {
    cond_t cond;
    struct futex *futex; // will be changed by a requeue
    struct list queue;
    struct task *task;   // owning task (for per-thread futex_pipe wakeup)
};

#define FUTEX_HASH_BITS 12
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)
static lock_t futex_lock = LOCK_INITIALIZER;
static struct list futex_hash[FUTEX_HASH_SIZE];

static void __attribute__((constructor)) init_futex_hash(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        list_init(&futex_hash[i]);
}

// The key of the futex at addr: process-shared, without FUTEX_PRIVATE_FLAG
static struct futex_key futex_key_of(addr_t addr, bool shared) {
    struct futex_key key = {.kind = FUTEX_KEY_PRIVATE, .space = current->mem, .a = addr};
    if (!shared)
        return key;
    read_wrlock(&current->mem->lock);
    struct pt_entry *pt = mem_pt(current->mem, PAGE(addr));
    if (pt != NULL && (pt->flags & P_SHARED)) {
        struct data *data = pt->data;
        size_t at = pt->offset + PGOFFSET(addr);
        struct stat st;
        if (data->fd != NULL && fstat(data->fd->real_fd, &st) == 0)
            key = (struct futex_key) {.kind = FUTEX_KEY_FILE, .a = st.st_dev, .b = st.st_ino, .c = data->file_base + at};
        else
            key = (struct futex_key) {.kind = FUTEX_KEY_MEMORY, .c = (uintptr_t) data->data + at};
    }
    read_wrunlock(&current->mem->lock);
    return key;
}

static bool futex_key_equal(const struct futex_key *x, const struct futex_key *y) {
    return x->kind == y->kind && x->space == y->space && x->a == y->a && x->b == y->b && x->c == y->c;
}

static struct futex *futex_get_unlocked(const struct futex_key *key) {
    uint64_t h = (uintptr_t) key->space ^ key->a ^ (key->b * 0x9e3779b97f4a7c15ull) ^ key->c;
    struct list *bucket = &futex_hash[(h ^ (h >> 12) ^ (h >> 24)) % FUTEX_HASH_SIZE];
    struct futex *futex;
    list_for_each_entry(bucket, futex, chain) {
        if (futex_key_equal(&futex->key, key)) {
            futex->refcount++;
            return futex;
        }
    }

    futex = malloc(sizeof(struct futex));
    if (futex == NULL) {
        unlock(&futex_lock);
        return NULL;
    }
    futex->refcount = 1;
    futex->key = *key;
    list_init(&futex->queue);
    list_add(bucket, &futex->chain);
    return futex;
}

// Returns the futex of the given key, and locks it
// Unlocked variant is available for times when you need to get two futexes at once
static struct futex *futex_get(const struct futex_key *key) {
    lock(&futex_lock);
    struct futex *futex = futex_get_unlocked(key);
    if (futex == NULL)
        unlock(&futex_lock);
    return futex;
}

static void futex_put_unlocked(struct futex *futex) {
    if (--futex->refcount == 0) {
        assert(list_empty(&futex->queue));
        list_remove(&futex->chain);
        free(futex);
    }
}

// Must be called on the result of futex_get when you're done with it
// Also has an unlocked version, for releasing the result of futex_get_unlocked
static void futex_put(struct futex *futex) {
    futex_put_unlocked(futex);
    unlock(&futex_lock);
}

// The word at addr, the current process's
static int futex_load(addr_t addr, dword_t *out) {
    read_wrlock(&current->mem->lock);
    dword_t *ptr = mem_ptr(current->mem, addr, MEM_READ);
    read_wrunlock(&current->mem->lock);
    if (ptr == NULL)
        return 1;
    *out = *ptr;
    return 0;
}

// Debug: per-thread record of the futex address currently being waited on
// (0 when not in futex_wait). Read by the ISH_FUTEX_DBG dumper via pid map.
#define FUTEX_DBG_MAX 64
static struct { int pid; addr_t uaddr; dword_t val; } g_futex_waiters[FUTEX_DBG_MAX];
// Gate the bookkeeping on ISH_FUTEX_DBG: without it, every futex_wait would run
// a 64-entry linear scan on entry AND exit for no reason. JSC/Go hammer futex,
// so keep this off the hot path unless the diagnostic is actually requested.
static int g_futex_dbg = 0;
__attribute__((constructor)) static void init_futex_dbg(void) {
    g_futex_dbg = getenv("ISH_FUTEX_DBG") ? 1 : 0;
}
static void futex_dbg_set(int pid, addr_t uaddr, dword_t val) {
    if (!g_futex_dbg)
        return;
    for (int i = 0; i < FUTEX_DBG_MAX; i++) {
        if (g_futex_waiters[i].pid == pid || g_futex_waiters[i].pid == 0) {
            g_futex_waiters[i].pid = pid;
            g_futex_waiters[i].uaddr = uaddr;
            g_futex_waiters[i].val = val;
            return;
        }
    }
}
addr_t futex_dbg_get(int pid) {
    for (int i = 0; i < FUTEX_DBG_MAX; i++)
        if (g_futex_waiters[i].pid == pid) return g_futex_waiters[i].uaddr;
    return 0;
}

static addr_t futex_watch_addr(void) {
    static addr_t a = 1;
    if (a == 1) { const char *e = getenv("ISH_FUTEX_WATCH"); a = e ? strtoull(e, NULL, 16) : 0; }
    return a;
}

// /proc/ish/threads (kernel/task.h)
_Atomic uint64_t st_futex_waits, st_futex_shared, st_futex_woken, st_futex_missed;

// How often a wait looks again whether its word changed with no wake seen,
// or a signal came with no word of it, and the stall diagnostics' clock.
// Wakes and signals each write the waiter's pipe (futex_wake_queue,
// futex_signal_kick), so one that waits for nothing never wakes but for this.
// It was 100 ms, every waiting thread's host thread woken ten times a second:
// an idle node 12 times a second, java 45, against 1.2 and 1.5 without.
#define FUTEX_RECHECK_MS 30000

static int64_t futex_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000000000LL + now.tv_nsec;
}

// The current task's futex pipe made, if it has none: under waiting_cond_lock,
// which a signal's kick and the task's exit take for it
static bool futex_pipe_make(void) {
    if (current->futex_pipe[0] != -1)
        return true;
    int fds[2];
    if (pipe(fds) < 0)
        return false;
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    lock(&current->waiting_cond_lock);
    current->futex_pipe[0] = fds[0];
    current->futex_pipe[1] = fds[1];
    unlock(&current->waiting_cond_lock);
    return true;
}

// A signal for task: a futex wait it is in ends to take it. Caller holds
// task->waiting_cond_lock. It was told by SIGUSR1 alone, which a wait not yet
// in poll() misses: it saw the signal at its next 100 ms re-check.
void futex_signal_kick(struct task *task) {
    if (task->futex_pipe[1] != -1) {
        char c = 1;
        (void) !write(task->futex_pipe[1], &c, 1);
    }
}

// The current task's futex pipe closed, as it exits
void futex_pipe_close(void) {
    lock(&current->waiting_cond_lock);
    int fds[2] = {current->futex_pipe[0], current->futex_pipe[1]};
    current->futex_pipe[0] = current->futex_pipe[1] = -1;
    unlock(&current->waiting_cond_lock);
    if (fds[0] != -1)
        close(fds[0]);
    if (fds[1] != -1)
        close(fds[1]);
}

static bool futex_signal_pending(void) {
    if (current->sighand == NULL)
        return false;
    lock(&current->sighand->lock);
    bool pending = task_signal_pending(current);
    unlock(&current->sighand->lock);
    return pending;
}

// Debug: dump all threads' state once when stalled past a short gated
// threshold (ISH_FUTEX_DBG=<seconds>). Helps diagnose which thread holds the
// lock the stalled thread is waiting on.
static int futex_dbg_secs(void) {
    static int dbg_secs = -2;
    if (dbg_secs == -2) {
        const char *e = getenv("ISH_FUTEX_DBG");
        dbg_secs = e ? atoi(e) : -1;
    }
    return dbg_secs;
}

static void futex_dbg_dump(addr_t uaddr, dword_t val, int64_t waited_ns) {
    static int dumped = 0;
    if (__atomic_exchange_n(&dumped, 1, __ATOMIC_SEQ_CST) != 0)
        return;
    dword_t curval = 0xffffffff;
    { dword_t *vp = mem_ptr(current->mem, uaddr, MEM_READ); if (vp) curval = *vp; }
    printk("FUTEX-DBG: pid=%d stalled %ds on uaddr=0x%llx (curval=0x%x, waited-for val=%d)\n",
           current->pid, (int) (waited_ns / 1000000000LL), (unsigned long long) uaddr, curval, val);
    lock(&pids_lock);
    struct task *t;
    list_for_each_entry(&current->group->threads, t, group_links) {
        addr_t waddr = futex_dbg_get(t->pid);
        dword_t wv = 0xffffffff;
        if (waddr) { dword_t *vp = mem_ptr(t->mem, waddr, MEM_READ); if (vp) wv = *vp; }
        printk("  thread pid=%d blocking=%d x8=%lld futex_uaddr=0x%llx curval=0x%x pc=0x%llx lr=0x%llx x0=0x%llx x1=0x%llx\n",
               t->pid, t->blocking, (long long)t->cpu.regs[8],
               (unsigned long long)waddr, wv,
               (unsigned long long)t->cpu.pc,
               (unsigned long long)t->cpu.regs[30],
               (unsigned long long)t->cpu.regs[0],
               (unsigned long long)t->cpu.regs[1]);
    }
    unlock(&pids_lock);
}

static int futex_wait(addr_t uaddr, dword_t val, struct timespec *timeout, bool shared) {
    struct futex_key key = futex_key_of(uaddr, shared);
    if (!futex_pipe_make())
        return _ENOMEM;
    struct futex *futex = futex_get(&key);
    if (futex == NULL)
        return _ENOMEM;
    futex_dbg_set(current->pid, uaddr, val);
    if (uaddr == futex_watch_addr()) {
        dword_t rv = 0xdeadbeef; void *hp = NULL;
        { dword_t *vp = mem_ptr(current->mem, uaddr, MEM_READ); if (vp) { rv = *vp; hp = vp; } }
        fprintf(stderr, "[fw] pid=%d WAIT uaddr=0x%llx expect_val=0x%x actual_mem=0x%x hostptr=%p %s\n",
                current->pid, (unsigned long long)uaddr, val, rv, hp,
                rv != val ? "(MISMATCH→EAGAIN)" : "(match→sleep)");
    }
    int err = 0;
    dword_t tmp;
    if (futex_load(uaddr, &tmp))
        err = _EFAULT;
    else if (tmp != val)
        err = _EAGAIN;
    else {
        // Drain any stale bytes from previous wakeups: a signal's kick comes
        // after the signal is pending, so one it was for is seen below
        { char buf[16]; while (read(current->futex_pipe[0], buf, sizeof(buf)) > 0) {} }

        struct futex_wait wait;
        wait.cond = COND_INITIALIZER;
        wait.futex = futex;
        wait.task = current;

        // Stay in queue so futex_wakelike can find us and write to our pipe
        list_add_tail(&futex->queue, &wait.queue);
        unlock(&futex_lock);

        // Calculate deadline for timed waits
        int64_t start_ns = futex_now_ns();
        int64_t deadline_ns = 0; // 0 = infinite
        if (timeout)
            deadline_ns = start_ns + (int64_t)timeout->tv_sec * 1000000000LL + timeout->tv_nsec;
        int recheck_ms = futex_dbg_secs() > 0 ? 1000 : FUTEX_RECHECK_MS;
        bool stall_said = false;

        current->blocking = true;
        atomic_fetch_add_explicit(&st_futex_waits, 1, memory_order_relaxed);
        if (key.kind != FUTEX_KEY_PRIVATE)
            atomic_fetch_add_explicit(&st_futex_shared, 1, memory_order_relaxed);
        struct pollfd pfd = { .fd = current->futex_pipe[0], .events = POLLIN };
        // A signal pending before the wait began ends it, as one that comes
        // in it does (its kick)
        if (current->group->doing_group_exit || futex_signal_pending())
            err = _EINTR;
        else for (;;) {
            // Compute poll timeout
            int poll_ms = recheck_ms;
            if (deadline_ns) {
                int64_t remain_ns = deadline_ns - futex_now_ns();
                if (remain_ns <= 0) { err = _ETIMEDOUT; break; }
                // [T-ish-futex-subms-timeout] This used to be
                // `remain_ms = remain / 1e6; if (remain_ms <= 0) ETIMEDOUT`:
                // integer truncation made every sub-millisecond timeout — and
                // the final <1 ms of every longer one — return ETIMEDOUT
                // WITHOUT waiting. Go's runtime sleeps on futexes for
                // "nanoseconds until the next timer", which is usually
                // sub-ms, so each wait came straight back, the scheduler
                // re-checked nanotime() and netpoll(0) and slept again: an
                // idle tsproxy burned ~35% of a core at ~98k syscalls/s
                // (futex / clock_gettime / epoll_pwait, [CPUTop] 2026-09-19).
                // Sub-ms remainders now sleep for exactly that long on the
                // host (a wake in the meantime is picked up by the 0 ms poll
                // below, so latency is bounded by the remainder itself);
                // ms-scale remainders poll for the whole milliseconds and
                // leave the fraction to the sub-ms branch on the next turn,
                // so a 1.5 ms wait is 1 ms of poll + 0.5 ms of sleep. A futex
                // may wake late by scheduling slack, never early.
                if (remain_ns < 1000000LL) {
                    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long) remain_ns };
                    nanosleep(&ts, NULL);   // EINTR (signal) just falls through to the checks
                    poll_ms = 0;
                } else {
                    int64_t remain_ms = remain_ns / 1000000LL;   // >= 1 here
                    if (remain_ms < poll_ms)
                        poll_ms = (int) remain_ms;
                }
            }

            bool tick = poll_ms == recheck_ms;
            int ret = poll(&pfd, 1, poll_ms);
            if (ret > 0) {
                char buf[16];
                while (read(current->futex_pipe[0], buf, sizeof(buf)) > 0) {}
            }
            // Woken: futex_wake_queue took it off its queue (that of the
            // futex a requeue moved it to, maybe)
            lock(&futex_lock);
            bool woken = list_null(&wait.queue);
            bool requeued = wait.futex != futex;
            unlock(&futex_lock);
            if (woken) {
                atomic_fetch_add_explicit(&st_futex_woken, 1, memory_order_relaxed);
                err = 0;
                break;
            }
            // Else a signal's kick, a host signal (EINTR), or the time
            if (current->group->doing_group_exit || futex_signal_pending()) {
                err = _EINTR;
                break;
            }
            // A kick for a signal another thread took, or one ignored; or the
            // deadline, which the top sees
            if (ret != 0 || !tick)
                continue;
            int64_t now_ns = futex_now_ns();
            // The re-check: the word changed with no wake seen. A requeued
            // wait's word is another's.
            if (!requeued) {
                dword_t now_val;
                if (futex_load(uaddr, &now_val)) { err = _EFAULT; break; }
                if (now_val != val) {
                    // One lost, or not yet sent (its waker between its store
                    // and its wake): either way the waiter goes on
                    uint64_t missed = atomic_fetch_add_explicit(&st_futex_missed, 1, memory_order_relaxed);
                    if (missed < 20)
                        printk("futex: pid %d (%s) found its word at %#llx changed at its re-check after %lld ms, "
                               "no wake seen (%s, the word %#x, waited for %#x)\n",
                               current->pid, current->comm, (unsigned long long) uaddr,
                               (long long) ((now_ns - start_ns) / 1000000), key.kind == FUTEX_KEY_PRIVATE ? "private" :
                               key.kind == FUTEX_KEY_FILE ? "process-shared, a file's" : "process-shared memory",
                               now_val, val);
                    err = 0;
                    break;
                }
            }
            int dbg_secs = futex_dbg_secs();
            if (dbg_secs > 0 && now_ns - start_ns >= dbg_secs * 1000000000LL)
                futex_dbg_dump(uaddr, val, now_ns - start_ns);
            // [T-ish-futex-valve-removal] This used to be a "safety valve"
            // that exit_group(0)'d the whole process once any one thread had
            // sat in an untimed futex_wait for 180s (5d8e1e1a, added for
            // Node/npm exit hangs). It judged "stuck" from a single thread,
            // so it killed every idle Go program: the Go runtime parks idle
            // Ms in untimed futex waits while the program is perfectly alive
            // (a 15 s time.Sleep loop died at exactly 3:00, reported
            // 2026-09-22), and it reported the kill as a clean exit 0.
            //
            // The hangs it papered over had real causes that are fixed:
            // lost wakes (condvar -> per-thread pipe, plus the value re-check
            // above), exit_group not interrupting blocked siblings
            // (doing_group_exit check above), and FUTEX_CMP_REQUEUE /
            // FUTEX_WAKE_OP returning ENOSYS (a820b16a, 20 min after the
            // valve). With the re-check in place, a wait still stalled here
            // means no thread ever changed the word: either the guest is
            // idle or deadlocked the way it would be on Linux, or iSH lost a
            // wake somewhere else. Neither is the kernel's to kill, and
            // killing destroys the evidence of the second. So only say so,
            // once per wait, without needing ISH_EXEC_TRACE; a stuck command
            // is ended by the caller's own timeout (shell_execute kills the
            // process group and reports it as a timeout).
            if (timeout == NULL && !stall_said && now_ns - start_ns >= 180 * 1000000000LL) {
                stall_said = true;
                int live = 0;
                lock(&pids_lock);
                lock(&current->group->lock);
                struct task *t;
                list_for_each_entry(&current->group->threads, t, group_links)
                    live++;
                unlock(&current->group->lock);
                unlock(&pids_lock);
                printk("FUTEX-STALL: pid=%d (%s) in untimed futex_wait(uaddr=0x%llx val=%d) for 180s, %d threads; not killed (ISH_FUTEX_DBG=<secs> dumps every thread)\n",
                       current->pid, current->comm, (unsigned long long) uaddr, val, live);
            }
        }
        current->blocking = false;

        // Remove from queue (pipe stays open for reuse). Woken all the same
        // as a signal or the deadline ended it, it says so, as Linux's: the
        // wake was this waiter's, and another would not have it.
        lock(&futex_lock);
        if (err != 0 && list_null(&wait.queue)) {
            atomic_fetch_add_explicit(&st_futex_woken, 1, memory_order_relaxed);
            err = 0;
        }
        list_remove_safe(&wait.queue);
        futex_put_unlocked(wait.futex);
        unlock(&futex_lock);
        goto futex_wait_done;
    }
    futex_put(futex);
futex_wait_done:
    futex_dbg_set(current->pid, 0, 0);
    STRACE("%d end futex(FUTEX_WAIT)", current->pid);
    return err;
}

// Wake up to `max` waiters on the given futex. Caller must hold futex_lock.
static unsigned futex_wake_queue(struct futex *futex, dword_t max) {
    struct futex_wait *wait, *tmp;
    unsigned woken = 0;
    list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
        if (woken >= max)
            break;
        // Wake via per-thread pipe write — waiter is blocked on poll(futex_pipe[0])
        if (wait->task->futex_pipe[1] != -1) {
            char c = 1;
            write(wait->task->futex_pipe[1], &c, 1);
        }
        list_remove(&wait->queue);
        woken++;
    }
    return woken;
}

static int futex_wakelike(int op, addr_t uaddr, dword_t wake_max, dword_t requeue_max, addr_t requeue_addr,
                          bool shared) {
    struct futex_key key = futex_key_of(uaddr, shared);
    struct futex_key key2 = op == FUTEX_REQUEUE_ ? futex_key_of(requeue_addr, shared) : key;
    struct futex *futex = futex_get(&key);
    if (futex == NULL)
        return _ENOMEM;

    unsigned woken = futex_wake_queue(futex, wake_max);
    if (uaddr == futex_watch_addr()) {
        dword_t mv = 0xdead; void *hp = NULL;
        { dword_t *vp = mem_ptr(current->mem, uaddr, MEM_READ); if (vp) { mv = *vp; hp = vp; } }
        fprintf(stderr, "[fw] pid=%d WAKE 0x%llx woken=%u mem_after_wake=0x%x hostptr=%p\n",
                current->pid, (unsigned long long)uaddr, woken, mv, hp);
    }

    if (op == FUTEX_REQUEUE_) {
        struct futex *futex2 = futex_get_unlocked(&key2);
        if (futex2 == NULL)
            return _ENOMEM;   // futex_get_unlocked unlocked futex_lock
        struct futex_wait *wait, *tmp;
        unsigned requeued = 0;
        list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
            if (requeued >= requeue_max)
                break;
            // sketchy as hell
            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            assert(futex->refcount > 1); // should be true because this function keeps a reference
            futex->refcount--;
            futex2->refcount++;
            wait->futex = futex2;
            requeued++;
        }
        futex_put_unlocked(futex2);
        woken += requeued;
    }

    futex_put(futex);
    return woken;
}

// A wake of the kernel's: a thread's clear_child_tid as it exits, process-shared
// as Linux's (the word is in a private mapping, as a rule)
int futex_wake(addr_t uaddr, dword_t wake_max) {
    return futex_wakelike(FUTEX_WAKE_, uaddr, wake_max, 0, 0, true);
}

// FUTEX_CMP_REQUEUE: like FUTEX_REQUEUE, but first atomically check *uaddr == expected.
// Returns total woken+requeued, or _EAGAIN on mismatch, or _EFAULT on bad addr.
static int futex_cmp_requeue(addr_t uaddr, dword_t wake_max, dword_t requeue_max,
                             addr_t requeue_addr, dword_t expected, bool shared) {
    struct futex_key key = futex_key_of(uaddr, shared);
    struct futex_key key2 = futex_key_of(requeue_addr, shared);
    struct futex *futex = futex_get(&key);
    if (futex == NULL)
        return _ENOMEM;

    dword_t cur;
    if (futex_load(uaddr, &cur)) {
        futex_put(futex);
        return _EFAULT;
    }
    if (cur != expected) {
        futex_put(futex);
        return _EAGAIN;
    }

    unsigned woken = futex_wake_queue(futex, wake_max);

    struct futex *futex2 = futex_get_unlocked(&key2);
    if (futex2 == NULL)
        return _ENOMEM;   // futex_get_unlocked unlocked futex_lock
    struct futex_wait *wait, *tmp;
    unsigned requeued = 0;
    list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
        if (requeued >= requeue_max)
            break;
        list_remove(&wait->queue);
        list_add_tail(&futex2->queue, &wait->queue);
        assert(futex->refcount > 1);
        futex->refcount--;
        futex2->refcount++;
        wait->futex = futex2;
        requeued++;
    }
    futex_put_unlocked(futex2);

    futex_put(futex);
    return woken + requeued;
}

// FUTEX_WAKE_OP: atomic RMW on *uaddr2, then wake up to val waiters on uaddr,
// and — if the old value of *uaddr2 satisfies the comparison — wake up to val2
// waiters on uaddr2. val3 encodes the op, oparg, cmp, and cmparg (see Linux).
static int futex_wake_op(addr_t uaddr, dword_t wake_max1, addr_t uaddr2,
                         dword_t wake_max2, dword_t val3, bool shared) {
    int op_code = (val3 >> 28) & 0xf;
    int cmp_code = (val3 >> 24) & 0xf;
    // oparg/cmparg are 12-bit sign-extended.
    int32_t oparg  = (int32_t)((val3 << 8)) >> 20;  // bits [23:12] sign-extended
    int32_t cmparg = (int32_t)((val3 << 20)) >> 20; // bits [11:0] sign-extended

    bool shift = (op_code & FUTEX_OP_OPARG_SHIFT) != 0;
    op_code &= ~FUTEX_OP_OPARG_SHIFT;
    if (shift) {
        if (oparg < 0 || oparg > 31)
            return _EINVAL;
        oparg = 1 << oparg;
    }

    // Atomic RMW on *uaddr2.
    // We must hold mem->lock across mem_ptr+atomic op so the mapping can't vanish.
    read_wrlock(&current->mem->lock);
    uint32_t *ptr = mem_ptr(current->mem, uaddr2, MEM_WRITE);
    if (ptr == NULL) {
        read_wrunlock(&current->mem->lock);
        return _EFAULT;
    }
    uint32_t oldval;
    switch (op_code) {
        case FUTEX_OP_SET:
            oldval = __atomic_exchange_n(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ADD:
            oldval = __atomic_fetch_add(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_OR:
            oldval = __atomic_fetch_or(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ANDN:
            oldval = __atomic_fetch_and(ptr, (uint32_t)~oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_XOR:
            oldval = __atomic_fetch_xor(ptr, (uint32_t)oparg, __ATOMIC_SEQ_CST);
            break;
        default:
            read_wrunlock(&current->mem->lock);
            return _ENOSYS;
    }
    read_wrunlock(&current->mem->lock);

    int32_t sold = (int32_t)oldval;
    bool cmp_true;
    switch (cmp_code) {
        case FUTEX_OP_CMP_EQ: cmp_true = (sold == cmparg); break;
        case FUTEX_OP_CMP_NE: cmp_true = (sold != cmparg); break;
        case FUTEX_OP_CMP_LT: cmp_true = (sold <  cmparg); break;
        case FUTEX_OP_CMP_LE: cmp_true = (sold <= cmparg); break;
        case FUTEX_OP_CMP_GT: cmp_true = (sold >  cmparg); break;
        case FUTEX_OP_CMP_GE: cmp_true = (sold >= cmparg); break;
        default: return _ENOSYS;
    }

    // Wake waiters on both queues. Acquire both futexes under futex_lock.
    struct futex_key key1 = futex_key_of(uaddr, shared);
    struct futex_key key2 = futex_key_of(uaddr2, shared);
    lock(&futex_lock);
    struct futex *f1 = futex_get_unlocked(&key1);
    if (f1 == NULL) return _ENOMEM;   // futex_get_unlocked unlocked futex_lock
    unsigned total = futex_wake_queue(f1, wake_max1);
    if (cmp_true) {
        struct futex *f2 = futex_get_unlocked(&key2);
        if (f2 != NULL) {
            total += futex_wake_queue(f2, wake_max2);
            futex_put_unlocked(f2);
        }
    }
    futex_put_unlocked(f1);
    unlock(&futex_lock);
    return total;
}

// Whether a futex call a signal interrupted starts again when no handler ran
// for it, as Linux's: a wait with no time limit (ERESTARTSYS), and
// FUTEX_WAIT_BITSET's to its deadline (as futex_wait_restart: the same
// absolute time). FUTEX_WAIT's own time is relative: started again it would
// wait all of it once more, so its EINTR stays.
bool futex_restarts_without_handler(dword_t op, addr_t timeout) {
    int cmd = op & FUTEX_CMD_MASK_;
    return (cmd == FUTEX_WAIT_ && timeout == 0) || cmd == FUTEX_WAIT_BITSET_;
}

dword_t sys_futex(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3) {
    bool shared = !(op & FUTEX_PRIVATE_FLAG_);
    if (shared) {
        STRACE("!FUTEX_PRIVATE ");
    }
    int cmd = op & FUTEX_CMD_MASK_;
    struct timespec timeout = {0};
    if ((cmd == FUTEX_WAIT_ || cmd == FUTEX_WAIT_BITSET_) && timeout_or_val2) {
        struct timespec_ timeout_;
        if (user_get(timeout_or_val2, timeout_))
            return _EFAULT;
        if (cmd == FUTEX_WAIT_BITSET_) {
            // FUTEX_WAIT_BITSET takes an absolute timeout, on CLOCK_MONOTONIC
            // unless FUTEX_CLOCK_REALTIME is set; make it relative for
            // futex_wait.
            struct timespec now;
            clock_gettime(op & FUTEX_CLOCK_REALTIME_ ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
            timeout.tv_sec = timeout_.sec - now.tv_sec;
            timeout.tv_nsec = timeout_.nsec - now.tv_nsec;
            if (timeout.tv_nsec < 0) {
                timeout.tv_sec--;
                timeout.tv_nsec += 1000000000;
            }
            if (timeout.tv_sec < 0) {
                timeout.tv_sec = 0;
                timeout.tv_nsec = 0;
            }
        } else {
            timeout.tv_sec = timeout_.sec;
            timeout.tv_nsec = timeout_.nsec;
        }
    }
    // An empty bitset matches no waiter; Linux rejects it.
    if ((cmd == FUTEX_WAIT_BITSET_ || cmd == FUTEX_WAKE_BITSET_) && val3 == 0)
        return _EINVAL;
    switch (cmd) {
        case FUTEX_WAIT_:
            STRACE("futex(FUTEX_WAIT, %#x, %d, 0x%x {%ds %dns}) = ...\n", uaddr, val, timeout_or_val2, timeout.tv_sec, timeout.tv_nsec);
            return futex_wait(uaddr, val, timeout_or_val2 ? &timeout : NULL, shared);
        case FUTEX_WAIT_BITSET_:
            // FUTEX_WAIT_BITSET with MATCH_ANY (0xffffffff) is equivalent to FUTEX_WAIT.
            // We ignore the bitset mask (val3) and treat all bits as matching.
            STRACE("futex(FUTEX_WAIT_BITSET, %#x, %d, mask=%#x) = ...\n", uaddr, val, val3);
            return futex_wait(uaddr, val, timeout_or_val2 ? &timeout : NULL, shared);
        case FUTEX_WAKE_:
            STRACE("futex(FUTEX_WAKE, %#x, %d)", uaddr, val);
            return futex_wakelike(FUTEX_WAKE_, uaddr, val, 0, 0, shared);
        case FUTEX_WAKE_BITSET_:
            // FUTEX_WAKE_BITSET with MATCH_ANY is equivalent to FUTEX_WAKE.
            STRACE("futex(FUTEX_WAKE_BITSET, %#x, %d, mask=%#x)", uaddr, val, val3);
            return futex_wakelike(FUTEX_WAKE_, uaddr, val, 0, 0, shared);
        case FUTEX_REQUEUE_:
            STRACE("futex(FUTEX_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_wakelike(FUTEX_REQUEUE_, uaddr, val, timeout_or_val2, uaddr2, shared);
        case FUTEX_CMP_REQUEUE_:
            STRACE("futex(FUTEX_CMP_REQUEUE, %#x, %d, %#x, expected=%d)", uaddr, val, uaddr2, val3);
            return futex_cmp_requeue(uaddr, val, timeout_or_val2, uaddr2, val3, shared);
        case FUTEX_WAKE_OP_:
            STRACE("futex(FUTEX_WAKE_OP, %#x, %d, %#x, %d, %#x)", uaddr, val, uaddr2, timeout_or_val2, val3);
            return futex_wake_op(uaddr, val, uaddr2, timeout_or_val2, val3, shared);
    }
    STRACE("futex(%#x, %d, %d, timeout=%#x, %#x, %d) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
    // Loud diagnostic — these should be rare now that CMP_REQUEUE and WAKE_OP are implemented.
    // Returning _EINVAL instead of _ENOSYS lets most userspace libraries fall back to
    // FUTEX_WAKE/WAIT rather than treating it as "kernel too old" and aborting.
    printk("SYS_FUTEX: unsupported op=%d (cmd=%d) uaddr=%#x val=%d uaddr2=%#x val3=%#x pid=%d comm=%s\n",
           op, cmd, uaddr, val, uaddr2, val3, current->pid, current->comm);
    return _EINVAL;
}

struct robust_list_head_ {
    addr_t list;
    dword_t offset;
    addr_t list_op_pending;
};

int_t sys_set_robust_list(addr_t robust_list, dword_t len) {
    STRACE("set_robust_list(%#x, %d)", robust_list, len);
    if (len != sizeof(struct robust_list_head_))
        return _EINVAL;
    current->robust_list = robust_list;
    return 0;
}

int_t sys_get_robust_list(pid_t_ pid, addr_t robust_list_ptr, addr_t len_ptr) {
    STRACE("get_robust_list(%d, %#x, %#x)", pid, robust_list_ptr, len_ptr);

    lock(&pids_lock);
    struct task *task = pid_get_task(pid);
    unlock(&pids_lock);
    if (task != current)
        return _EPERM;

    if (user_put(robust_list_ptr, current->robust_list))
        return _EFAULT;
    if (user_put(len_ptr, (int[]) {sizeof(struct robust_list_head_)}))
        return _EFAULT;
    return 0;
}
