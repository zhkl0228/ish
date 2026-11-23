// Regression test for unix stream connections accepted after connect returns.
//
// On Linux, connect on a unix stream socket returns once the connection is
// queued on the listener; accept takes it later. iSH's connect waited for the
// other end's accept, so a program that connects to its own listener and then
// accepts (an event loop serving and using one socket, as asyncio and node's
// net do) hung in connect forever. A unix datagram socket waited the same way
// for an accept that never comes, so connecting one hung too.
//
// File descriptors did not pass on arm64 at all: the guest's control message
// header was laid out as on x86 (a 4-byte cmsg_len), and recvmsg with room for
// the sender's address failed with EINVAL when the sender was unnamed (a
// socketpair). The cases after the connection ones check those, several
// descriptors in one message, and one too many for the buffer (MSG_CTRUNC).
//
// Each case runs in a child with a 10 second alarm, so one that hangs dies of
// SIGALRM instead of hanging the test. Sockets have abstract names, which
// iSH's realfs can bind. Natively all pass.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_unix_connect regress_unix_connect.c

#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed;

static socklen_t name(struct sockaddr_un *un, const char *abstract) {
    memset(un, 0, sizeof(*un));
    un->sun_family = AF_UNIX;
    size_t n = strlen(abstract);
    memcpy(un->sun_path + 1, abstract, n);
    return offsetof(struct sockaddr_un, sun_path) + 1 + n;
}

static int listener(const char *abstract, int type, int flags) {
    struct sockaddr_un un;
    socklen_t len = name(&un, abstract);
    int fd = socket(AF_UNIX, type | flags, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *) &un, len) < 0)
        return -1;
    if (type != SOCK_DGRAM && listen(fd, 4) < 0)
        return -1;
    return fd;
}

static int connector(const char *abstract, int type) {
    struct sockaddr_un un;
    socklen_t len = name(&un, abstract);
    int fd = socket(AF_UNIX, type, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *) &un, len) < 0)
        return -1;
    return fd;
}

static int send_fd(int sock, int fd, const char *data) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {(void *) data, strlen(data)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == (ssize_t) strlen(data) ? 0 : -1;
}

// Receives data into buf (NUL-terminated) and a file descriptor, or -1.
static int recv_fd(int sock, char *buf, size_t size) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {buf, size - 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control)};
    ssize_t n = recvmsg(sock, &msg, 0);
    buf[n > 0 ? n : 0] = '\0';
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (n <= 0 || cmsg == NULL || cmsg->cmsg_type != SCM_RIGHTS)
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

// Whether a pipe's write end fd_w reaches read end fd_r.
static int pipe_works(int fd_w, int fd_r) {
    char c = 0;
    return write(fd_w, "p", 1) == 1 && read(fd_r, &c, 1) == 1 && c == 'p';
}

// Single thread: connect, write, then accept and read.
static int connect_then_accept(void) {
    int l = listener("ish-uc-1", SOCK_STREAM, 0);
    int c = connector("ish-uc-1", SOCK_STREAM);
    if (l < 0 || c < 0 || write(c, "before", 6) != 6)
        return 1;
    int a = accept(l, NULL, NULL);
    char buf[16] = {0};
    if (a < 0 || read(a, buf, sizeof(buf)) != 6 || strcmp(buf, "before") != 0)
        return 2;
    if (write(a, "back", 4) != 4 || read(c, buf, sizeof(buf)) != 4 || memcmp(buf, "back", 4) != 0)
        return 3;
    return 0;
}

// A file descriptor sent before the accept arrives with its data.
static int fd_before_accept(void) {
    int p[2];
    pipe(p);
    int l = listener("ish-uc-2", SOCK_STREAM, 0);
    int c = connector("ish-uc-2", SOCK_STREAM);
    if (l < 0 || c < 0 || send_fd(c, p[1], "fd") < 0)
        return 1;
    close(p[1]);
    int a = accept(l, NULL, NULL);
    char buf[16];
    int got = recv_fd(a, buf, sizeof(buf));
    if (got < 0 || strcmp(buf, "fd") != 0)
        return 2;
    return pipe_works(got, p[0]) ? 0 : 3;
}

// The connecting end closes before the accept: its data, then end of file.
static int close_before_accept(void) {
    int l = listener("ish-uc-3", SOCK_STREAM, 0);
    int c = connector("ish-uc-3", SOCK_STREAM);
    if (l < 0 || c < 0 || write(c, "bye", 3) != 3)
        return 1;
    close(c);
    int a = accept(l, NULL, NULL);
    char buf[16] = {0};
    if (a < 0 || read(a, buf, sizeof(buf)) != 3 || memcmp(buf, "bye", 3) != 0)
        return 2;
    return read(a, buf, sizeof(buf)) == 0 ? 0 : 3;
}

// The connecting end sends a file descriptor and closes before the accept.
static int fd_then_close_before_accept(void) {
    int p[2];
    pipe(p);
    int l = listener("ish-uc-4", SOCK_STREAM, 0);
    int c = connector("ish-uc-4", SOCK_STREAM);
    if (l < 0 || c < 0 || send_fd(c, p[1], "fd") < 0)
        return 1;
    close(p[1]);
    close(c);
    int a = accept(l, NULL, NULL);
    char buf[16];
    int got = recv_fd(a, buf, sizeof(buf));
    if (got < 0 || strcmp(buf, "fd") != 0 || !pipe_works(got, p[0]))
        return 2;
    return read(a, buf, sizeof(buf)) == 0 ? 0 : 3;
}

// SO_PEERCRED of an accepted socket names the connecting process.
static int peer_credentials(void) {
    int l = listener("ish-uc-5", SOCK_STREAM, 0);
    pid_t child = fork();
    if (child == 0) {
        int c = connector("ish-uc-5", SOCK_STREAM);
        char x;
        read(c, &x, 1); // until the parent closes
        _exit(0);
    }
    int a = accept(l, NULL, NULL);
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int bad = getsockopt(a, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.pid != child;
    close(a);
    waitpid(child, NULL, 0);
    return bad;
}

// A connected datagram socket: connect returns, and a datagram arrives whole.
static int datagram(void) {
    int s = listener("ish-uc-6", SOCK_DGRAM, 0);
    int c = connector("ish-uc-6", SOCK_DGRAM);
    if (s < 0 || c < 0 || send(c, "dgram", 5, 0) != 5)
        return 1;
    char buf[16] = {0};
    return recv(s, buf, sizeof(buf), 0) == 5 && memcmp(buf, "dgram", 5) == 0 ? 0 : 2;
}

// A non-blocking listener: the accepted socket reads only what was written.
static int nonblocking_listener(void) {
    int l = listener("ish-uc-7", SOCK_STREAM, SOCK_NONBLOCK);
    int c = connector("ish-uc-7", SOCK_STREAM);
    if (l < 0 || c < 0 || write(c, "data", 4) != 4)
        return 1;
    int a = accept(l, NULL, NULL);
    if (a < 0)
        return 2;
    fcntl(a, F_SETFL, 0);
    char buf[16] = {0};
    return read(a, buf, sizeof(buf)) == 4 && memcmp(buf, "data", 4) == 0 ? 0 : 3;
}

// A socketpair still passes file descriptors.
static int socketpair_fd(void) {
    int s[2], p[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    pipe(p);
    if (send_fd(s[0], p[1], "pair") < 0)
        return 1;
    char buf[16];
    int got = recv_fd(s[1], buf, sizeof(buf));
    return got >= 0 && strcmp(buf, "pair") == 0 && pipe_works(got, p[0]) ? 0 : 2;
}

// recvmsg with room for the sender's address, from an unnamed socket.
static int recvmsg_with_name(void) {
    int s[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    if (write(s[0], "x", 1) != 1)
        return 1;
    struct sockaddr_un from;
    char c;
    struct iovec iov = {&c, 1};
    struct msghdr msg = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &iov, .msg_iovlen = 1};
    if (recvmsg(s[1], &msg, 0) != 1)
        return 2;
    return msg.msg_namelen == 0 ? 0 : 3;
}

// Three descriptors in one message, received into exactly CMSG_SPACE.
static int three_fds(void) {
    int s[2], p[3][2], fds[3];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    for (int i = 0; i < 3; i++) {
        pipe(p[i]);
        fds[i] = p[i][1];
    }
    char control[CMSG_SPACE(sizeof(fds))] = {0};
    struct iovec iov = {"3", 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
    if (sendmsg(s[0], &msg, 0) != 1)
        return 1;
    char in[CMSG_SPACE(sizeof(fds))] = {0}, c;
    struct iovec riov = {&c, 1};
    struct msghdr rmsg = {.msg_iov = &riov, .msg_iovlen = 1, .msg_control = in, .msg_controllen = sizeof(in)};
    if (recvmsg(s[1], &rmsg, 0) != 1 || rmsg.msg_controllen != CMSG_SPACE(sizeof(fds)) || (rmsg.msg_flags & MSG_CTRUNC))
        return 2;
    cmsg = CMSG_FIRSTHDR(&rmsg);
    if (cmsg == NULL || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len != CMSG_LEN(sizeof(fds)))
        return 3;
    int got[3];
    memcpy(got, CMSG_DATA(cmsg), sizeof(got));
    for (int i = 0; i < 3; i++)
        if (!pipe_works(got[i], p[i][0]))
            return 4 + i;
    return 0;
}

// Two descriptors, room for one: it arrives, the other is closed, MSG_CTRUNC.
static int control_truncated(void) {
    int s[2], p[2][2], fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    for (int i = 0; i < 2; i++) {
        pipe(p[i]);
        fds[i] = p[i][1];
    }
    char control[CMSG_SPACE(sizeof(fds))] = {0};
    struct iovec iov = {"2", 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
    if (sendmsg(s[0], &msg, 0) != 1)
        return 1;
    close(p[1][1]); // the sent copy is then the only write end of the second pipe
    // CMSG_LEN, not CMSG_SPACE: CMSG_SPACE(sizeof(int)) is padded to 8 bytes on
    // arm64, and Linux counts that padding as room for a second descriptor.
    char in[CMSG_LEN(sizeof(int))] __attribute__((aligned(8))) = {0}, c;
    struct iovec riov = {&c, 1};
    struct msghdr rmsg = {.msg_iov = &riov, .msg_iovlen = 1, .msg_control = in, .msg_controllen = sizeof(in)};
    if (recvmsg(s[1], &rmsg, 0) != 1 || !(rmsg.msg_flags & MSG_CTRUNC))
        return 2;
    cmsg = CMSG_FIRSTHDR(&rmsg);
    int got;
    if (cmsg == NULL || cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        return 3;
    memcpy(&got, CMSG_DATA(cmsg), sizeof(int));
    if (!pipe_works(got, p[0][0]))
        return 4;
    // the second was closed: its pipe reads end of file
    return read(p[1][0], &c, 1) == 0 ? 0 : 5;
}

static void check(const char *name, int (*test)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        _exit(test());
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        passed++;
    } else {
        failed++;
        if (WIFSIGNALED(status))
            printf("FAIL %s: killed by signal %d\n", name, WTERMSIG(status));
        else
            printf("FAIL %s: step %d\n", name, WEXITSTATUS(status));
    }
}

int main(void) {
    check("connect, then accept, in one thread", connect_then_accept);
    check("fd sent before accept", fd_before_accept);
    check("close before accept", close_before_accept);
    check("fd sent, then close, before accept", fd_then_close_before_accept);
    check("SO_PEERCRED of accepted socket", peer_credentials);
    check("connected datagram socket", datagram);
    check("non-blocking listener", nonblocking_listener);
    check("socketpair passes fds", socketpair_fd);
    check("recvmsg with a name buffer from an unnamed socket", recvmsg_with_name);
    check("three fds in one message", three_fds);
    check("fds beyond the control buffer are closed (MSG_CTRUNC)", control_truncated);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
