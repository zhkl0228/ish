// Regression test: a sleep a signal interrupts with no handler run for it
// goes on, as Linux's ERESTART_RESTARTBLOCK (restart_syscall).
//
// nanosleep and clock_nanosleep returned EINTR to a process that was stopped
// and continued while sleeping (Ctrl-Z, bg), or that got a signal it ignores
// (SIGCHLD, by default): busybox's sleep, which takes no EINTR, ended at
// once, "Done" in bash's jobs. On Linux they sleep on to the time they slept
// to: what is left, nothing if the stop outlasted it. A handler still gets
// EINTR and the remainder, SA_RESTART or not; restart_syscall with nothing to
// restart says EINTR.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sleep_restart regress_sleep_restart.c

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
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

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pause_ms(long ms) {
    struct timespec ts = {ms / 1000, ms % 1000 * 1000000};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

struct outcome {
    long ret, err, elapsed_ms, rem_ms;
};

enum how { NANOSLEEP, CLOCK_NANOSLEEP };

static void on_usr1(int sig) {
    (void) sig;
}

// A child sleeping ms with how, its outcome through a pipe; the caller does
// to it what the case does. *pid the child's.
static int sleeper(enum how how, long ms, pid_t *pid) {
    int p[2];
    pipe(p);
    *pid = fork();
    if (*pid == 0) {
        close(p[0]);
        struct timespec req = {ms / 1000, ms % 1000 * 1000000}, rem = {0, 0};
        long start = now_ms();
        long ret = how == NANOSLEEP ? syscall(SYS_nanosleep, &req, &rem)
                                    : syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &req, &rem);
        struct outcome o = {ret, ret < 0 ? errno : 0, now_ms() - start, rem.tv_sec * 1000 + rem.tv_nsec / 1000000};
        write(p[1], &o, sizeof(o));
        _exit(0);
    }
    close(p[1]);
    return p[0];
}

static struct outcome outcome_of(int fd, pid_t pid) {
    struct outcome o = {-2, 0, 0, 0};
    read(fd, &o, sizeof(o));
    close(fd);
    waitpid(pid, NULL, 0);
    return o;
}

static const char *describe(struct outcome o) {
    static char buf[160];
    snprintf(buf, sizeof(buf), "returned %ld (%s) after %ld ms, remainder %ld ms", o.ret, o.ret < 0 ? strerror(o.err) : "ok",
             o.elapsed_ms, o.rem_ms);
    return buf;
}

// stopped at stop_ms, continued at cont_ms
static struct outcome stopped(enum how how, long ms, long stop_ms, long cont_ms) {
    pid_t pid;
    int fd = sleeper(how, ms, &pid);
    pause_ms(stop_ms);
    kill(pid, SIGSTOP);
    pause_ms(cont_ms - stop_ms);
    kill(pid, SIGCONT);
    return outcome_of(fd, pid);
}

int main(void) {
    // 1. stopped and continued: it sleeps on, the whole of it
    struct outcome o = stopped(NANOSLEEP, 1500, 300, 700);
    check("nanosleep: stopped and continued, it sleeps on", o.ret == 0 && o.elapsed_ms >= 1500 && o.elapsed_ms < 2300,
          describe(o));

    // 2. a stop that outlasts it: done when continued
    o = stopped(NANOSLEEP, 600, 200, 1300);
    check("nanosleep: a stop that outlasts it, done when continued", o.ret == 0 && o.elapsed_ms >= 1300 &&
          o.elapsed_ms < 1900, describe(o));

    // 3. clock_nanosleep likewise
    o = stopped(CLOCK_NANOSLEEP, 1500, 300, 700);
    check("clock_nanosleep: stopped and continued, it sleeps on", o.ret == 0 && o.elapsed_ms >= 1500 &&
          o.elapsed_ms < 2300, describe(o));

    // 4. SIGCHLD, ignored by default, from a child that ends while it sleeps
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (fork() == 0) {
            pause_ms(200);
            _exit(0);
        }
        struct timespec req = {1, 0}, rem = {0, 0};
        long start = now_ms();
        long ret = syscall(SYS_nanosleep, &req, &rem);
        struct outcome c = {ret, ret < 0 ? errno : 0, now_ms() - start, rem.tv_sec * 1000 + rem.tv_nsec / 1000000};
        write(p[1], &c, sizeof(c));
        _exit(0);
    }
    close(p[1]);
    o = outcome_of(p[0], pid);
    check("nanosleep: an ignored SIGCHLD does not end it", o.ret == 0 && o.elapsed_ms >= 1000 && o.elapsed_ms < 1800,
          describe(o));

    // 5. and 6. a handler, SA_RESTART or not: EINTR, and the remainder
    for (int restart = 0; restart <= 1; restart++) {
        struct sigaction sa = {.sa_handler = on_usr1, .sa_flags = restart ? SA_RESTART : 0};
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        int fd = sleeper(NANOSLEEP, 2000, &pid);
        pause_ms(300);
        kill(pid, SIGUSR1);
        o = outcome_of(fd, pid);
        check(restart ? "a handler with SA_RESTART: EINTR, and the remainder" : "a handler: EINTR, and the remainder",
              o.ret == -1 && o.err == EINTR && o.elapsed_ms < 1000 && o.rem_ms > 1000 && o.rem_ms <= 1700, describe(o));
    }
    signal(SIGUSR1, SIG_DFL);

    // 7. restart_syscall with nothing to restart
    errno = 0;
    long ret = syscall(SYS_restart_syscall);
    int e = errno;
    char detail[80];
    snprintf(detail, sizeof(detail), "returned %ld (%s)", ret, strerror(e));
    check("restart_syscall with nothing to restart: EINTR", ret == -1 && e == EINTR, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
