#ifndef TASK_H
#define TASK_H

#include <pthread.h>
#include "emu/cpu.h"
#include "kernel/mm.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/resource.h"
#include "fs/sockrestart.h"
#include "util/list.h"
#include "util/timer.h"
#include "util/sync.h"

// everything here is private to the thread executing this task and needs no
// locking, unless otherwise specified
struct task {
    struct cpu_state cpu;
    struct mm *mm; // locked by general_lock
    struct mem *mem; // pointer to mm.mem, for convenience
    pthread_t thread;
    uint64_t threadid;

    struct tgroup *group; // immutable
    struct list group_links;
    pid_t_ pid, tgid; // immutable
    uid_t_ uid, gid;
    uid_t_ euid, egid;
    uid_t_ suid, sgid;
#define MAX_GROUPS 32
    unsigned ngroups;
    uid_t_ groups[MAX_GROUPS];
    char comm[16] __strncpy_safe; // locked by general_lock
    bool did_exec; // for that one annoying setsid edge case
    // The CPUs it may run on, a bit each, as sched_setaffinity set them; 0
    // for all. Locked by pids_lock. A child has its parent's (task_create_).
    uint64_t cpu_affinity;
    // Its nice, -20 to 19, as setpriority set it. Locked by pids_lock. A
    // child has its parent's; exec keeps it.
    int nice;
    bool undumpable; // prctl(PR_SET_DUMPABLE, 0); inherited by fork, cleared by exec

    struct fdtable *files;
    struct fs_info *fs;

    // locked by sighand->lock
    struct sighand *sighand;
    sigset_t_ blocked;
    // Signals sent to this thread (tkill, faults, SIGPIPE), queued on queue.
    // Those sent to its process are on the group's shared_queue.
    sigset_t_ pending;
    sigset_t_ waiting; // if nonzero, an ongoing call to sigtimedwait is waiting on these
    // When it was created, in hundredths of a second since boot (USER_HZ):
    // /proc/<pid>/stat's starttime, ps's START
    uint64_t start_ticks;
    // Alternate signal stack (sigaltstack). Per thread, as on Linux: only the
    // task itself reads or writes it, so it needs no lock.
    addr_t altstack;
    dword_t altstack_size;
    struct list queue;
    struct sigqueue kill_entry; // a SIGKILL on queue (queue_signal)
    cond_t pause; // please don't signal this
    // private
    sigset_t_ saved_mask;
    bool has_saved_mask;

    // Set when thread is in a blocking syscall (futex_wait, poll_wait, etc.)
    // Used for deadlock detection: if all threads are blocking, it's a hang.
    bool blocking;
    // Per-thread pipe for futex_wait wakeup (reused across calls to avoid
    // pipe creation overhead in Go runtime spin loops): written by a wake and
    // by a signal's kick. Set and closed under waiting_cond_lock, which the
    // kick takes (kernel/futex.c).
    int futex_pipe[2]; // [0]=read, [1]=write; -1 if not yet created
    // Timestamp (CLOCK_MONOTONIC ns) of last time blocking became false.
    // Used by deadlock detector to check how long ALL threads have been stuck.
    uint64_t last_unblocked_ns;

    struct {
        // Locks all ptrace-related things
        lock_t lock;
        cond_t cond;

        bool traced;
        bool stopped;
        int signal;
        struct siginfo_ info;
        int trap_event;
    } ptrace;

    // locked by pids_lock
    struct task *parent;
    struct list children;
    struct list siblings;

    addr_t clear_tid;
    addr_t robust_list;

    // SA_RESTART support: on each syscall entry we snapshot the number and the
    // first arg (x0/ebx), which the return value overwrites. If the syscall is
    // interrupted (returns EINTR) and a signal with SA_RESTART is then
    // delivered, receive_signals rewinds PC to the syscall instruction and
    // restores the original first arg so the syscall re-executes — matching
    // Linux. Without it, JSC's SIGPWR-based GC safepoint handshake (which
    // registers SA_RESTART) livelocks on a futex that keeps returning EINTR.
    unsigned syscall_restart_num;
    uint64_t syscall_restart_arg0;
    uint64_t syscall_restart_pc; // cpu.pc at syscall entry (== svc_addr+4); the
                                 // SA_RESTART rewind only fires when cpu.pc still
                                 // equals this, so a stale syscall_restartable
                                 // flag can't corrupt PC on a later interrupt.
    bool syscall_restartable; // this syscall returned EINTR and may restart
    // ... with no handler run for the signal, as Linux's ERESTARTSYS (see
    // receive_signals); for the signal handling right after it only
    bool syscall_restart_nohandler;
    // ... as restart_syscall, as Linux's ERESTART_RESTARTBLOCK: a sleep
    // (nanosleep, clock_nanosleep) goes on to the time it slept to, not the
    // whole of it again
    bool syscall_restart_block;
    // the interrupted sleep's, for restart_syscall: valid while pending
    bool restart_sleep_pending;
    struct timespec restart_sleep_deadline; // CLOCK_MONOTONIC
    addr_t restart_sleep_rem;

    // Run on the task's thread, as it, before its first instruction: set
    // before task_start. The app's sessions wait there for the boot.
    void (*before_run)(void);

    // locked by pids_lock
    dword_t exit_code;
    bool zombie;
    bool exiting;
    // [T-ish-mm-leak-refcount-handoff] Set by the do_exit_group safety valve
    // when it orphans a thread stuck in an uninterruptible host syscall: the
    // valve can't mm_release() now (the thread may still hold mem->lock inside
    // user_write → UAF, see T-ish-mem-uaf-user-write), so it hands the release
    // off to this thread's own pthread cleanup handler, which runs only after
    // the thread has left the read_wrlock critical section. Prevents the mm
    // (whole guest address space) from leaking forever when the thread never
    // re-enters do_exit(). Cleared once released so it never double-frees.
    bool mm_release_deferred;
    // Inside a system call (the slow path), where a task waits: /proc shows it
    // sleeping (S), as near as iSH can tell, and running (R) otherwise.
    _Atomic bool in_syscall;
    // [T-ish-exit-valve-progress] Set by the thread itself on entering
    // do_exit, before it queues for pids_lock. do_exit_group's safety valve
    // reads it to tell a thread that is already exiting (merely waiting its
    // turn for the lock) from one stuck elsewhere.
    _Atomic bool exit_started;

    // this structure is allocated on the stack of the parent's clone() call
    struct vfork_info {
        bool done;
        cond_t cond;
        lock_t lock;
    } *vfork;
    int exit_signal;

    // lock for anything that needs locking but is not covered by some other lock
    // specifically: comm, mm
    lock_t general_lock;

    // [T-ish-fork-rate] How hard THIS task has been forking lately: an
    // exponentially weighted forks-per-second with a 1 s time constant, only
    // ever touched by the task itself inside sys_clone. A fresh shell doing a
    // handful of forks (a tool call) stays "light" and bypasses the global
    // fork-rate governor; the looping shells of a fork storm do not.
    uint64_t fork_rate_last_ns;
    double fork_rate_ewma;

    struct task_sockrestart sockrestart;

    // Native offload: when is_native_proxy is true, this task is waiting
    // for a host-native process instead of running emulated code.
    pid_t native_pid;
    bool is_native_proxy;

    // [T-ish-offload-signal-forward] Set while this task is executing an
    // IN-PROCESS offload handler (native_offload_add_handler). Distinct from
    // is_native_proxy, which covers only the posix_spawn shape: a handler runs
    // on this very thread with no host pid, so a signal can't be forwarded with
    // kill() — it has to go through the handler's abort callback instead.
    // Points into the static offload registry, which lives for the process
    // lifetime, so it never dangles. NULL when not in a handler.
    void *native_offload_entry;

    // current condition/lock, so it can be notified in case of a signal
    cond_t *waiting_cond;
    lock_t *waiting_lock;
    lock_t waiting_cond_lock;

    // Who holds this struct: the kernel until task_destroy, and the task's
    // host thread while it runs (task_start to the cleanup of
    // task_run_current). Freed when both are done with it. Last, so
    // task_destroy can poison all before it.
    _Atomic int refs;
};

// current will always give the process that is currently executing
// if I have to stop using __thread, current will become a macro
extern __thread struct task *current;

static inline void task_set_mm(struct task *task, struct mm *mm) {
    task->mm = mm;
    task->mem = &task->mm->mem;
    task->cpu.mmu = &task->mem->mmu;
}

// Creates a new process, initializes most fields from the parent. Specify
// parent as NULL to create the init process. Returns NULL if out of memory.
// Ends with an underscore because there's a mach function by the same name
struct task *task_create_(struct task *parent);
// Removes the process from the process table and frees it. Must be called with pids_lock.
void task_destroy(struct task *task);

// misc
void vfork_notify(struct task *task);
pid_t_ task_setsid(struct task *task);
void task_leave_session(struct task *task);

struct posix_timer {
    struct timer *timer;
    int_t timer_id;
    struct tgroup *tgroup; // NULL for SIGEV_NONE
    int_t method; // SIGEV_SIGNAL_: to the process; SIGEV_THREAD_ID_: to thread_pid
    pid_t_ thread_pid;
    int_t signal;
    union sigval_ sig_value;
};

// struct thread_group is way too long to type comfortably
struct tgroup {
    struct list threads; // locked by pids_lock, by majority vote
    struct task *leader; // immutable
    struct rusage_ rusage;

    // locked by pids_lock
    pid_t_ sid, pgid;
    struct list session;
    struct list pgroup;

    bool stopped;
    cond_t stopped_cond;
    // SIGCONT continued it from a stop: for its parent's wait (WCONTINUED),
    // and for its parent's SIGCHLD, which it sends as it runs on (calls.c).
    // Locked by the group's lock.
    bool continued_unwaited;
    bool continued_unnotified;

    // Signals sent to the process as a whole: kill(), a process group's
    // signal (a terminal's SIGINT), SIGCHLD, SIGALRM, a POSIX timer's
    // SIGEV_SIGNAL. As Linux's shared_pending, any thread that does not block
    // one takes it (send_process_signal). Locked by the sighand lock, which
    // the threads share.
    sigset_t_ shared_pending;
    struct list shared_queue;
    struct sigqueue shared_kill_entry; // a SIGKILL on shared_queue (queue_signal)

    struct tty *tty;
    struct timer *itimer;
    // ITIMER_VIRTUAL's [0] and ITIMER_PROF's [1]: the process's CPU time, in
    // us, at which each fires (0: disarmed), and its interval. Locked by the
    // group's lock. A thread samples that time from the first one set until
    // the process exits (cpu_itimer_thread; delete_timers_if_last stops and
    // joins it), and while none is armed waits on cpu_itimer_armed, notified
    // when one is and when it is to stop; not a child's (tgroup_copy), kept
    // across exec.
    uint64_t cpu_itimer_expires[2];
    uint64_t cpu_itimer_interval[2];
    bool cpu_itimer_running;
    bool cpu_itimer_stop;
    pthread_t cpu_itimer_thread;
    cond_t cpu_itimer_armed;
#define TIMERS_MAX 16
    struct posix_timer posix_timers[TIMERS_MAX];

    struct rlimit_ limits[RLIMIT_NLIMITS_];

    // From https://twitter.com/tblodt/status/957706819236904960
    // > there are two distinct ways for a p̶r̶o̶c̶e̶s̶s̶ thread group to exit:
    // > 
    // > - each thread calls exit
    // > wait will return the exit code for the group leader
    // > 
    // > - any thread calls exit_group
    // > the SIGNAL_GROUP_EXIT flag will be set and wait will return the status passed to exit_group
    //
    // TODO locking
    bool doing_group_exit;
    dword_t group_exit_code;

    struct rusage_ children_rusage;
    cond_t child_exit;

    dword_t personality;

    // Monotonically increasing syscall counter for deadlock detection.
    atomic_uint syscall_count;

    // Timestamp of last real progress (non-blocking syscall completion).
    // Updated atomically. Used by futex/poll safety valves to detect
    // process-wide hangs where all threads are idle.
    _Atomic uint64_t last_progress_ns;

    // Opaque per-thread-group context for host integrations (e.g. fakefs
    // path-translate hook). Inherited verbatim on fork via tgroup_copy().
    // iSH itself never interprets this value; host registers a meaning.
    uint64_t fs_context;

    // for everything in this struct not locked by something else
    lock_t lock;
};

// Whether task has a signal waiting that it does not block, its own or its
// process's: what ends an interruptible wait.
static inline bool task_signal_pending(struct task *task) {
    return ((task->pending | task->group->shared_pending) & ~task->blocked) != 0;
}

static inline bool task_is_leader(struct task *task) {
    return task->group->leader == task;
}

// [T-ish-leaked-thread-poll] True once do_exit_group's safety valve has given
// up on this thread (it sets `exiting` and unlinks it from the group), or once
// task_destroy has zeroed the struct under it (group == NULL). Such a thread's
// group, sighand and files are gone or about to be freed, so a wait loop that
// wakes up must leave before touching any of them: return to the syscall
// dispatcher, whose post-syscall guard (calls.c) pthread_exits the thread.
// Only valid for `current`: no other thread may be leaked while it runs.
static inline bool task_orphaned(struct task *task) {
    return task->exiting || task->group == NULL;
}

struct pid {
    dword_t id;
    struct task *task;
    struct list session;
    struct list pgroup;
};

// synchronizes obtaining a pointer to a task and freeing that task
extern lock_t pids_lock;
// these functions must be called with pids_lock
struct pid *pid_get(dword_t pid);
struct task *pid_get_task(dword_t pid);
// The pid last given out (/proc/loadavg). Caller holds pids_lock.
pid_t_ pid_last_allocated(void);
// Its children to init (none if it is init, or init is gone), for a task that
// leaves without do_exit giving them away. Caller holds pids_lock.
void task_give_children_to_init(struct task *task);
struct task *pid_get_task_zombie(dword_t id); // don't return null if the task exists as a zombie

#define MAX_PID (1 << 15) // oughta be enough

// [T-ish-cpu-top] Successful fork/clone count (kernel/fork.c), for host diagnostics.
extern _Atomic uint64_t ish_guest_forks;

// [T-ish-freeze-watch] Lock-free progress counters for the host's freeze
// detector (kernel/freeze_watch.h): every completed syscall, and guest
// processes (thread groups) created and not yet reaped. Relaxed atomics only;
// readers must never need a kernel lock to see them.
extern _Atomic uint64_t ish_guest_syscalls;
extern _Atomic int64_t ish_live_guest_procs;

// [T-ish-fork-rate] Global fork-rate governor: a token bucket that every
// non-thread clone() draws from. When it is empty, heavy forkers sleep until
// their token arrives (never denied, only delayed); light forkers (see
// task->fork_rate_ewma) go through. per_sec == 0 turns it off. The host sets
// the rate from thermal state; the CLI reads ISH_FORK_RATE=per_sec[,burst].
void ish_set_fork_rate_limit(unsigned per_sec, unsigned burst);
void ish_fork_rate_stats(uint64_t *throttled, uint64_t *bypassed, uint64_t *sleep_ns);

// TODO document
// Starts the task's thread. Returns 0, or a negative guest errno (_EAGAIN when
// the host is out of thread resources) -- callers must not assume success.
__attribute__((warn_unused_result))
int task_start(struct task *task);
void task_run_current(void);

// Called when a process exits, with its leader (whose pid is the process id).
extern void (*exit_hook)(struct task *task, int code);

typedef int (*ish_timer_tick_hook_t)(void);
void ish_set_timer_tick_hook(ish_timer_tick_hook_t hook);

// [fork-guard] Called in sys_clone before a new task is created.
// Return 0 to allow the fork, or a negative errno (e.g. _EAGAIN) to deny it;
// the value is returned to the guest verbatim and no task is created.
// Set with ish_set_fork_guard(); pass NULL to remove. Thread-safe via atomic store.
// The callback runs on the forking thread and must not allocate or take kernel locks.
typedef int (*ish_fork_guard_t)(void);
void ish_set_fork_guard(ish_fork_guard_t guard);

#define superuser() (current != NULL && current->euid == 0)

// Update the thread name to match the current task, in the format "comm-pid".
// Will ensure that the -pid part always fits, then will fit as much of comm as possible.
void update_thread_name(void);

// What starting a thread or a process (clone) and a futex wait cost, for
// /proc/ish/threads: an iSH running in iSH started 128 threads in 10 to 50 s
// at times, where it takes 0.1 s, the time in the outer's clone and in the
// inner's pids_lock. Kept as they happen (stat_max: the longest so far).
extern _Atomic uint64_t st_pids_waits, st_pids_wait_ns, st_pids_wait_max_ns;
extern _Atomic uint64_t st_futex_waits, st_futex_shared, st_futex_woken, st_futex_missed;
void stat_max(_Atomic uint64_t *max, uint64_t value);
uint64_t stat_now_ns(void);
void thread_stats_describe(char *buf, size_t size);

#endif
