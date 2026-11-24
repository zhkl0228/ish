// Regression test for unix sockets' names and socket calls on what is no
// socket, as Linux has them.
//
// iSH's bind of no name was ENOENT, where Linux autobinds to an abstract
// name (CPython's test_socket testAutobind); getpeername of a connected unix
// socket said the host's socket iSH backs it with, an abstract name's bytes
// zeros (testLinuxAbstractNamespace); recvmsg said MSG_DONTWAIT in msg_flags
// of every receive, the flag iSH calls the host with (testSendAndRecvFds);
// and a socket call on a file was EBADF, where Linux says ENOTSOCK
// (test_socket_fileno_requires_socket_fd).
//
// Checked: bind of no name gives an abstract name of five hex digits, a
// second socket another; connect to no name is EINVAL; getpeername of a
// connecting end is the listener's abstract name, of the accepted end the
// connecting end's own; getsockname into 0 bytes says the whole length;
// recvmsg's msg_flags is 0 for a whole message;
// getsockopt on a file is ENOTSOCK, on no fd EBADF. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_unix_names regress_unix_names.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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

// A file's fd. Not /dev/null's: a realfs root has no device files, and the
// CI's, an Alpine minirootfs fresh each run, no /dev/null at all, where open
// said -1 and every check of that fd EBADF
static int a_file(void) {
    char path[] = "/tmp/regress_unix_names.XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    return fd;
}

static int is_autobound(const struct sockaddr_un *a, socklen_t len) {
    if (len != offsetof(struct sockaddr_un, sun_path) + 6 || a->sun_path[0] != '\0')
        return 0;
    for (int i = 1; i < 6; i++) {
        char c = a->sun_path[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

int main(void) {
    char detail[200];
    struct sockaddr_un empty = {.sun_family = AF_UNIX};
    int s1 = socket(AF_UNIX, SOCK_STREAM, 0), s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    int r1 = bind(s1, (struct sockaddr *) &empty, sizeof(sa_family_t));
    int e = errno;
    int r2 = bind(s2, (struct sockaddr *) &empty, sizeof(sa_family_t));
    struct sockaddr_un n1, n2;
    socklen_t l1 = sizeof(n1), l2 = sizeof(n2);
    getsockname(s1, (struct sockaddr *) &n1, &l1);
    getsockname(s2, (struct sockaddr *) &n2, &l2);
    snprintf(detail, sizeof(detail), "%d (%s), %d; lengths %u %u, names '\\0%.5s' '\\0%.5s'", r1,
             r1 < 0 ? strerror(e) : "ok", r2, (unsigned) l1, (unsigned) l2, n1.sun_path + 1, n2.sun_path + 1);
    check("bind of no name autobinds to an abstract name", r1 == 0 && r2 == 0 && is_autobound(&n1, l1) &&
          is_autobound(&n2, l2) && memcmp(n1.sun_path, n2.sun_path, 6) != 0, detail);
    close(s1);
    close(s2);

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    int r = connect(c, (struct sockaddr *) &empty, sizeof(sa_family_t));
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("connect to no name", r == -1 && e == EINVAL, detail);
    close(c);

    struct sockaddr_un abstract = {.sun_family = AF_UNIX};
    const char name[] = "\0regress-unix-names\0\xff";
    memcpy(abstract.sun_path, name, sizeof(name) - 1);
    socklen_t alen = offsetof(struct sockaddr_un, sun_path) + sizeof(name) - 1;
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    bind(listener, (struct sockaddr *) &abstract, alen);
    listen(listener, 1);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un client_name = {.sun_family = AF_UNIX};
    memcpy(client_name.sun_path, "\0regress-client", 15);
    bind(client, (struct sockaddr *) &client_name, offsetof(struct sockaddr_un, sun_path) + 15);
    connect(client, (struct sockaddr *) &abstract, alen);
    int accepted = accept(listener, NULL, NULL);
    struct sockaddr_un peer;
    socklen_t plen = sizeof(peer);
    memset(&peer, 0x55, sizeof(peer));
    r = getpeername(client, (struct sockaddr *) &peer, &plen);
    snprintf(detail, sizeof(detail), "%d, length %u (want %u), name %s", r, (unsigned) plen, (unsigned) alen,
             plen == alen && memcmp(peer.sun_path, name, sizeof(name) - 1) == 0 ? "the listener's" : "another");
    check("getpeername of a connecting end", r == 0 && plen == alen &&
          memcmp(peer.sun_path, name, sizeof(name) - 1) == 0, detail);
    plen = sizeof(peer);
    r = getpeername(accepted, (struct sockaddr *) &peer, &plen);
    snprintf(detail, sizeof(detail), "%d, length %u, name %s", r, (unsigned) plen,
             memcmp(peer.sun_path, "\0regress-client", 15) == 0 ? "the client's" : "another");
    check("getpeername of the accepted end", r == 0 && plen == offsetof(struct sockaddr_un, sun_path) + 15 &&
          memcmp(peer.sun_path, "\0regress-client", 15) == 0, detail);

    // a buffer of 0 bytes: nothing written, the whole length said (its room
    // was taken less the family's, and memset past it)
    socklen_t zero = 0;
    r = getsockname(listener, (struct sockaddr *) &peer, &zero);
    snprintf(detail, sizeof(detail), "%d, length %u (want %u)", r, (unsigned) zero, (unsigned) alen);
    check("getsockname into 0 bytes says the whole length", r == 0 && zero == alen, detail);

    write(client, "hello", 5);
    char buf[16];
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n = recvmsg(accepted, &msg, 0);
    snprintf(detail, sizeof(detail), "%zd, msg_flags %#x", n, msg.msg_flags);
    check("recvmsg's msg_flags for a whole message", n == 5 && msg.msg_flags == 0, detail);
    close(accepted);
    close(client);
    close(listener);

    int file = a_file();
    int type;
    socklen_t tlen = sizeof(type);
    r = getsockopt(file, SOL_SOCKET, SO_TYPE, &type, &tlen);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("getsockopt on a file", r == -1 && e == ENOTSOCK, detail);
    close(file);
    r = getsockopt(file, SOL_SOCKET, SO_TYPE, &type, &tlen);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("getsockopt on no fd", r == -1 && e == EBADF, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
