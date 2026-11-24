// Regression test for the options Java sets on every UDP socket.
//
// OpenJDK's DatagramChannel clears IP_MULTICAST_ALL (level 0, option 49) on
// an IPv4 socket; on an IPv6 one it passes the same number at the IPv6 level
// (Linux's IPV6_RECVPKTINFO), sets IPV6_MULTICAST_HOPS to 1 and clears
// IPV6_MULTICAST_ALL (option 29, JDK 25). iSH knew none
// of them and failed each with EINVAL, and Java gives up on the socket when
// any fails, so no DatagramSocket could be made: Gradle died at its start.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_udp_java_opts regress_udp_java_opts.c

#define _GNU_SOURCE
#include <errno.h>
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

// set an int option as Java does, 0 meaning success
static void set_int(const char *name, int domain, int level, int option, int value) {
    char detail[200];
    int s = socket(domain, SOCK_DGRAM, 0);
    int rc = setsockopt(s, level, option, &value, sizeof(value));
    snprintf(detail, sizeof(detail), "setsockopt(%d, %d, %d) = %d (%s)", level, option, value, rc,
             rc < 0 ? strerror(errno) : "ok");
    check(name, rc == 0, detail);
    close(s);
}

int main(void) {
    set_int("IPv4 UDP: IP_MULTICAST_ALL 0", AF_INET, IPPROTO_IP, IP_MULTICAST_ALL, 0);
    // IP_MULTICAST_ALL's number at the IPv6 level, as Java passes it
    set_int("IPv6 UDP: option 49 (IPV6_RECVPKTINFO) 0", AF_INET6, IPPROTO_IPV6, IPV6_RECVPKTINFO, 0);
    set_int("IPv6 UDP: IPV6_MULTICAST_HOPS 1", AF_INET6, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 1);
    set_int("IPv6 UDP: IPV6_MULTICAST_ALL 0", AF_INET6, IPPROTO_IPV6, IPV6_MULTICAST_ALL, 0);

    // A hop limit other than the default reaches the socket and reads back.
    char detail[200];
    int s = socket(AF_INET6, SOCK_DGRAM, 0);
    int hops = 7, got = 0;
    socklen_t len = sizeof(got);
    int set = setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
    int set_errno = errno;
    int get = getsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &got, &len);
    snprintf(detail, sizeof(detail), "set %d (%s), get %d, read back %d (%u bytes)", set,
             set < 0 ? strerror(set_errno) : "ok", get, got, len);
    check("IPv6 UDP: IPV6_MULTICAST_HOPS 7 reads back", set == 0 && get == 0 && got == 7 && len == sizeof(got),
          detail);
    close(s);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
