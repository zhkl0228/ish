// Regression test for a pipe end, or a FIFO opened one way, used the other
// way: EBADF at once, as Linux says before anything else.
//
// iSH's blocking pipe read and write wait for poll to say the end ready
// (realfs_wait_ready), and an end not open that way never is: on a Linux
// host a write to a pipe's read end waited for good (CPython's test_signal,
// test_wakeup_write_error, hung there), and a read of its write end waits
// on either host.
//
// Checked, each under a 2 s alarm (EINTR: it waited): write to a pipe's
// read end, read of its write end, and the same of a FIFO opened for
// reading and for writing. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_pipe_wrong_end regress_pipe_wrong_end.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

static void on_alarm(int sig) {
    (void) sig;
}

static void expect_ebadf(const char *name, int fd, int writing) {
    char c = 'x';
    alarm(2);
    ssize_t r = writing ? write(fd, &c, 1) : read(fd, &c, 1);
    int e = errno;
    alarm(0);
    char detail[100];
    snprintf(detail, sizeof(detail), "%zd (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, r == -1 && e == EBADF, detail);
}

int main(void) {
    struct sigaction sa = {.sa_handler = on_alarm};
    sigaction(SIGALRM, &sa, NULL);

    int p[2];
    pipe(p);
    expect_ebadf("write to a pipe's read end", p[0], 1);
    expect_ebadf("read of a pipe's write end", p[1], 0);
    close(p[0]);
    close(p[1]);

    char fifo[64];
    snprintf(fifo, sizeof(fifo), "/tmp/regress_pipe_wrong_end.%d", getpid());
    unlink(fifo);
    mkfifo(fifo, 0600);
    int r = open(fifo, O_RDONLY | O_NONBLOCK);
    int w = open(fifo, O_WRONLY);
    fcntl(r, F_SETFL, 0); // blocking again, as the pipe's
    expect_ebadf("write to a FIFO opened for reading", r, 1);
    expect_ebadf("read of a FIFO opened for writing", w, 0);
    close(r);
    close(w);
    unlink(fifo);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
