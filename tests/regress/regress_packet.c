// Regression test for AF_PACKET sockets (kernel/packet.c) and the interface
// ioctls, as libpcap 1.10 (tcpdump -i) uses them.
//
// AF_PACKET failed with EAFNOSUPPORT ("PF_PACKET sockets not supported - is
// this WSL1?") and the interface ioctls with ENOTTY. Now a packet socket gets
// the packets /dev/netcap's are, made up from iSH's sockets' calls: in a
// TPACKET_V2 ring mapped with mmap (TPACKET_V3 refused with EINVAL, as libpcap
// takes from a kernel without it), poll saying when a frame is there, each
// with its header and sockaddr_ll: loopback's on lo, incoming, once. SOCK_RAW
// has the Ethernet header lo has. Filters are not here (ENOPROTOOPT, as
// libpcap takes from a kernel without them).
//
// The ioctls ifconfig asks besides (SIOCGIFCONF and the addresses) failed
// with ENOTTY too: "SIOCGIFCONF: Not a tty", "error fetching interface
// information".
//
// iSH only: on Linux the socket sees real traffic. Run as root.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_packet regress_packet.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/ethtool.h>
#include <linux/filter.h>
#include <net/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

static const char *err(int r) {
    return r < 0 ? strerror(errno) : "ok";
}

// a ring of 8 frames of 4096 bytes, mapped; NULL if it could not be had
static char *ring_up(int s, size_t *size) {
    int v = TPACKET_V2;
    if (setsockopt(s, SOL_PACKET, PACKET_VERSION, &v, sizeof(v)) < 0)
        return NULL;
    struct tpacket_req req = {.tp_block_size = 4096, .tp_block_nr = 8, .tp_frame_size = 4096, .tp_frame_nr = 8};
    if (setsockopt(s, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
        return NULL;
    *size = 8 * 4096;
    char *ring = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, s, 0);
    return ring == MAP_FAILED ? NULL : ring;
}

// a TCP exchange over loopback: the handshake, "hi" and "yo", both FINs
static void exchange(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(addr);
    bind(server, (struct sockaddr *) &addr, sizeof(addr));
    listen(server, 1);
    getsockname(server, (struct sockaddr *) &addr, &len);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *) &addr, sizeof(addr));
    int conn = accept(server, NULL, NULL);
    char buf[8];
    send(client, "hi", 2, 0);
    recv(conn, buf, sizeof(buf), 0);
    send(conn, "yo", 2, 0);
    recv(client, buf, sizeof(buf), 0);
    close(client);
    recv(conn, buf, sizeof(buf), 0);
    close(conn);
    close(server);
}

// the frames there, each released; *out what they say
static int frames(char *ring, char *out, size_t out_size, int lo_index, int *hdr_ok) {
    int n = 0;
    char *o = out;
    *hdr_ok = 1;
    for (int i = 0; i < 8; i++) {
        struct tpacket2_hdr *h = (void *) (ring + i * 4096);
        if (!(h->tp_status & TP_STATUS_USER))
            continue;
        struct sockaddr_ll *sll = (void *) ((char *) h + TPACKET_ALIGN(sizeof(*h)));
        const unsigned char *data = (const unsigned char *) h + h->tp_mac;
        const unsigned char *ip = (const unsigned char *) h + h->tp_net;
        int tcp_flags = ip[9] == IPPROTO_TCP ? ip[20 + 13] : -1;
        o += snprintf(o, out + out_size - o, " [len %u/%u mac %u net %u if %d hatype %u pkttype %u proto %#x flags %#x ll %u]",
                      h->tp_snaplen, h->tp_len, h->tp_mac, h->tp_net, sll->sll_ifindex, sll->sll_hatype,
                      sll->sll_pkttype, ntohs(sll->sll_protocol), tcp_flags, h->tp_net - h->tp_mac);
        *hdr_ok = *hdr_ok && h->tp_sec != 0 && h->tp_snaplen == h->tp_len && sll->sll_family == AF_PACKET &&
            sll->sll_ifindex == lo_index && sll->sll_hatype == ARPHRD_LOOPBACK && sll->sll_pkttype == PACKET_HOST &&
            ntohs(sll->sll_protocol) == ETH_P_IP && (ip[0] >> 4) == 4 && (data == ip || data[12] == 0x08);
        __atomic_store_n(&h->tp_status, TP_STATUS_KERNEL, __ATOMIC_RELEASE);
        n++;
    }
    return n;
}

int main(void) {
    char detail[2048];

    // 1. the interface ioctls, on lo
    int inet = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr = {0};
    strcpy(ifr.ifr_name, "lo");
    int r_index = ioctl(inet, SIOCGIFINDEX, &ifr);
    int lo_index = ifr.ifr_ifindex;
    struct ifreq by_index = {.ifr_ifindex = lo_index};
    int r_name = ioctl(inet, SIOCGIFNAME, &by_index);
    int r_flags = ioctl(inet, SIOCGIFFLAGS, &ifr);
    int flags = ifr.ifr_flags;
    int r_hw = ioctl(inet, SIOCGIFHWADDR, &ifr);
    int hw = ifr.ifr_hwaddr.sa_family;
    int r_mtu = ioctl(inet, SIOCGIFMTU, &ifr);
    int mtu = ifr.ifr_mtu;
    struct ethtool_value tso = {.cmd = ETHTOOL_GTSO};
    ifr.ifr_data = (void *) &tso;
    int r_tso = ioctl(inet, SIOCETHTOOL, &ifr);
    struct ethtool_value link = {.cmd = ETHTOOL_GLINK};
    ifr.ifr_data = (void *) &link;
    int r_link = ioctl(inet, SIOCETHTOOL, &ifr);
    int link_errno = errno;
    struct ifreq none = {0};
    strcpy(none.ifr_name, "nonesuch0");
    int r_none = ioctl(inet, SIOCGIFINDEX, &none);
    int none_errno = errno;
    close(inet);
    snprintf(detail, sizeof(detail), "SIOCGIFINDEX %s %d, SIOCGIFNAME %s %s, SIOCGIFFLAGS %s %#x, SIOCGIFHWADDR %s %d, "
             "SIOCGIFMTU %s %d, ETHTOOL_GTSO %s %u, ETHTOOL_GLINK %s, nonesuch0 %s", err(r_index), lo_index, err(r_name),
             by_index.ifr_name, err(r_flags), flags, err(r_hw), hw, err(r_mtu), mtu, err(r_tso), tso.data,
             r_link < 0 ? strerror(link_errno) : "ok", r_none < 0 ? strerror(none_errno) : "ok");
    check("interface ioctls on lo", r_index == 0 && lo_index > 0 && r_name == 0 && strcmp(by_index.ifr_name, "lo") == 0 &&
          r_flags == 0 && (flags & IFF_UP) && (flags & IFF_LOOPBACK) && r_hw == 0 && hw == ARPHRD_LOOPBACK && r_mtu == 0 && mtu > 0 &&
          r_tso == 0 && tso.data == 1 && r_link < 0 && link_errno == EOPNOTSUPP && r_none < 0 && none_errno == ENODEV,
          detail);

    // 2. what ifconfig asks besides, on lo: the IPv4 addresses (SIOCGIFCONF,
    // first the room they need), lo's address, mask, destination (its own),
    // broadcast (none), metric, map and queue length
    inet = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifconf ifc = {.ifc_len = 0, .ifc_buf = NULL};
    int r_need = ioctl(inet, SIOCGIFCONF, &ifc);
    int need = ifc.ifc_len;
    struct ifreq reqs[64];
    ifc.ifc_len = sizeof(reqs);
    ifc.ifc_req = reqs;
    int r_conf = ioctl(inet, SIOCGIFCONF, &ifc);
    int lo_listed = 0;
    for (int i = 0; r_conf == 0 && i < ifc.ifc_len / (int) sizeof(struct ifreq); i++) {
        struct sockaddr_in *sin = (void *) &reqs[i].ifr_addr;
        if (strcmp(reqs[i].ifr_name, "lo") == 0 && sin->sin_family == AF_INET &&
                sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
            lo_listed = 1;
    }
    ifc.ifc_len = sizeof(struct ifreq) - 1;
    int r_small = ioctl(inet, SIOCGIFCONF, &ifc);
    int small_len = ifc.ifc_len;
    unsigned long cmds[] = {SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFDSTADDR, SIOCGIFBRDADDR};
    uint32_t want[] = {htonl(INADDR_LOOPBACK), htonl(0xff000000), htonl(INADDR_LOOPBACK), 0};
    const char *names[] = {"addr", "netmask", "dstaddr", "brdaddr"};
    int addrs_ok = 1;
    char *o = detail + sprintf(detail, "SIOCGIFCONF need %s %d, got %s %d (lo %s), too small %s %d;", err(r_need), need,
                               err(r_conf), ifc.ifc_req == reqs ? (int) sizeof(reqs) : -1, lo_listed ? "listed" : "not",
                               err(r_small), small_len);
    for (int i = 0; i < 4; i++) {
        struct ifreq req = {0};
        strcpy(req.ifr_name, "lo");
        int r = ioctl(inet, cmds[i], &req);
        struct sockaddr_in *sin = (void *) &req.ifr_addr;
        addrs_ok = addrs_ok && r == 0 && sin->sin_family == AF_INET && sin->sin_addr.s_addr == want[i];
        o += sprintf(o, " %s %s %d %s", names[i], err(r), sin->sin_family, inet_ntoa(sin->sin_addr));
    }
    struct ifreq more = {0};
    strcpy(more.ifr_name, "lo");
    int r_metric = ioctl(inet, SIOCGIFMETRIC, &more);
    int metric = more.ifr_metric;
    memset(&more.ifr_map, 0xff, sizeof(more.ifr_map));
    int r_map = ioctl(inet, SIOCGIFMAP, &more);
    int map_zero = more.ifr_map.mem_start == 0 && more.ifr_map.irq == 0 && more.ifr_map.base_addr == 0;
    int r_qlen = ioctl(inet, SIOCGIFTXQLEN, &more);
    close(inet);
    sprintf(o, "; metric %s %d, map %s %s, txqlen %s %d", err(r_metric), metric, err(r_map), map_zero ? "zero" : "set",
            err(r_qlen), more.ifr_qlen);
    check("ifconfig's ioctls on lo", r_need == 0 && need > 0 && need % (int) sizeof(struct ifreq) == 0 && r_conf == 0 &&
          lo_listed && r_small == 0 && small_len == 0 && addrs_ok && r_metric == 0 && metric == 0 && r_map == 0 &&
          map_zero && r_qlen == 0 && more.ifr_qlen == 1000, detail);

    // 3. another user may not have one
    pid_t child = fork();
    if (child == 0) {
        setgid(1000);
        setuid(1000);
        int s = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
        _exit(s >= 0 ? 0 : errno);
    }
    int status;
    waitpid(child, &status, 0);
    snprintf(detail, sizeof(detail), "socket: %s", WIFEXITED(status) ? strerror(WEXITSTATUS(status)) : "killed");
    check("another user: EPERM", WIFEXITED(status) && WEXITSTATUS(status) == EPERM, detail);

    // 4. as libpcap sets one up: TPACKET_V3 refused, V2's header, a ring, no
    // filter
    int s = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
    int v = TPACKET_V3;
    socklen_t len = sizeof(v);
    int r_v3 = getsockopt(s, SOL_PACKET, PACKET_HDRLEN, &v, &len);
    int v3_errno = errno;
    v = TPACKET_V2;
    len = sizeof(v);
    int r_v2 = getsockopt(s, SOL_PACKET, PACKET_HDRLEN, &v, &len);
    struct sockaddr_ll bad = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = 99999};
    int r_bad = bind(s, (struct sockaddr *) &bad, sizeof(bad));
    int bad_errno = errno;
    struct sockaddr_ll all = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL)};
    int r_bind = bind(s, (struct sockaddr *) &all, sizeof(all));
    size_t ring_size;
    char *ring = ring_up(s, &ring_size);
    struct sock_filter code[] = {{0x6, 0, 0, 0xffff}};
    struct sock_fprog prog = {1, code};
    int r_filter = setsockopt(s, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog));
    int filter_errno = errno;
    snprintf(detail, sizeof(detail), "PACKET_HDRLEN V3 %s, V2 %s %d; bind ifindex 99999 %s, any %s; ring %s; "
             "SO_ATTACH_FILTER %s", r_v3 < 0 ? strerror(v3_errno) : "ok", err(r_v2), v,
             r_bad < 0 ? strerror(bad_errno) : "ok", err(r_bind), ring != NULL ? "mapped" : strerror(errno),
             r_filter < 0 ? strerror(filter_errno) : "ok");
    check("set up as libpcap does", r_v3 < 0 && v3_errno == EINVAL && r_v2 == 0 && v == 32 && r_bad < 0 &&
          bad_errno == ENODEV && r_bind == 0 && ring != NULL && r_filter < 0 && filter_errno == ENOPROTOOPT, detail);

    // 5. nothing yet: not readable
    struct pollfd pfd = {s, POLLIN, 0};
    int idle = poll(&pfd, 1, 0);
    check("nothing yet: not readable", idle == 0, "readable");

    // 6. a loopback exchange: its 7 packets, once each, on lo, incoming
    exchange();
    int ready = poll(&pfd, 1, 1000);
    int hdr_ok;
    o = detail + sprintf(detail, "poll %d (%#x):", ready, pfd.revents);
    int n = ring != NULL ? frames(ring, o, detail + sizeof(detail) - o, lo_index, &hdr_ok) : -1;
    struct tpacket_stats stats;
    len = sizeof(stats);
    int r_stats = getsockopt(s, SOL_PACKET, PACKET_STATISTICS, &stats, &len);
    o += strlen(o);
    snprintf(o, detail + sizeof(detail) - o, "; %d frames, stats %s %u/%u", n, err(r_stats), stats.tp_packets,
             stats.tp_drops);
    check("SOCK_DGRAM: a loopback exchange in the ring", ready == 1 && n == 7 && hdr_ok && r_stats == 0 &&
          stats.tp_packets == 7 && stats.tp_drops == 0, detail);
    if (ring != NULL)
        munmap(ring, ring_size);
    close(s);

    // 7. SOCK_RAW on lo: Ethernet's header, all zero but the type
    s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    struct sockaddr_ll on_lo = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = lo_index};
    int r_lo = bind(s, (struct sockaddr *) &on_lo, sizeof(on_lo));
    ring = ring_up(s, &ring_size);
    exchange();
    pfd.fd = s;
    ready = poll(&pfd, 1, 1000);
    int ether_ok = 0;
    if (ring != NULL) {
        struct tpacket2_hdr *h = (void *) ring;
        const unsigned char *eth = (const unsigned char *) h + h->tp_mac;
        static const unsigned char zeros[12];
        ether_ok = (h->tp_status & TP_STATUS_USER) && h->tp_net - h->tp_mac == 14 && memcmp(eth, zeros, 12) == 0 &&
            eth[12] == 0x08 && eth[13] == 0x00 && h->tp_len == 14 + 40;
    }
    o = detail + sprintf(detail, "bind lo %s, poll %d:", err(r_lo), ready);
    n = ring != NULL ? frames(ring, o, detail + sizeof(detail) - o, lo_index, &hdr_ok) : -1;
    check("SOCK_RAW on lo: Ethernet's header", r_lo == 0 && ready == 1 && ether_ok && n == 7 && hdr_ok, detail);
    if (ring != NULL)
        munmap(ring, ring_size);
    close(s);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
