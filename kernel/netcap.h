#ifndef KERNEL_NETCAP_H
#define KERNEL_NETCAP_H

// /dev/netcap (kernel/netcap.c): the guest's network traffic, as a pcap
// stream, for tcpdump -r. iOS gives an app no packets of its own, nor anyone
// else's: the packets are made up from what iSH's processes do with their
// sockets, as a capture would show them. Only IPv4 and IPv6 sockets', and
// only while a capture is open.

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>

struct fd;

// open captures
extern _Atomic unsigned netcap_readers;
static inline bool netcap_on(void) {
    return atomic_load_explicit(&netcap_readers, memory_order_relaxed) != 0;
}

// The hooks, from fs/sock.c, holding no lock. sock is the guest's socket;
// addresses are the host's, as its calls take and give them.

// connect() of a stream socket to to: host_errno its outcome (0,
// EINPROGRESS, or the error)
void netcap_connect(struct fd *sock, const struct sockaddr *to, int host_errno);
// a connection accept() gave
void netcap_accept(struct fd *sock);
// data sent, to to for a datagram socket that named it (NULL otherwise)
void netcap_send(struct fd *sock, const struct iovec *iov, int iovcnt, size_t len, const struct sockaddr *from_or_to);
// data received: len 0 on a stream socket is the other end's FIN. from the
// sender of a datagram, or NULL.
void netcap_recv(struct fd *sock, const struct iovec *iov, int iovcnt, size_t len, const struct sockaddr *from);
// a call on the socket failed with host_errno (a reset connection is a RST)
void netcap_error(struct fd *sock, int host_errno);
// shutdown(how)
void netcap_shutdown(struct fd *sock, int how);
// the socket's last close
void netcap_close(struct fd *sock);

// /dev/netcap's device
struct dev_ops;
extern struct dev_ops netcap_dev;

// ===== between kernel/netcap.c and kernel/packet.c =====

#include <stdint.h>
#include <time.h>
#include "util/sync.h"

// Guards the captures, the flows and the packet sockets
extern lock_t netcap_lock;

// An end of a connection or datagram: IPv4 (an IPv4-mapped IPv6 address is
// one) or IPv6, in network order
struct endpoint {
    bool v6;
    uint8_t addr[16];
    uint16_t port;
};

// A packet made up: its pcap record (record header, cooked header, IP), and
// what a packet socket takes of it
struct packet {
    uint8_t *buf;
    size_t len;
    struct timespec time;
    // PACKET_OUTGOING, or PACKET_HOST (received, or on loopback)
    bool out;
    bool loopback;
    uint16_t ethertype;
    // this end's address, whose interface it is on
    struct endpoint local;
    const uint8_t *ip;
    size_t ip_len;
};

// To the packet sockets that take it; netcap_lock held
void packet_deliver(const struct packet *p);

#endif
