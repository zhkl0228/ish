// Regression test for a send with MSG_DONTWAIT on a socket without
// O_NONBLOCK: when what it sends does not fit, it is EAGAIN, as on Linux.
//
// Darwin's send takes no MSG_DONTWAIT (sosend asks the socket's own
// non-blocking state only), and iSH passed the flag to it: the send blocked
// until the other end went away, then said EPIPE (CPython's test_socket,
// testSendmsgDontWait, TCP and unix stream).
//
// Checked, of a TCP connection on the loopback and a unix stream socketpair,
// each with send() and with sendmsg(): 512 bytes at a time with MSG_DONTWAIT,
// nothing read, end in EAGAIN within 10 seconds (each in a child, killed if
// it blocks); then the socket still blocks without the flag (a blocking send
// of what does not fit waits, killed after a second). Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_send_dontwait regress_send_dontwait.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

static ssize_t send_one(int s, int use_sendmsg, int flags) {
    static char chunk[512];
    if (!use_sendmsg)
        return send(s, chunk, sizeof(chunk), flags);
    struct iovec iov = {chunk, sizeof(chunk)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    return sendmsg(s, &msg, flags);
}

// The child's exit status: 0 for EAGAIN, 1 for another error; killed if it
// blocks. Waits at most seconds.
static int run_child(int s, int use_sendmsg, int flags, int seconds, int *errno_out) {
    int report[2];
    pipe(report);
    pid_t child = fork();
    if (child == 0) {
        close(report[0]);
        ssize_t r;
        do {
            r = send_one(s, use_sendmsg, flags);
        } while (r > 0);
        int e = errno;
        write(report[1], &e, sizeof(e));
        _exit(e == EAGAIN ? 0 : 1);
    }
    close(report[1]);
    int status = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (waitpid(child, &status, WNOHANG) == child)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= seconds) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        usleep(10000);
    }
    *errno_out = 0;
    read(report[0], errno_out, sizeof(*errno_out));
    close(report[0]);
    return status;
}

static void check_pair(const char *what, int a, int b) {
    (void) b;
    char name[120], detail[120];
    for (int use_sendmsg = 0; use_sendmsg <= 1; use_sendmsg++) {
        int e;
        int status = run_child(a, use_sendmsg, MSG_DONTWAIT, 10, &e);
        int blocked = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        snprintf(name, sizeof(name), "%s: %s with MSG_DONTWAIT until EAGAIN", what, use_sendmsg ? "sendmsg" : "send");
        snprintf(detail, sizeof(detail), "%s", blocked ? "blocked" : e != 0 ? strerror(e) : "no error");
        check(name, !blocked && WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);
    }
    int e;
    int status = run_child(a, 0, 0, 1, &e);
    snprintf(name, sizeof(name), "%s: send without it still blocks", what);
    snprintf(detail, sizeof(detail), "%s", e != 0 ? strerror(e) : "returned");
    check(name, WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, detail);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(listener, (struct sockaddr *) &addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(listener, (struct sockaddr *) &addr, &len);
    listen(listener, 1);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *) &addr, sizeof(addr));
    int server = accept(listener, NULL, NULL);
    check_pair("TCP", client, server);
    close(client);
    close(server);
    close(listener);

    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    check_pair("unix stream", pair[0], pair[1]);
    close(pair[0]);
    close(pair[1]);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
