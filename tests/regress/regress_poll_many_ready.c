// Regression test: waits on many fds ready at once.
//
// Before each wait iSH takes the edges its host has queued, so that one an
// edge-triggered registration already reported does not come out again. It
// took them until a host wait returned fewer than 16 events; a
// level-triggered fd that is ready comes out of every host wait, so with 16
// of them ready the wait never began: the thread spun holding the poll's
// lock, and whatever would wake the poll (a key typed for Claude Code, whose
// runtime polls dozens of sockets) waited for the lock, until the watchdog
// killed the app.
//
// Each wait runs in a child: one that does not return in 5 s fails.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_poll_many_ready regress_poll_many_ready.c

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define READY 24

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

static int pipes[READY][2];

// READY pipes, each with a byte to read
static void make_ready(void) {
    for (int i = 0; i < READY; i++) {
        pipe(pipes[i]);
        write(pipes[i][1], "x", 1);
    }
}

// 0 if it returned, what it returned in its exit status; -1 if it hung
static int in_child(int (*body)(void), int *status_out) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(body());
    for (int waited = 0; waited < 500; waited++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            *status_out = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            return 0;
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    // A child spinning in the host does not take its SIGKILL either: not
    // waited for long, or the test hangs with it
    kill(pid, SIGKILL);
    for (int waited = 0; waited < 100 && waitpid(pid, NULL, WNOHANG) != pid; waited++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

// epoll_wait on READY level-triggered fds, three times: each returns them all
static int epoll_level(void) {
    int ep = epoll_create1(0);
    for (int i = 0; i < READY; i++) {
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = i};
        epoll_ctl(ep, EPOLL_CTL_ADD, pipes[i][0], &ev);
    }
    struct epoll_event out[64];
    for (int round = 0; round < 3; round++)
        if (epoll_wait(ep, out, 64, 1000) != READY)
            return 1;
    return 0;
}

// poll on the same, three times
static int poll_level(void) {
    struct pollfd p[READY];
    for (int i = 0; i < READY; i++)
        p[i] = (struct pollfd) {.fd = pipes[i][0], .events = POLLIN};
    for (int round = 0; round < 3; round++)
        if (poll(p, READY, 1000) != READY)
            return 1;
    return 0;
}

// An edge-triggered pipe among them: its one write reported once
static int edge_among_level(void) {
    int ep = epoll_create1(0);
    for (int i = 0; i < READY; i++) {
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = i};
        epoll_ctl(ep, EPOLL_CTL_ADD, pipes[i][0], &ev);
    }
    int edge[2];
    pipe(edge);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u32 = 1000};
    epoll_ctl(ep, EPOLL_CTL_ADD, edge[0], &ev);
    write(edge[1], "y", 1);
    struct epoll_event out[64];
    int reports = 0;
    for (int round = 0; round < 3; round++) {
        int n = epoll_wait(ep, out, 64, 1000);
        if (n < READY)
            return 2;
        for (int i = 0; i < n; i++)
            if (out[i].data.u32 == 1000)
                reports++;
    }
    return reports == 1 ? 0 : 10 + reports;
}

int main(void) {
    char detail[128];
    int status;
    make_ready();

    int r = in_child(epoll_level, &status);
    snprintf(detail, sizeof(detail), r < 0 ? "hung for 5 s" : "exit %d", status);
    check("epoll_wait on 24 ready level-triggered fds returns them", r == 0 && status == 0, detail);

    r = in_child(poll_level, &status);
    snprintf(detail, sizeof(detail), r < 0 ? "hung for 5 s" : "exit %d", status);
    check("poll on 24 ready fds returns them", r == 0 && status == 0, detail);

    r = in_child(edge_among_level, &status);
    snprintf(detail, sizeof(detail), r < 0 ? "hung for 5 s" : status >= 10 ? "the edge reported %d times" : "exit %d",
             status >= 10 ? status - 10 : status);
    check("an edge among them is reported once", r == 0 && status == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
