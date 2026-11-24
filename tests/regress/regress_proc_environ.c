// Regression test for /proc/<pid>/environ: the environment a process was
// started with, a NUL after each string.
//
// iSH had no environ (exec never set the environment's range either), so
// nothing could say what a running process had been given: ps e, pgrep's
// environment matches, tr '\0' '\n' < /proc/<pid>/environ. And cmdline had
// one NUL too many at its end, the argument list's own.
//
// Checked: this program, started again with an environment of its own, reads
// it back from /proc/self/environ byte for byte, and the file's mode is 0400
// (its owner's only, as on Linux); its cmdline is its arguments, a NUL after
// each and no more; a child started with another environment reads back as
// that from /proc/<its pid>/environ; a thread's task/<tid>/environ is the
// process's. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_proc_environ regress_proc_environ.c

#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static const char own[] = "A=1\0B=two words\0EMPTY=\0";
static const char other[] = "OTHER=1\0";

// path's bytes into buf, how many
static ssize_t read_all(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = 0, r;
    while ((r = read(fd, buf + n, size - n)) > 0)
        n += r;
    close(fd);
    return n;
}

// the bytes as text, a \0 for each NUL
static void shown(const char *buf, ssize_t n, char *out, size_t size) {
    size_t o = 0;
    for (ssize_t i = 0; i < n && o + 3 < size; i++) {
        if (buf[i] == '\0') {
            out[o++] = '\\';
            out[o++] = '0';
        } else {
            out[o++] = buf[i];
        }
    }
    out[o] = '\0';
}

static volatile pid_t thread_tid;
static volatile int finish;

static void *thread(void *arg) {
    (void) arg;
    thread_tid = syscall(SYS_gettid);
    while (!finish)
        usleep(10000);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        // started again, with an environment of its own
        char *child_argv[] = {argv[0], "check", NULL};
        char *child_env[] = {"A=1", "B=two words", "EMPTY=", NULL};
        execve("/proc/self/exe", child_argv, child_env);
        perror("execve");
        return 1;
    }
    if (strcmp(argv[1], "wait") == 0) {
        pause();
        return 0;
    }

    char buf[512], text[600], detail[700];
    ssize_t n = read_all("/proc/self/environ", buf, sizeof(buf));
    shown(buf, n, text, sizeof(text));
    snprintf(detail, sizeof(detail), "%zd bytes: '%s'", n, text);
    check("/proc/self/environ is the environment it was started with",
          n == sizeof(own) - 1 && memcmp(buf, own, n) == 0, detail);

    char want[256];
    int want_n = snprintf(want, sizeof(want), "%s%ccheck%c", argv[0], 0, 0);
    n = read_all("/proc/self/cmdline", buf, sizeof(buf));
    shown(buf, n, text, sizeof(text));
    snprintf(detail, sizeof(detail), "%zd bytes: '%s'", n, text);
    check("/proc/self/cmdline: a NUL after each argument, no more", n == want_n && memcmp(buf, want, n) == 0, detail);

    struct stat st;
    int r = stat("/proc/self/environ", &st);
    snprintf(detail, sizeof(detail), "%d, mode %o", r, r == 0 ? (unsigned) (st.st_mode & 07777) : 0);
    check("its mode is 0400", r == 0 && (st.st_mode & 07777) == 0400, detail);

    pid_t child = fork();
    if (child == 0) {
        char *wait_argv[] = {argv[0], "wait", NULL};
        char *wait_env[] = {"OTHER=1", NULL};
        execve("/proc/self/exe", wait_argv, wait_env);
        _exit(1);
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", child);
    // until the child has exec'd: its environ is the new one
    for (int i = 0; i < 200; i++) {
        n = read_all(path, buf, sizeof(buf));
        if (n == sizeof(other) - 1 && memcmp(buf, other, n) == 0)
            break;
        usleep(10000);
    }
    shown(buf, n, text, sizeof(text));
    snprintf(detail, sizeof(detail), "%zd bytes: '%s'", n, text);
    check("a child's environ is the one it was started with", n == sizeof(other) - 1 && memcmp(buf, other, n) == 0,
          detail);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    pthread_t t;
    pthread_create(&t, NULL, thread, NULL);
    while (!thread_tid)
        usleep(1000);
    snprintf(path, sizeof(path), "/proc/self/task/%d/environ", thread_tid);
    n = read_all(path, buf, sizeof(buf));
    shown(buf, n, text, sizeof(text));
    snprintf(detail, sizeof(detail), "%zd bytes: '%s'", n, text);
    check("a thread's task/<tid>/environ is the process's", n == sizeof(own) - 1 && memcmp(buf, own, n) == 0,
          detail);
    finish = 1;
    pthread_join(t, NULL);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
