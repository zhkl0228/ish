#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "emu/tlb.h"
#include "asbestos/asbestos.h"
#include "platform/platform.h"

__thread struct task *current;

static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock = LOCK_INITIALIZER;

static bool pid_empty(struct pid *pid) {
    return pid->task == NULL && list_empty(&pid->session) && list_empty(&pid->pgroup);
}

struct pid *pid_get(dword_t id) {
    if (id >= sizeof(pids)/sizeof(pids[0]))
        return NULL;
    struct pid *pid = &pids[id];
    if (pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && task->zombie)
        return NULL;
    return task;
}

static int cur_pid = 0; // the last pid given out; locked by pids_lock

pid_t_ pid_last_allocated(void) {
    return cur_pid;
}

struct task *task_create_(struct task *parent) {
    lock(&pids_lock);
    do {
        cur_pid++;
        if (cur_pid > MAX_PID) cur_pid = 1;
    } while (!pid_empty(&pids[cur_pid]));
    struct pid *pid = &pids[cur_pid];
    pid->id = cur_pid;
    list_init(&pid->session);
    list_init(&pid->pgroup);

    struct task *task = malloc(sizeof(struct task));
    if (task == NULL) {
        // Release pids_lock before bailing out. Returning while still holding
        // it wedged the whole kernel: every later task_create_, pid_get_task
        // and signal delivery blocks on this lock forever, so an allocation
        // failure turned into a total freeze instead of one failed fork.
        // Also roll back the pid slot claimed above, which would otherwise be
        // permanently reserved for a task that was never created.
        pid->id = 0;
        pid->task = NULL;
        unlock(&pids_lock);
        return NULL;
    }
    *task = (struct task) {};
    if (parent != NULL)
        *task = *parent;
    atomic_init(&task->refs, 1); // the kernel's
    task->pid = pid->id;
    // the parent's own
    task->before_run = NULL;
    task->restart_sleep_pending = false;
    task->start_ticks = get_uptime().uptime_ticks;
    pid->task = task;
    // Copied from the parent above; a new task has not begun exiting.
    atomic_store(&task->exit_started, false);
    // copied from the parent, which is in its clone
    atomic_store(&task->in_syscall, false);

    // Invalidate exclusive monitor after copying parent state.
    // Child must not inherit parent's LDXR reservation, as any context
    // switch or interrupt (including fork/clone) invalidates exclusive state.
    task->cpu.excl_addr = UINT64_MAX;

    // The poke flag must not be inherited: poked_ptr would alias the parent's
    // _poked byte (cross-task pokes), and a copied _poked=true seeds a
    // permanent one-interrupt-per-block storm in the child.
    task->cpu.poked_ptr = &task->cpu._poked;
    task->cpu._poked = false;

    // Initialize blocking state for deadlock detection.
    task->blocking = false;
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        task->last_unblocked_ns = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
    }
    list_init(&task->children);
    list_init(&task->siblings);
    if (parent != NULL) {
        task->parent = parent;
        list_add(&parent->children, &task->siblings);
    }
    unlock(&pids_lock);

    task->pending = 0;
    list_init(&task->queue);
    task->clear_tid = 0;
    task->robust_list = 0;
    task->futex_pipe[0] = -1;
    task->futex_pipe[1] = -1;
    task->did_exec = false;
    lock_init(&task->general_lock);

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    lock_init(&task->waiting_cond_lock);
    cond_init(&task->pause);

    lock_init(&task->ptrace.lock);
    cond_init(&task->ptrace.cond);
    return task;
}

void task_give_children_to_init(struct task *task) {
    struct task *init = pid_get_task(1);
    if (init == task)
        init = NULL;
    struct task *child, *tmp;
    list_for_each_entry_safe(&task->children, child, tmp, siblings) {
        list_remove(&child->siblings);
        child->parent = init;
        if (init != NULL)
            list_add(&init->children, &child->siblings);
    }
}

static void task_unref(struct task *task) {
    if (atomic_fetch_sub(&task->refs, 1) == 1)
        free(task);
}

// Takes the task out of the pid table and its parent's children, and lets go
// of the kernel's hold on its struct. Its host thread may still be running
// it: a thread do_exit_group's safety valve gave up on, stuck in a host call,
// whose leader is reaped meanwhile. That thread holds the struct too, so it
// lives until the thread is done; until then the thread finds it poisoned (no
// group, exiting) and leaves at its next check. (It used to be freed at the
// next task_destroy, whenever that came: a thread still stuck by then read
// freed memory.) Caller holds pids_lock.
void task_destroy(struct task *task) {
    list_remove_safe(&task->siblings);
    pid_get(task->pid)->task = NULL;

    // Children it still has go to init, as do_exit gives them away: a thread
    // the valve gave up on never went through do_exit.
    task_give_children_to_init(task);

    // The guest memory of a thread the valve gave up on is released by the
    // thread when it leaves (task_run_current's cleanup): that is kept, with
    // the lock the cleanup takes around it.
    bool mm_release_deferred = task->mm_release_deferred;
    struct mm *mm = task->mm;
    struct mem *mem = task->mem;
    memset(task, 0, offsetof(struct task, refs));
    lock_init(&task->general_lock);
    if (mm_release_deferred) {
        task->mm_release_deferred = true;
        task->mm = mm;
        task->mem = mem;
    }
    task->exiting = true;
    task_unref(task);
}

struct task_run_cleanup {
    struct tlb *tlb;
    struct task *task;
};

static void task_run_cleanup(void *arg) {
    struct task_run_cleanup *cleanup = arg;
    tlb_detach(cleanup->tlb);   // exited while running guest code
    tlb_free(cleanup->tlb);
    // [T-ish-mm-leak-refcount-handoff] If the do_exit_group safety valve
    // orphaned this thread (stuck in an uninterruptible host syscall) it
    // deferred the mm_release to here — this runs only when the host pthread
    // actually terminates, i.e. AFTER the thread has left the read_wrlock
    // critical section, so releasing now can't UAF the mem lock. This reclaims
    // the whole guest address space instead of leaking it forever. The task is
    // this thread's own, held until the end of this; not current, which the
    // ways out of do_exit set to NULL first.
    struct task *self = cleanup->task;
    if (self->mm_release_deferred && self->mm != NULL) {
        self->mm_release_deferred = false;
        // [T-ish-exit-mm-general-lock] Same handoff as do_exit: unpublish the
        // mm under general_lock so a procfs reader cannot lock it after we
        // destroy it, then release outside the lock.
        lock(&self->general_lock);
        struct mm *mm = self->mm;
        self->mm = NULL;
        self->mem = NULL;
        unlock(&self->general_lock);
        // [T-ish-mm-diag] The other half of the SAFETY-VALVE line: when the
        // leaked thread finally unwinds and releases, and what refcount it
        // finds — a value other than 1 here means a second holder.
        printk("[iSH][DEFERRED-RELEASE] pid=%d mm_release from cleanup handler, mm=%p refcount_before=%u\n",
               self->pid, (void *) mm, (unsigned) mm->refcount);
        mm_release_from(mm, "cleanup_handler");
    }

    // A thread the valve gave up on, done at last: its task leaves the pid
    // table now (do_exit took out the others before they got here, and a
    // leader's goes when it is reaped). It used to stay there for good.
    if (self->exiting && self->pid != 0 && self->pid != self->tgid) {
        lock(&pids_lock);
        if (pid_get_task_zombie(self->pid) == self)
            task_destroy(self);
        unlock(&pids_lock);
    }
    task_unref(self);
}

void task_run_current(void) {
    struct cpu_state *cpu = &current->cpu;
    struct tlb *tlb = calloc(1, sizeof(struct tlb));
    if (!tlb)
        host_out_of_memory("a TLB", sizeof(struct tlb));

    // Register cleanup so the TLB (and its fiber_frame) is freed even when
    // the thread exits via pthread_exit() from deep in handle_interrupt()
    // (e.g. do_exit() after a native-offloaded execve, or SIGKILL path).
    // Without this, every guest process leaks ~304KB of TLB + ~48KB of
    // fiber_frame, dominating app memory after repeated ffmpeg invocations.
    struct task_run_cleanup cleanup = {.tlb = tlb, .task = current};
    pthread_cleanup_push(task_run_cleanup, &cleanup);

    while (true) {
        // Check for group exit before entering JIT — this catches threads
        // returning from blocking host syscalls (futex, nanosleep, etc.)
        // that were interrupted by SIGUSR1 from do_exit_group.
        // Also bail if our task struct was destroyed (current zeroed or NULLed).
        struct task *self = current;
        if (self == NULL || self->group == NULL) {
            // Task struct was destroyed under us (leaked thread).
            // Exit the host thread silently; cleanup handler frees tlb.
            pthread_exit(NULL);
        }
        if (self->group->doing_group_exit) {
            do_exit(self->group->group_exit_code);
        }
        if (self->mem == NULL) {
            pthread_exit(NULL);
        }
#ifdef ISH_JIT
        // between passes, no lock held: where a full code region is flushed
        asbestos_jit_flush();
#endif
        read_wrlock_after_writers(&self->mem->lock);
        tlb->cpu = cpu;
        tlb_attach(tlb, &self->mem->mmu);
        tlb_refresh(tlb, &self->mem->mmu);
        int interrupt = cpu_run_to_interrupt(cpu, tlb);
        tlb_detach(tlb);
        read_wrunlock(&self->mem->lock);
        handle_interrupt(interrupt);
    }

    // Never reached in practice (loop only exits via pthread_exit/do_exit),
    // but the pop is required for pthread_cleanup_push/pop balance.
    pthread_cleanup_pop(1);
}

static void *task_thread(void *vtask) {
    current = vtask;
    update_thread_name();
    if (current->before_run != NULL) {
        void (*before_run)(void) = current->before_run;
        current->before_run = NULL;
        before_run();
    }
    task_run_current();
    die("task_thread returned"); // above function call should never return
}

// A guest thread's host stack: 512KB, what iOS and macOS give a thread, set
// so that every host gives the same (musl's default is 128KB, glibc's 8MB),
// and a frame too large for the app fails on any host, not on the phone alone.
// AddressSanitizer's redzones make frames some times larger.
#ifdef ISH_ASAN
#define TASK_STACK_SIZE ((size_t) 4 << 20)
#else
#define TASK_STACK_SIZE ((size_t) 512 << 10)
#endif

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr(void) {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_attr_setstacksize(&task_thread_attr, TASK_STACK_SIZE);
    if (err != 0)
        die("pthread_attr_setstacksize(%zu): %s", TASK_STACK_SIZE, strerror(err));
}

int task_start(struct task *task) {
    // pthread_create returns an errno directly and never -1, so the old
    // "< 0" test never fired and a failure fell through to a task whose
    // thread was never started. Under thread-stack exhaustion (EAGAIN) that
    // has to become a guest-visible clone() failure, not a dead task or a
    // die(). [T-ish-jit-oom-abort]
    atomic_fetch_add(&task->refs, 1); // the thread's, until task_run_cleanup
    int err = pthread_create(&task->thread, &task_thread_attr, task_thread, task);
    if (err != 0) {
        task_unref(task); // no thread; the kernel's hold stays
        return err == EAGAIN ? _EAGAIN : _ENOMEM;
    }
    return 0;
}

int_t sys_sched_yield(void) {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name(void) {
    char name[16]; // As long as Linux will let us make this
    snprintf(name, sizeof(name), "-%d", current->pid);
    size_t pid_width = strlen(name);
    size_t name_width = snprintf(name, sizeof(name), "%s", current->comm);
    sprintf(name + (name_width < sizeof(name) - 1 - pid_width ? name_width : sizeof(name) - 1 - pid_width), "-%d", current->pid);
#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

