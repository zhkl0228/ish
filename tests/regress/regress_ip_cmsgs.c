// Regression test for an IP or IPv6 socket's control messages, received and
// sent, as Linux has them.
//
// iSH took IPV6_RECVHOPLIMIT and IPV6_RECVTCLASS for options it did not know
// (ENOPROTOOPT), dropped every control message a receive gave (IP_TTL for
// iputils' ping, SO_TIMESTAMP), and did not send a send's (CPython's
// test_socket, its RFC 3542 tests).
//
// Checked, on the loopback: a hop limit and a traffic class sent with
// sendmsg are received, IPV6_HOPLIMIT and IPV6_TCLASS, by a socket that asked
// for them; a buffer with room for one: the first whole, the second cut to
// what is left, MSG_CTRUNC; no buffer: none, MSG_CTRUNC; IPv4's TTL
// (IP_RECVTTL: IP_TTL, an int) and SO_TIMESTAMP (a timeval near now); a
// control message of a type there is not, sent, is EINVAL. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_ip_cmsgs regress_ip_cmsgs.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

static ssize_t send6(int s, struct sockaddr_in6 *to, int hoplimit, int tclass) {
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    char control[CMSG_SPACE(sizeof(int)) * 2] = {0};
    struct msghdr msg = {.msg_name = to, .msg_namelen = sizeof(*to), .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_HOPLIMIT;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &hoplimit, sizeof(int));
    c = CMSG_NXTHDR(&msg, c);
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_TCLASS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &tclass, sizeof(int));
    return sendmsg(s, &msg, 0);
}

// a datagram's control messages into a buffer of room bytes
static ssize_t receive(int s, char *control, size_t room, struct msghdr *msg) {
    static char byte;
    static struct iovec iov = {&byte, 1};
    *msg = (struct msghdr) {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = room ? control : NULL,
                            .msg_controllen = room};
    return recvmsg(s, msg, 0);
}

int main(void) {
    char detail[200];
    int one = 1;

    int r6 = socket(AF_INET6, SOCK_DGRAM, 0), s6 = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    bind(r6, (struct sockaddr *) &a6, sizeof(a6));
    socklen_t alen = sizeof(a6);
    getsockname(r6, (struct sockaddr *) &a6, &alen);
    int o1 = setsockopt(r6, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one));
    int o2 = setsockopt(r6, IPPROTO_IPV6, IPV6_RECVTCLASS, &one, sizeof(one));
    snprintf(detail, sizeof(detail), "IPV6_RECVHOPLIMIT %d, IPV6_RECVTCLASS %d (%s)", o1, o2, strerror(errno));
    check("IPV6_RECVHOPLIMIT and IPV6_RECVTCLASS are set", o1 == 0 && o2 == 0, detail);

    ssize_t sent = send6(s6, &a6, 7, 0x20);
    char control[256];
    struct msghdr msg;
    ssize_t got = receive(r6, control, sizeof(control), &msg);
    int hoplimit = -1, tclass = -1, count = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        count++;
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT)
            memcpy(&hoplimit, CMSG_DATA(c), sizeof(int));
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS)
            memcpy(&tclass, CMSG_DATA(c), sizeof(int));
    }
    snprintf(detail, sizeof(detail), "sent %zd, got %zd, %d control messages, hop limit %d, traffic class %#x, "
             "flags %#x", sent, got, count, hoplimit, tclass, msg.msg_flags);
    check("a hop limit and traffic class sent, received", sent == 1 && got == 1 && hoplimit == 7 &&
          tclass == 0x20 && !(msg.msg_flags & MSG_CTRUNC), detail);

    send6(s6, &a6, 9, 0x40);
    got = receive(r6, control, CMSG_SPACE(sizeof(int)), &msg);
    struct cmsghdr *first = CMSG_FIRSTHDR(&msg);
    snprintf(detail, sizeof(detail), "got %zd, controllen %zu (want %zu), first len %zu, flags %#x", got,
             (size_t) msg.msg_controllen, (size_t) CMSG_SPACE(sizeof(int)), first ? (size_t) first->cmsg_len : 0,
             msg.msg_flags);
    check("room for one: it whole, MSG_CTRUNC", got == 1 && first != NULL &&
          first->cmsg_len == CMSG_LEN(sizeof(int)) && msg.msg_controllen == CMSG_SPACE(sizeof(int)) &&
          (msg.msg_flags & MSG_CTRUNC), detail);

    send6(s6, &a6, 9, 0x40);
    got = receive(r6, control, 0, &msg);
    snprintf(detail, sizeof(detail), "got %zd, controllen %zu, flags %#x", got, (size_t) msg.msg_controllen,
             msg.msg_flags);
    check("no room: none, MSG_CTRUNC", got == 1 && msg.msg_controllen == 0 && (msg.msg_flags & MSG_CTRUNC), detail);

    int r4 = socket(AF_INET, SOCK_DGRAM, 0), s4 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a4 = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(r4, (struct sockaddr *) &a4, sizeof(a4));
    alen = sizeof(a4);
    getsockname(r4, (struct sockaddr *) &a4, &alen);
    setsockopt(r4, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one));
    setsockopt(r4, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof(one));
    sendto(s4, "y", 1, 0, (struct sockaddr *) &a4, sizeof(a4));
    got = receive(r4, control, sizeof(control), &msg);
    int ttl = -1;
    struct timeval stamp = {0, 0}, now;
    gettimeofday(&now, NULL);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL && c->cmsg_len == CMSG_LEN(sizeof(int)))
            memcpy(&ttl, CMSG_DATA(c), sizeof(int));
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMP)
            memcpy(&stamp, CMSG_DATA(c), sizeof(stamp));
    }
    long off = (long) (now.tv_sec - stamp.tv_sec);
    snprintf(detail, sizeof(detail), "got %zd, ttl %d, timestamp %lld.%06ld (now %lld)", got, ttl,
             (long long) stamp.tv_sec, (long) stamp.tv_usec, (long long) now.tv_sec);
    check("IPv4's TTL and SO_TIMESTAMP received", got == 1 && ttl > 0 && ttl <= 255 && off >= 0 && off < 60 &&
          stamp.tv_usec >= 0 && stamp.tv_usec < 1000000, detail);

    char byte = 'z';
    struct iovec iov = {&byte, 1};
    char bad[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr bad_msg = {.msg_name = &a4, .msg_namelen = sizeof(a4), .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = bad, .msg_controllen = sizeof(bad)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&bad_msg);
    c->cmsg_level = IPPROTO_IP;
    c->cmsg_type = 99;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    ssize_t r = sendmsg(s4, &bad_msg, 0);
    int e = errno;
    snprintf(detail, sizeof(detail), "%zd (%s)", r, r < 0 ? strerror(e) : "ok");
    check("a control message of a type there is not, sent", r == -1 && e == EINVAL, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
