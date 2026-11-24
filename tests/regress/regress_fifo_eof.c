// Regression test for the end of a FIFO's data: a blocking read says 0 once
// its last writer has closed it.
//
// Darwin's poll says nothing of a FIFO's read end whose writers are gone
// (no POLLIN, no POLLHUP), and iSH made a blocking read of a FIFO once poll
// said it ready: at the end of the data it waited for good (cat of a FIFO;
// CPython's test_logging, test_should_not_rollover_named_pipe).
//
// Checked, each in a child killed after 10 seconds: a reader waiting in
// read gets what a writer writes, then 0 once the writer closes; a reader
// with a writer that has not written yet still waits (neither 0 nor
// EAGAIN), and gets what is written late; a non-blocking reader with a
// writer and nothing written gets EAGAIN. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_fifo_eof regress_fifo_eof.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
static char path[64];

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

// The writer: opens the FIFO, after delay_ms writes text, then after as
// long closes it. Runs in a child of its own.
static pid_t writer(int delay_ms, const char *text) {
    pid_t pid = fork();
    if (pid == 0) {
        int w = open(path, O_WRONLY);
        usleep(delay_ms * 1000);
        write(w, text, strlen(text));
        usleep(delay_ms * 1000);
        close(w);
        _exit(0);
    }
    return pid;
}

// What a reader of the FIFO read till 0 (into got), in a child killed after
// 10 seconds: "blocked" if it was, else the reads' total and the last's
// errno, as the report says
static void read_to_end(int nonblock, char *report, size_t size) {
    int pipe_fds[2];
    pipe(pipe_fds);
    pid_t child = fork();
    if (child == 0) {
        close(pipe_fds[0]);
        int r = open(path, O_RDONLY | (nonblock ? O_NONBLOCK : 0));
        char buf[64], got[64] = "";
        ssize_t n, total = 0;
        while ((n = read(r, buf, sizeof(buf))) > 0) {
            memcpy(got + total, buf, n);
            total += n;
        }
        int e = n < 0 ? errno : 0;
        char line[160];
        int len = snprintf(line, sizeof(line), "read '%.*s', then %zd (%s)", (int) total, got, n, e ? strerror(e) : "end");
        write(pipe_fds[1], line, len);
        _exit(0);
    }
    close(pipe_fds[1]);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status;
    for (;;) {
        if (waitpid(child, &status, WNOHANG) == child)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 10) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        usleep(10000);
    }
    ssize_t n = read(pipe_fds[0], report, size - 1);
    report[n > 0 ? n : 0] = '\0';
    if (n <= 0)
        snprintf(report, size, "blocked");
    close(pipe_fds[0]);
}

int main(void) {
    snprintf(path, sizeof(path), "/tmp/regress_fifo_eof.%d", getpid());
    mkfifo(path, 0600);
    char report[200];

    // the reader opens first and waits in read; the writer writes at once
    // and closes 0.2 s later
    pid_t w = writer(0, "hello");
    read_to_end(0, report, sizeof(report));
    waitpid(w, NULL, 0);
    check("a blocking read: the data, then 0 when the writer closes",
          strcmp(report, "read 'hello', then 0 (end)") == 0, report);

    // the writer is open 0.5 s before it writes: the reader waits for it
    w = writer(500, "late");
    read_to_end(0, report, sizeof(report));
    waitpid(w, NULL, 0);
    check("a blocking read waits for a writer that has not written",
          strcmp(report, "read 'late', then 0 (end)") == 0, report);

    // non-blocking: the writer is open and has written nothing yet
    w = writer(500, "x");
    usleep(200000);
    read_to_end(1, report, sizeof(report));
    waitpid(w, NULL, 0);
    char want[100];
    snprintf(want, sizeof(want), "read '', then -1 (%s)", strerror(EAGAIN));
    check("a non-blocking read with a writer and nothing written is EAGAIN", strcmp(report, want) == 0, report);

    unlink(path);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
