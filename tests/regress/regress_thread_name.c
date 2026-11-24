// Regression test for a thread's name: prctl(PR_GET_NAME),
// /proc/<pid>/comm and /proc/self/task/<tid>.
//
// iSH said EINVAL of PR_GET_NAME, had no /proc/<pid>/comm, and listed only
// the leader in /proc/<pid>/task: musl's pthread_getname_np failed for any
// thread, and pthread_setname_np of another thread with ENOENT (CPython's
// test_threading, its thread name tests).
//
// Checked: PR_GET_NAME says what PR_SET_NAME set; pthread_setname_np and
// pthread_getname_np of this thread and of another; /proc/self/task lists
// both threads; /proc/self/comm reads the name with a newline; a write to
// another process's comm is EINVAL. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_thread_name regress_thread_name.c

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
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

static volatile pid_t other_tid;
static volatile int done;

static void *other(void *arg) {
    (void) arg;
    other_tid = syscall(SYS_gettid);
    while (!done)
        usleep(10000);
    return NULL;
}

int main(void) {
    char name[32] = "?", detail[200];

    prctl(PR_SET_NAME, "named", 0, 0, 0);
    int r = prctl(PR_GET_NAME, name, 0, 0, 0);
    int e = errno;
    snprintf(detail, sizeof(detail), "%d (%s), '%s'", r, r < 0 ? strerror(e) : "ok", name);
    check("PR_GET_NAME says what PR_SET_NAME set", r == 0 && strcmp(name, "named") == 0, detail);

    r = pthread_setname_np(pthread_self(), "mine");
    int r2 = pthread_getname_np(pthread_self(), name, sizeof(name));
    snprintf(detail, sizeof(detail), "%d, %d (%s), '%s'", r, r2, strerror(r2), name);
    check("pthread_setname_np and pthread_getname_np of this thread", r == 0 && r2 == 0 && strcmp(name, "mine") == 0,
          detail);

    pthread_t t;
    pthread_create(&t, NULL, other, NULL);
    while (!other_tid)
        usleep(1000);
    r = pthread_setname_np(t, "theirs");
    strcpy(name, "?");
    r2 = pthread_getname_np(t, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%d (%s), %d (%s), '%s'", r, strerror(r), r2, strerror(r2), name);
    check("pthread_setname_np and pthread_getname_np of another thread", r == 0 && r2 == 0 &&
          strcmp(name, "theirs") == 0, detail);

    int saw_self = 0, saw_other = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *de;
    while (d != NULL && (de = readdir(d)) != NULL) {
        if (atoi(de->d_name) == getpid())
            saw_self = 1;
        if (atoi(de->d_name) == other_tid)
            saw_other = 1;
    }
    if (d != NULL)
        closedir(d);
    snprintf(detail, sizeof(detail), "this thread %s, the other (%d) %s", saw_self ? "listed" : "not listed",
             other_tid, saw_other ? "listed" : "not listed");
    check("/proc/self/task lists both threads", saw_self && saw_other, detail);

    char buf[64] = "";
    int fd = open("/proc/self/comm", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
        close(fd);
    buf[n > 0 ? n : 0] = '\0';
    snprintf(detail, sizeof(detail), "read %zd: '%s'", n, buf);
    check("/proc/self/comm", n == 5 && strcmp(buf, "mine\n") == 0, detail);

    pid_t child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", child);
    fd = open(path, O_WRONLY);
    n = fd >= 0 ? write(fd, "x", 1) : -1;
    e = errno;
    if (fd >= 0)
        close(fd);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "open %d, write %zd (%s)", fd, n, n < 0 ? strerror(e) : "ok");
    check("a write to another process's comm", fd >= 0 && n == -1 && e == EINVAL, detail);

    done = 1;
    pthread_join(t, NULL);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
