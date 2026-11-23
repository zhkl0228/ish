// AF_PACKET sockets: what tcpdump -i (libpcap) captures with, on the packets
// /dev/netcap's are (kernel/netcap.c): made up from iSH's processes' socket
// calls, as iOS gives an app none. Root's only, as on Linux (CAP_NET_RAW).
//
// As libpcap 1.10 takes them: socket(AF_PACKET, SOCK_DGRAM or SOCK_RAW),
// bind() to an interface (or none for all), a TPACKET_V2 receive ring mapped
// with mmap, and poll. A packet is on the interface whose address is its
// local end's (lo on loopback; on none if no interface has it, which only a
// socket bound to none takes). SOCK_RAW has the link layer header the
// interface's type gives (ARPHRD_ETHER and ARPHRD_LOOPBACK: Ethernet's, the
// other end's address unknown, 0; ARPHRD_NONE: none), SOCK_DGRAM none.
//
// Not here, as a kernel can be without them:
//   - TPACKET_V3 (before Linux 3.2): PACKET_HDRLEN says EINVAL for it, and
//     libpcap takes TPACKET_V2.
//   - Socket filters (without CONFIG_FILTER): SO_ATTACH_FILTER says
//     ENOPROTOOPT, and libpcap filters in userspace.
// Not here, logged when asked for: a TPACKET_V1 ring, receiving without a
// ring (recv, read), and sending (nothing to send it on).
//
// netcap_lock guards the sockets. poll takes not it but a lock of the
// socket's own: it runs under a poll's lock, which delivery takes
// (poll_wakeup) holding netcap_lock.

#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/netcap.h"
#include "kernel/packet.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/netlink.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "util/list.h"

#define SOCKET_TYPE_MASK 0xf
#define SOL_PACKET_ 263
#define ETH_P_ALL_ 0x0003
#define PACKET_HOST_ 0
#define PACKET_OUTGOING_ 4

#define PACKET_ADD_MEMBERSHIP_ 1
#define PACKET_DROP_MEMBERSHIP_ 2
#define PACKET_RX_RING_ 5
#define PACKET_STATISTICS_ 6
#define PACKET_AUXDATA_ 8
#define PACKET_VERSION_ 10
#define PACKET_HDRLEN_ 11
#define PACKET_RESERVE_ 12

#define TPACKET_V1_ 0
#define TPACKET_V2_ 1

#define TP_STATUS_KERNEL_ 0
#define TP_STATUS_USER_ 1
#define TP_STATUS_LOSING_ 4

#define SO_RCVBUFFORCE_ 33
#define SO_ATTACH_FILTER_ 26
#define SO_DETACH_FILTER_ 27
#define SO_TIMESTAMPNS_ 35
#define SO_BPF_EXTENSIONS_ 48

#define ARPHRD_ETHER_ 1
#define ARPHRD_LOOPBACK_ 772
#define ARPHRD_NONE_ 0xfffe

// Linux's structures, 64-bit
struct sockaddr_ll_ {
    uint16_t family;
    uint16_t protocol; // network order
    int32_t ifindex;
    uint16_t hatype;
    uint8_t pkttype;
    uint8_t halen;
    uint8_t addr[8];
};
struct tpacket_req_ {
    uint32_t block_size, block_nr, frame_size, frame_nr;
};
struct tpacket2_hdr_ {
    uint32_t status, len, snaplen;
    uint16_t mac, net;
    uint32_t sec, nsec;
    uint16_t vlan_tci, vlan_tpid;
    uint8_t padding[4];
};
struct tpacket_stats_ {
    uint32_t packets, drops;
};
struct packet_mreq_ {
    int32_t ifindex;
    uint16_t type;
    uint16_t alen;
    uint8_t address[8];
};
static_assert(sizeof(struct sockaddr_ll_) == 20, "sockaddr_ll layout");
static_assert(sizeof(struct tpacket2_hdr_) == 32, "tpacket2_hdr layout");
static_assert(sizeof(struct packet_mreq_) == 16, "packet_mreq layout");

#define TPACKET_ALIGN(x) (((x) + 15u) & ~15u)
// a TPACKET_V2 frame's headers before its data
#define TPACKET_HDRLEN_ (TPACKET_ALIGN(sizeof(struct tpacket2_hdr_)) + sizeof(struct sockaddr_ll_))
// TPACKET_V1's header, struct tpacket_hdr: its tp_status is a long
#ifdef GUEST_ARM64
#define TPACKET1_HDR_SIZE 32
#else
#define TPACKET1_HDR_SIZE 24
#endif
// the most memory a ring may have
#define RING_MAX (64u << 20)

// A receive ring: the socket's while set, and each mapping's
struct packet_ring {
    uint8_t *mem;
    size_t size;
    unsigned block_size, frames_per_block, frame_size, frame_nr;
    atomic_uint refs;
};

struct packet_sock {
    struct list socks;
    struct fd *fd;
    int type;
    // what it takes, in network order: ETH_P_ALL, an ethertype, or 0 for
    // nothing
    uint16_t protocol;
    // the interface it is bound to, 0 for all
    int ifindex;
    int version;
    unsigned reserve;
    int rcvbuf;
    bool auxdata;
    // the ring, and the frame the next packet goes to: changed holding
    // netcap_lock and ring_lock, which poll takes (and nothing under it)
    lock_t ring_lock;
    struct packet_ring *ring;
    _Atomic unsigned head;
    // since PACKET_STATISTICS last read them
    uint32_t packets, drops;
};

static struct list socks = LIST_INITIALIZER(socks);

// The host's interfaces, as the guest knows them (fs/netlink.c), looked up
// when a socket is made or bound
static struct netlink_host_link *links;
static size_t n_links;
static struct netlink_host_addr *addrs;
static size_t n_addrs;

static void interfaces_refresh(void) {
    struct netlink_host_link *new_links;
    struct netlink_host_addr *new_addrs;
    size_t new_n_links, new_n_addrs;
    netlink_host_snapshot(&new_links, &new_n_links, &new_addrs, &new_n_addrs);
    lock(&netcap_lock);
    struct netlink_host_link *old_links = links;
    struct netlink_host_addr *old_addrs = addrs;
    links = new_links;
    n_links = new_n_links;
    addrs = new_addrs;
    n_addrs = new_n_addrs;
    unlock(&netcap_lock);
    netlink_host_free(old_links, old_addrs);
}

static const struct netlink_host_link *link_by_index(int index) {
    for (size_t i = 0; i < n_links; i++)
        if ((int) links[i].index == index)
            return &links[i];
    return NULL;
}

// The interface p is on (see above); netcap_lock held
static const struct netlink_host_link *link_of(const struct packet *p) {
    if (p->loopback) {
        for (size_t i = 0; i < n_links; i++)
            if (links[i].arphrd == ARPHRD_LOOPBACK_)
                return &links[i];
        return NULL;
    }
    // Linux's AF_INET and AF_INET6
    int family = p->local.v6 ? 10 : 2;
    for (size_t i = 0; i < n_addrs; i++)
        if (addrs[i].family == family && memcmp(addrs[i].local, p->local.addr, p->local.v6 ? 16 : 4) == 0)
            return link_by_index(addrs[i].index);
    return NULL;
}

static uint8_t *ring_frame(struct packet_ring *ring, unsigned i) {
    return ring->mem + (i / ring->frames_per_block) * ring->block_size + (i % ring->frames_per_block) * ring->frame_size;
}

static void ring_unref(struct packet_ring *ring) {
    if (atomic_fetch_sub(&ring->refs, 1) == 1) {
        munmap(ring->mem, ring->size);
        free(ring);
    }
}

// Into the socket's ring, if its next frame is free; netcap_lock held
static void ring_put(struct packet_sock *ps, const struct packet *p, const uint8_t *ll, size_t ll_len,
                     const struct sockaddr_ll_ *sll) {
    struct packet_ring *ring = ps->ring;
    unsigned head = ps->head;
    uint8_t *frame = ring_frame(ring, head);
    _Atomic uint32_t *status = (void *) frame;
    unsigned maclen = ps->type == SOCK_RAW_ ? ll_len : 0;
    unsigned netoff, macoff;
    if (ps->type == SOCK_DGRAM_) {
        netoff = macoff = TPACKET_ALIGN(TPACKET_HDRLEN_ + 16) + ps->reserve;
    } else {
        netoff = TPACKET_ALIGN(TPACKET_HDRLEN_ + (maclen < 16 ? 16 : maclen)) + ps->reserve;
        macoff = netoff - maclen;
    }
    if (atomic_load_explicit(status, memory_order_acquire) != TP_STATUS_KERNEL_ || macoff >= ring->frame_size) {
        ps->drops++;
        return;
    }
    size_t len = maclen + p->ip_len;
    size_t snaplen = len < ring->frame_size - macoff ? len : ring->frame_size - macoff;
    uint8_t *data = frame + macoff;
    size_t ll_copy = maclen < snaplen ? maclen : snaplen;
    memcpy(data, ll, ll_copy);
    memcpy(data + ll_copy, p->ip, snaplen - ll_copy);
    struct tpacket2_hdr_ hdr = {
        .len = len,
        .snaplen = snaplen,
        .mac = macoff,
        .net = netoff,
        .sec = p->time.tv_sec,
        .nsec = p->time.tv_nsec,
    };
    memcpy(frame + 4, (uint8_t *) &hdr + 4, sizeof(hdr) - 4);
    memcpy(frame + TPACKET_ALIGN(sizeof(hdr)), sll, sizeof(*sll));
    atomic_store_explicit(status, TP_STATUS_USER_ | (ps->drops != 0 ? TP_STATUS_LOSING_ : 0), memory_order_release);
    ps->head = (head + 1) % ring->frame_nr;
    ps->packets++;
    poll_wakeup(ps->fd, POLL_READ);
}

void packet_deliver(const struct packet *p) {
    if (list_empty(&socks))
        return;
    const struct netlink_host_link *link = link_of(p);
    int ifindex = link != NULL ? (int) link->index : 0;
    uint16_t hatype = link != NULL ? link->arphrd : ARPHRD_NONE_;
    uint8_t local_mac[6] = {0};
    if (link != NULL && link->has_lladdr)
        memcpy(local_mac, link->lladdr, 6);

    // the link layer header SOCK_RAW has, and its address: the sender's
    uint8_t ll[14];
    size_t ll_len = 0;
    struct sockaddr_ll_ sll = {
        .family = AF_PACKET_,
        .protocol = htons(p->ethertype),
        .ifindex = ifindex,
        .hatype = hatype,
        .pkttype = p->out ? PACKET_OUTGOING_ : PACKET_HOST_,
    };
    if (hatype == ARPHRD_ETHER_ || hatype == ARPHRD_LOOPBACK_) {
        memset(ll, 0, 12);
        if (p->out)
            memcpy(ll + 6, local_mac, 6);
        else
            memcpy(ll, local_mac, 6);
        ll[12] = p->ethertype >> 8;
        ll[13] = p->ethertype;
        ll_len = 14;
        sll.halen = 6;
        memcpy(sll.addr, ll + 6, 6);
    }

    struct packet_sock *ps;
    list_for_each_entry(&socks, ps, socks) {
        if (ps->protocol == 0 || (ps->protocol != htons(ETH_P_ALL_) && ps->protocol != htons(p->ethertype)))
            continue;
        if (ps->ifindex != 0 && ps->ifindex != ifindex)
            continue;
        // receiving without a ring is not here: the packet is lost to it
        if (ps->ring == NULL) {
            ps->drops++;
            continue;
        }
        ring_put(ps, p, ll, ll_len, &sll);
    }
}

static const struct fd_ops packet_fdops;

fd_t packet_socket(int type, int protocol) {
    STRACE("socket(AF_PACKET, %#x, %#x)", type, protocol);
    int kind = type & SOCKET_TYPE_MASK;
    if (kind != SOCK_RAW_ && kind != SOCK_DGRAM_) {
        // SOCK_PACKET, Linux 2.0's
        FIXME("socket(AF_PACKET, %#x, %#x): socket type not implemented", type, protocol);
        return _ESOCKTNOSUPPORT;
    }
    if (!superuser())
        return _EPERM;
    interfaces_refresh();
    struct fd *fd = adhoc_fd_create(&packet_fdops);
    if (fd == NULL)
        return _ENOMEM;
    fd->stat.mode = S_IFSOCK | 0777;
    fd->stat.inode = adhoc_new_inode();
    struct packet_sock *ps = calloc(1, sizeof(*ps));
    ps->fd = fd;
    ps->type = kind;
    ps->protocol = protocol;
    ps->version = TPACKET_V1_;
    ps->rcvbuf = 212992;
    lock_init(&ps->ring_lock);
    fd->packet.sock = ps;
    lock(&netcap_lock);
    list_add_tail(&socks, &ps->socks);
    netcap_readers++;
    unlock(&netcap_lock);
    fd_t f = f_install(fd, type & ~SOCKET_TYPE_MASK);
    if (f < 0)
        fd_close(fd);
    return f;
}

int_t packet_bind(struct fd *fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    struct sockaddr_ll_ sll;
    if (sockaddr_len < sizeof(sll))
        return _EINVAL;
    if (user_get(sockaddr_addr, sll))
        return _EFAULT;
    if (sll.family != AF_PACKET_)
        return _EINVAL;
    interfaces_refresh();
    struct packet_sock *ps = fd->packet.sock;
    lock(&netcap_lock);
    int_t err = 0;
    if (sll.ifindex != 0 && link_by_index(sll.ifindex) == NULL) {
        err = _ENODEV;
    } else {
        ps->ifindex = sll.ifindex;
        if (sll.protocol != 0)
            ps->protocol = sll.protocol;
    }
    unlock(&netcap_lock);
    return err;
}

int_t packet_getsockname(struct fd *fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    uint_t len;
    if (user_get(sockaddr_len_addr, len))
        return _EFAULT;
    struct packet_sock *ps = fd->packet.sock;
    struct sockaddr_ll_ sll = {.family = AF_PACKET_};
    lock(&netcap_lock);
    sll.protocol = ps->protocol;
    sll.ifindex = ps->ifindex;
    const struct netlink_host_link *link = link_by_index(ps->ifindex);
    if (link != NULL) {
        sll.hatype = link->arphrd;
        if (link->has_lladdr) {
            sll.halen = 6;
            memcpy(sll.addr, link->lladdr, 6);
        }
    }
    unlock(&netcap_lock);
    // what fits, and the size it has: Linux's, without the unused address
    uint_t full = offsetof(struct sockaddr_ll_, addr) + sll.halen;
    if (user_write(sockaddr_addr, &sll, len < full ? len : full) || user_put(sockaddr_len_addr, full))
        return _EFAULT;
    return 0;
}

// A ring as PACKET_RX_RING asks (as Linux's packet_set_ring checks it), or
// none; netcap_lock held
static int_t ring_set(struct packet_sock *ps, const struct tpacket_req_ *req) {
    struct packet_ring *old = ps->ring;
    // mapped: not to be changed under its mappings
    if (old != NULL && atomic_load(&old->refs) > 1)
        return _EBUSY;
    if (req->block_nr == 0) {
        if (req->frame_nr != 0)
            return _EINVAL;
        lock(&ps->ring_lock);
        ps->ring = NULL;
        unlock(&ps->ring_lock);
        if (old != NULL)
            ring_unref(old);
        return 0;
    }
    if (ps->version != TPACKET_V2_) {
        FIXME("packet socket: a TPACKET_V%d receive ring not implemented", ps->version + 1);
        return _EINVAL;
    }
    if ((int) req->block_size <= 0 || req->block_size % PAGE_SIZE != 0)
        return _EINVAL;
    if (req->frame_size < TPACKET_HDRLEN_ + ps->reserve || req->frame_size % 16 != 0)
        return _EINVAL;
    unsigned frames_per_block = req->block_size / req->frame_size;
    if (frames_per_block == 0 || req->block_nr > UINT32_MAX / req->block_size ||
            frames_per_block * req->block_nr != req->frame_nr)
        return _EINVAL;
    size_t size = (size_t) req->block_size * req->block_nr;
    if (size > RING_MAX)
        return _ENOMEM;
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return _ENOMEM;
    struct packet_ring *ring = calloc(1, sizeof(*ring));
    ring->mem = mem;
    ring->size = size;
    ring->block_size = req->block_size;
    ring->frames_per_block = frames_per_block;
    ring->frame_size = req->frame_size;
    ring->frame_nr = req->frame_nr;
    ring->refs = 1;
    lock(&ps->ring_lock);
    ps->ring = ring;
    ps->head = 0;
    unlock(&ps->ring_lock);
    if (old != NULL)
        ring_unref(old);
    return 0;
}

static int_t opt_int(const void *value, uint_t len, int *out) {
    if (len < sizeof(int))
        return _EINVAL;
    memcpy(out, value, sizeof(int));
    return 0;
}

int_t packet_setsockopt(struct fd *fd, int level, int option, const void *value, uint_t len) {
    struct packet_sock *ps = fd->packet.sock;
    int n;
    int_t err;
    if (level == SOL_PACKET_) {
        switch (option) {
            case PACKET_ADD_MEMBERSHIP_:
            case PACKET_DROP_MEMBERSHIP_: {
                // promiscuous, multicast: all there is to see is seen
                struct packet_mreq_ mreq;
                if (len < sizeof(mreq))
                    return _EINVAL;
                memcpy(&mreq, value, sizeof(mreq));
                lock(&netcap_lock);
                err = link_by_index(mreq.ifindex) == NULL ? _ENODEV : 0;
                unlock(&netcap_lock);
                return err;
            }
            case PACKET_AUXDATA_:
                if ((err = opt_int(value, len, &n)) < 0)
                    return err;
                ps->auxdata = n != 0;
                return 0;
            case PACKET_VERSION_:
                if ((err = opt_int(value, len, &n)) < 0)
                    return err;
                // TPACKET_V3 is not here (see above)
                if (n != TPACKET_V1_ && n != TPACKET_V2_)
                    return _EINVAL;
                lock(&netcap_lock);
                err = ps->ring != NULL ? _EBUSY : 0;
                if (err == 0)
                    ps->version = n;
                unlock(&netcap_lock);
                return err;
            case PACKET_RESERVE_:
                if ((err = opt_int(value, len, &n)) < 0)
                    return err;
                lock(&netcap_lock);
                err = ps->ring != NULL ? _EBUSY : 0;
                if (err == 0)
                    ps->reserve = n;
                unlock(&netcap_lock);
                return err;
            case PACKET_RX_RING_: {
                struct tpacket_req_ req;
                if (len < sizeof(req))
                    return _EINVAL;
                memcpy(&req, value, sizeof(req));
                lock(&netcap_lock);
                err = ring_set(ps, &req);
                unlock(&netcap_lock);
                return err;
            }
        }
    } else if (level == SOL_SOCKET_) {
        switch (option) {
            case SO_RCVBUF_:
            case SO_RCVBUFFORCE_:
                if ((err = opt_int(value, len, &n)) < 0)
                    return err;
                ps->rcvbuf = n;
                return 0;
            // for a receive's ancillary data, which is not here
            case SO_TIMESTAMP_:
            case SO_TIMESTAMPNS_:
                return 0;
            // no socket filters (see above)
            case SO_ATTACH_FILTER_:
                return _ENOPROTOOPT;
            case SO_DETACH_FILTER_:
                return _ENOENT;
        }
    }
    FIXME("packet socket: setsockopt level %d option %d not implemented", level, option);
    return _ENOPROTOOPT;
}

int_t packet_getsockopt(struct fd *fd, int level, int option, void *value, uint_t *len) {
    struct packet_sock *ps = fd->packet.sock;
    int n;
    if (level == SOL_PACKET_) {
        switch (option) {
            case PACKET_HDRLEN_:
                if (*len < sizeof(int))
                    return _EINVAL;
                memcpy(&n, value, sizeof(int));
                if (n != TPACKET_V1_ && n != TPACKET_V2_)
                    return _EINVAL;
                // the header's own size, not with the sockaddr_ll after it,
                // as Linux says it
                n = n == TPACKET_V1_ ? TPACKET1_HDR_SIZE : (int) sizeof(struct tpacket2_hdr_);
                goto put_int;
            case PACKET_VERSION_:
                n = ps->version;
                goto put_int;
            case PACKET_RESERVE_:
                n = ps->reserve;
                goto put_int;
            case PACKET_AUXDATA_:
                n = ps->auxdata;
                goto put_int;
            case PACKET_STATISTICS_: {
                // as Linux's: the drops counted among the packets, and both
                // from zero again
                lock(&netcap_lock);
                struct tpacket_stats_ stats = {ps->packets + ps->drops, ps->drops};
                ps->packets = ps->drops = 0;
                unlock(&netcap_lock);
                if (*len > sizeof(stats))
                    *len = sizeof(stats);
                memcpy(value, &stats, *len);
                return 0;
            }
        }
    } else if (level == SOL_SOCKET_) {
        switch (option) {
            case SO_TYPE_:
                n = ps->type;
                goto put_int;
            case SO_ERROR_:
                n = 0;
                goto put_int;
            case SO_PROTOCOL_:
                n = ps->protocol;
                goto put_int;
            case SO_DOMAIN_:
                n = AF_PACKET_;
                goto put_int;
            case SO_RCVBUF_:
                n = ps->rcvbuf;
                goto put_int;
            // no socket filters (see above)
            case SO_BPF_EXTENSIONS_:
                return _ENOPROTOOPT;
        }
    }
    FIXME("packet socket: getsockopt level %d option %d not implemented", level, option);
    return _ENOPROTOOPT;
put_int:
    if (*len > sizeof(int))
        *len = sizeof(int);
    memcpy(value, &n, *len);
    return 0;
}

int_t packet_unsupported(struct fd *fd, const char *call) {
    (void) fd;
    FIXME("packet socket: %s not implemented", call);
    return _EOPNOTSUPP;
}

// A mapping of the ring: the ring's, and the socket's (data->fd, mmap's)
struct ring_mapping {
    struct data data;
    struct packet_ring *ring;
};

static void ring_mapping_release(struct data *data) {
    struct ring_mapping *m = (void *) data;
    if (data->fd != NULL)
        fd_close(data->fd);
    ring_unref(m->ring);
    free(m);
}

static int packet_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot,
                       int flags) {
    (void) flags;
    struct packet_sock *ps = fd->packet.sock;
    lock(&netcap_lock);
    struct packet_ring *ring = ps->ring;
    if (ring == NULL || offset != 0 || ((size_t) pages << PAGE_BITS) != ring->size) {
        unlock(&netcap_lock);
        return _EINVAL;
    }
    atomic_fetch_add(&ring->refs, 1);
    unlock(&netcap_lock);
    struct ring_mapping *m = calloc(1, sizeof(*m));
    m->ring = ring;
    m->data = (struct data) {
        .data = ring->mem,
        .size = ring->size,
        .refcount = 1, // this function's
        .release = ring_mapping_release,
    };
    pt_map_data(mem, start, pages, &m->data, 0, prot | P_SHARED);
    data_unref(&m->data);
    return 0;
}

// the last frame filled not yet taken back: Linux's packet_poll
static int packet_poll(struct fd *fd) {
    struct packet_sock *ps = fd->packet.sock;
    int events = 0;
    lock(&ps->ring_lock);
    struct packet_ring *ring = ps->ring;
    if (ring != NULL) {
        unsigned prev = (ps->head + ring->frame_nr - 1) % ring->frame_nr;
        _Atomic uint32_t *status = (void *) ring_frame(ring, prev);
        if (atomic_load_explicit(status, memory_order_acquire) != TP_STATUS_KERNEL_)
            events = POLL_READ;
    }
    unlock(&ps->ring_lock);
    return events;
}

static ssize_t packet_read(struct fd *fd, void *buf, size_t size) {
    (void) buf;
    (void) size;
    return packet_unsupported(fd, "read");
}

static ssize_t packet_write(struct fd *fd, const void *buf, size_t size) {
    (void) buf;
    (void) size;
    return packet_unsupported(fd, "write");
}

static int packet_close(struct fd *fd) {
    struct packet_sock *ps = fd->packet.sock;
    lock(&netcap_lock);
    list_remove(&ps->socks);
    netcap_readers--;
    struct packet_ring *ring = ps->ring;
    unlock(&netcap_lock);
    if (ring != NULL)
        ring_unref(ring);
    free(ps);
    return 0;
}

static int packet_ioctl(struct fd *fd, int cmd, void *arg) {
    (void) fd;
    return netif_ioctl(cmd, arg);
}

static const struct fd_ops packet_fdops = {
    .read = packet_read,
    .write = packet_write,
    .mmap = packet_mmap,
    .poll = packet_poll,
    .ioctl_size = netif_ioctl_size,
    .ioctl = packet_ioctl,
    .close = packet_close,
};

bool fd_is_packet(struct fd *fd) {
    return fd != NULL && fd->ops == &packet_fdops;
}
