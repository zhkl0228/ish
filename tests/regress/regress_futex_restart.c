// Regression test for a futex wait a signal interrupts with no handler run
// for it: it starts again, as Linux's (ERESTARTSYS), and the program sees no
// EINTR; with a handler without SA_RESTART run, EINTR.
//
// iSH returned EINTR from every futex call a signal interrupted, handler or
// none, and musl passes a futex wait's EINTR on once the program has a handler
// installed without SA_RESTART: sem_wait returned -1 for a signal that was
// ignored. In regress_process_signals a forked child's SIGCHLD, sent while the
// process blocked it (musl's fork blocks every signal), went to a worker
// waiting in sem_wait, which went on before it was told to, about one run in
// a hundred under load.
//
// Checked: with SIGUSR2's handler installed without SA_RESTART, main blocking
// SIGCHLD and a worker not, a child forked and exited (SIGCHLD to the process,
// taken by the worker and ignored) leaves the worker in sem_wait until main
// posts; SIGUSR2 to the worker (pthread_kill), its handler run, makes sem_wait
// return EINTR.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_futex_restart regress_futex_restart.c -lpthread

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
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

static sem_t go, waiting;
static volatile int returned, result, result_errno, handled;

static void on_usr2(int sig) {
    (void) sig;
    handled = 1;
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0)
        continue;
}

// Waits once in sem_wait, SIGCHLD unblocked, and keeps what it returned
static void *worker(void *arg) {
    (void) arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    sem_post(&waiting);
    errno = 0;
    result = sem_wait(&go);
    result_errno = errno;
    returned = 1;
    return NULL;
}

int main(void) {
    char detail[200];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr2;  // no SA_RESTART: musl passes EINTR on
    sigaction(SIGUSR2, &sa, NULL);
    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &chld, NULL);
    sem_init(&go, 0, 0);
    sem_init(&waiting, 0, 0);

    // 1. SIGCHLD, ignored, taken by the waiting worker: sem_wait goes on waiting
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    sem_wait(&waiting);
    sleep_ms(100);  // in sem_wait by now
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    waitpid(child, NULL, 0);
    sleep_ms(200);
    int early = returned;
    sem_post(&go);
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "returned before the post: %d; sem_wait %d (%s)", early, result,
             result < 0 ? strerror(result_errno) : "ok");
    check("SIGCHLD ignored by the thread in sem_wait: it waits on, until posted", !early && result == 0,
          detail);

    // 2. SIGUSR2 to the worker, its handler (without SA_RESTART) run: EINTR
    returned = 0;
    pthread_create(&t, NULL, worker, NULL);
    sem_wait(&waiting);
    sleep_ms(100);
    pthread_kill(t, SIGUSR2);
    for (int i = 0; i < 300 && !returned; i++)
        sleep_ms(10);
    int interrupted = returned;
    if (!interrupted)
        sem_post(&go);
    pthread_join(t, NULL);
    snprintf(detail, sizeof(detail), "returned on the signal: %d; handled %d; sem_wait %d (%s)", interrupted,
             handled, result, result < 0 ? strerror(result_errno) : "ok");
    check("a handler without SA_RESTART run: sem_wait returns EINTR",
          interrupted && handled && result == -1 && result_errno == EINTR, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
