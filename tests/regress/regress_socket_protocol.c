// Regression test for socket and socketpair of a protocol the type does not
// have: EPROTONOSUPPORT, as Linux's; a type there is not stays EINVAL.
//
// iSH said EINVAL of both: CPython's test_socket probes UDP-Lite
// (IPPROTO_UDPLITE) at import, takes only EPROTONOSUPPORT for "none", and
// could not be imported.
//
// Checked: an AF_INET datagram socket of UDP-Lite, a stream socket of UDP,
// a unix socketpair of protocol 1 say EPROTONOSUPPORT; a type there is not
// EINVAL; a datagram socket of UDP opens. Natively passes (where the kernel
// has no UDP-Lite, as Linux 7.1 on).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_socket_protocol regress_socket_protocol.c

#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef IPPROTO_UDPLITE
#define IPPROTO_UDPLITE 136
#endif

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

static void expect(const char *name, int r, int want) {
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, r == -1 && e == want, detail);
    if (r >= 0)
        close(r);
}

int main(void) {
    expect("an AF_INET datagram socket of UDP-Lite", socket(AF_INET, SOCK_DGRAM, IPPROTO_UDPLITE), EPROTONOSUPPORT);
    expect("an AF_INET stream socket of UDP", socket(AF_INET, SOCK_STREAM, IPPROTO_UDP), EPROTONOSUPPORT);
    int pair[2];
    expect("a unix socketpair of protocol 1", socketpair(AF_UNIX, SOCK_STREAM, 1, pair), EPROTONOSUPPORT);
    expect("a type there is not", socket(AF_INET, 99, 0), EINVAL);

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", fd, fd < 0 ? strerror(e) : "ok");
    check("a datagram socket of UDP opens", fd >= 0, detail);
    if (fd >= 0)
        close(fd);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
