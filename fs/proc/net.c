#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "fs/proc.h"
#include "fs/netlink.h"
#include "fs/sock.h"

// /proc/net/dev - one line per interface the host has, with the host's own
// counters. The same snapshot answers RTM_GETLINK (fs/netlink.c), so `ip
// link`, getifaddrs() and this file name the same interfaces.
static int proc_show_net_dev(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "Inter-|   Receive                                                |  Transmit\n");
    proc_printf(buf, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");

    struct netlink_host_link *links;
    struct netlink_host_addr *addrs;
    size_t n_links, n_addrs;
    netlink_host_snapshot(&links, &n_links, &addrs, &n_addrs);
    for (size_t i = 0; i < n_links; i++) {
        const struct netlink_host_link *l = &links[i];
        proc_printf(buf, "%6s: %7llu %7llu %4llu %4llu    0     0          0 %9llu",
                    l->name, (unsigned long long) l->rx_bytes, (unsigned long long) l->rx_packets,
                    (unsigned long long) l->rx_errs, (unsigned long long) l->rx_drop,
                    (unsigned long long) l->rx_multicast);
        proc_printf(buf, " %8llu %7llu %4llu    0    0 %5llu       0          0\n",
                    (unsigned long long) l->tx_bytes, (unsigned long long) l->tx_packets,
                    (unsigned long long) l->tx_errs, (unsigned long long) l->collisions);
    }
    netlink_host_free(links, addrs);
    return 0;
}

// /proc/net/if_inet6 - the host's IPv6 addresses, in Linux's format:
// address ifindex prefixlen scope flags name
static int proc_show_net_if_inet6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct netlink_host_link *links;
    struct netlink_host_addr *addrs;
    size_t n_links, n_addrs;
    netlink_host_snapshot(&links, &n_links, &addrs, &n_addrs);
    for (size_t i = 0; i < n_addrs; i++) {
        const struct netlink_host_addr *a = &addrs[i];
        if (a->family != 10)
            continue;
        for (int b = 0; b < 16; b++)
            proc_printf(buf, "%02x", a->local[b]);
        // Linux's scope byte here: 0x10 host, 0x20 link, 0x00 global.
        unsigned scope = a->scope == 254 ? 0x10 : a->scope == 253 ? 0x20 : 0x00;
        proc_printf(buf, " %02x %02x %02x 80 %8s\n", a->index & 0xff, a->prefixlen, scope, a->name);
    }
    netlink_host_free(links, addrs);
    return 0;
}

// /proc/net/tcp, tcp6, udp, udp6, raw and raw6: the sockets the guest has
// open, in Linux's format, which netstat and ss parse.
//
// An address prints as Linux prints it: each 32-bit word of it in network
// byte order, read as a host integer, so 127.0.0.1 is 0100007F; the port in
// host order.
static void proc_net_addr(struct proc_data *buf, int family, const uint8_t ip[16], uint16_t port) {
    for (int word = 0; word < (family == AF_INET6_ ? 4 : 1); word++) {
        uint32_t value;
        memcpy(&value, ip + 4 * word, 4);
        proc_printf(buf, "%08X", value);
    }
    proc_printf(buf, ":%04X", port);
}

// Fields iSH has no source for print as 0: the retransmit timer and count, the
// zero-window probes, and the kernel's socket pointer. The reference count is
// 1 for TCP and 2 for UDP and raw, as Linux shows an idle socket. Rows number
// from 0, but a raw socket's is its hash bucket, its protocol, as on Linux;
// IPv4 TCP and raw rows number in 4 columns, the rest in 5.
static int proc_show_net_inet(struct proc_data *buf, int family, int type) {
    struct sock_inet_entry *entries;
    int n = sock_inet_list(family, type, &entries);
    if (n < 0)
        return n;
    bool tcp = type == SOCK_STREAM_;
    bool raw = type == SOCK_RAW_;
    if (family == AF_INET_)
        proc_printf(buf, tcp ? "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                    : raw ? "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n"
                          : "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    else
        proc_printf(buf, tcp ? "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                             : "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    for (int i = 0; i < n; i++) {
        const struct sock_inet_entry *e = &entries[i];
        bool narrow = tcp || (raw && family == AF_INET_);
        proc_printf(buf, narrow ? "%4d: " : "%5d: ", raw ? (int) (e->local_port & 0xff) : i);
        proc_net_addr(buf, family, e->local, e->local_port);
        proc_printf(buf, " ");
        proc_net_addr(buf, family, e->remote, e->remote_port);
        proc_printf(buf, " %02X %08X:%08X 00:00000000 00000000 %5u        0 %lu %d 0000000000000000",
                    e->state, e->tx_queue, e->rx_queue, (unsigned) e->uid, e->inode, tcp ? 1 : 2);
        proc_printf(buf, tcp ? "\n" : " 0\n");
    }
    free(entries);
    return 0;
}
static int proc_show_net_tcp(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_net_inet(buf, AF_INET_, SOCK_STREAM_);
}
static int proc_show_net_tcp6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_net_inet(buf, AF_INET6_, SOCK_STREAM_);
}
static int proc_show_net_udp(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_net_inet(buf, AF_INET_, SOCK_DGRAM_);
}
static int proc_show_net_udp6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_net_inet(buf, AF_INET6_, SOCK_DGRAM_);
}
static int proc_show_net_raw(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_net_inet(buf, AF_INET_, SOCK_RAW_);
}
static int proc_show_net_raw6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_net_inet(buf, AF_INET6_, SOCK_RAW_);
}

// /proc/net/unix, which netstat -x reads. The kernel's socket pointer prints as
// 0; the reference count is 2, or 3 when connected, as Linux shows an idle
// socket. Flags is __SO_ACCEPTCON for a listener; St is SS_CONNECTED (3) or
// SS_UNCONNECTED (1). The path is the bound address (an accepted socket has its
// listener's), an abstract one as @ and its bytes with NULs as @.
static int proc_show_net_unix(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct sock_unix_entry *entries;
    int n = sock_unix_list(&entries);
    if (n < 0)
        return n;
    proc_printf(buf, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    for (int i = 0; i < n; i++) {
        const struct sock_unix_entry *e = &entries[i];
        proc_printf(buf, "0000000000000000: %08X %08X %08X %04X %02X %5lu",
                    e->connected ? 3 : 2, 0, e->listening ? 0x10000 : 0, e->type,
                    e->connected ? 3 : 1, e->inode);
        if (e->name_len > 0) {
            proc_printf(buf, " ");
            if (e->name[0] != '\0') {
                proc_printf(buf, "%.*s", (int) strnlen(e->name, e->name_len), e->name);
            } else {
                for (int c = 0; c < e->name_len; c++)
                    proc_printf(buf, "%c", c == 0 || e->name[c] == '\0' ? '@' : e->name[c]);
            }
        }
        proc_printf(buf, "\n");
    }
    free(entries);
    return 0;
}

// There is deliberately no /proc/net/route: the host's routing table is not
// available to us, and a made-up one would be read as the truth.

// Exported /proc/net directory entry
struct proc_children proc_net_children = PROC_CHILDREN({
    {"dev", .show = proc_show_net_dev},
    {"if_inet6", .show = proc_show_net_if_inet6},
    {"tcp", .show = proc_show_net_tcp},
    {"tcp6", .show = proc_show_net_tcp6},
    {"udp", .show = proc_show_net_udp},
    {"udp6", .show = proc_show_net_udp6},
    {"raw", .show = proc_show_net_raw},
    {"raw6", .show = proc_show_net_raw6},
    {"unix", .show = proc_show_net_unix},
});
