#ifdef __linux__
#define _GNU_SOURCE
#include <sys/resource.h>
#endif
#include "debug.h"
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/time.h"
#include "fs/poll.h"
#include "platform/platform.h"

#define TIMER_ABSTIME_ (1 << 0)

static int clockid_to_real(uint_t clock, clockid_t *real) {
    switch (clock) {
        case CLOCK_REALTIME_:
        case CLOCK_REALTIME_COARSE_:
            *real = CLOCK_REALTIME; break;
        case CLOCK_MONOTONIC_:
        case CLOCK_MONOTONIC_RAW_:
        case CLOCK_MONOTONIC_COARSE_:
        case CLOCK_BOOTTIME_:
            *real = CLOCK_MONOTONIC; break;
        default: return _EINVAL;
    }
    return 0;
}

// A time and interval Linux takes (itimerspec64_valid): seconds not below 0,
// nanoseconds in [0, 1e9). timer_settime and timerfd_settime took any, a
// negative one for a time long past (CPython's test_os,
// test_timerfd_negative).
static bool itimerspec_valid(struct itimerspec_ itspec) {
    return itspec.value.sec >= 0 && itspec.value.nsec >= 0 && itspec.value.nsec < 1000000000 &&
        itspec.interval.sec >= 0 && itspec.interval.nsec >= 0 && itspec.interval.nsec < 1000000000;
}

static struct timer_spec timer_spec_to_real(struct itimerspec_ itspec) {
    struct timer_spec spec = {
        .value.tv_sec = itspec.value.sec,
        .value.tv_nsec = itspec.value.nsec,
        .interval.tv_sec = itspec.interval.sec,
        .interval.tv_nsec = itspec.interval.nsec,
    };
    return spec;
};

static struct itimerspec_ timer_spec_from_real(struct timer_spec spec) {
    struct itimerspec_ itspec = {
        .value.sec = spec.value.tv_sec,
        .value.nsec = spec.value.tv_nsec,
        .interval.sec = spec.interval.tv_sec,
        .interval.nsec = spec.interval.tv_nsec,
    };
    return itspec;
};

dword_t sys_time(addr_t time_out) {
    dword_t now = time(NULL);
    if (time_out != 0)
        if (user_put(time_out, now))
            return _EFAULT;
    return now;
}

dword_t sys_stime(addr_t UNUSED(time)) {
    return _EPERM;
}

static struct timespec cpu_time(struct rusage_ rusage, bool user_only) {
    uint64_t usec = (uint64_t) rusage.utime.sec * 1000000 + rusage.utime.usec;
    if (!user_only)
        usec += (uint64_t) rusage.stime.sec * 1000000 + rusage.stime.usec;
    return (struct timespec) {.tv_sec = usec / 1000000, .tv_nsec = usec % 1000000 * 1000};
}

// A CPU-time clock, user and system time together as Linux's: the process's
// (all its threads, those that exited too; it was the calling thread's user
// time) or the calling thread's (CLOCK_THREAD_CPUTIME_ID was EINVAL: Python's
// time.thread_time()).
static struct timespec cpu_clock(uint_t clock) {
    struct rusage_ rusage;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        lock(&pids_lock);
        rusage = rusage_of_group(current->group);
        unlock(&pids_lock);
    } else {
        rusage = rusage_get_current();
    }
    return cpu_time(rusage, false);
}

// A CPU-time clock as Linux encodes one: ~pid << 3, 4 if it is a thread's
// (CPUCLOCK_PERTHREAD), and in the low 2 bits what it counts: user and
// system time (CPUCLOCK_PROF, 0), user time (CPUCLOCK_VIRT, 1), scheduler
// time (CPUCLOCK_SCHED, 2), here user and system time as there is nothing
// else (and on a Linux host another thread's user time is all of its time:
// rusage_of_task). pid 0 is the caller, or its process. All were EINVAL:
// pthread_getcpuclockid gives a thread's, HotSpot times its threads so (and
// takes clock_getres's word that it can) and iSH's own Linux CLI its other
// threads (rusage_of_task); clock_getcpuclockid a process's, after
// clock_getres takes it, and said ESRCH of every process, its caller too.
//
// Which clocks there are is Linux's pid_for_clock's: a thread's of a thread
// of the caller's process, that has not exited; a process's of any process,
// named by its pid, a zombie's too (until it is reaped, the leader's task and
// its group stay), or for clock_gettime by the caller's own tid. A thread's
// pid is no process: a leaked thread's (do_exit_group's safety valve) group
// may be freed, so its tgid says so before its group is looked at.
#define CPUCLOCK_PERTHREAD_MASK_ 4
#define CPUCLOCK_WHICH_MASK_ 3
#define CPUCLOCK_VIRT_ 1
#define CPUCLOCK_SCHED_ 2
#define CPUCLOCK_MAX_ 3
static int cpu_clock_of(int_t clock, bool gettime, struct timespec *ts) {
    int which = clock & CPUCLOCK_WHICH_MASK_;
    if (which >= CPUCLOCK_MAX_)
        return _EINVAL;
    pid_t_ pid = ~(clock >> 3);
    struct rusage_ rusage;
    lock(&pids_lock);
    if (clock & CPUCLOCK_PERTHREAD_MASK_) {
        struct task *thread = pid == 0 ? current : pid_get_task(pid);
        if (thread == NULL || thread->exiting || thread->group != current->group) {
            unlock(&pids_lock);
            return _EINVAL;
        }
        rusage = rusage_of_thread(thread);
    } else {
        struct tgroup *group = NULL;
        if (pid == 0) {
            group = current->group;
        } else {
            struct task *task = pid_get_task_zombie(pid);
            if (task != NULL && task->tgid == task->pid)
                group = task->group;
            else if (gettime && task == current)
                group = current->group;
        }
        if (group == NULL) {
            unlock(&pids_lock);
            return _EINVAL;
        }
        rusage = rusage_of_group(group);
    }
    unlock(&pids_lock);
    *ts = cpu_time(rusage, which == CPUCLOCK_VIRT_);
    return 0;
}

dword_t sys_clock_gettime(dword_t clock, addr_t tp) {
    STRACE("clock_gettime(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_ || clock == CLOCK_THREAD_CPUTIME_ID_) {
        ts = cpu_clock(clock);
    } else if ((int_t) clock < 0) {
        int err = cpu_clock_of((int_t) clock, true, &ts);
        if (err < 0)
            return err;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_gettime(clock_id, &ts);
        if (err < 0)
            return errno_map();
    }
    struct timespec_ t;
    t.sec = ts.tv_sec;
    t.nsec = ts.tv_nsec;
    if (user_put(tp, t))
        return _EFAULT;
    STRACE(" {%llds %lldns}", (long long)t.sec, (long long)t.nsec);
    return 0;
}

dword_t sys_clock_getres(dword_t clock, addr_t res_addr) {
    STRACE("clock_getres(%d, %#x)", clock, res_addr);
    struct timespec res;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_ || clock == CLOCK_THREAD_CPUTIME_ID_) {
        res = (struct timespec) {.tv_sec = 0, .tv_nsec = 1}; // as Linux says
    } else if ((int_t) clock < 0) {
        // a CPU-time clock, if it is one: 1ns for scheduler time, as Linux
        // says; user and system time is counted in what Linux counts it in,
        // its tick, and here in what rusage counts it in, microseconds
        int err = cpu_clock_of((int_t) clock, false, &res);
        if (err < 0)
            return err;
        bool sched = ((int_t) clock & CPUCLOCK_WHICH_MASK_) == CPUCLOCK_SCHED_;
        res = (struct timespec) {.tv_sec = 0, .tv_nsec = sched ? 1 : 1000};
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_getres(clock_id, &res);
        if (err < 0)
            return errno_map();
    }
    // No place for it is no error, as on Linux: the clock is checked and that
    // is all. glibc's clock_getcpuclockid asks so (clock_getres(clock, NULL)),
    // and with EFAULT here no process's CPU-time clock could be had.
    if (res_addr == 0)
        return 0;
    struct timespec_ t;
    t.sec = res.tv_sec;
    t.nsec = res.tv_nsec;
    if (user_put(res_addr, t))
        return _EFAULT;
    return 0;
}

dword_t sys_clock_settime(dword_t UNUSED(clock), addr_t UNUSED(tp)) {
    return _EPERM;
}

// SIGALRM goes to the process, not to the thread that set the timer (which
// may have exited since).
static void itimer_notify(struct tgroup *group) {
    struct siginfo_ info = {
        .code = SI_TIMER_,
    };
    lock(&pids_lock);
    send_process_signal(group, SIGALRM_, info);
    unlock(&pids_lock);
}

static int itimer_set(struct tgroup *group, int which, struct timer_spec spec, struct timer_spec *old_spec) {
    if (which != ITIMER_REAL_)
        return _EINVAL;

    if (!group->itimer) {
        struct timer *timer = timer_new(CLOCK_REALTIME, (timer_callback_t) itimer_notify, group);
        if (IS_ERR(timer))
            return PTR_ERR(timer);
        group->itimer = timer;
    }

    return timer_set(group->itimer, spec, old_spec);
}

// ITIMER_VIRTUAL and ITIMER_PROF fire as the process's user time, or user and
// system time, all its threads' and its exited ones', passes theirs. They
// were EINVAL: profilers set them (Go's pprof, where timer_create of a CPU
// clock is not, gperftools, Python's statistical ones), and CPython's
// test_signal ItimerTest. A thread samples the time (rusage_of_group) every
// 10ms while one is armed, as Linux checks them at its tick (and while none
// is, waits to be notified, not woken and not taking pids_lock: a process
// that profiled a while would have it so for good), and sends
// SIGVTALRM or SIGPROF to the process, SI_KERNEL as Linux's; a periodic one
// is due again an interval after it was due, as Linux's check_cpu_itimer, so
// one behind fires at the next samples until it is not. Locks as everything
// else takes them: pids_lock (the threads, and to send), then the group's.
#define CPU_ITIMER_SAMPLE_NS 10000000

static uint64_t usec_of_timeval(struct timeval_ t) {
    return (uint64_t) t.sec * 1000000 + t.usec;
}

static uint64_t usec_of_timespec(struct timespec t) {
    return (uint64_t) t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static struct timespec timespec_of_usec(uint64_t usec) {
    return (struct timespec) {.tv_sec = usec / 1000000, .tv_nsec = usec % 1000000 * 1000};
}

static uint64_t cpu_itimer_now(struct rusage_ rusage, int i) {
    uint64_t usec = usec_of_timeval(rusage.utime);
    if (i == 1)
        usec += usec_of_timeval(rusage.stime);
    return usec;
}

// What is left of it, as Linux's get_cpu_itimer: 0 if it is disarmed, 1us if
// it is due and yet to fire. The group's lock held.
static struct timer_spec cpu_itimer_get_locked(struct tgroup *group, int i, uint64_t now) {
    uint64_t expires = group->cpu_itimer_expires[i];
    uint64_t left = expires == 0 ? 0 : expires > now ? expires - now : 1;
    return (struct timer_spec) {
        .value = timespec_of_usec(left),
        .interval = timespec_of_usec(group->cpu_itimer_interval[i]),
    };
}

static void *cpu_itimer_thread(void *arg) {
    struct tgroup *group = arg;
    static const int signals[2] = {SIGVTALRM_, SIGPROF_};
    for (;;) {
        lock(&group->lock);
        while (!group->cpu_itimer_stop &&
                group->cpu_itimer_expires[0] == 0 && group->cpu_itimer_expires[1] == 0)
            wait_for_ignore_signals(&group->cpu_itimer_armed, &group->lock, NULL);
        bool stop = group->cpu_itimer_stop;
        unlock(&group->lock);
        if (stop)
            return NULL;
        struct timespec period = {.tv_sec = 0, .tv_nsec = CPU_ITIMER_SAMPLE_NS};
        nanosleep(&period, NULL);
        // Stopped, or disarmed, while it slept: one disarmed does not fire
        // (its expires is 0), and the next turn waits
        lock(&pids_lock);
        lock(&group->lock);
        stop = group->cpu_itimer_stop;
        unlock(&group->lock);
        if (stop) {
            unlock(&pids_lock);
            return NULL;
        }
        struct rusage_ rusage = rusage_of_group(group);
        bool fire[2] = {false, false};
        lock(&group->lock);
        for (int i = 0; i < 2; i++) {
            uint64_t expires = group->cpu_itimer_expires[i];
            if (expires != 0 && cpu_itimer_now(rusage, i) >= expires) {
                fire[i] = true;
                uint64_t interval = group->cpu_itimer_interval[i];
                group->cpu_itimer_expires[i] = interval != 0 ? expires + interval : 0;
            }
        }
        unlock(&group->lock);
        for (int i = 0; i < 2; i++) {
            if (fire[i]) {
                struct siginfo_ info = {.code = SI_KERNEL_};
                send_process_signal(group, signals[i], info);
            }
        }
        unlock(&pids_lock);
    }
}

static int cpu_itimer_set(struct tgroup *group, int which, struct timer_spec spec, struct timer_spec *old_spec) {
    int i = which == ITIMER_PROF_;
    lock(&pids_lock);
    uint64_t now = cpu_itimer_now(rusage_of_group(group), i);
    lock(&group->lock);
    *old_spec = cpu_itimer_get_locked(group, i, now);
    uint64_t value = usec_of_timespec(spec.value);
    group->cpu_itimer_expires[i] = value == 0 ? 0 : now + value;
    group->cpu_itimer_interval[i] = usec_of_timespec(spec.interval);
    int err = 0;
    if (value != 0 && group->cpu_itimer_running) {
        notify(&group->cpu_itimer_armed);
    } else if (value != 0) {
        group->cpu_itimer_stop = false;
        if (pthread_create(&group->cpu_itimer_thread, NULL, cpu_itimer_thread, group) == 0) {
            group->cpu_itimer_running = true;
        } else {
            group->cpu_itimer_expires[i] = 0;
            err = _EAGAIN;
        }
    }
    unlock(&group->lock);
    unlock(&pids_lock);
    return err;
}

static struct timer_spec cpu_itimer_get(struct tgroup *group, int which) {
    int i = which == ITIMER_PROF_;
    lock(&pids_lock);
    uint64_t now = cpu_itimer_now(rusage_of_group(group), i);
    lock(&group->lock);
    struct timer_spec spec = cpu_itimer_get_locked(group, i, now);
    unlock(&group->lock);
    unlock(&pids_lock);
    return spec;
}

// An itimerval in microseconds: an armed timer's under 1us says 1us, as Linux
// says (itimer_get_remtime), not 0, which is a disarmed one's.
static struct itimerval_ itimerval_from_real(struct timer_spec spec) {
    struct itimerval_ val = {
        .interval.sec = spec.interval.tv_sec,
        .interval.usec = spec.interval.tv_nsec / 1000,
        .value.sec = spec.value.tv_sec,
        .value.usec = spec.value.tv_nsec / 1000,
    };
    if (val.value.sec == 0 && val.value.usec == 0 && timespec_positive(spec.value))
        val.value.usec = 1;
    return val;
}

// It was ENOSYS: Python's signal.getitimer, and so its test_signal's
// ItimerTest.
int_t sys_getitimer(int_t which, addr_t val_addr) {
    STRACE("getitimer(%d, %#x)", which, val_addr);
    if (which != ITIMER_REAL_ && which != ITIMER_VIRTUAL_ && which != ITIMER_PROF_)
        return _EINVAL;
    struct timer_spec spec = {};
    struct tgroup *group = current->group;
    if (which == ITIMER_REAL_) {
        lock(&group->lock);
        if (group->itimer != NULL)
            timer_get(group->itimer, &spec);
        unlock(&group->lock);
    } else {
        spec = cpu_itimer_get(group, which);
    }
    struct itimerval_ val = itimerval_from_real(spec);
    if (user_put(val_addr, val))
        return _EFAULT;
    return 0;
}

int_t sys_setitimer(int_t which, addr_t new_val_addr, addr_t old_val_addr) {
    struct itimerval_ val;
    if (user_get(new_val_addr, val))
        return _EFAULT;
    STRACE("setitimer(%d, {%llds %lldus, %llds %lldus}, 0x%x)",
        which,
        (long long)val.value.sec, (long long)val.value.usec,
        (long long)val.interval.sec, (long long)val.interval.usec,
        old_val_addr);

    struct timer_spec spec = {
        .interval.tv_sec = val.interval.sec,
        .interval.tv_nsec = val.interval.usec * 1000,
        .value.tv_sec = val.value.sec,
        .value.tv_nsec = val.value.usec * 1000,
    };
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    int err;
    if (which == ITIMER_VIRTUAL_ || which == ITIMER_PROF_) {
        err = cpu_itimer_set(group, which, spec, &old_spec);
    } else {
        lock(&group->lock);
        err = itimer_set(group, which, spec, &old_spec);
        unlock(&group->lock);
    }
    if (err < 0)
        return err;

    if (old_val_addr != 0) {
        struct itimerval_ old_val = itimerval_from_real(old_spec);
        if (user_put(old_val_addr, old_val))
            return _EFAULT;
    }

    return 0;
}

uint_t sys_alarm(uint_t seconds) {
    STRACE("alarm(%d)", seconds);
    struct timer_spec spec = {
        .value.tv_sec = seconds,
    };
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    lock(&group->lock);
    int err = itimer_set(group, ITIMER_REAL_, spec, &old_spec);
    unlock(&group->lock);
    if (err < 0)
        return err;

    // Round up, and make sure to not return 0 if old_spec is > 0
    seconds = old_spec.value.tv_sec;
    if (old_spec.value.tv_nsec >= 500000000)
        seconds++;
    if (seconds == 0 && !timespec_is_zero(old_spec.value))
        seconds = 1;
    return seconds;
}


// [T-ish-nanosleep-uninterruptible] A guest sleep must be interruptible by a
// guest signal.
//
// Both nanosleep paths used to call the HOST's nanosleep() directly. That is a
// host-level block: iSH's signal machinery cannot reach a thread parked inside
// it, so a SIGTERM sent to a sleeping task just sat in the pending queue until
// the host sleep ran to completion on its own. `sleep 8` therefore always slept
// the full 8 seconds no matter what was sent to it.
//
// What that broke, and why it looked like the posix-timer bug: GNU coreutils
// `timeout` arms its deadline with timer_create (fixed separately in
// e1d57948), then on expiry signals its child. With the timer fixed, `timeout
// 3 sleep 8` DOES fire at 3s and does return 124 -- but the child ignored the
// signal until its own 8s elapsed, so the observable behaviour ("the process is
// still alive at t=7") did not change at all. Two bugs in series; fixing only
// the first one shows no improvement, which is exactly what the field reports
// described.
//
// Wait on a private condition with a timeout instead. wait_for() returns
// _EINTR the moment a signal is pending, which is the behaviour nanosleep(2)
// is specified to have. `rem` matters: sleep(1) and friends resume with the
// remainder, so an interrupted sleep that reports a wrong remainder either
// returns early or loops forever.
static dword_t nanosleep_interruptible(struct timespec *req, addr_t rem_addr) {
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
        return errno_map();
    long long req_ns = (long long) req->tv_sec * 1000000000LL + req->tv_nsec;

    // Private cond/lock. Nothing ever notifies it deliberately, so the wait is
    // meant to end by timeout or by signal.
    cond_t sleep_cond = COND_INITIALIZER;
    lock_t sleep_lock;
    lock_init(&sleep_lock);
    cond_init(&sleep_cond);

    long long left_ns = req_ns;
    int err = _ETIMEDOUT;
    lock(&sleep_lock);
    // Loop until the deadline is actually reached. wait_for() returns 0 for a
    // wakeup that is neither a timeout nor a deliverable signal, and that is
    // reachable here, not just theoretical: deliver_signal() notifies
    // waiting_cond for EVERY signal, while is_signal_pending() masks out
    // BLOCKED ones — so a blocked signal (a shell blocking SIGCHLD around a
    // critical section, say) wakes this cond and reports 0. Returning success
    // on that would silently truncate the sleep, which is exactly the bug the
    // caller is relying on us not to have. Every other wait_for() caller in
    // the kernel loops for the same reason (eventfd.c:32, time.c timerfd,
    // exit.c) — this one must too.
    //
    // The timeout passed to wait_for is RELATIVE
    // (pthread_cond_timedwait_relative_np on Darwin), so it has to be
    // recomputed from the remaining time on each pass, never re-passed as the
    // original `req`.
    while (left_ns > 0) {
        struct timespec wait_ts = {
            .tv_sec = (time_t) (left_ns / 1000000000LL),
            .tv_nsec = (long) (left_ns % 1000000000LL),
        };
        err = wait_for(&sleep_cond, &sleep_lock, &wait_ts);

        // Re-measure BEFORE acting on the result: the _EINTR path reports
        // `left_ns` as the remainder, so it has to reflect time actually
        // elapsed. Breaking out first would hand back the full original
        // duration and make an interrupted sleep restart from scratch.
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            long long elapsed_ns = (long long) (now.tv_sec - start.tv_sec) * 1000000000LL
                                 + (long long) (now.tv_nsec - start.tv_nsec);
            left_ns = req_ns - elapsed_ns;
        } else if (err != _EINTR) {
            break;   // cannot measure progress and not interrupted; treat as complete
        }

        if (err == _EINTR)
            break;
        if (err == _ETIMEDOUT)
            break;   // the deadline is what expired; done regardless of drift
    }
    unlock(&sleep_lock);
    cond_destroy(&sleep_cond);

    // Only an interrupted sleep reports a remainder; a completed one reports
    // zero. Clamp: the signal can arrive after the deadline has already passed.
    struct timespec_ rem_ts = {.sec = 0, .nsec = 0};
    if (err == _EINTR && left_ns > 0) {
        rem_ts.sec = left_ns / 1000000000LL;
        rem_ts.nsec = left_ns % 1000000000LL;
    }
    if (rem_addr != 0 && user_put(rem_addr, rem_ts))
        return _EFAULT;
    if (err == _EINTR) {
        // where restart_syscall sleeps on to, when no handler runs for the
        // signal (kernel/calls.c)
        long long end_ns = (long long) start.tv_nsec + req_ns % 1000000000LL;
        current->restart_sleep_deadline.tv_sec = start.tv_sec + req_ns / 1000000000LL + end_ns / 1000000000LL;
        current->restart_sleep_deadline.tv_nsec = end_ns % 1000000000LL;
        current->restart_sleep_rem = rem_addr;
        current->restart_sleep_pending = true;
        return _EINTR;
    }
    return 0;
}

// restart_syscall: a sleep a signal interrupted with no handler run for it
// (one that stopped the process, which was then continued; one ignored), as
// Linux's ERESTART_RESTARTBLOCK. It sleeps on to the time it slept to: what
// is left, nothing if the stop outlasted it. nanosleep's and
// clock_nanosleep's are the only ones here.
dword_t sys_restart_syscall(void) {
    STRACE("restart_syscall()");
    if (!current->restart_sleep_pending)
        return _EINTR;
    current->restart_sleep_pending = false;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return errno_map();
    long long left_ns = (long long) (current->restart_sleep_deadline.tv_sec - now.tv_sec) * 1000000000LL +
        (current->restart_sleep_deadline.tv_nsec - now.tv_nsec);
    if (left_ns <= 0)
        return 0;
    struct timespec left = {.tv_sec = left_ns / 1000000000LL, .tv_nsec = left_ns % 1000000000LL};
    return nanosleep_interruptible(&left, current->restart_sleep_rem);
}

dword_t sys_nanosleep(addr_t req_addr, addr_t rem_addr) {
    struct timespec_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("nanosleep({%lld, %lld}, 0x%x", (long long)req_ts.sec, (long long)req_ts.nsec, rem_addr);

    // A tiny sleep (<=50us: Go's usleep(3..20) in its scheduler's spin loops,
    // a lock's back-off) sleeps on the host, for at least as long as asked,
    // as Linux's does (and its timer slack longer). It was a sched_yield, and
    // before that every sleep up to 1ms was ([T-ish-futex-subms-timeout] is
    // the futex's twin of that): a back-off of 2us returned in 0.2us, a
    // spin. An iSH running in iSH, whose locks back off so (util/sync.h),
    // spun on all of the phone's six cores while six processes started
    // their threads, and a lock's holder got no CPU: 96 threads took 10 to
    // 20 s to start, where 80 took 0.2 (regress_leaked_thread_exit's 8 x
    // 16 failed its 30 s alarm). Not interruptible, as the helper below is:
    // a signal is seen 50us later at most. The host's SIGUSR1 (a guest
    // signal's kick) interrupts it; it sleeps on what is left.
    if (req_ts.sec == 0 && req_ts.nsec <= 50000) {
        struct timespec left = {.tv_sec = 0, .tv_nsec = (long) req_ts.nsec};
        while (nanosleep(&left, &left) < 0 && errno == EINTR) {}
        if (rem_addr != 0) {
            struct timespec_ rem_ts = {.sec = 0, .nsec = 0};
            if (user_put(rem_addr, rem_ts))
                return _EFAULT;
        }
        return 0;
    }

    struct timespec req;
    req.tv_sec = req_ts.sec;
    req.tv_nsec = req_ts.nsec;
    // [T-ish-nanosleep-uninterruptible] interruptible; see the helper above.
    return nanosleep_interruptible(&req, rem_addr);
}

dword_t sys_clock_nanosleep(dword_t clock, dword_t flags, addr_t req_addr, addr_t rem_addr) {
    struct timespec_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("clock_nanosleep(%d, %d, {%lld, %lld}, 0x%x)", clock, flags,
           (long long)req_ts.sec, (long long)req_ts.nsec, rem_addr);

    clockid_t real_clock;
    if (clockid_to_real(clock, &real_clock))
        return _EINVAL;

    struct timespec req;
    req.tv_sec = req_ts.sec;
    req.tv_nsec = req_ts.nsec;

    if (flags & TIMER_ABSTIME_) {
        // Convert absolute time to relative by subtracting current time
        struct timespec now;
        if (clock_gettime(real_clock, &now) < 0)
            return errno_map();
        req.tv_sec -= now.tv_sec;
        req.tv_nsec -= now.tv_nsec;
        if (req.tv_nsec < 0) {
            req.tv_sec--;
            req.tv_nsec += 1000000000;
        }
        if (req.tv_sec < 0)
            return 0; // Already past the target time
    }

    // [T-ish-nanosleep-uninterruptible] interruptible; see the helper above.
    // POSIX: with TIMER_ABSTIME the remainder is NOT written back (the caller
    // already knows the absolute deadline), so pass 0 to suppress it.
    return nanosleep_interruptible(&req, (flags & TIMER_ABSTIME_) ? 0 : rem_addr);
}

// The process's CPU time (all its threads', as Linux), that of its children it
// waited for, and the clock ticks since boot, which callers take differences
// of (/proc/uptime's, in USER_HZ: 100 a second)
sqword_t sys_times(addr_t tbuf) {
    STRACE("times(0x%x)", tbuf);
    if (tbuf) {
        lock(&pids_lock);
        struct rusage_ self = rusage_of_group(current->group);
        unlock(&pids_lock);
        lock(&current->group->lock);
        struct rusage_ children = current->group->children_rusage;
        unlock(&current->group->lock);
        struct tms_ tmp = {
            .tms_utime = clock_from_timeval(self.utime),
            .tms_stime = clock_from_timeval(self.stime),
            .tms_cutime = clock_from_timeval(children.utime),
            .tms_cstime = clock_from_timeval(children.stime),
        };
        if (user_put(tbuf, tmp))
            return _EFAULT;
    }
    return get_uptime().uptime_ticks;
}

dword_t sys_gettimeofday(addr_t tv, addr_t tz) {
    STRACE("gettimeofday(0x%x, 0x%x)", tv, tz);
    struct timeval timeval;
    struct timezone timezone;
    if (gettimeofday(&timeval, &timezone) < 0) {
        return errno_map();
    }
    struct timeval_ tv_;
    struct timezone_ tz_;
    tv_.sec = timeval.tv_sec;
    tv_.usec = timeval.tv_usec;
    tz_.minuteswest = timezone.tz_minuteswest;
    tz_.dsttime = timezone.tz_dsttime;
    if ((tv && user_put(tv, tv_)) || (tz && user_put(tz, tz_))) {
        return _EFAULT;
    }
    return 0;
}

dword_t sys_settimeofday(addr_t UNUSED(tv), addr_t UNUSED(tz)) {
    return _EPERM;
}

#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_ 1
#define SIGEV_THREAD_ID_ 4

static void posix_timer_callback(struct posix_timer *timer) {
    if (timer->tgroup == NULL)
        return;
    struct siginfo_ info = {
        .code = SI_TIMER_,
        .timer.timer = timer->timer_id,
        .timer.overrun = 0,
        .timer.value = timer->sig_value,
    };
    // The timer and its group live until the group dies, when exit_tgroup
    // frees the timer.
    lock(&pids_lock);
    if (timer->method == SIGEV_SIGNAL_) {
        send_process_signal(timer->tgroup, timer->signal, info);
    } else {
        // SIGEV_THREAD_ID: the thread, unless it has exited (its pid then
        // names no task, or one of another process)
        struct task *thread = pid_get_task(timer->thread_pid);
        if (thread != NULL && thread->group == timer->tgroup)
            send_signal(thread, timer->signal, info);
    }
    unlock(&pids_lock);
}


int_t sys_timer_create(dword_t clock, addr_t sigevent_addr, addr_t timer_addr) {
    STRACE("timer_create(%d, %#x, %#x)", clock, sigevent_addr, timer_addr);
    clockid_t real_clockid;
    if (clockid_to_real(clock, &real_clockid))
        return _EINVAL;
    struct sigevent_ sigev;
    if (sigevent_addr == 0) {
        // NULL sigevent: POSIX/Linux default is SIGEV_SIGNAL delivering SIGALRM
        // to the process, with sival_int = timer id. musl's timer_create passes
        // NULL for the common `timer_create(clock, NULL, &id)` form (e.g.
        // coreutils `timeout`). iSH used to user_get(NULL) → EFAULT here, so
        // every such call failed spuriously.
        sigev = (struct sigevent_){0};
        sigev.method = SIGEV_SIGNAL_;
        sigev.signo = SIGALRM_;
    } else if (user_get(sigevent_addr, sigev)) {
        return _EFAULT;
    }
    if (sigev.method != SIGEV_SIGNAL_ && sigev.method != SIGEV_NONE_ && sigev.method != SIGEV_THREAD_ID_)
        return _EINVAL;

    // As Linux's good_sigevent: a thread of this process. (An unknown one
    // returned with pids_lock still held, and iSH hung on the next use.)
    if (sigev.method == SIGEV_THREAD_ID_) {
        lock(&pids_lock);
        struct task *thread = pid_get_task(sigev.tid);
        bool ours = thread != NULL && thread->group == current->group;
        unlock(&pids_lock);
        if (!ours)
            return _EINVAL;
    }

    struct tgroup *group = current->group;
    lock(&group->lock);
    unsigned timer_id;
    for (timer_id = 0; timer_id < TIMERS_MAX; timer_id++) {
        if (group->posix_timers[timer_id].timer == NULL)
            break;
    }
    if (timer_id >= TIMERS_MAX) {
        unlock(&group->lock);
        return _ENOMEM;
    }
    if (user_put(timer_addr, timer_id)) {
        unlock(&group->lock);
        return _EFAULT;
    }

    struct posix_timer *timer = &group->posix_timers[timer_id];
    timer->timer_id = timer_id;
    timer->timer = timer_new(real_clockid, (timer_callback_t) posix_timer_callback, timer);
    timer->timer->silent = sigev.method == SIGEV_NONE_;
    timer->signal = sigev.signo;
    timer->sig_value = sigev.value;
    timer->method = sigev.method;
    timer->tgroup = NULL;
    if (sigev.method == SIGEV_SIGNAL_) {
        timer->tgroup = group;
    } else if (sigev.method == SIGEV_THREAD_ID_) {
        // The thread named, which glibc's and musl's SIGEV_THREAD timers wait
        // on for the signal: it went to the leader.
        timer->tgroup = group;
        timer->thread_pid = sigev.tid;
    }
    unlock(&group->lock);
    return 0;
}

int_t sys_timer_settime(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    STRACE("timer_settime(%d, %d, %#x, %#x)", timer_id, flags, new_value_addr, old_value_addr);
    struct itimerspec_ value;
    if (user_get(new_value_addr, value))
        return _EFAULT;
    if (!itimerspec_valid(value))
        return _EINVAL;
    // A timer that is not, deleted or never made, is EINVAL, as Linux says.
    // Its NULL timer was set (and TIMERS_MAX's past the array): iSH crashed.
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    lock(&current->group->lock);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }
    struct timer_spec spec = timer_spec_to_real(value);
    struct timer_spec old_spec;
    // a time of 0 disarms it, absolute or not, as on Linux: it was one long
    // past, and it fired at once
    if (flags & TIMER_ABSTIME_ && !timespec_is_zero(spec.value)) {
        struct timespec now = timespec_now(timer->timer->clockid);
        spec.value = timespec_subtract(spec.value, now);
    }
    int err = timer_set(timer->timer, spec, &old_spec);
    unlock(&current->group->lock);
    if (err < 0)
        return err;

    if (old_value_addr) {
        struct itimerspec_ old_value = timer_spec_from_real(old_spec);
        if (user_put(old_value_addr, old_value))
            return _EFAULT;
    }
    return 0;
}

int_t sys_timer_gettime(dword_t timer_id, addr_t curr_value_addr) {
    STRACE("timer_gettime(%d, %#x)", timer_id, curr_value_addr);
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    lock(&current->group->lock);
    struct posix_timer *pt = &current->group->posix_timers[timer_id];
    if (pt->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }

    struct timer_spec spec;
    timer_get(pt->timer, &spec);
    unlock(&current->group->lock);

    struct itimerspec_ value = timer_spec_from_real(spec);
    if (user_put(curr_value_addr, value))
        return _EFAULT;
    return 0;
}

int_t sys_timer_getoverrun(dword_t timer_id) {
    STRACE("timer_getoverrun(%d)", timer_id);
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    lock(&current->group->lock);
    struct posix_timer *pt = &current->group->posix_timers[timer_id];
    if (pt->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }
    unlock(&current->group->lock);
    return 0;
}

// Freed without the group's lock held: a timer's callback runs holding the
// timer's lock, and timer_free takes that lock, waiting for one under way.
void posix_timers_delete(struct tgroup *group) {
    struct timer *timers[TIMERS_MAX];
    int count = 0;
    lock(&group->lock);
    for (int i = 0; i < TIMERS_MAX; i++) {
        struct posix_timer *timer = &group->posix_timers[i];
        if (timer->timer != NULL)
            timers[count++] = timer->timer;
        timer->timer = NULL;
        timer->tgroup = NULL;
    }
    unlock(&group->lock);
    for (int i = 0; i < count; i++)
        timer_free(timers[i]);
}

int_t sys_timer_delete(dword_t timer_id) {
    STRACE("timer_delete(%d)\n", timer_id);
    // read past the array, as timer_settime did
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;
    lock(&current->group->lock);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }
    timer_free(timer->timer);
    timer->timer = NULL;
    unlock(&current->group->lock);
    return 0;
}

static struct fd_ops timerfd_ops;

static void timerfd_reset(struct fd *fd) {
    lock(&fd->lock);
    fd->timerfd.expirations = 0;
    unlock(&fd->lock);
}

static void timerfd_callback(struct fd *fd) {
    lock(&fd->lock);
    fd->timerfd.expirations++;
    notify(&fd->cond);
    unlock(&fd->lock);
    poll_wakeup(fd, POLL_READ);
}

fd_t sys_timerfd_create(int_t clockid, int_t flags) {
    STRACE("timerfd_create(%d, %#x)", clockid, flags);
    clockid_t real_clockid;
    if (clockid_to_real(clockid, &real_clockid)) return _EINVAL;

    struct fd *fd = adhoc_fd_create(&timerfd_ops);
    if (fd == NULL)
        return _ENOMEM;

    fd->timerfd.timer = timer_new(real_clockid, (timer_callback_t) timerfd_callback, fd);
    return f_install(fd, flags);
}

int_t sys_timerfd_settime(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    STRACE("timerfd_settime(%d, %d, %#x, %#x)", f, flags, new_value_addr, old_value_addr);
    if (flags & ~(TIMER_ABSTIME_))
        return _EINVAL;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &timerfd_ops)
        return _EINVAL;
    struct itimerspec_ value;
    if (user_get(new_value_addr, value))
        return _EFAULT;
    if (!itimerspec_valid(value))
        return _EINVAL;
    struct timer_spec spec = timer_spec_to_real(value);
    struct timer_spec old_spec;
    if (flags & TIMER_ABSTIME_ && !timespec_is_zero(spec.value)) {
        struct timespec now = timespec_now(fd->timerfd.timer->clockid);
        spec.value = timespec_subtract(spec.value, now);
    }

    // As Linux's timerfd_settime: the expirations of the old setting are
    // gone (read said them after it); none of it comes after. Not under
    // fd->lock, which the timer's callback takes under the timer's lock:
    // setting it as it fired was a deadlock.
    int err = timer_set_with(fd->timerfd.timer, spec, &old_spec, (timer_callback_t) timerfd_reset);
    if (err < 0)
        return err;

    if (old_value_addr) {
        struct itimerspec_ old_value = timer_spec_from_real(old_spec);
        if (user_put(old_value_addr, old_value))
            return _EFAULT;
    }

    return 0;
}

int_t sys_timerfd_gettime(fd_t f, addr_t curr_value_addr) {
    STRACE("timerfd_gettime(%d, %#x)", f, curr_value_addr);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &timerfd_ops)
        return _EINVAL;

    struct timer_spec spec;
    timer_get(fd->timerfd.timer, &spec);

    struct itimerspec_ value = timer_spec_from_real(spec);
    if (user_put(curr_value_addr, value))
        return _EFAULT;
    return 0;
}

static ssize_t timerfd_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(uint64_t))
        return _EINVAL;
    lock(&fd->lock);
    while (fd->timerfd.expirations == 0) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        int err = wait_for(&fd->cond, &fd->lock, NULL);
        if (err < 0) {
            unlock(&fd->lock);
            return err;
        }
    }

    *(uint64_t *) buf = fd->timerfd.expirations;
    fd->timerfd.expirations = 0;
    unlock(&fd->lock);
    return sizeof(uint64_t);
}
static int timerfd_poll(struct fd *fd) {
    int res = 0;
    lock(&fd->lock);
    if (fd->timerfd.expirations != 0)
        res |= POLL_READ;
    unlock(&fd->lock);
    return res;
}
static int timerfd_close(struct fd *fd) {
    timer_free(fd->timerfd.timer);
    return 0;
}

static struct fd_ops timerfd_ops = {
    .read = timerfd_read,
    .poll = timerfd_poll,
    .close = timerfd_close,
};
