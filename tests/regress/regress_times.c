// Regression test for times() and SIGCHLD's CPU times: clock_t is a long on
// AArch64 Linux.
//
// iSH had clock_t 32 bits, from the i386 ABI: times() wrote half of struct
// tms (two of its four fields, each as two of its own), SIGCHLD's si_utime
// read past the one written and si_stime nothing. times() also gave the
// caller's own CPU time as its children's, and returned 0 where Linux returns
// clock ticks since boot, which callers take differences of.
//
// Checked: times() writes the four fields of struct tms and nothing past
// them; its result grows by about a sleep's ticks; a child it waited for that
// computed 0.3 s is in tms_cutime + tms_cstime, at least 0.2 s and less than
// ten times that; SIGCHLD's siginfo, SA_SIGINFO, has that child's pid, its
// exit status and its CPU time, as much. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_times regress_times.c

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/times.h>
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

static double cpu_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// computes for about the CPU time given
static void burn(double cpu) {
    double start = cpu_seconds();
    volatile unsigned long x = 0;
    while (cpu_seconds() - start < cpu)
        for (int i = 0; i < 100000; i++)
            x += i;
}

static volatile sig_atomic_t chld_seen;
static siginfo_t chld_info;

static void on_chld(int sig, siginfo_t *info, void *uc) {
    (void) sig;
    (void) uc;
    chld_info = *info;
    chld_seen = 1;
}

int main(void) {
    char detail[256];
    long hz = sysconf(_SC_CLK_TCK);

    // the fields, and a guard after them
    struct {
        struct tms t;
        long guard;
    } buf;
    long unset;
    memset(&unset, 0x5a, sizeof(unset));
    memset(&buf, 0x5a, sizeof(buf));
    clock_t before = times(&buf.t);
    snprintf(detail, sizeof(detail), "utime %ld stime %ld cutime %ld cstime %ld guard %#lx",
             (long) buf.t.tms_utime, (long) buf.t.tms_stime, (long) buf.t.tms_cutime,
             (long) buf.t.tms_cstime, (unsigned long) buf.guard);
    check("times() writes struct tms whole, nothing past it",
          buf.t.tms_utime != unset && buf.t.tms_stime != unset && buf.t.tms_cutime != unset &&
          buf.t.tms_cstime != unset && buf.guard == unset, detail);

    struct timespec nap = {0, 300 * 1000 * 1000};
    nanosleep(&nap, NULL);
    clock_t after = times(NULL);
    long ticks = (long) (after - before);
    snprintf(detail, sizeof(detail), "%ld ticks over a 0.3 s sleep at %ld a second (%ld, then %ld)",
             ticks, hz, (long) before, (long) after);
    check("times()'s result grows by the ticks slept", ticks >= hz / 4 && ticks <= hz * 5, detail);

    struct sigaction sa = {0};
    sa.sa_sigaction = on_chld;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    pid_t child = fork();
    if (child == 0) {
        burn(0.3);
        _exit(7);
    }
    int status;
    while (waitpid(child, &status, 0) < 0)
        ;
    while (!chld_seen)
        nanosleep(&(struct timespec) {0, 1000 * 1000}, NULL);

    struct tms t;
    times(&t);
    long cticks = (long) (t.tms_cutime + t.tms_cstime);
    snprintf(detail, sizeof(detail), "cutime %ld + cstime %ld, the caller's own %ld + %ld",
             (long) t.tms_cutime, (long) t.tms_cstime, (long) t.tms_utime, (long) t.tms_stime);
    check("tms_cutime + tms_cstime: the child's 0.3 s", cticks >= hz / 5 && cticks <= hz * 10, detail);

    long sticks = (long) (chld_info.si_utime + chld_info.si_stime);
    snprintf(detail, sizeof(detail), "pid %d (child %d) status %d utime %ld stime %ld",
             (int) chld_info.si_pid, (int) child, chld_info.si_status,
             (long) chld_info.si_utime, (long) chld_info.si_stime);
    check("SIGCHLD's siginfo: pid, status, the child's 0.3 s",
          chld_info.si_pid == child && chld_info.si_status == 7 && sticks >= hz / 5 && sticks <= hz * 10,
          detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
