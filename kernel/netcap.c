// /dev/netcap: the guest's network traffic as a pcap stream (see
// kernel/netcap.h).
//
// Each open of /dev/netcap is a capture: a pcap file header, then a record
// for each packet from then on, in Linux's cooked format (LINKTYPE_LINUX_SLL,
// as tcpdump -i any captures), with nanosecond timestamps. Root's only, as a
// packet socket is. A capture that is not read fast enough loses what does
// not fit its buffer.
//
// The packets are made up from the socket calls of iSH's processes, with the
// addresses the host's sockets have:
//   - TCP: connect() and accept() make the handshake, each send and receive a
//     segment (one of up to MAX_SEGMENT bytes per call: as a capture with
//     segmentation offload shows), a receive of nothing the other end's FIN,
//     shutdown() and close() this end's, a reset connection a RST. The
//     sequence numbers of a direction start from a hash of the connection's
//     ends, so that both ends of a connection within iSH agree on them.
//   - UDP: each datagram.
//   - ICMP and ICMPv6 (ping): each message.
// Not seen: what the host's own TCP does (retransmissions, windows, timing,
// segmentation), other apps' traffic, and iOS's.
//
// A connection or datagram between two of iSH's sockets over loopback is
// seen once, from its sender, as incoming on lo: as tcpdump -i any shows
// loopback (libpcap drops a packet's outgoing copy on lo). The receiving end
// makes nothing, so that what a process on the host sends over loopback to
// one of iSH's is not seen.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/netcap.h"
#include "kernel/task.h"
#include "fs/dev.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "util/list.h"
#include "util/sync.h"

#define LINKTYPE_LINUX_SLL 113
#define SNAPLEN 262144
#define READER_BUFFER (4 << 20)
#define MAX_SEGMENT 65000

#define SLL_HOST 0
#define SLL_OUTGOING 4
#define ARPHRD_ETHER_ 1
#define ARPHRD_LOOPBACK_ 772
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_IPV6 0x86dd

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

_Atomic unsigned netcap_readers;

// Guards the readers, their buffers, the flows, and the packet sockets
// (kernel/packet.c)
lock_t netcap_lock = LOCK_INITIALIZER;
static struct list readers = LIST_INITIALIZER(readers);

struct netcap_reader {
    struct list readers;
    struct fd *fd;
    uint8_t *buf;
    size_t start, len;
    // len, read without netcap_lock by poll, which runs under a poll's lock
    _Atomic size_t available;
    uint64_t dropped;
    cond_t changed;
};


// A TCP connection's state, a stream socket's from its first packet
struct netcap_flow {
    struct endpoint local, remote;
    uint32_t seq_out, seq_in;
    // the handshake is over (or was before the capture)
    bool established;
    bool fin_out;
};

static bool endpoint_from(struct endpoint *e, const struct sockaddr *sa) {
    memset(e, 0, sizeof(*e));
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const void *) sa;
        memcpy(e->addr, &in->sin_addr, 4);
        e->port = in->sin_port;
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const void *) sa;
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            memcpy(e->addr, &in6->sin6_addr.s6_addr[12], 4);
        } else {
            e->v6 = true;
            memcpy(e->addr, &in6->sin6_addr, 16);
        }
        e->port = in6->sin6_port;
        return true;
    }
    return false;
}

static bool is_loopback(const struct endpoint *e) {
    if (!e->v6)
        return e->addr[0] == 127;
    static const uint8_t loopback6[16] = {[15] = 1};
    return memcmp(e->addr, loopback6, 16) == 0;
}

static bool is_unspecified(const struct endpoint *e) {
    static const uint8_t zeros[16];
    return memcmp(e->addr, zeros, e->v6 ? 16 : 4) == 0;
}

static bool host_end(int real_fd, bool peer, struct endpoint *e) {
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    int err = peer ? getpeername(real_fd, (void *) &sa, &len) : getsockname(real_fd, (void *) &sa, &len);
    return err == 0 && endpoint_from(e, (void *) &sa);
}

static bool sock_end(struct fd *sock, bool peer, struct endpoint *e) {
    return host_end(sock->real_fd, peer, e);
}

// The address the host would send from to dst: a socket that has not sent
// yet, or sends from any, has none of its own
static void source_for(struct endpoint *local, const struct endpoint *dst) {
    if (!is_unspecified(local))
        return;
    int s = socket(dst->v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;
    struct sockaddr_storage sa = {0};
    socklen_t len;
    if (dst->v6) {
        struct sockaddr_in6 *in6 = (void *) &sa;
        in6->sin6_family = AF_INET6;
        memcpy(&in6->sin6_addr, dst->addr, 16);
        in6->sin6_port = htons(9);
        len = sizeof(*in6);
    } else {
        struct sockaddr_in *in = (void *) &sa;
        in->sin_family = AF_INET;
        memcpy(&in->sin_addr, dst->addr, 4);
        in->sin_port = htons(9);
        len = sizeof(*in);
    }
    struct endpoint found;
    if (connect(s, (void *) &sa, len) == 0 && host_end(s, false, &found)) {
        memcpy(local->addr, found.addr, 16);
        local->v6 = found.v6;
    }
    close(s);
}

static uint32_t initial_seq(const struct endpoint *from, const struct endpoint *to) {
    uint32_t hash = 2166136261u;
    const struct endpoint *ends[] = {from, to};
    for (int i = 0; i < 2; i++) {
        const uint8_t *bytes = (const void *) ends[i];
        for (size_t j = 0; j < sizeof(*ends[i]); j++)
            hash = (hash ^ bytes[j]) * 16777619u;
    }
    return hash;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v);
}

static uint32_t sum16(const uint8_t *p, size_t len, uint32_t sum) {
    for (; len > 1; p += 2, len -= 2)
        sum += (uint32_t) p[0] << 8 | p[1];
    if (len)
        sum += (uint32_t) p[0] << 8;
    return sum;
}
static uint16_t fold(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return ~sum & 0xffff;
}

// One packet's record: pcap's record header, the cooked header, IP, then the
// transport header and payload given (from skip in iov, payload_len bytes),
// with the transport checksum, at csum_at in it, filled. in: received (or on
// loopback, see above).
static void make_packet(struct packet *p, bool in, bool loopback, int ipproto, const struct endpoint *src,
                        const struct endpoint *dst, const uint8_t *l4, size_t l4_len, int csum_at,
                        const struct iovec *iov, int iovcnt, size_t skip, size_t payload_len) {
    bool v6 = dst->v6;
    size_t ip_len = v6 ? 40 : 20;
    size_t packet_len = ip_len + l4_len + payload_len;
    p->len = 16 + 16 + packet_len;
    p->buf = malloc(p->len);
    uint8_t *rec = p->buf;
    clock_gettime(CLOCK_REALTIME, &p->time);
    uint32_t hdr[4] = {(uint32_t) p->time.tv_sec, (uint32_t) p->time.tv_nsec, 16 + packet_len, 16 + packet_len};
    memcpy(rec, hdr, sizeof(hdr));
    p->out = !in && !loopback;
    p->loopback = loopback;
    p->ethertype = v6 ? ETHERTYPE_IPV6 : ETHERTYPE_IPV4;
    p->local = in ? *dst : *src;
    p->ip = rec + 32;
    p->ip_len = packet_len;

    uint8_t *sll = rec + 16;
    memset(sll, 0, 16);
    put16(sll, in || loopback ? SLL_HOST : SLL_OUTGOING);
    put16(sll + 2, loopback ? ARPHRD_LOOPBACK_ : ARPHRD_ETHER_);
    put16(sll + 14, v6 ? ETHERTYPE_IPV6 : ETHERTYPE_IPV4);

    uint8_t *ip = sll + 16;
    static _Atomic uint16_t ip_id;
    if (v6) {
        put32(ip, 0x60000000);
        put16(ip + 4, l4_len + payload_len);
        ip[6] = ipproto;
        ip[7] = 64;
        memcpy(ip + 8, src->addr, 16);
        memcpy(ip + 24, dst->addr, 16);
    } else {
        ip[0] = 0x45;
        ip[1] = 0;
        put16(ip + 2, packet_len);
        put16(ip + 4, atomic_fetch_add(&ip_id, 1));
        put16(ip + 6, ipproto == IPPROTO_TCP ? 0x4000 : 0);
        ip[8] = 64;
        ip[9] = ipproto;
        put16(ip + 10, 0);
        memcpy(ip + 12, src->addr, 4);
        memcpy(ip + 16, dst->addr, 4);
        put16(ip + 10, fold(sum16(ip, 20, 0)));
    }

    uint8_t *seg = ip + ip_len;
    if (l4_len > 0)
        memcpy(seg, l4, l4_len);
    uint8_t *out = seg + l4_len;
    size_t left = payload_len;
    for (int i = 0; i < iovcnt && left > 0; i++) {
        size_t n = iov[i].iov_len;
        if (skip >= n) {
            skip -= n;
            continue;
        }
        n -= skip;
        if (n > left)
            n = left;
        memcpy(out, (const uint8_t *) iov[i].iov_base + skip, n);
        out += n;
        left -= n;
        skip = 0;
    }

    // ICMP's checksum covers the message alone, the others' a pseudo-header
    // too
    if (csum_at < 0)
        return;
    size_t seg_len = l4_len + payload_len;
    put16(seg + csum_at, 0);
    uint32_t sum = 0;
    if (ipproto != IPPROTO_ICMP) {
        if (v6) {
            sum = sum16(src->addr, 16, sum16(dst->addr, 16, 0));
            uint8_t tail[8] = {0};
            put32(tail, seg_len);
            tail[7] = ipproto;
            sum = sum16(tail, 8, sum);
        } else {
            sum = sum16(src->addr, 4, sum16(dst->addr, 4, 0));
            uint8_t tail[4] = {0, ipproto};
            put16(tail + 2, seg_len);
            sum = sum16(tail, 4, sum);
        }
    }
    uint16_t csum = fold(sum16(seg, seg_len, sum));
    // UDP's 0 means none
    if (ipproto == IPPROTO_UDP && csum == 0)
        csum = 0xffff;
    put16(seg + csum_at, csum);
}

// To every capture, and every packet socket; netcap_lock held
static void deliver(struct packet *p) {
    packet_deliver(p);
    struct netcap_reader *reader;
    list_for_each_entry(&readers, reader, readers) {
        if (READER_BUFFER - reader->len < p->len) {
            if (reader->dropped++ == 0)
                printk("netcap: a capture is not read fast enough: packets are lost\n");
            continue;
        }
        size_t end = (reader->start + reader->len) % READER_BUFFER;
        size_t first = READER_BUFFER - end < p->len ? READER_BUFFER - end : p->len;
        memcpy(reader->buf + end, p->buf, first);
        memcpy(reader->buf, p->buf + first, p->len - first);
        reader->len += p->len;
        atomic_store(&reader->available, reader->len);
        notify(&reader->changed);
        poll_wakeup(reader->fd, POLL_READ);
    }
    free(p->buf);
}

static void tcp_packet(struct netcap_flow *flow, bool in, uint8_t flags, uint32_t seq, uint32_t ack,
                       const struct iovec *iov, int iovcnt, size_t skip, size_t len) {
    uint8_t tcp[20] = {0};
    const struct endpoint *src = in ? &flow->remote : &flow->local;
    const struct endpoint *dst = in ? &flow->local : &flow->remote;
    memcpy(tcp, &src->port, 2);
    memcpy(tcp + 2, &dst->port, 2);
    put32(tcp + 4, seq);
    put32(tcp + 8, flags & TCP_ACK ? ack : 0);
    tcp[12] = 5 << 4;
    tcp[13] = flags;
    put16(tcp + 14, 65535);
    struct packet p;
    make_packet(&p, in, is_loopback(&flow->remote), IPPROTO_TCP, src, dst, tcp, sizeof(tcp), 16, iov, iovcnt, skip, len);
    deliver(&p);
}

static bool is_inet(struct fd *sock) {
    return sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_;
}

static bool is_tcp(struct fd *sock) {
    return is_inet(sock) && sock->socket.type == SOCK_STREAM_;
}

// The stream socket's flow, made from its ends as the capture finds it;
// netcap_lock held. NULL if it has no ends (not connected, or no more).
static struct netcap_flow *flow_of(struct fd *sock, const struct endpoint *local, const struct endpoint *remote) {
    if (sock->socket.netcap != NULL)
        return sock->socket.netcap;
    struct netcap_flow *flow = calloc(1, sizeof(*flow));
    flow->local = *local;
    flow->remote = *remote;
    flow->seq_out = initial_seq(local, remote);
    flow->seq_in = initial_seq(remote, local);
    sock->socket.netcap = flow;
    return flow;
}

// A flow found mid-connection: its SYNs went before the capture
static struct netcap_flow *flow_found(struct fd *sock) {
    if (sock->socket.netcap != NULL)
        return sock->socket.netcap;
    struct endpoint local, remote;
    if (!sock_end(sock, false, &local) || !sock_end(sock, true, &remote))
        return NULL;
    struct netcap_flow *flow = flow_of(sock, &local, &remote);
    flow->seq_out++;
    flow->seq_in++;
    flow->established = true;
    return flow;
}

// The rest of a handshake connect() left in progress, as the connection
// turns out to work
static void flow_establish(struct netcap_flow *flow) {
    if (flow->established)
        return;
    flow->established = true;
    tcp_packet(flow, true, TCP_SYN | TCP_ACK, flow->seq_in, flow->seq_out, NULL, 0, 0, 0);
    flow->seq_in++;
    tcp_packet(flow, false, TCP_ACK, flow->seq_out, flow->seq_in, NULL, 0, 0, 0);
}

void netcap_connect(struct fd *sock, const struct sockaddr *to, int host_errno) {
    if (!is_tcp(sock))
        return;
    struct endpoint local, remote;
    if (!endpoint_from(&remote, to) || !sock_end(sock, false, &local))
        return;
    source_for(&local, &remote);
    lock(&netcap_lock);
    if (sock->socket.netcap != NULL) {
        unlock(&netcap_lock);
        return;
    }
    struct netcap_flow *flow = flow_of(sock, &local, &remote);
    tcp_packet(flow, false, TCP_SYN, flow->seq_out, 0, NULL, 0, 0, 0);
    flow->seq_out++;
    if (host_errno == 0) {
        flow_establish(flow);
    } else if (host_errno != EINPROGRESS) {
        // refused (or unreachable: no packet comes back then, but the
        // connection ends all the same)
        flow->established = true;
        flow->fin_out = true;
        tcp_packet(flow, true, TCP_RST | TCP_ACK, 0, flow->seq_out, NULL, 0, 0, 0);
    }
    unlock(&netcap_lock);
}

void netcap_accept(struct fd *sock) {
    if (!is_tcp(sock))
        return;
    struct endpoint local, remote;
    if (!sock_end(sock, false, &local) || !sock_end(sock, true, &remote))
        return;
    lock(&netcap_lock);
    struct netcap_flow *flow = flow_of(sock, &local, &remote);
    if (is_loopback(&remote)) {
        // the connecting end made the handshake
        flow->seq_out++;
        flow->seq_in++;
    } else {
        tcp_packet(flow, true, TCP_SYN, flow->seq_in, 0, NULL, 0, 0, 0);
        flow->seq_in++;
        tcp_packet(flow, false, TCP_SYN | TCP_ACK, flow->seq_out, flow->seq_in, NULL, 0, 0, 0);
        flow->seq_out++;
        tcp_packet(flow, true, TCP_ACK, flow->seq_in, flow->seq_out, NULL, 0, 0, 0);
    }
    flow->established = true;
    unlock(&netcap_lock);
}

// A datagram socket's message, to or from peer (the socket's connected peer
// if NULL)
static void datagram(struct fd *sock, bool in, const struct iovec *iov, int iovcnt, size_t len,
                     const struct sockaddr *peer) {
    struct endpoint local, remote;
    if (peer != NULL ? !endpoint_from(&remote, peer) : !sock_end(sock, true, &remote))
        return;
    if (!sock_end(sock, false, &local))
        memset(&local, 0, sizeof(local));
    local.v6 = remote.v6;
    source_for(&local, &remote);
    // between two of iSH's sockets on loopback: the sender's
    bool loopback = is_loopback(&remote);
    if (in && loopback)
        return;
    const struct endpoint *src = in ? &remote : &local;
    const struct endpoint *dst = in ? &local : &remote;

    int protocol = sock->socket.protocol;
    bool icmp = protocol == IPPROTO_ICMP || protocol == IPPROTO_ICMPV6 ||
        (sock->socket.type == SOCK_RAW_ && protocol == IPPROTO_RAW);
    struct packet p;
    if (!icmp) {
        uint8_t udp[8];
        memcpy(udp, &src->port, 2);
        memcpy(udp + 2, &dst->port, 2);
        put16(udp + 4, 8 + len);
        put16(udp + 6, 0);
        make_packet(&p, in, loopback, IPPROTO_UDP, src, dst, udp, sizeof(udp), 6, iov, iovcnt, 0, len);
    } else {
#ifdef __APPLE__
        // a raw IPv4 socket gets the IP header with the message, as Darwin's
        // gives it and Linux's does
        if (in && sock->socket.type == SOCK_RAW_ && !remote.v6) {
            size_t packet_len = len;
            p.len = 32 + packet_len;
            p.buf = malloc(p.len);
            clock_gettime(CLOCK_REALTIME, &p.time);
            uint32_t hdr[4] = {(uint32_t) p.time.tv_sec, (uint32_t) p.time.tv_nsec, 16 + packet_len, 16 + packet_len};
            memcpy(p.buf, hdr, sizeof(hdr));
            p.out = false;
            p.loopback = loopback;
            p.ethertype = ETHERTYPE_IPV4;
            p.local = local;
            p.ip = p.buf + 32;
            p.ip_len = packet_len;
            uint8_t *sll = p.buf + 16;
            memset(sll, 0, 16);
            put16(sll + 2, ARPHRD_ETHER_);
            put16(sll + 14, ETHERTYPE_IPV4);
            uint8_t *out = sll + 16;
            for (int i = 0; i < iovcnt && (size_t) (out - (sll + 16)) < len; i++) {
                size_t n = iov[i].iov_len;
                if (n > len - (size_t) (out - (sll + 16)))
                    n = len - (size_t) (out - (sll + 16));
                memcpy(out, iov[i].iov_base, n);
                out += n;
            }
            lock(&netcap_lock);
            deliver(&p);
            unlock(&netcap_lock);
            return;
        }
#endif
        int ipproto = remote.v6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;
#ifdef __linux__
        // Linux's ping socket sends an echo request with its port for the
        // id, whatever the message has
        uint8_t *echo = NULL;
        if (!in && len >= 8) {
            echo = malloc(len);
            size_t at = 0;
            for (int i = 0; i < iovcnt && at < len; i++) {
                size_t n = iov[i].iov_len < len - at ? iov[i].iov_len : len - at;
                memcpy(echo + at, iov[i].iov_base, n);
                at += n;
            }
            if (echo[0] == (remote.v6 ? 128 : 8)) {
                memcpy(echo + 4, &local.port, 2);
                iov = &(struct iovec) {echo, len};
                iovcnt = 1;
            }
        }
#endif
        // the host fills in the checksum (and the ICMPv6 one's
        // pseudo-header), whatever the message has
        make_packet(&p, in, loopback, ipproto, src, dst, NULL, 0, len >= 4 ? 2 : -1, iov, iovcnt, 0, len);
#ifdef __linux__
        free(echo);
#endif
    }
    lock(&netcap_lock);
    deliver(&p);
    unlock(&netcap_lock);
}

void netcap_send(struct fd *sock, const struct iovec *iov, int iovcnt, size_t len, const struct sockaddr *to) {
    if (!is_inet(sock))
        return;
    if (!is_tcp(sock)) {
        datagram(sock, false, iov, iovcnt, len, to);
        return;
    }
    lock(&netcap_lock);
    struct netcap_flow *flow = flow_found(sock);
    if (flow != NULL) {
        flow_establish(flow);
        for (size_t done = 0; done < len;) {
            size_t n = len - done < MAX_SEGMENT ? len - done : MAX_SEGMENT;
            tcp_packet(flow, false, TCP_PSH | TCP_ACK, flow->seq_out, flow->seq_in, iov, iovcnt, done, n);
            flow->seq_out += n;
            done += n;
        }
    }
    unlock(&netcap_lock);
}

void netcap_recv(struct fd *sock, const struct iovec *iov, int iovcnt, size_t len, const struct sockaddr *from) {
    if (!is_inet(sock))
        return;
    if (!is_tcp(sock)) {
        datagram(sock, true, iov, iovcnt, len, from);
        return;
    }
    lock(&netcap_lock);
    struct netcap_flow *flow = flow_found(sock);
    if (flow != NULL) {
        flow_establish(flow);
        // over loopback the sender made the packets: only the count
        bool shown = !is_loopback(&flow->remote);
        if (len == 0) {
            if (shown)
                tcp_packet(flow, true, TCP_FIN | TCP_ACK, flow->seq_in, flow->seq_out, NULL, 0, 0, 0);
            flow->seq_in++;
        }
        for (size_t done = 0; done < len;) {
            size_t n = len - done < MAX_SEGMENT ? len - done : MAX_SEGMENT;
            if (shown)
                tcp_packet(flow, true, TCP_PSH | TCP_ACK, flow->seq_in, flow->seq_out, iov, iovcnt, done, n);
            flow->seq_in += n;
            done += n;
        }
    }
    unlock(&netcap_lock);
}

void netcap_error(struct fd *sock, int host_errno) {
    if (!is_tcp(sock) || (host_errno != ECONNRESET && host_errno != EPIPE && host_errno != ECONNREFUSED))
        return;
    lock(&netcap_lock);
    struct netcap_flow *flow = sock->socket.netcap;
    if (flow != NULL && !flow->fin_out && !is_loopback(&flow->remote)) {
        flow->fin_out = true;
        flow->established = true;
        tcp_packet(flow, true, TCP_RST | TCP_ACK, flow->seq_in, flow->seq_out, NULL, 0, 0, 0);
    }
    unlock(&netcap_lock);
}

static void fin_out(struct fd *sock) {
    struct netcap_flow *flow = flow_found(sock);
    if (flow == NULL || flow->fin_out)
        return;
    flow_establish(flow);
    flow->fin_out = true;
    tcp_packet(flow, false, TCP_FIN | TCP_ACK, flow->seq_out, flow->seq_in, NULL, 0, 0, 0);
    flow->seq_out++;
}

void netcap_shutdown(struct fd *sock, int how) {
    if (!is_tcp(sock) || how == SHUT_RD)
        return;
    lock(&netcap_lock);
    fin_out(sock);
    unlock(&netcap_lock);
}

void netcap_close(struct fd *sock) {
    if (!is_tcp(sock))
        return;
    lock(&netcap_lock);
    // a connection made before the capture: its own FIN, if it is one
    if (netcap_on())
        fin_out(sock);
    free(sock->socket.netcap);
    sock->socket.netcap = NULL;
    unlock(&netcap_lock);
}

// ===== /dev/netcap =====

static int netcap_dev_open(int major, int minor, struct fd *fd) {
    (void) major;
    (void) minor;
    if (!superuser())
        return _EPERM;
    struct netcap_reader *reader = calloc(1, sizeof(*reader));
    reader->buf = malloc(READER_BUFFER);
    if (reader->buf == NULL) {
        free(reader);
        return _ENOMEM;
    }
    reader->fd = fd;
    cond_init(&reader->changed);
    // pcap's file header: nanosecond timestamps
    uint32_t header[6] = {0xa1b23c4d, 2 | 4 << 16, 0, 0, SNAPLEN, LINKTYPE_LINUX_SLL};
    memcpy(reader->buf, header, sizeof(header));
    reader->len = sizeof(header);
    atomic_store(&reader->available, reader->len);
    fd->netcap.reader = reader;
    lock(&netcap_lock);
    list_add_tail(&readers, &reader->readers);
    netcap_readers++;
    unlock(&netcap_lock);
    return 0;
}

static ssize_t netcap_dev_read(struct fd *fd, void *buf, size_t size) {
    struct netcap_reader *reader = fd->netcap.reader;
    lock(&netcap_lock);
    while (reader->len == 0) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&netcap_lock);
            return _EAGAIN;
        }
        int err = wait_for(&reader->changed, &netcap_lock, NULL);
        if (err < 0) {
            unlock(&netcap_lock);
            return err;
        }
    }
    size_t n = size < reader->len ? size : reader->len;
    size_t first = READER_BUFFER - reader->start < n ? READER_BUFFER - reader->start : n;
    memcpy(buf, reader->buf + reader->start, first);
    memcpy((uint8_t *) buf + first, reader->buf, n - first);
    reader->start = (reader->start + n) % READER_BUFFER;
    reader->len -= n;
    atomic_store(&reader->available, reader->len);
    unlock(&netcap_lock);
    return n;
}

static int netcap_dev_poll(struct fd *fd) {
    return atomic_load(&fd->netcap.reader->available) > 0 ? POLL_READ : 0;
}

static int netcap_dev_close(struct fd *fd) {
    struct netcap_reader *reader = fd->netcap.reader;
    lock(&netcap_lock);
    list_remove(&reader->readers);
    netcap_readers--;
    unlock(&netcap_lock);
    if (reader->dropped != 0)
        printk("netcap: a capture lost %llu packets\n", (unsigned long long) reader->dropped);
    cond_destroy(&reader->changed);
    free(reader->buf);
    free(reader);
    return 0;
}

struct dev_ops netcap_dev = {
    .open = netcap_dev_open,
    .fd.read = netcap_dev_read,
    .fd.poll = netcap_dev_poll,
    .fd.close = netcap_dev_close,
};
