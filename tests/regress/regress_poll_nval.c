// Regression test for poll of an fd that is not: POLLNVAL, counted as
// ready, at once, as Linux has it.
//
// iSH set POLLNVAL in its revents but counted only the fds it watched: with
// no timeout the poll waited for good (CPython's test_poll hung in its unit
// tests), and with one it said 0 ready.
//
// Checked, under a 2 s alarm (EINTR: it waited): poll of a closed fd with no
// timeout; with one, and with an open pipe beside it that is not ready.
// Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_poll_nval regress_poll_nval.c

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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

int main(void) {
    struct sigaction sa = {.sa_handler = on_alarm};
    sigaction(SIGALRM, &sa, NULL);
    char detail[120];

    // the open pipe first: one made after would have the closed one's numbers
    int open_pipe[2];
    pipe(open_pipe);
    int closed[2];
    pipe(closed);
    close(closed[0]);
    close(closed[1]);

    struct pollfd one = {.fd = closed[0], .events = POLLIN};
    alarm(2);
    int r = poll(&one, 1, -1);
    int e = errno;
    alarm(0);
    snprintf(detail, sizeof(detail), "%d (%s), revents %#x", r, r < 0 ? strerror(e) : "ok", one.revents);
    check("a closed fd, no timeout", r == 1 && one.revents == POLLNVAL, detail);

    struct pollfd two[2] = {{.fd = open_pipe[0], .events = POLLIN}, {.fd = closed[0], .events = POLLIN}};
    alarm(2);
    r = poll(two, 2, 1000);
    e = errno;
    alarm(0);
    snprintf(detail, sizeof(detail), "%d (%s), revents %#x and %#x", r, r < 0 ? strerror(e) : "ok", two[0].revents,
             two[1].revents);
    check("a closed fd beside a pipe not ready, with a timeout", r == 1 && two[0].revents == 0 &&
          two[1].revents == POLLNVAL, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
