// Regression test for SCM_RIGHTS of an fd that is not: EBADF, nothing sent.
//
// iSH retained the fd it got for it, NULL, and crashed, not the caller
// (CPython's test_socket, test_sendmsg_reentrant_ancillary_mutation, sends
// the bytes "xxxx" as an fd).
//
// Checked: sendmsg of a closed fd, and of an open one with a closed one,
// is EBADF and the other end has nothing to read; of an open one, the other
// end gets a working fd. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_scm_rights_bad_fd regress_scm_rights_bad_fd.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

static ssize_t send_fds(int sock, const int *fds, int count) {
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    char control[CMSG_SPACE(4 * sizeof(int))];
    memset(control, 0, sizeof(control));
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                         .msg_controllen = CMSG_SPACE(count * sizeof(int))};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));
    return sendmsg(sock, &msg, 0);
}

int main(void) {
    char detail[160];
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    fcntl(pair[1], F_SETFL, O_NONBLOCK);

    // the open pipe first: one made after would have the closed one's numbers
    int open_pipe[2];
    pipe(open_pipe);
    int closed[2];
    pipe(closed);
    close(closed[0]);
    close(closed[1]);
    int one[1] = {closed[0]};
    ssize_t r = send_fds(pair[0], one, 1);
    int e = errno;
    char buf[8];
    ssize_t got = recv(pair[1], buf, sizeof(buf), 0);
    snprintf(detail, sizeof(detail), "%zd (%s), the other end read %zd", r, r < 0 ? strerror(e) : "ok", got);
    check("SCM_RIGHTS of a closed fd", r == -1 && e == EBADF && got == -1, detail);

    int two[2] = {open_pipe[0], closed[0]};
    r = send_fds(pair[0], two, 2);
    e = errno;
    got = recv(pair[1], buf, sizeof(buf), 0);
    snprintf(detail, sizeof(detail), "%zd (%s), the other end read %zd", r, r < 0 ? strerror(e) : "ok", got);
    check("SCM_RIGHTS of an open fd and a closed one", r == -1 && e == EBADF && got == -1, detail);

    int good[1] = {open_pipe[0]};
    r = send_fds(pair[0], good, 1);
    e = errno;
    char control[CMSG_SPACE(sizeof(int))];
    char byte;
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    got = recvmsg(pair[1], &msg, 0);
    int received = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (got == 1 && cmsg != NULL && cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&received, CMSG_DATA(cmsg), sizeof(int));
    write(open_pipe[1], "y", 1);
    char through = 0;
    ssize_t n = received >= 0 ? read(received, &through, 1) : -1;
    snprintf(detail, sizeof(detail), "%zd (%s), received fd %d, read %zd '%c'", r, r < 0 ? strerror(e) : "ok",
             received, n, through);
    check("SCM_RIGHTS of an open fd", r == 1 && received >= 0 && n == 1 && through == 'y', detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
