// Regression test for a thread's CPU-time clock (pthread_getcpuclockid).
//
// Linux encodes it ~tid << 3 | 6 (a thread's, its scheduler time), and iSH
// said EINVAL of it: HotSpot times its threads so, and takes clock_getres's
// word that it can, and iSH's own Linux CLI, run by iSH, said every other live
// thread's CPU time 0 (/proc/<pid>/stat, getrusage).
//
// Checked: this thread's clock grows as it computes; another thread's, read
// while it computes 0.3 s, says at least 0.2 s; clock_getres takes the clock;
// a thread of another process is no clock of this one (EINVAL, as Linux).
// Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_thread_cpu_clock regress_thread_cpu_clock.c

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
    clockid_t foreign = (clockid_t) ((~(unsigned) child << 3) | 6);
    struct timespec ts;
    r = clock_gettime(foreign, &ts);
    e = errno;
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "clock %#x: %d (%s)", (unsigned) foreign, r, r < 0 ? strerror(e) : "ok");
    check("a thread of another process is no clock of this one", r == -1 && e == EINVAL, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
