// Regression test for what interval timers say and take: getitimer, a fired
// one-shot timer, a timer id that is not, TIMER_ABSTIME's 0, and a timerfd
// set again.
//
// iSH's getitimer was ENOSYS; a one-shot timer that fired said a time below
// 0 (setitimer's old value, timer_settime's); timer_settime of a deleted or
// never made timer set a NULL timer, and timer_delete read past the array of
// them (both crashed iSH, not the caller); a time of 0 with TIMER_ABSTIME,
// which disarms, armed the timer for long ago and it fired at once; and a
// timerfd set again kept the expirations of its old setting, and setting it
// as it fired could deadlock (its lock and the timer's, taken in turn).
//
// Checked: getitimer says 0 of a disarmed ITIMER_REAL and of ITIMER_VIRTUAL,
// and what is left of an armed one; a one-shot setitimer that fired says 0,
// to getitimer and as setitimer's old value; so does a one-shot POSIX timer,
// as timer_settime's old value; timer_settime and timer_delete of a timer
// that is not say EINVAL; TIMER_ABSTIME with 0 disarms a POSIX timer and a
// timerfd; a timerfd set again has no expirations of its old setting; a
// timerfd set again 2000 times as it fires every 50us is not stuck.
// Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_timer_values regress_timer_values.c

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
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

static volatile int alarms;
static void on_alarm(int sig) {
    (void) sig;
    alarms++;
}

static volatile int usr2s;
static void on_usr2(int sig) {
    (void) sig;
    usr2s++;
}

static double tv(struct timeval t) {
    return t.tv_sec + t.tv_usec / 1e6;
}

static double ts(struct timespec t) {
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
    char detail[200];
    signal(SIGALRM, on_alarm);

    struct itimerval v;
    memset(&v, 0xff, sizeof(v));
    int r = getitimer(ITIMER_REAL, &v);
    int e = errno;
    snprintf(detail, sizeof(detail), "%d (%s), value %.6f, interval %.6f", r, r < 0 ? strerror(e) : "ok",
             tv(v.it_value), tv(v.it_interval));
    check("getitimer of a disarmed ITIMER_REAL", r == 0 && tv(v.it_value) == 0 && tv(v.it_interval) == 0, detail);

    memset(&v, 0xff, sizeof(v));
    r = getitimer(ITIMER_VIRTUAL, &v);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s), value %.6f", r, r < 0 ? strerror(e) : "ok", tv(v.it_value));
    check("getitimer of a disarmed ITIMER_VIRTUAL", r == 0 && tv(v.it_value) == 0, detail);

    struct itimerval armed = {.it_value = {10, 0}, .it_interval = {5, 0}};
    setitimer(ITIMER_REAL, &armed, NULL);
    r = getitimer(ITIMER_REAL, &v);
    snprintf(detail, sizeof(detail), "%d, value %.6f, interval %.6f", r, tv(v.it_value), tv(v.it_interval));
    check("getitimer of an armed ITIMER_REAL", r == 0 && tv(v.it_value) > 9 && tv(v.it_value) <= 10 &&
          tv(v.it_interval) == 5, detail);

    struct itimerval once = {.it_value = {0, 50000}};
    alarms = 0;
    setitimer(ITIMER_REAL, &once, NULL);
    while (!alarms)
        pause();
    usleep(100000);
    r = getitimer(ITIMER_REAL, &v);
    snprintf(detail, sizeof(detail), "%d, value %.6f", r, tv(v.it_value));
    check("getitimer of a one-shot ITIMER_REAL that fired", r == 0 && tv(v.it_value) == 0, detail);
    struct itimerval zero = {0}, old;
    memset(&old, 0xff, sizeof(old));
    setitimer(ITIMER_REAL, &zero, &old);
    snprintf(detail, sizeof(detail), "%ld s %ld us", (long) old.it_value.tv_sec, (long) old.it_value.tv_usec);
    check("setitimer's old value of one that fired", old.it_value.tv_sec == 0 && old.it_value.tv_usec == 0, detail);

    timer_t t;
    struct sigevent none = {.sigev_notify = SIGEV_NONE};
    timer_create(CLOCK_MONOTONIC, &none, &t);
    struct itimerspec spec = {.it_value = {0, 20000000}}, old_spec;
    timer_settime(t, 0, &spec, NULL);
    usleep(100000);
    memset(&old_spec, 0xff, sizeof(old_spec));
    timer_settime(t, 0, &(struct itimerspec) {0}, &old_spec);
    snprintf(detail, sizeof(detail), "%.9f", ts(old_spec.it_value));
    check("timer_settime's old value of a one-shot that fired", ts(old_spec.it_value) == 0, detail);

    // with a signal: armed for long ago, it fires at once, and says 0 after
    // as a disarmed one does
    signal(SIGUSR2, on_usr2);
    timer_t t2;
    struct sigevent usr2 = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR2};
    timer_create(CLOCK_MONOTONIC, &usr2, &t2);
    struct itimerspec far = {.it_value = {100, 0}}, cur;
    timer_settime(t2, 0, &far, NULL);
    r = timer_settime(t2, TIMER_ABSTIME, &(struct itimerspec) {0}, NULL);
    usleep(50000);
    timer_gettime(t2, &cur);
    snprintf(detail, sizeof(detail), "%d, then value %.9f, %d signals", r, ts(cur.it_value), usr2s);
    check("TIMER_ABSTIME with 0 disarms a POSIX timer", r == 0 && ts(cur.it_value) == 0 && usr2s == 0, detail);
    timer_delete(t2);

    // the kernel's ids, past what musl's wrappers check
    long id = (long) (intptr_t) t;
    timer_delete(t);
    r = syscall(SYS_timer_settime, id, 0, &far, NULL);
    e = errno;
    snprintf(detail, sizeof(detail), "id %ld: %d (%s)", id, r, r < 0 ? strerror(e) : "ok");
    check("timer_settime of a deleted timer", r == -1 && e == EINVAL, detail);
    r = syscall(SYS_timer_settime, 100000L, 0, &far, NULL);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("timer_settime of a timer never made", r == -1 && e == EINVAL, detail);
    r = syscall(SYS_timer_delete, 100000L);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("timer_delete of a timer never made", r == -1 && e == EINVAL, detail);

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec every = {.it_value = {0, 10000000}, .it_interval = {0, 10000000}};
    timerfd_settime(fd, 0, &every, NULL);
    usleep(100000);
    timerfd_settime(fd, 0, &far, NULL);
    uint64_t n = 0;
    ssize_t got = read(fd, &n, sizeof(n));
    e = errno;
    snprintf(detail, sizeof(detail), "read %zd (%s), %llu expirations", got, got < 0 ? strerror(e) : "ok",
             (unsigned long long) n);
    check("a timerfd set again has none of its old expirations", got == -1 && e == EAGAIN, detail);

    r = timerfd_settime(fd, TFD_TIMER_ABSTIME, &(struct itimerspec) {0}, NULL);
    usleep(50000);
    n = 0;
    got = read(fd, &n, sizeof(n));
    e = errno;
    timerfd_gettime(fd, &cur);
    snprintf(detail, sizeof(detail), "settime %d, read %zd (%s), %llu expirations, value %.9f", r, got,
             got < 0 ? strerror(e) : "ok", (unsigned long long) n, ts(cur.it_value));
    check("TFD_TIMER_ABSTIME with 0 disarms a timerfd", r == 0 && got == -1 && e == EAGAIN && ts(cur.it_value) == 0,
          detail);

    // a negative time, or nanoseconds past a second, is EINVAL, as Linux's
    // itimerspec64_valid (iSH took one for a time long past: CPython's
    // test_os, test_timerfd_negative)
    struct itimerspec bad[] = {
        {.it_value = {-1, 0}}, {.it_value = {1, 0}, .it_interval = {0, -1}},
        {.it_value = {0, 1000000000}},
    };
    int all_einval = 1;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        errno = 0;
        if (timerfd_settime(fd, 0, &bad[i], NULL) != -1 || errno != EINVAL)
            all_einval = 0;
        errno = 0;
        if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &bad[i], NULL) != -1 || errno != EINVAL)
            all_einval = 0;
    }
    timer_t bad_t;
    timer_create(CLOCK_MONOTONIC, &none, &bad_t);
    errno = 0;
    r = timer_settime(bad_t, 0, &bad[0], NULL);
    e = errno;
    timer_delete(bad_t);
    snprintf(detail, sizeof(detail), "timerfd: %s; timer_settime: %d (%s)", all_einval ? "EINVAL" : "not all EINVAL",
             r, r < 0 ? strerror(e) : "ok");
    check("a negative time or nanoseconds past a second is EINVAL", all_einval && r == -1 && e == EINVAL, detail);

    struct itimerspec fast = {.it_value = {0, 50000}, .it_interval = {0, 50000}};
    for (int i = 0; i < 2000; i++)
        timerfd_settime(fd, 0, &fast, NULL);
    timerfd_settime(fd, 0, &(struct itimerspec) {0}, NULL);
    check("a timerfd set again 2000 times as it fires is not stuck", 1, "");
    close(fd);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
