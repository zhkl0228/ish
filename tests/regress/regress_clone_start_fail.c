// Regression test for a clone whose thread does not start (run with
// ISH_TASK_START_FAIL=3: every third task_start fails, as with no host thread
// to give; skips if none failed).
//
// clone gave the guest EAGAIN then, and destroyed the task without undoing
// copy_task: a failed fork's copy of the parent's files stayed open, so a
// pipe's reader whose writer the parent had closed never saw EOF; its memory,
// fs and signal handlers stayed allocated; and the task stayed in its group's
// list of threads, which for a thread is its parent's, read again once freed:
// by the next thread to exit, where an iSH built with AddressSanitizer stops
// at a heap-use-after-free, and by the process's CPU time and its signals.
//
// Checked: fork fails with EAGAIN at times, and after one that did, the
// reader of a pipe gets EOF as soon as the parent closes the writer (5 s, or
// it hangs); pthread_create fails with EAGAIN at times, and the process's CPU
// time (getrusage, across its threads) is read after, and every thread made is
// joined; at the end no process is left in /proc but this one.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_clone_start_fail regress_clone_start_fail.c

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
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

static void hung(int sig) {
    (void) sig;
    printf("FAIL a pipe's reader after a failed fork: no EOF in 5 s (the failed child's copy of the writer open)\n");
    fflush(stdout);
    _exit(1);
}

static void *quick(void *arg) {
    return arg;
}

static int processes(void) {
    int n = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        n += e->d_name[0] >= '0' && e->d_name[0] <= '9';
    closedir(d);
    return n;
}

int main(void) {
    char detail[200];

    // 1. forks, each with a pipe whose writer it would copy
    int fork_failures = 0, eof_after_failure = 0, children = 0;
    signal(SIGALRM, hung);
    for (int i = 0; i < 30; i++) {
        int fds[2];
        pipe(fds);
        pid_t pid = fork();
        if (pid == 0)
            _exit(0);
        if (pid > 0) {
            children++;
            waitpid(pid, NULL, 0);
            close(fds[0]);
            close(fds[1]);
            continue;
        }
        if (errno != EAGAIN) {
            snprintf(detail, sizeof(detail), "fork failed with %s", strerror(errno));
            check("a failed fork's error is EAGAIN", 0, detail);
            return 1;
        }
        fork_failures++;
        close(fds[1]);
        char c;
        alarm(5);
        ssize_t got = read(fds[0], &c, 1);
        alarm(0);
        eof_after_failure += got == 0;
        close(fds[0]);
    }

    // 2. threads, and the process's CPU time across them
    pthread_t threads[30];
    int made = 0, thread_failures = 0;
    for (int i = 0; i < 30; i++) {
        int err = pthread_create(&threads[made], NULL, quick, NULL);
        if (err == 0)
            made++;
        else if (err == EAGAIN)
            thread_failures++;
        else {
            snprintf(detail, sizeof(detail), "pthread_create failed with %s", strerror(err));
            check("a failed pthread_create's error is EAGAIN", 0, detail);
            return 1;
        }
    }
    struct rusage ru;
    int rusage_err = getrusage(RUSAGE_SELF, &ru);
    for (int i = 0; i < made; i++)
        pthread_join(threads[i], NULL);

    if (fork_failures == 0 && thread_failures == 0) {
        printf("SKIP no fork or thread failed to start (ISH_TASK_START_FAIL unset)\n");
        return 0;
    }
    snprintf(detail, sizeof(detail), "%d of 30 failed, %d made", fork_failures, children);
    check("forks fail with EAGAIN at times, the others run", fork_failures > 0 && children > 0, detail);
    snprintf(detail, sizeof(detail), "EOF after %d of %d failed forks", eof_after_failure, fork_failures);
    check("a pipe's reader gets EOF after a failed fork", eof_after_failure == fork_failures, detail);
    snprintf(detail, sizeof(detail), "%d of 30 failed, %d made and joined, getrusage %d", thread_failures, made,
             rusage_err);
    check("threads fail with EAGAIN at times, CPU time read after", thread_failures > 0 && made > 0 && rusage_err == 0,
          detail);
    int left = processes();
    snprintf(detail, sizeof(detail), "%d processes in /proc", left);
    check("no process left but this one", left == 1, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
