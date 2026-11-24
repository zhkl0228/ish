// Regression test for a socket option iSH does not know: ENOPROTOOPT, as Linux
// says for one it does not know at that level.
//
// The JDK probes TCP_QUICKACK and SO_INCOMING_NAPI_ID with getsockopt on an
// IPv6 TCP socket at its start, and takes any error but ENOPROTOOPT for
// "supported". iSH said EINVAL, so Java offered both, and setting
// TCP_QUICKACK failed with "Invalid argument".
//
// Each case passes when the call succeeds (Linux knows these) or fails with
// ENOPROTOOPT. Prints one line per case and "N passed, M failed". Natively
// passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sockopt_unknown regress_sockopt_unknown.c

#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

static void result(const char *name, int rc, int error) {
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", rc, rc < 0 ? strerror(error) : "ok");
    check(name, rc == 0 || error == ENOPROTOOPT, detail);
}

// getsockopt as the JDK's socketOptionSupported does it
static void probe(const char *name, int level, int option) {
    int s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    int value = 1;
    socklen_t len = sizeof(value);
    int rc = getsockopt(s, level, option, &value, &len);
    result(name, rc, errno);
    close(s);
}

int main(void) {
    probe("getsockopt TCP_QUICKACK", IPPROTO_TCP, TCP_QUICKACK);
    probe("getsockopt SO_INCOMING_NAPI_ID", SOL_SOCKET, SO_INCOMING_NAPI_ID);

    int s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    int on = 1;
    int rc = setsockopt(s, IPPROTO_TCP, TCP_QUICKACK, &on, sizeof(on));
    result("setsockopt TCP_QUICKACK 1", rc, errno);
    close(s);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
