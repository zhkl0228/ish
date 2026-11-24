// Regression test for getsockopt(SO_ACCEPTCONN): whether a socket listens.
//
// iSH said ENOPROTOOPT of it. A server handed its socket (inetd, socket
// activation) asks it, and iSH's own Linux CLI, run in iSH, asks the host
// for /proc/net/tcp.
//
// Checked: a TCP socket says 0 before listen and 1 after; the connection it
// accepts and the one that connected say 0; a unix stream socket says 1 when
// it listens and its connections 0; a UDP socket says 0. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_so_acceptconn regress_so_acceptconn.c

#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
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

static void expect(const char *name, int fd, int listening) {
    int value = -1;
    socklen_t len = sizeof(value);
    int r = getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &len);
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s), value %d, length %u", r, r < 0 ? strerror(e) : "ok", value,
             (unsigned) len);
    check(name, r == 0 && value == listening && len == sizeof(value), detail);
}

int main(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(server, (struct sockaddr *) &addr, sizeof(addr));
    expect("a TCP socket before listen", server, 0);
    listen(server, 1);
    expect("a TCP socket that listens", server, 1);
    socklen_t len = sizeof(addr);
    getsockname(server, (struct sockaddr *) &addr, &len);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *) &addr, sizeof(addr));
    int accepted = accept(server, NULL, NULL);
    expect("the TCP connection it accepted", accepted, 0);
    expect("the TCP connection that connected", client, 0);
    close(accepted);
    close(client);
    close(server);

    char path[64];
    snprintf(path, sizeof(path), "/tmp/regress_so_acceptconn.%d", getpid());
    unlink(path);
    int unix_server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un = {.sun_family = AF_UNIX};
    strcpy(un.sun_path, path);
    bind(unix_server, (struct sockaddr *) &un, sizeof(un));
    expect("a unix stream socket before listen", unix_server, 0);
    listen(unix_server, 1);
    expect("a unix stream socket that listens", unix_server, 1);
    int unix_client = socket(AF_UNIX, SOCK_STREAM, 0);
    connect(unix_client, (struct sockaddr *) &un, sizeof(un));
    int unix_accepted = accept(unix_server, NULL, NULL);
    expect("the unix connection it accepted", unix_accepted, 0);
    expect("the unix connection that connected", unix_client, 0);
    close(unix_accepted);
    close(unix_client);
    close(unix_server);
    unlink(path);

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    expect("a UDP socket", udp, 0);
    close(udp);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
