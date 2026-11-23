// Regression test for /proc/net/tcp, udp, raw and unix and socket links in
// /proc/self/fd.
//
// iSH had no /proc/net/tcp, udp, tcp6 or udp6, so netstat (net-tools and
// busybox) printed "no support for `AF INET (tcp)' on this system" and ss
// listed nothing, and /proc/<pid>/fd showed sockets as anon_inode:[unknown],
// so nothing could tell which process owned a socket.
//
// Opens a listening TCP socket, a connected pair with unread data and a bound
// UDP socket, then finds each by its inode (fstat) in /proc/net/tcp or udp and
// checks its addresses, state and receive queue, and that /proc/self/fd links
// to socket:[inode].
//
// There was no /proc/net/raw or unix either (netstat -w and -x showed
// nothing). An ICMP raw socket is checked in /proc/net/raw (as root: a raw
// socket needs it natively), and a listening unix socket, a connection to it
// from both ends, a datagram socket and a socketpair in /proc/net/unix, field
// by field as Linux prints them, sampled from Linux 7.0. The unix sockets
// have abstract names, which iSH's realfs can bind (it makes no socket files).
// Running natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_proc_net regress_proc_net.c
// Run:   ish -r <rootfs> /tmp/regress_proc_net

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int passed, failed;

static void check(const char *name, int ok) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", name);
    }
}

struct row {
    unsigned local_ip, local_port, remote_ip, remote_port, state, tx, rx;
    unsigned long inode;
};

// The row of file for the socket with inode, or 0 if there is none.
static int find_row(const char *file, unsigned long inode, struct row *r) {
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return 0;
    char line[512];
    int found = 0;
    fgets(line, sizeof(line), f); // header
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        int n = sscanf(line, " %*d: %X:%X %X:%X %X %X:%X %*X:%*X %*X %*u %*u %lu",
                       &r->local_ip, &r->local_port, &r->remote_ip, &r->remote_port,
                       &r->state, &r->tx, &r->rx, &r->inode);
        found = n == 8 && r->inode == inode;
    }
    fclose(f);
    return found;
}

// Whether /proc/net/unix has the row want for the socket with inode, comparing
// everything after the kernel's pointer, which Linux hashes.
static int unix_row(unsigned long inode, const char *want) {
    FILE *f = fopen("/proc/net/unix", "r");
    if (f == NULL)
        return 0;
    char line[512], tag[32];
    snprintf(tag, sizeof(tag), " %lu", inode);
    int found = 0;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *fields = strstr(line, ": ");
        if (fields == NULL)
            continue;
        fields += 2;
        // the inode is the 6th field
        char *p = fields;
        for (int i = 0; i < 5 && p != NULL; i++)
            p = strchr(p + 1, ' ');
        if (p == NULL || strncmp(p, tag, strlen(tag)) != 0 ||
                (p[strlen(tag)] != '\0' && p[strlen(tag)] != ' '))
            continue;
        found = 1;
        if (strcmp(fields, want) != 0) {
            printf("  unix row %s\n  expected %s\n", fields, want);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static struct sockaddr_un listen_name = {.sun_family = AF_UNIX};
static socklen_t listen_name_len;
static int uclient = -1;

static void *connect_unix(void *arg) {
    (void) arg;
    uclient = socket(AF_UNIX, SOCK_STREAM, 0);
    connect(uclient, (struct sockaddr *) &listen_name, listen_name_len);
    return NULL;
}

static unsigned long inode_of(int fd) {
    struct stat st;
    fstat(fd, &st);
    return st.st_ino;
}

static int links_to_socket(int fd) {
    char path[64], target[64], expected[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0)
        return 0;
    target[n] = '\0';
    snprintf(expected, sizeof(expected), "socket:[%lu]", inode_of(fd));
    if (strcmp(target, expected) != 0)
        printf("  %s links to %s, expected %s\n", path, target, expected);
    return strcmp(target, expected) == 0;
}

int main(void) {
    unsigned loopback = htonl(INADDR_LOOPBACK);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = loopback};
    socklen_t len = sizeof(addr);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    bind(listener, (struct sockaddr *) &addr, sizeof(addr));
    listen(listener, 4);
    getsockname(listener, (struct sockaddr *) &addr, &len);
    unsigned port = ntohs(addr.sin_port);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *) &addr, sizeof(addr));
    int server = accept(listener, NULL, NULL);
    send(client, "hello", 5, 0);
    usleep(200 * 1000);

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in uaddr = {.sin_family = AF_INET, .sin_addr.s_addr = loopback};
    bind(udp, (struct sockaddr *) &uaddr, sizeof(uaddr));
    len = sizeof(uaddr);
    getsockname(udp, (struct sockaddr *) &uaddr, &len);

    struct row r;
    // Addresses are words in network byte order read as host integers, as
    // Linux prints them; loopback reads back as loopback.
    check("listener in tcp", find_row("/proc/net/tcp", inode_of(listener), &r) &&
          r.local_ip == loopback && r.local_port == port && r.state == 0x0A);
    check("accepted connection in tcp", find_row("/proc/net/tcp", inode_of(server), &r) &&
          r.local_port == port && r.remote_ip == loopback && r.state == 0x01 && r.rx == 5);
    check("client connection in tcp", find_row("/proc/net/tcp", inode_of(client), &r) &&
          r.remote_port == port && r.state == 0x01);
    check("udp socket in udp", find_row("/proc/net/udp", inode_of(udp), &r) &&
          r.local_port == ntohs(uaddr.sin_port) && r.state == 0x07);
    check("fd links to socket:[inode]", links_to_socket(listener) && links_to_socket(udp));

    if (geteuid() == 0) {
        // Linux numbers a raw socket's row by its hash bucket, its protocol,
        // and shows the protocol as its local port.
        int raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        check("icmp socket in raw", raw >= 0 && find_row("/proc/net/raw", inode_of(raw), &r) &&
              r.local_port == IPPROTO_ICMP && r.remote_port == 0 && r.state == 0x07);
        close(raw);
    }

    // "\0ish\0listen", shown as @ish@listen. It connects from another thread:
    // iSH's connect on a unix socket waits for the other end to accept, where
    // Linux's returns once the connection is queued.
    memcpy(listen_name.sun_path, "\0ish\0listen", 11);
    listen_name_len = offsetof(struct sockaddr_un, sun_path) + 11;
    int ulistener = socket(AF_UNIX, SOCK_STREAM, 0);
    bind(ulistener, (struct sockaddr *) &listen_name, listen_name_len);
    listen(ulistener, 4);
    pthread_t connector;
    pthread_create(&connector, NULL, connect_unix, NULL);
    int userver = accept(ulistener, NULL, NULL);
    pthread_join(connector, NULL);
    struct sockaddr_un abstract = {.sun_family = AF_UNIX};
    memcpy(abstract.sun_path, "\0ish\0regress", 12);
    int udgram = socket(AF_UNIX, SOCK_DGRAM, 0);
    bind(udgram, (struct sockaddr *) &abstract, offsetof(struct sockaddr_un, sun_path) + 12);
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);

    char want[256];
    snprintf(want, sizeof(want), "00000002 00000000 00010000 0001 01 %5lu @ish@listen", inode_of(ulistener));
    check("unix listener", unix_row(inode_of(ulistener), want));
    snprintf(want, sizeof(want), "00000003 00000000 00000000 0001 03 %5lu @ish@listen", inode_of(userver));
    check("accepted unix connection has the listener's name", unix_row(inode_of(userver), want));
    snprintf(want, sizeof(want), "00000003 00000000 00000000 0001 03 %5lu", inode_of(uclient));
    check("unix client", unix_row(inode_of(uclient), want));
    snprintf(want, sizeof(want), "00000002 00000000 00000000 0002 01 %5lu @ish@regress", inode_of(udgram));
    check("abstract unix datagram socket", unix_row(inode_of(udgram), want));
    snprintf(want, sizeof(want), "00000003 00000000 00000000 0001 03 %5lu", inode_of(pair[0]));
    check("unix socketpair", unix_row(inode_of(pair[0]), want));
    struct sockaddr_un name;
    len = sizeof(name);
    check("accepted unix socket's name is the listener's",
          getsockname(userver, (struct sockaddr *) &name, &len) == 0 && len == listen_name_len &&
          memcmp(name.sun_path, listen_name.sun_path, 11) == 0);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
