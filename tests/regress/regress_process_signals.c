// Regression test for signals sent to a process as a whole.
//
// On Linux a signal sent to a process (kill, a terminal's SIGINT, SIGCHLD,
// alarm's SIGALRM) is pending for the whole process: any thread that does
// not block it takes it, also one that unblocks it later or waits for it in
// sigwait, and it still arrives after the main thread has left with
// pthread_exit. A signal sent to one thread (tgkill) stays with that thread.
// iSH queued every signal on one thread, mostly the main thread: a signal
// all threads blocked when it came stayed there, SIGCHLD and SIGALRM went to
// the main thread even when another waited for them, and a kill after the
// main thread had left was dropped (or crashed iSH, which locked that
// thread's released signal handlers).
//
// Also POSIX timers: SIGEV_THREAD_ID's signal went to the main thread, not
// the thread named, and glibc's and musl's SIGEV_THREAD timers wait for it in
// a thread of their own; a thread id not of the process returned EINVAL with
// a lock still held, and iSH hung.
//
// Also: a blocked signal whose action is to ignore it (SIGCHLD's default) was
// dropped when sent, so sigwait could never take it. Linux keeps it.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_process_signals regress_process_signals.c

#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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

static pid_t tid(void) {
    return (pid_t) syscall(SYS_gettid);
}

static volatile pid_t handled_by; // the thread whose SIGUSR1 handler ran

static void on_usr1(int sig) {
    (void) sig;
    handled_by = tid();
}

static void mask(int how, int sig) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    pthread_sigmask(how, &set, NULL);
}

static int pending_here(int sig) {
    sigset_t set;
    sigpending(&set);
    return sigismember(&set, sig);
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0)
        continue;
}

// Waits up to 3 s for handled_by; nanosleep returns early on a signal.
static void wait_handled(void) {
    for (int i = 0; i < 300 && handled_by == 0; i++) {
        struct timespec ts = {0, 10 * 1000000};
        nanosleep(&ts, NULL);
    }
}

// From another process, as kill(1) would.
static void kill_from_child(int sig) {
    pid_t target = getpid();
    pid_t child = fork();
    if (child == 0) {
        kill(target, sig);
        _exit(0);
    }
    waitpid(child, NULL, 0);
}

static sem_t ready, go, done;
static pid_t worker_tid;
static int worker_result;

// Cases 1 and 2: a worker that does not block SIGUSR1, or blocks it until told.
static void *worker_unblocked(void *arg) {
    (void) arg;
    worker_tid = tid();
    mask(SIG_UNBLOCK, SIGUSR1);
    sem_post(&ready);
    wait_handled();
    sem_post(&done);
    return NULL;
}

static void *worker_unblocks_later(void *arg) {
    (void) arg;
    worker_tid = tid();
    // blocked, as inherited from main
    sem_post(&ready);
    sem_wait(&go);
    worker_result = pending_here(SIGUSR1);
    mask(SIG_UNBLOCK, SIGUSR1); // the handler runs on the way out
    sem_post(&done);
    sem_wait(&go);
    return NULL;
}

// Cases 3, 5-8: a worker that waits for sig with sigtimedwait.
static int wait_sig;
static void *worker_sigwait(void *arg) {
    (void) arg;
    worker_tid = tid();
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, wait_sig);
    struct timespec timeout = {3, 0};
    sem_post(&ready);
    worker_result = sigtimedwait(&set, NULL, &timeout);
    return NULL;
}

// Case 4: a worker that does not block SIGUSR1 while tgkill targets main.
static void *worker_bystander(void *arg) {
    (void) arg;
    mask(SIG_UNBLOCK, SIGUSR1);
    sem_post(&ready);
    sem_wait(&go);
    worker_result = pending_here(SIGUSR1);
    return NULL;
}

static volatile int timer_fired;
static void on_timer(union sigval value) {
    (void) value;
    timer_fired = 1;
}

static int arm(timer_t timer) {
    struct itimerspec its = {.it_value = {0, 50 * 1000000}};
    return timer_settime(timer, 0, &its, NULL);
}

// Case 11, after the main thread has left.
static pid_t main_tid;
static void *after_main_exit(void *arg) {
    (void) arg;
    char detail[128];
    sleep_ms(100); // let the main thread finish leaving
    handled_by = 0;
    mask(SIG_UNBLOCK, SIGUSR1);
    kill_from_child(SIGUSR1);
    wait_handled();
    snprintf(detail, sizeof(detail), "handled by %d, want %d", handled_by, tid());
    check("kill after the main thread's pthread_exit", handled_by == tid(), detail);
    printf("%d passed, %d failed\n", passed, failed);
    exit(failed != 0);
}

int main(int argc, char **argv) {
    (void) argc;
    // run.sh runs this as init, whose process group is 1 (kill(-1) is every
    // process) and which ignores signals it has no handler for: the cases
    // run in a child. The child runs this program anew, not as forked: a
    // forked child's main thread has no clear_child_tid (musl's _Fork sets
    // none, and fork does not pass one on), so the thread list lock it holds
    // as it leaves with pthread_exit (case 11) was never let go, and the
    // threads that took it after hung: case 11's fork, and case 10's
    // SIGEV_THREAD thread if it had not left yet, which under load (ASan,
    // tests run side by side) it had not, about one run in twenty. The same
    // on Linux; started anew, musl gives the main thread one. Its own file is
    // /proc/self/exe, wherever it was found (argv[0] need hold no path).
    if (getpid() == 1) {
        pid_t child = fork();
        if (child == 0) {
            execv("/proc/self/exe", argv);
            printf("FAIL execv /proc/self/exe: %s\n", strerror(errno));
            _exit(1);
        }
        int status;
        waitpid(child, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    char detail[160];
    pthread_t t;
    sem_init(&ready, 0, 0);
    sem_init(&go, 0, 0);
    sem_init(&done, 0, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    main_tid = tid();

    // 1. Main blocks it, a worker does not: the worker takes it.
    mask(SIG_BLOCK, SIGUSR1);
    handled_by = 0;
    pthread_create(&t, NULL, worker_unblocked, NULL);
    sem_wait(&ready);
    kill_from_child(SIGUSR1);
    sem_wait(&done);
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "handled by %d, want worker %d", handled_by, worker_tid);
    check("kill: taken by the thread that does not block it", handled_by == worker_tid, detail);

    // 2. Every thread blocks it: pending for the process, seen by sigpending in
    // any thread, and taken by the first to unblock it, here not main.
    handled_by = 0;
    pthread_create(&t, NULL, worker_unblocks_later, NULL);
    sem_wait(&ready);
    kill_from_child(SIGUSR1);
    sleep_ms(50);
    int main_sees = pending_here(SIGUSR1);
    sem_post(&go);
    sem_wait(&done);
    snprintf(detail, sizeof(detail), "pending in main %d, in worker %d; handled by %d, want worker %d",
             main_sees, worker_result, handled_by, worker_tid);
    check("kill, all threads block it: pending for the process, taken by the thread that unblocks it",
          main_sees && worker_result && handled_by == worker_tid, detail);
    sem_post(&go);
    pthread_join(t, NULL);

    // 3. Taken by sigwait in another thread.
    wait_sig = SIGUSR1;
    worker_result = 0;
    pthread_create(&t, NULL, worker_sigwait, NULL);
    sem_wait(&ready);
    sleep_ms(20);
    kill_from_child(SIGUSR1);
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "sigtimedwait returned %d, want %d", worker_result, SIGUSR1);
    check("kill: taken by sigwait in another thread", worker_result == SIGUSR1, detail);

    // 4. A signal sent to one thread stays with it.
    handled_by = 0;
    pthread_create(&t, NULL, worker_bystander, NULL);
    sem_wait(&ready);
    syscall(SYS_tgkill, getpid(), main_tid, SIGUSR1);
    sleep_ms(50);
    main_sees = pending_here(SIGUSR1);
    sem_post(&go);
    pthread_join(t, NULL);
    pid_t before_unblock = handled_by;
    mask(SIG_UNBLOCK, SIGUSR1);
    mask(SIG_BLOCK, SIGUSR1);
    snprintf(detail, sizeof(detail), "handled before unblock by %d, pending in main %d, in other %d; then handled by %d",
             before_unblock, main_sees, worker_result, handled_by);
    check("tgkill to a thread that blocks it: stays with that thread",
          before_unblock == 0 && main_sees && !worker_result && handled_by == main_tid, detail);

    // 5. SIGCHLD goes to the process: a thread waiting for it gets it.
    mask(SIG_BLOCK, SIGCHLD);
    wait_sig = SIGCHLD;
    worker_result = 0;
    pthread_create(&t, NULL, worker_sigwait, NULL);
    sem_wait(&ready);
    sleep_ms(20);
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    pthread_join(t, NULL);
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "sigtimedwait returned %d, want %d", worker_result, SIGCHLD);
    check("SIGCHLD: taken by sigwait in another thread", worker_result == SIGCHLD, detail);

    // 6. So does alarm's SIGALRM.
    mask(SIG_BLOCK, SIGALRM);
    wait_sig = SIGALRM;
    worker_result = 0;
    pthread_create(&t, NULL, worker_sigwait, NULL);
    sem_wait(&ready);
    sleep_ms(20);
    alarm(1);
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "sigtimedwait returned %d, want %d", worker_result, SIGALRM);
    check("alarm: SIGALRM taken by sigwait in another thread", worker_result == SIGALRM, detail);

    // 7. A process group's signal, as a terminal's SIGINT (kill(-pgid)).
    mask(SIG_BLOCK, SIGINT);
    wait_sig = SIGINT;
    worker_result = 0;
    pthread_create(&t, NULL, worker_sigwait, NULL);
    sem_wait(&ready);
    sleep_ms(20);
    setpgid(0, 0);
    pid_t pgid = getpgrp();
    child = fork();
    if (child == 0) {
        kill(-pgid, SIGINT); // blocked here, and then the default: dies
        _exit(0);
    }
    pthread_join(t, NULL);
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "sigtimedwait returned %d, want %d", worker_result, SIGINT);
    check("kill of a process group: taken by sigwait in another thread", worker_result == SIGINT, detail);

    // 8. A POSIX timer's signal to a thread (SIGEV_THREAD_ID).
    mask(SIG_BLOCK, SIGUSR2);
    wait_sig = SIGUSR2;
    worker_result = 0;
    pthread_create(&t, NULL, worker_sigwait, NULL);
    sem_wait(&ready);
    sleep_ms(20);
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGUSR2;
#ifdef sigev_notify_thread_id
    sev.sigev_notify_thread_id = worker_tid;
#else
    sev._sigev_un._tid = worker_tid;
#endif
    timer_t timer;
    int created = timer_create(CLOCK_MONOTONIC, &sev, &timer);
    if (created == 0)
        arm(timer);
    pthread_join(t, NULL);
    if (created == 0)
        timer_delete(timer);
    snprintf(detail, sizeof(detail), "timer_create %d (%s), the thread's sigtimedwait returned %d, want %d",
             created, created == 0 ? "ok" : strerror(errno), worker_result, SIGUSR2);
    check("timer SIGEV_THREAD_ID: to the thread named", created == 0 && worker_result == SIGUSR2, detail);

    // 9. ... which must be one of this process's.
#ifdef sigev_notify_thread_id
    sev.sigev_notify_thread_id = getppid();
#else
    sev._sigev_un._tid = getppid();
#endif
    errno = 0;
    created = timer_create(CLOCK_MONOTONIC, &sev, &timer);
    int error = errno;
    if (created == 0)
        timer_delete(timer);
    snprintf(detail, sizeof(detail), "timer_create returned %d (%s)", created, strerror(error));
    check("timer SIGEV_THREAD_ID, a thread of another process: EINVAL", created == -1 && error == EINVAL, detail);

    // 10. SIGEV_THREAD: the C library's thread runs the function.
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = on_timer;
    created = timer_create(CLOCK_MONOTONIC, &sev, &timer);
    if (created == 0)
        arm(timer);
    for (int i = 0; i < 300 && !timer_fired; i++)
        sleep_ms(10);
    if (created == 0)
        timer_delete(timer);
    snprintf(detail, sizeof(detail), "timer_create %d, function ran %d", created, timer_fired);
    check("timer SIGEV_THREAD: the function runs", created == 0 && timer_fired, detail);

    // 11. The main thread leaves; the process still gets signals.
    pthread_create(&t, NULL, after_main_exit, NULL);
    pthread_exit(NULL);
}
