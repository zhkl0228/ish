// Regression test for the task of a thread do_exit_group's safety valve gives
// up on.
//
// A thread stuck where signals do not reach it (here: waiting for a vfork
// child, which ignores them) is left running when its process exits: the
// valve unlinks it from its group and goes on. iSH then
//
// - freed its task, if it was the leader, at the second task_destroy after
//   the process was reaped (the "deferred free" list), whenever that came.
//   The thread, waking later, read the freed struct (ASan: heap use after
//   free), as did its vfork child exiting, through its parent pointer;
// - never took it out of the pid table, if it was not the leader: its tid
//   stayed alive for good (kill(tid, 0) succeeded) and its slot was lost.
//
// The struct now lives as long as its thread holds it, children go to init,
// and a left thread's task leaves the pid table when the thread is done.
//
// Each case takes about 4 s: the valve waits 1.5 s, the vfork child 3 s.
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_leaked_task regress_leaked_task.c

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0)
        continue;
}

// Waits in vfork for a child that sleeps 3 s: no signal ends that wait.
static void vfork_wait(void) {
    pid_t child = vfork();
    if (child == 0) {
        struct timespec ts = {3, 0};
        nanosleep(&ts, NULL);
        _exit(0);
    }
}

// Tasks created and destroyed: what freed a leaked leader's struct before.
static void churn(int n) {
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0)
            _exit(0);
        waitpid(p, NULL, 0);
    }
}

static void *exit_soon(void *arg) {
    (void) arg;
    sleep_ms(300);
    _exit(7); // exit_group, the main thread in vfork
}

static int tid_pipe[2];
static void *stuck_thread(void *arg) {
    (void) arg;
    pid_t tid = (pid_t) syscall(SYS_gettid);
    write(tid_pipe[1], &tid, sizeof(tid));
    vfork_wait();
    return NULL;
}

int main(void) {
    char detail[200];
    int status;

    // 1. The leader stuck, its process reaped, more tasks come and go, then
    // the leader's vfork child exits and the leader wakes.
    pid_t p = fork();
    if (p == 0) {
        pthread_t t;
        pthread_create(&t, NULL, exit_soon, NULL);
        vfork_wait();
        _exit(1);
    }
    waitpid(p, &status, 0);
    churn(8);
    sleep_ms(4000);
    churn(4);
    snprintf(detail, sizeof(detail), "status %#x", status);
    check("a stuck leader, reaped, wakes after more tasks come and go",
          WIFEXITED(status) && WEXITSTATUS(status) == 7, detail);

    // 2. Another thread stuck while the process exits: once it wakes, its
    // tid is gone.
    pipe(tid_pipe);
    p = fork();
    if (p == 0) {
        pthread_t t;
        pthread_create(&t, NULL, stuck_thread, NULL);
        sleep_ms(300);
        exit(5);
    }
    pid_t tid = 0;
    read(tid_pipe[0], &tid, sizeof(tid));
    waitpid(p, &status, 0);
    sleep_ms(4000);
    errno = 0;
    int alive = kill(tid, 0);
    int kill_errno = errno;
    snprintf(detail, sizeof(detail), "status %#x; kill(%d, 0) = %d (%s)", status, tid, alive, strerror(kill_errno));
    check("a stuck thread, its process exited, is gone once it wakes",
          WIFEXITED(status) && WEXITSTATUS(status) == 5 && alive == -1 && kill_errno == ESRCH, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
