// Regression test for /dev/netcap: the guest's network traffic as a pcap
// stream (kernel/netcap.c), for tcpdump -r.
//
// iOS gives an app no packets, so iSH makes them up from its processes'
// socket calls. This opens a capture, makes a TCP connection and a UDP
// datagram over loopback, and reads them back: a pcap header (nanosecond
// timestamps, Linux cooked), the handshake, the data each way with its
// sequence and acknowledgement numbers, both FINs, the datagram, each seen
// once (from its sender, as incoming on lo), with valid IP, TCP and UDP
// checksums. Another user may not open the device, 0600.
//
// iSH only (there is no /dev/netcap elsewhere). Run as /init of a fakefs
// root, where the device node is made at boot.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_netcap regress_netcap.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

static uint16_t get16(const uint8_t *p) {
    return p[0] << 8 | p[1];
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t) get16(p) << 16 | get16(p + 2);
}

static uint16_t csum(const uint8_t *p, size_t len, uint32_t sum) {
    for (; len > 1; p += 2, len -= 2)
        sum += get16(p);
    if (len)
        sum += p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return sum;
}

// what the capture has so far, without waiting
static uint8_t capture[1 << 20];
static size_t captured;

static void drain(int fd) {
    for (;;) {
        ssize_t n = read(fd, capture + captured, sizeof(capture) - captured);
        if (n <= 0)
            return;
        captured += n;
    }
}

struct packet {
    int pkttype, hatype;
    uint32_t src, dst;
    int proto;
    uint16_t sport, dport;
    uint32_t seq, ack;
    int flags;
    const uint8_t *data;
    size_t len;
    int csums_ok;
};

static int parse(struct packet *out, int max) {
    int n = 0;
    size_t at = 24;
    while (at + 16 <= captured && n < max) {
        uint32_t incl;
        memcpy(&incl, capture + at + 8, 4);
        const uint8_t *sll = capture + at + 16;
        const uint8_t *ip = sll + 16;
        struct packet *p = &out[n++];
        memset(p, 0, sizeof(*p));
        p->pkttype = get16(sll);
        p->hatype = get16(sll + 2);
        size_t ip_total = get16(ip + 2);
        p->src = get32(ip + 12);
        p->dst = get32(ip + 16);
        p->proto = ip[9];
        const uint8_t *l4 = ip + 20;
        size_t l4_len = ip_total - 20;
        uint32_t pseudo = csum(ip + 12, 8, 0) + p->proto + l4_len;
        p->csums_ok = csum(ip, 20, 0) == 0xffff && incl == 16 + ip_total;
        p->sport = get16(l4);
        p->dport = get16(l4 + 2);
        if (p->proto == IPPROTO_TCP) {
            p->seq = get32(l4 + 4);
            p->ack = get32(l4 + 8);
            p->flags = l4[13];
            size_t hl = (l4[12] >> 4) * 4;
            p->data = l4 + hl;
            p->len = l4_len - hl;
        } else {
            p->data = l4 + 8;
            p->len = l4_len - 8;
        }
        p->csums_ok = p->csums_ok && csum(l4, l4_len, pseudo) == 0xffff;
        at += 16 + incl;
    }
    return n;
}

static const char *flag_names(int flags) {
    static char out[8][8];
    static int next;
    char *s = out[next++ % 8];
    char *o = s;
    if (flags & 0x02) *o++ = 'S';
    if (flags & 0x01) *o++ = 'F';
    if (flags & 0x04) *o++ = 'R';
    if (flags & 0x08) *o++ = 'P';
    if (flags & 0x10) *o++ = '.';
    *o = 0;
    return s;
}

int main(void) {
    char detail[4096];

    // 1. only root may capture
    pid_t child = fork();
    if (child == 0) {
        setgid(1000);
        setuid(1000);
        int fd = open("/dev/netcap", O_RDONLY);
        _exit(fd >= 0 ? 0 : errno);
    }
    int status;
    waitpid(child, &status, 0);
    snprintf(detail, sizeof(detail), "open: %s", WIFEXITED(status) ? strerror(WEXITSTATUS(status)) : "killed");
    // the node is root's alone (0600), as a packet socket is
    check("another user: EACCES", WIFEXITED(status) && WEXITSTATUS(status) == EACCES, detail);

    // 2. the file header
    int cap = open("/dev/netcap", O_RDONLY | O_NONBLOCK);
    drain(cap);
    uint32_t magic = 0, linktype = 0;
    if (captured >= 24) {
        memcpy(&magic, capture, 4);
        memcpy(&linktype, capture + 20, 4);
    }
    snprintf(detail, sizeof(detail), "open %d (%s), %zu bytes, magic %#x, linktype %u", cap, strerror(errno), captured,
             magic, linktype);
    check("pcap header: nanoseconds, Linux cooked", cap >= 0 && captured == 24 && magic == 0xa1b23c4d && linktype == 113,
          detail);

    // 3. nothing more to read: EAGAIN, not readable
    char c;
    ssize_t none = read(cap, &c, 1);
    int none_errno = errno;
    struct pollfd pfd = {cap, POLLIN, 0};
    int idle = poll(&pfd, 1, 0);
    snprintf(detail, sizeof(detail), "read %zd (%s), poll %d", none, strerror(none_errno), idle);
    check("an empty capture: EAGAIN, not readable", none < 0 && none_errno == EAGAIN && idle == 0, detail);

    // 4. a TCP connection over loopback
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t addr_len = sizeof(addr);
    bind(server, (struct sockaddr *) &addr, sizeof(addr));
    listen(server, 1);
    getsockname(server, (struct sockaddr *) &addr, &addr_len);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *) &addr, sizeof(addr));
    int conn = accept(server, NULL, NULL);
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    getsockname(client, (struct sockaddr *) &client_addr, &client_len);
    char buf[64];
    send(client, "hello", 5, 0);
    ssize_t got1 = recv(conn, buf, sizeof(buf), 0);
    write(conn, "world!", 6);
    ssize_t got2 = read(client, buf, sizeof(buf));
    close(client);
    ssize_t eof = recv(conn, buf, sizeof(buf), 0);
    close(conn);
    close(server);

    // 5. a UDP datagram over loopback
    int u1 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in uaddr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t ulen = sizeof(uaddr);
    bind(u1, (struct sockaddr *) &uaddr, sizeof(uaddr));
    getsockname(u1, (struct sockaddr *) &uaddr, &ulen);
    int u2 = socket(AF_INET, SOCK_DGRAM, 0);
    sendto(u2, "ping", 4, 0, (struct sockaddr *) &uaddr, sizeof(uaddr));
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t ugot = recvfrom(u1, buf, sizeof(buf), 0, (struct sockaddr *) &from, &from_len);
    close(u1);
    close(u2);

    int readable = poll(&pfd, 1, 0);
    drain(cap);
    struct packet pkts[32];
    int n = parse(pkts, 32);
    uint16_t cport = ntohs(client_addr.sin_port), sport = ntohs(addr.sin_port);
    char *o = detail;
    o += sprintf(o, "readable %d, %d packets:", readable, n);
    for (int i = 0; i < n; i++) {
        struct packet *p = &pkts[i];
        o += snprintf(o, detail + sizeof(detail) - o, " [%s %u>%u %s seq %u ack %u len %zu type %d/%d csum %d]",
                      p->proto == IPPROTO_TCP ? "tcp" : "udp", p->sport, p->dport, flag_names(p->flags), p->seq, p->ack,
                      p->len, p->pkttype, p->hatype, p->csums_ok);
    }
    int ok = readable == 1 && n == 8 && got1 == 5 && got2 == 6 && eof == 0 && ugot == 4;
    for (int i = 0; ok && i < n; i++)
        ok = pkts[i].csums_ok && pkts[i].pkttype == 0 && pkts[i].hatype == 772 && pkts[i].src == INADDR_LOOPBACK &&
            pkts[i].dst == INADDR_LOOPBACK;
    if (ok) {
        struct packet *syn = &pkts[0], *synack = &pkts[1], *ack = &pkts[2], *hello = &pkts[3], *world = &pkts[4],
                      *fin_c = &pkts[5], *fin_s = &pkts[6], *udp = &pkts[7];
        uint32_t c0 = syn->seq, s0 = synack->seq;
        ok = syn->flags == 0x02 && syn->sport == cport && syn->dport == sport &&
            synack->flags == 0x12 && synack->sport == sport && synack->ack == c0 + 1 &&
            ack->flags == 0x10 && ack->sport == cport && ack->seq == c0 + 1 && ack->ack == s0 + 1 &&
            hello->flags == 0x18 && hello->sport == cport && hello->seq == c0 + 1 && hello->ack == s0 + 1 &&
            hello->len == 5 && memcmp(hello->data, "hello", 5) == 0 &&
            world->flags == 0x18 && world->sport == sport && world->seq == s0 + 1 && world->ack == c0 + 6 &&
            world->len == 6 && memcmp(world->data, "world!", 6) == 0 &&
            fin_c->flags == 0x11 && fin_c->sport == cport && fin_c->seq == c0 + 6 && fin_c->ack == s0 + 7 &&
            fin_s->flags == 0x11 && fin_s->sport == sport && fin_s->seq == s0 + 7 && fin_s->ack == c0 + 7 &&
            udp->proto == IPPROTO_UDP && udp->dport == ntohs(uaddr.sin_port) && udp->sport == ntohs(from.sin_port) &&
            udp->len == 4 && memcmp(udp->data, "ping", 4) == 0;
    }
    check("loopback TCP and UDP, each once, as Linux shows them", ok, detail);

    close(cap);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
