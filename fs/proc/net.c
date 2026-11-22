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

// /proc/net/tcp, tcp6, udp and udp6: the sockets the guest has open, in
// Linux's format, which netstat and ss parse.
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
// 1 for TCP and 2 for UDP, as Linux shows an idle socket.
static int proc_show_net_inet(struct proc_data *buf, int family, int type) {
    struct sock_inet_entry *entries;
    int n = sock_inet_list(family, type, &entries);
    if (n < 0)
        return n;
    bool tcp = type == SOCK_STREAM_;
    if (family == AF_INET_)
        proc_printf(buf, tcp ? "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                             : "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    else
        proc_printf(buf, tcp ? "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                             : "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    for (int i = 0; i < n; i++) {
        const struct sock_inet_entry *e = &entries[i];
        proc_printf(buf, tcp || family == AF_INET6_ ? "%4d: " : "%5d: ", i);
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

// There is deliberately no /proc/net/route: the host's routing table is not
// available to us, and a made-up one would be read as the truth.

// /proc/net directory entries
struct proc_dir_entry proc_net_entries[] = {
    {"dev", .show = proc_show_net_dev},
    {"if_inet6", .show = proc_show_net_if_inet6},
    {"tcp", .show = proc_show_net_tcp},
    {"tcp6", .show = proc_show_net_tcp6},
    {"udp", .show = proc_show_net_udp},
    {"udp6", .show = proc_show_net_udp6},
};
#define PROC_NET_LEN (sizeof(proc_net_entries)/sizeof(proc_net_entries[0]))

static bool proc_net_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_NET_LEN) {
        *next_entry = (struct proc_entry) {&proc_net_entries[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }
    return false;
}

// Exported /proc/net directory entry
struct proc_children proc_net_children = PROC_CHILDREN({
    {"dev", .show = proc_show_net_dev},
    {"if_inet6", .show = proc_show_net_if_inet6},
    {"tcp", .show = proc_show_net_tcp},
    {"tcp6", .show = proc_show_net_tcp6},
    {"udp", .show = proc_show_net_udp},
    {"udp6", .show = proc_show_net_udp6},
});
