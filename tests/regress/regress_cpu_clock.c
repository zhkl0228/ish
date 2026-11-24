// Regression test for a thread's and a process's CPU-time clock
// (pthread_getcpuclockid, clock_getcpuclockid).
//
// Linux encodes one ~pid << 3, 4 if it is a thread's, and in the low 2 bits
// what it counts (0 user and system time, 1 user time, 2 scheduler time).
// iSH said EINVAL of all of them: HotSpot times its threads so, and takes
// clock_getres's word that it can; iSH's own Linux CLI, run by iSH, said
// every other live thread's CPU time 0 (/proc/<pid>/stat, getrusage); and
// clock_getcpuclockid, which asks clock_getres first, said ESRCH of every
// process, its caller too.
//
// Checked: this thread's clock grows as it computes; another thread's, read
// while it computes 0.3 s, says at least 0.2 s; clock_getres takes the clock;
// a thread of another process is no clock of this one (EINVAL); this
// process's clock, from clock_getcpuclockid(0), says at least what this
// thread computed; its user time (1) is no more than its user and system
// time (0); a child's, computing 0.3 s, says at least 0.2 s, and so does its
// zombie's before it is reaped; a thread's tid that is not its process's pid
// names no process for clock_getcpuclockid (ESRCH), but clock_gettime of the
// thread's own tid as a process clock is its process's. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_cpu_clock regress_cpu_clock.c

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;

static void check(const char *name, int ok, const char *detail) {
    if (ok) {
        passed++;
        printf("PASS %s\n", name);
    } else {
        failed++;
        printf("FAIL %s: %s\n", name, detail);
    }
    fflush(stdout);
}

static double seconds(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0)
        return -1;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// a process's (pid 0: this one's) or a thread's clock, as Linux encodes it
static clockid_t cpu_clock(pid_t pid, int thread, int which) {
    return (clockid_t) ((~(unsigned) pid << 3) | (thread ? 4 : 0) | which);
}

// computes for about the CPU time given
static void burn(double cpu) {
    double start = seconds(CLOCK_THREAD_CPUTIME_ID);
    volatile unsigned long x = 0;
    while (seconds(CLOCK_THREAD_CPUTIME_ID) - start < cpu)
        for (int i = 0; i < 100000; i++)
            x += i;
}

static volatile int burned;

static void *burner(void *arg) {
    (void) arg;
    burn(0.3);
    burned = 1;
    while (burned != 2)
        ; // computes on until read
    return NULL;
}

struct tid_as_process {
    int getcpuclockid_err;
    int gettime;
    int gettime_errno;
};

static void *tid_as_process(void *arg) {
    struct tid_as_process *out = arg;
    clockid_t clock;
    out->getcpuclockid_err = clock_getcpuclockid(gettid(), &clock);
    struct timespec ts;
    out->gettime = clock_gettime(cpu_clock(gettid(), 0, 2), &ts);
    out->gettime_errno = errno;
    return NULL;
}

int main(void) {
    char detail[200];

    clockid_t self;
    int err = pthread_getcpuclockid(pthread_self(), &self);
    double before = seconds(self);
    burn(0.1);
    double after = seconds(self);
    snprintf(detail, sizeof(detail), "pthread_getcpuclockid %d, clock %#x: %.3f s, then %.3f s", err,
             (unsigned) self, before, after);
    check("this thread's clock grows as it computes", err == 0 && before >= 0 && after - before >= 0.05, detail);

    pthread_t t;
    pthread_create(&t, NULL, burner, NULL);
    while (!burned)
        ;
    clockid_t other;
    err = pthread_getcpuclockid(t, &other);
    double its = seconds(other);
    burned = 2;
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "pthread_getcpuclockid %d, clock %#x: %.3f s", err, (unsigned) other, its);
    check("another thread's clock, read while it computes", err == 0 && its >= 0.2, detail);

    struct timespec res = {-1, -1};
    int r = clock_getres(self, &res);
    int e = errno;
    snprintf(detail, sizeof(detail), "%d (%s), %lld s %ld ns", r, r < 0 ? strerror(e) : "ok", (long long) res.tv_sec,
             res.tv_nsec);
    check("clock_getres takes a thread's clock", r == 0 && res.tv_sec == 0 && res.tv_nsec > 0, detail);

    // a thread of another process: its main thread's id is its pid
    pid_t child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    clockid_t foreign = cpu_clock(child, 1, 2);
    struct timespec ts;
    r = clock_gettime(foreign, &ts);
    e = errno;
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "clock %#x: %d (%s)", (unsigned) foreign, r, r < 0 ? strerror(e) : "ok");
    check("a thread of another process is no clock of this one", r == -1 && e == EINVAL, detail);

    clockid_t process;
    err = clock_getcpuclockid(0, &process);
    double mine = seconds(process);
    double thread_time = seconds(CLOCK_THREAD_CPUTIME_ID);
    snprintf(detail, sizeof(detail), "clock_getcpuclockid %d (%s), clock %#x: %.3f s, this thread %.3f s", err,
             strerror(err), (unsigned) process, mine, thread_time);
    check("this process's clock says at least what this thread computed", err == 0 && mine >= thread_time - 0.01,
          detail);

    double user = seconds(cpu_clock(0, 0, 1));
    double total = seconds(cpu_clock(0, 0, 0));
    snprintf(detail, sizeof(detail), "user %.3f s, user and system %.3f s", user, total);
    check("this process's user time is no more than its user and system time", user >= 0 && total >= user, detail);

    int ready[2];
    pipe(ready);
    child = fork();
    if (child == 0) {
        burn(0.3);
        write(ready[1], "x", 1);
        pause();
        _exit(0);
    }
    char c;
    read(ready[0], &c, 1);
    clockid_t childs;
    err = clock_getcpuclockid(child, &childs);
    double child_time = seconds(childs);
    snprintf(detail, sizeof(detail), "clock_getcpuclockid %d (%s), clock %#x: %.3f s", err, strerror(err),
             (unsigned) childs, child_time);
    check("a child's clock, after it computed 0.3 s", err == 0 && child_time >= 0.2, detail);

    kill(child, SIGKILL);
    siginfo_t info;
    waitid(P_PID, child, &info, WEXITED | WNOWAIT);
    double zombie_time = seconds(childs);
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "clock %#x: %.3f s", (unsigned) childs, zombie_time);
    check("a zombie child's clock, before it is reaped", err == 0 && zombie_time >= 0.2, detail);

    struct tid_as_process tid_out;
    pthread_create(&t, NULL, tid_as_process, &tid_out);
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "clock_getcpuclockid %d (%s)", tid_out.getcpuclockid_err,
             strerror(tid_out.getcpuclockid_err));
    check("a thread's tid names no process for clock_getcpuclockid", tid_out.getcpuclockid_err == ESRCH, detail);
    snprintf(detail, sizeof(detail), "clock_gettime %d (%s)", tid_out.gettime,
             tid_out.gettime < 0 ? strerror(tid_out.gettime_errno) : "ok");
    check("clock_gettime of the caller's own tid as a process clock", tid_out.gettime == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
