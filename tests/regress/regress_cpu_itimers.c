// Regression test for ITIMER_VIRTUAL and ITIMER_PROF: interval timers of
// the process's CPU time, user or user and system, all its threads'.
//
// iSH said EINVAL of both: profilers set them (Go's pprof, gperftools,
// Python's statistical ones), and CPython's test_signal ItimerTest.
//
// Checked: getitimer says what is left of an armed one and its interval,
// and setitimer's old value; SIGPROF comes when a thread that computed and
// exited put the process past the time; SIGVTALRM comes again and again,
// every 50ms of CPU, while this thread computes; none comes while the
// process sleeps; a child has none of its parent's; a process with one
// armed exits at once. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_cpu_itimers regress_cpu_itimers.c

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
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

static volatile int profs, vtalrms;
static void on_signal(int sig) {
    if (sig == SIGPROF)
        profs++;
    else
        vtalrms++;
}

static double seconds(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double tv(struct timeval t) {
    return t.tv_sec + t.tv_usec / 1e6;
}

// computes for about the CPU time given
static void burn(double cpu) {
    double start = seconds(CLOCK_THREAD_CPUTIME_ID);
    volatile unsigned long x = 0;
    while (seconds(CLOCK_THREAD_CPUTIME_ID) - start < cpu)
        for (int i = 0; i < 100000; i++)
            x += i;
}

static void *burner(void *arg) {
    (void) arg;
    burn(0.3);
    return NULL;
}

static void set(int which, double value, double interval, struct itimerval *old) {
    struct itimerval v = {
        .it_value = {(time_t) value, (suseconds_t) ((value - (time_t) value) * 1e6)},
        .it_interval = {(time_t) interval, (suseconds_t) ((interval - (time_t) interval) * 1e6)},
    };
    setitimer(which, &v, old);
}

int main(void) {
    char detail[200];
    struct sigaction sa = {.sa_handler = on_signal, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    sigaction(SIGVTALRM, &sa, NULL);

    struct itimerval v, old;
    errno = 0;
    set(ITIMER_PROF, 10, 2, NULL);
    int r = getitimer(ITIMER_PROF, &v);
    snprintf(detail, sizeof(detail), "%d (%s), value %.6f, interval %.6f", r, r < 0 ? strerror(errno) : "ok",
             tv(v.it_value), tv(v.it_interval));
    check("getitimer of an armed ITIMER_PROF", r == 0 && tv(v.it_value) > 9 && tv(v.it_value) <= 10 &&
          tv(v.it_interval) == 2, detail);
    set(ITIMER_PROF, 0, 0, &old);
    snprintf(detail, sizeof(detail), "value %.6f, interval %.6f", tv(old.it_value), tv(old.it_interval));
    check("setitimer's old value of it", tv(old.it_value) > 9 && tv(old.it_interval) == 2, detail);

    // another thread's time, once it exited
    profs = 0;
    set(ITIMER_PROF, 0.1, 0, NULL);
    pthread_t t;
    pthread_create(&t, NULL, burner, NULL);
    pthread_join(t, NULL);
    for (int i = 0; i < 100 && !profs; i++)
        usleep(10000);
    snprintf(detail, sizeof(detail), "%d SIGPROF", profs);
    check("SIGPROF once a thread that computed 0.3s exited", profs == 1, detail);

    // this thread's, as it computes
    vtalrms = 0;
    set(ITIMER_VIRTUAL, 0.05, 0.05, NULL);
    burn(0.5);
    set(ITIMER_VIRTUAL, 0, 0, NULL);
    snprintf(detail, sizeof(detail), "%d SIGVTALRM in 0.5s", vtalrms);
    check("SIGVTALRM every 50ms of CPU while this thread computes", vtalrms >= 4 && vtalrms <= 11, detail);

    vtalrms = 0;
    set(ITIMER_VIRTUAL, 0.05, 0.05, NULL);
    usleep(300000);
    set(ITIMER_VIRTUAL, 0, 0, NULL);
    snprintf(detail, sizeof(detail), "%d SIGVTALRM", vtalrms);
    check("none while the process sleeps", vtalrms == 0, detail);

    set(ITIMER_PROF, 10, 0, NULL);
    pid_t child = fork();
    if (child == 0) {
        struct itimerval its;
        getitimer(ITIMER_PROF, &its);
        _exit(tv(its.it_value) == 0 ? 0 : 1);
    }
    int status;
    waitpid(child, &status, 0);
    set(ITIMER_PROF, 0, 0, NULL);
    snprintf(detail, sizeof(detail), "child exit %d", WEXITSTATUS(status));
    check("a child has none of its parent's", WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);

    double start = seconds(CLOCK_MONOTONIC);
    child = fork();
    if (child == 0) {
        set(ITIMER_PROF, 10, 1, NULL);
        set(ITIMER_VIRTUAL, 10, 1, NULL);
        _exit(0);
    }
    waitpid(child, &status, 0);
    double took = seconds(CLOCK_MONOTONIC) - start;
    snprintf(detail, sizeof(detail), "%.3fs", took);
    check("a process with them armed exits at once", WIFEXITED(status) && took < 2, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
