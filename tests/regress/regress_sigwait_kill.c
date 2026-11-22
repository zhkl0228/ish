// Regression test for a signal sent to a process whose waiting thread blocks it.
//
// kill() put the signal on the process's main thread only. When that thread
// blocked it, it stayed pending there and no other thread got it, not even one
// waiting for it in sigwait. MariaDB blocks SIGTERM and SIGQUIT in every thread
// and handles them in a thread that sigwaits; on shutdown the server signals
// itself, so mariadbd hung with "Signal handler thread did not exit in a
// timely manner".
//
// With the signal blocked everywhere, a thread in sigwait must receive it,
// sent to the thread with pthread_kill and to the process with kill.
//
// Build: aarch64-linux-musl-gcc -static -O1 -pthread -o regress_sigwait_kill regress_sigwait_kill.c
// Run:   ish -r <rootfs> /tmp/regress_sigwait_kill

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static sigset_t set;
static atomic_int received;
static int passed, failed;

static void *waiter(void *arg) {
    (void) arg;
    int sig;
    sigwait(&set, &sig);
    atomic_store(&received, sig);
    return NULL;
}

static void check(const char *name, int process_wide) {
    atomic_store(&received, 0);
    pthread_t thread;
    pthread_create(&thread, NULL, waiter, NULL);
    struct timespec tick = {0, 100 * 1000 * 1000};
    nanosleep(&tick, NULL);  // let it reach sigwait
    if (process_wide)
        kill(getpid(), SIGTERM);
    else
        pthread_kill(thread, SIGTERM);
    for (int i = 0; i < 100 && atomic_load(&received) == 0; i++)
        nanosleep(&tick, NULL);  // up to 10 seconds
    int sig = atomic_load(&received);
    if (sig == SIGTERM) {
        passed++;
        pthread_join(thread, NULL);
    } else {
        failed++;
        printf("FAIL %s: waiting thread got %d\n", name, sig);
    }
}

int main(void) {
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGQUIT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    check("pthread_kill", 0);
    check("kill", 1);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
