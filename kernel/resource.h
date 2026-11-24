#ifndef RESOURCE_H
#define RESOURCE_H
#include "kernel/time.h"

typedef qword_t rlim_t_;
typedef dword_t rlim32_t_;
#define RLIM_INFINITY_ ((rlim_t_) -1)

struct rlimit_ {
    rlim_t_ cur;
    rlim_t_ max;
};

struct rlimit32_ {
    rlim32_t_ cur;
    rlim32_t_ max;
};

#define RLIMIT_CPU_ 0
#define RLIMIT_FSIZE_ 1
#define RLIMIT_DATA_ 2
#define RLIMIT_STACK_ 3
#define RLIMIT_CORE_ 4
#define RLIMIT_RSS_ 5
#define RLIMIT_NPROC_ 6
#define RLIMIT_NOFILE_ 7
#define RLIMIT_MEMLOCK_ 8
#define RLIMIT_AS_ 9
#define RLIMIT_LOCKS_ 10
#define RLIMIT_SIGPENDING_ 11
#define RLIMIT_MSGQUEUE_ 12
#define RLIMIT_NICE_ 13
#define RLIMIT_RTPRIO_ 14
#define RLIMIT_RTTIME_ 15
#define RLIMIT_NLIMITS_ 16

dword_t sys_getrlimit32(dword_t resource, addr_t rlim_addr);
dword_t sys_setrlimit32(dword_t resource, addr_t rlim_addr);
dword_t sys_getrlimit64(dword_t resource, addr_t rlim_addr);
dword_t sys_setrlimit64(dword_t resource, addr_t rlim_addr);
dword_t sys_prlimit64(pid_t_ pid, dword_t resource, addr_t new_limit_addr, addr_t old_limit_addr);
dword_t sys_old_getrlimit32(dword_t resource, addr_t rlim_addr);

rlim_t_ rlimit(int resource);

// The guest's struct rusage: arm64's, whose counts are longs (8 bytes). They
// were 4 bytes, 32-bit x86's: getrusage and wait4 wrote 88 bytes of 144, the
// guest read each count of two, and the last ones of what was there before
// (ru_nvcsw 4020917316).
struct rusage_ {
    struct timeval_ utime;
    struct timeval_ stime;
    sqword_t maxrss;
    sqword_t ixrss;
    sqword_t idrss;
    sqword_t isrss;
    sqword_t minflt;
    sqword_t majflt;
    sqword_t nswap;
    sqword_t inblock;
    sqword_t oublock;
    sqword_t msgsnd;
    sqword_t msgrcv;
    sqword_t nsignals;
    sqword_t nvcsw;
    sqword_t nivcsw;
};

struct rusage_ rusage_get_current(void);
void rusage_add(struct rusage_ *dst, struct rusage_ *src);
#define RUSAGE_SELF_ 0
#define RUSAGE_CHILDREN_ -1
#define RUSAGE_THREAD_ 1
// The CPU time of a process: its threads that exited, and those running.
// Caller holds pids_lock.
struct tgroup;
struct rusage_ rusage_of_group(struct tgroup *group);
// One thread's CPU time, current's or another running one's of its process
// (pids_lock held, so that it cannot finish exiting meanwhile)
struct rusage_ rusage_of_thread(struct task *task);
dword_t sys_getrusage(dword_t who, addr_t rusage_addr);

int_t sys_sched_getaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr);
int_t sys_sched_setaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr);
int_t sys_getcpu(addr_t cpu_addr, addr_t node_addr, addr_t tcache_addr);
int_t sys_getpriority(int_t which, pid_t_ who);
int_t sys_setpriority(int_t which, pid_t_ who, int_t prio);

int_t sys_sched_getparam(pid_t_ pid, addr_t param_addr);
int_t sys_sched_getscheduler(pid_t_ UNUSED(pid));
int_t sys_sched_setscheduler(pid_t_ UNUSED(pid), int_t policy, addr_t param_addr);
int_t sys_sched_get_priority_max(int_t policy);
int_t sys_sched_get_priority_min(int_t policy);

int_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio));
int_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio));

#endif
