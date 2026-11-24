// Regression test for getpeername of a TCP connection its peer reset:
// ENOTCONN, as Linux says.
//
// Darwin says EINVAL of it ("the socket has been shut down"), and iSH passed
// that on. CPython's ssl asks getpeername before a TLS handshake: ENOTCONN is
// "not connected", and it reads what the peer sent before (and raises
// SSLError, "Closed before TLS handshake with data"); EINVAL it raised as it
// was (test_ssl, TestPreHandshakeClose).
//
// Checked: getpeername of the accepted end says the peer's port while it is
// connected; once the peer has closed with SO_LINGER 0 (a reset) after
// sending, it is ENOTCONN, and what was sent is still read. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_getpeername_reset regress_getpeername_reset.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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

int main(void) {
    char detail[200];
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(listener, (struct sockaddr *) &addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(listener, (struct sockaddr *) &addr, &len);
    listen(listener, 1);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *) &addr, sizeof(addr));
    int server = accept(listener, NULL, NULL);

    struct sockaddr_in mine, peer;
    len = sizeof(mine);
    getsockname(client, (struct sockaddr *) &mine, &len);
    len = sizeof(peer);
    int r = getpeername(server, (struct sockaddr *) &peer, &len);
    int e = errno;
    snprintf(detail, sizeof(detail), "%d (%s), port %d, the client's %d", r, r < 0 ? strerror(e) : "ok",
             ntohs(peer.sin_port), ntohs(mine.sin_port));
    check("getpeername while connected says the peer", r == 0 && peer.sin_port == mine.sin_port, detail);

    struct linger reset = {1, 0};
    setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    send(client, "DELETE /data HTTP/1.0\r\n\r\n", 25, 0);
    close(client);
    usleep(300000);

    len = sizeof(peer);
    r = getpeername(server, (struct sockaddr *) &peer, &len);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("getpeername once the peer reset it is ENOTCONN", r == -1 && e == ENOTCONN, detail);

    fcntl(server, F_SETFL, O_NONBLOCK);
    char byte = 0;
    ssize_t n = recv(server, &byte, 1, 0);
    e = errno;
    snprintf(detail, sizeof(detail), "%zd (%s), '%c'", n, n < 0 ? strerror(e) : "ok", n > 0 ? byte : '?');
    check("what the peer sent before is still read", n == 1 && byte == 'D', detail);

    close(server);
    close(listener);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
