#if __linux__
// pull in RUSAGE_THREAD
#define _GNU_SOURCE
#include <sys/resource.h>
#include <pthread.h>
#include <time.h>
#elif __APPLE__
// pull in thread_info and friends
#include <mach/mach.h>
#else
#error
#endif

#include <limits.h>
#include <string.h>
#include "kernel/calls.h"

static bool resource_valid(int resource) {
    return resource >= 0 && resource < RLIMIT_NLIMITS_;
}

static int rlimit_get(struct task *task, int resource, struct rlimit_ *limit) {
    if (!resource_valid(resource))
        return _EINVAL;
    struct tgroup *group = task->group;
    lock(&group->lock);
    *limit = group->limits[resource];
    unlock(&group->lock);
    return 0;
}

static int rlimit_set(struct task *task, int resource, struct rlimit_ limit) {
    if (!resource_valid(resource))
        return _EINVAL;
    struct tgroup *group = task->group;
    lock(&group->lock);
    group->limits[resource] = limit;
    unlock(&group->lock);
    return 0;
}

rlim_t_ rlimit(int resource) {
    struct rlimit_ limit;
    if (rlimit_get(current, resource, &limit) != 0)
        die("invalid resource %d", resource);
    return limit.cur;
}

static int do_getrlimit32(int resource, struct rlimit32_ *rlimit32) {
    STRACE("getlimit(%d)", resource);
    struct rlimit_ rlimit;
    int err = rlimit_get(current, resource, &rlimit);
    if (err < 0)
        return err;
    STRACE(" {cur=%#x, max=%#x}", rlimit.cur, rlimit.max);

    rlimit32->max = rlimit.max;
    rlimit32->cur = rlimit.cur;
    return 0;
}

dword_t sys_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;
    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

dword_t sys_old_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;

    // This version of the call is for programs that aren't aware of rlim_t
    // being 64 bit. RLIM_INFINITY looks like -1 when truncated to 32 bits.
    if (rlimit.cur > INT_MAX)
        rlimit.cur = INT_MAX;
    if (rlimit.max > INT_MAX)
        rlimit.max = INT_MAX;

    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

// A limit the task's process may be given, as Linux's do_prlimit: a soft one
// above the hard one is EINVAL (it was taken), and raising the hard one
// needs privilege.
static int check_setrlimit(struct task *task, int resource, struct rlimit_ new_limit) {
    if (new_limit.cur > new_limit.max)
        return _EINVAL;
    if (superuser())
        return 0;
    struct rlimit_ old_limit;
    int err = rlimit_get(task, resource, &old_limit);
    if (err < 0)
        return err;
    if (new_limit.max > old_limit.max)
        return _EPERM;
    return 0;
}

dword_t sys_setrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit_ rlimit;
    if (user_get(rlim_addr, rlimit))
        return _EFAULT;
    STRACE("setrlimit(%d, {cur=%#x, max=%#x})", resource, rlimit.cur, rlimit.max);
    int err = check_setrlimit(current, resource, rlimit);
    if (err < 0)
        return err;
    return rlimit_set(current, resource, rlimit);
}

// 64-bit getrlimit/setrlimit for ARM64 (struct rlimit with 64-bit cur/max)
dword_t sys_getrlimit64(dword_t resource, addr_t rlim_addr) {
    STRACE("getrlimit64(%d)", resource);
    struct rlimit_ rlimit;
    int err = rlimit_get(current, resource, &rlimit);
    if (err < 0)
        return err;
    STRACE(" {cur=%#llx, max=%#llx}", (unsigned long long)rlimit.cur, (unsigned long long)rlimit.max);
    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

dword_t sys_setrlimit64(dword_t resource, addr_t rlim_addr) {
    struct rlimit_ rlimit;
    if (user_get(rlim_addr, rlimit))
        return _EFAULT;
    STRACE("setrlimit64(%d, {cur=%#llx, max=%#llx})", resource, (unsigned long long)rlimit.cur, (unsigned long long)rlimit.max);
    int err = check_setrlimit(current, resource, rlimit);
    if (err < 0)
        return err;
    return rlimit_set(current, resource, rlimit);
}

// Any process's limits, by its pid (a thread's tid names its process's), 0
// the caller's, as Linux's. It was EINVAL for any pid but 0, the caller's own
// too: util-linux's prlimit --pid, CPython's resource.prlimit(pid), and for a
// process that is not, EINVAL where Linux says ESRCH (CPython's test_prlimit,
// which takes EINVAL from it for a soft limit above the hard one).
dword_t sys_prlimit64(pid_t_ pid, dword_t resource, addr_t new_limit_addr, addr_t old_limit_addr) {
    STRACE("prlimit64(%d, %d)", pid, resource);
    struct rlimit_ new_limit;
    if (new_limit_addr != 0) {
        if (user_get(new_limit_addr, new_limit))
            return _EFAULT;
        STRACE(" new={cur=%#x, max=%#x}", new_limit.cur, new_limit.max);
    }

    lock(&pids_lock);
    struct task *task = pid == 0 ? current : pid_get_task(pid);
    if (task == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct rlimit_ old_limit;
    int err = rlimit_get(task, resource, &old_limit);
    if (err == 0 && new_limit_addr != 0) {
        err = check_setrlimit(task, resource, new_limit);
        if (err == 0)
            err = rlimit_set(task, resource, new_limit);
    }
    unlock(&pids_lock);
    if (err < 0)
        return err;

    if (old_limit_addr != 0) {
        STRACE(" old={cur=%#x, max=%#x}", old_limit.cur, old_limit.max);
        if (user_put(old_limit_addr, old_limit))
            return _EFAULT;
    }
    return 0;
}

// [T-ish-port-leak-probe] Mach-port accounting hooks for the embedding app.
//
// The real implementations live in the Minis app's Swift side
// (ResourceDiagnostics, exported via @_cdecl) and exist only when this
// translation unit is linked into that app. Weak no-op defaults here mean
// every other target — the standalone native/CLI build in particular — links
// without them; the strong Swift definitions override these when present.
//
// Same pattern and rationale as `restore_termios` in kernel/exit.c.
#if __APPLE__
__attribute__((weak)) void minis_diag_note_thread_port_acquired(void) {}
__attribute__((weak)) void minis_diag_note_thread_port_released(void) {}
#endif

struct rusage_ rusage_get_current(void) {
    // only the time fields are currently implemented
    struct rusage_ rusage;
#if __linux__
    struct rusage usage;
    int err = getrusage(RUSAGE_THREAD, &usage);
    assert(err == 0);
    rusage.utime.sec = usage.ru_utime.tv_sec;
    rusage.utime.usec = usage.ru_utime.tv_usec;
    rusage.stime.sec = usage.ru_stime.tv_sec;
    rusage.stime.usec = usage.ru_stime.tv_usec;
#elif __APPLE__
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    // [T-ish-port-leak-probe] `mach_thread_self()` returns a SEND RIGHT that
    // the caller owns and must deallocate. This call site never did, and it is
    // reached from do_exit (exit.c:125) — i.e. exactly once per guest task
    // teardown, which is the shape the field data shows: port_count tracked
    // guestForks at a ratio of 1.000 over 93k forks.
    //
    // Deallocated below, and instrumented so the next run can PROVE the
    // accounting rather than infer it. The counters are read by the app's
    // ResourceDiagnostics sampler.
    mach_port_t self_thread = mach_thread_self();
    thread_info(self_thread, THREAD_BASIC_INFO, (thread_info_t) &info, &count);
    minis_diag_note_thread_port_acquired();
    if (mach_port_deallocate(mach_task_self(), self_thread) == KERN_SUCCESS)
        minis_diag_note_thread_port_released();
    rusage.utime.sec = info.user_time.seconds;
    rusage.utime.usec = info.user_time.microseconds;
    rusage.stime.sec = info.system_time.seconds;
    rusage.stime.usec = info.system_time.microseconds;
#endif
    return rusage;
}

// The CPU time of another task's thread, which is running (in its group's
// threads, not exiting: it cannot finish do_exit while the caller holds
// pids_lock). Darwin splits it into user and system time; a Linux host's
// thread clock has only the sum, given as user time.
static struct rusage_ rusage_of_task(struct task *task) {
    struct rusage_ rusage = {};
#if __linux__
    clockid_t clock;
    struct timespec ts;
    if (pthread_getcpuclockid(task->thread, &clock) == 0 && clock_gettime(clock, &ts) == 0) {
        rusage.utime.sec = ts.tv_sec;
        rusage.utime.usec = ts.tv_nsec / 1000;
    }
#elif __APPLE__
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    // the thread's port, borrowed from pthread: nothing to deallocate
    if (thread_info(pthread_mach_thread_np(task->thread), THREAD_BASIC_INFO,
                    (thread_info_t) &info, &count) == KERN_SUCCESS) {
        rusage.utime.sec = info.user_time.seconds;
        rusage.utime.usec = info.user_time.microseconds;
        rusage.stime.sec = info.system_time.seconds;
        rusage.stime.usec = info.system_time.microseconds;
    }
#endif
    return rusage;
}

struct rusage_ rusage_of_thread(struct task *task) {
    return task == current ? rusage_get_current() : rusage_of_task(task);
}

static void timeval_add(struct timeval_ *dst, struct timeval_ *src);

struct rusage_ rusage_of_group(struct tgroup *group) {
    lock(&group->lock);
    struct rusage_ rusage = group->rusage; // its threads that exited
    unlock(&group->lock);
    struct task *thread;
    list_for_each_entry(&group->threads, thread, group_links) {
        if (thread->exiting)
            continue;
        struct rusage_ thread_rusage = rusage_of_thread(thread);
        rusage_add(&rusage, &thread_rusage);
    }
    return rusage;
}

static void timeval_add(struct timeval_ *dst, struct timeval_ *src) {
    dst->sec += src->sec;
    dst->usec += src->usec;
    if (dst->usec >= 1000000) {
        dst->usec -= 1000000;
        dst->sec++;
    }
}

void rusage_add(struct rusage_ *dst, struct rusage_ *src) {
    timeval_add(&dst->utime, &src->utime);
    timeval_add(&dst->stime, &src->stime);
}

dword_t sys_getrusage(dword_t who, addr_t rusage_addr) {
    struct rusage_ rusage;
    switch (who) {
        // all its threads', as Linux: it was the calling thread's alone
        case RUSAGE_SELF_:
            lock(&pids_lock);
            rusage = rusage_of_group(current->group);
            unlock(&pids_lock);
            break;
        case RUSAGE_THREAD_:
            rusage = rusage_get_current();
            break;
        case RUSAGE_CHILDREN_:
            lock(&current->group->lock);
            rusage = current->group->children_rusage;
            unlock(&current->group->lock);
            break;
        default:
            return _EINVAL;
    }
    if (user_put(rusage_addr, rusage))
        return _EFAULT;
    return 0;
}

static unsigned online_cpus(void) {
    unsigned cpus = sysconf(_SC_NPROCESSORS_ONLN);
    { const char *e = getenv("ISH_FAKE_NCPU"); if (e) { int n = atoi(e); if (n > 0) cpus = n; } }
    return cpus;
}

static uint64_t all_cpus(unsigned cpus) {
    return cpus >= 64 ? ~(uint64_t) 0 : ((uint64_t) 1 << cpus) - 1;
}

// A task's CPUs are those sched_setaffinity gave it (task->cpu_affinity),
// as Linux says them. iSH cannot pin a host thread, so it runs where iOS
// puts it; what is kept is what the task is to run on, which is what is
// read of it: nproc (taskset -c 0 make -j$(nproc)), HotSpot's
// availableProcessors, CPython's os.sched_getaffinity. setaffinity said
// success and forgot it, and every task had all CPUs.
int_t sys_sched_getaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr) {
    STRACE("sched_getaffinity(%d, %d, %#x)", pid, cpusetsize, cpuset_addr);
    lock(&pids_lock);
    struct task *task = pid == 0 ? current : pid_get_task(pid);
    if (task == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    uint64_t mask = task->cpu_affinity;
    unlock(&pids_lock);

    unsigned cpus = online_cpus();
    if (mask == 0)
        mask = all_cpus(cpus);
    char cpuset[cpus / 8 + 1];
    if (cpusetsize < sizeof(cpuset))
        return _EINVAL;
    memset(cpuset, 0, sizeof(cpuset));
    for (unsigned i = 0; i < cpus && i < 64; i++) {
        if (mask >> i & 1)
            bit_set(i, cpuset);
    }
    if (user_write(cpuset_addr, cpuset, sizeof(cpuset)))
        return _EFAULT;
    // return the number of bytes written
    return sizeof(cpuset);
}
// Every task reports CPU 0 on NUMA node 0. Tasks are host threads that iSH does
// not pin, so any answer may be stale by the time it is read, as on Linux.
// HotSpot refuses to start without this call.
int_t sys_getcpu(addr_t cpu_addr, addr_t node_addr, addr_t UNUSED(tcache_addr)) {
    STRACE("getcpu(%#x, %#x)", cpu_addr, node_addr);
    uint32_t zero = 0;
    if (cpu_addr != 0 && user_put(cpu_addr, zero))
        return _EFAULT;
    if (node_addr != 0 && user_put(node_addr, zero))
        return _EFAULT;
    return 0;
}

// As Linux: a shorter mask has no CPUs after it, a longer one's bits past
// the last CPU are not looked at, and one with no CPU there is is EINVAL.
// cpu_set_t's bit i is byte i/8's bit i%8, a little-endian uint64_t's.
int_t sys_sched_setaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr) {
    STRACE("sched_setaffinity(%d, %d, %#x)", pid, cpusetsize, cpuset_addr);
    uint64_t mask = 0;
    if (user_read(cpuset_addr, &mask, cpusetsize < sizeof(mask) ? cpusetsize : sizeof(mask)))
        return _EFAULT;
    uint64_t all = all_cpus(online_cpus());
    mask &= all;
    if (mask == 0)
        return _EINVAL;
    lock(&pids_lock);
    struct task *task = pid == 0 ? current : pid_get_task(pid);
    if (task == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    task->cpu_affinity = mask == all ? 0 : mask;
    unlock(&pids_lock);
    return 0;
}

int_t sys_getpriority(int_t which, pid_t_ who) {
    STRACE("getpriority(%d, %d)", which, who);
    return 20;
}
int_t sys_setpriority(int_t which, pid_t_ who, int_t prio) {
    STRACE("setpriority(%d, %d, %d)", which, who, prio);
    return 0;
}

// realtime scheduling stubs
int_t sys_sched_getparam(pid_t_ UNUSED(pid), addr_t param_addr) {
    int_t sched_priority = 0;
    if (user_put(param_addr, sched_priority))
        return _EFAULT;
    return 0;
}
#define SCHED_OTHER_ 0
int_t sys_sched_getscheduler(pid_t_ UNUSED(pid)) {
    return SCHED_OTHER_;
}
int_t sys_sched_setscheduler(pid_t_ UNUSED(pid), int_t policy, addr_t param_addr) {
    if (policy != SCHED_OTHER_)
        return _EINVAL;
    int_t sched_priority;
    if (user_get(param_addr, sched_priority))
        return _EFAULT;
    if (sched_priority != 0)
        return _EINVAL;
    return 0;
}

// The priorities of a policy, as Linux says them: 1 to 99 for the realtime
// ones, SCHED_FIFO and SCHED_RR, and 0 for the rest. The realtime ones were
// EINVAL (and min was max): CPython's test_sched_priority. Setting one stays
// refused (sched_setscheduler).
#define SCHED_FIFO_ 1
#define SCHED_RR_ 2
#define SCHED_BATCH_ 3
#define SCHED_IDLE_ 5
#define SCHED_DEADLINE_ 6
static int_t sched_priority_range(int_t policy, bool max) {
    switch (policy) {
        case SCHED_FIFO_:
        case SCHED_RR_:
            return max ? 99 : 1;
        case SCHED_OTHER_:
        case SCHED_BATCH_:
        case SCHED_IDLE_:
        case SCHED_DEADLINE_:
            return 0;
    }
    return _EINVAL;
}

int_t sys_sched_get_priority_max(int_t policy) {
    STRACE("sched_get_priority_max(%d)", policy);
    return sched_priority_range(policy, true);
}

int_t sys_sched_get_priority_min(int_t policy) {
    STRACE("sched_get_priority_min(%d)", policy);
    return sched_priority_range(policy, false);
}

int_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
int_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
