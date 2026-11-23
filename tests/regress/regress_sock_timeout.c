// Regression test for SO_RCVTIMEO and SO_SNDTIMEO as a 64-bit guest's libc
// names them.
//
// musl's and glibc's SO_RCVTIMEO and SO_SNDTIMEO on a 64-bit target are
// Linux's 20 and 21 (its _OLD ones, of the same 64-bit timeval there as 66
// and 67): iSH knew only 66 and 67, and failed these with EINVAL. iputils'
// ping warned of it on every run, and no receive timed out.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sock_timeout regress_sock_timeout.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

static long ms_since(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

// set, read back, and a receive with nothing to receive
static void timeouts(const char *name, int s) {
    char detail[200];
    struct timeval tv = {0, 200000};
    int set_rcv = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int set_rcv_errno = errno;
    struct timeval snd = {1, 500000};
    int set_snd = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    struct timeval got_rcv = {0}, got_snd = {0};
    socklen_t len = sizeof(got_rcv);
    getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &got_rcv, &len);
    len = sizeof(got_snd);
    getsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &got_snd, &len);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    char c;
    ssize_t n = recv(s, &c, 1, 0);
    int recv_errno = errno;
    long waited = ms_since(&start);
    snprintf(detail, sizeof(detail), "SO_RCVTIMEO %d (%s), SO_SNDTIMEO %d; read back %ld.%06ld, %ld.%06ld; "
             "recv %zd (%s) after %ld ms", set_rcv, strerror(set_rcv_errno), set_snd, (long) got_rcv.tv_sec,
             (long) got_rcv.tv_usec, (long) got_snd.tv_sec, (long) got_snd.tv_usec, n, strerror(recv_errno), waited);
    check(name, set_rcv == 0 && set_snd == 0 && got_rcv.tv_sec == 0 && got_rcv.tv_usec == 200000 &&
          got_snd.tv_sec == 1 && got_snd.tv_usec == 500000 && n < 0 && recv_errno == EAGAIN && waited >= 150 &&
          waited < 2000, detail);
}

int main(void) {
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(udp, (struct sockaddr *) &addr, sizeof(addr));
    timeouts("UDP: set, read back, a receive times out", udp);
    close(udp);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    socklen_t addr_len = sizeof(addr);
    bind(server, (struct sockaddr *) &addr, sizeof(addr));
    listen(server, 1);
    getsockname(server, (struct sockaddr *) &addr, &addr_len);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *) &addr, sizeof(addr));
    timeouts("TCP: set, read back, a receive times out", client);
    close(client);
    close(server);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
