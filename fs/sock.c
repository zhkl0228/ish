// musl's netinet/tcp.h has struct tcp_info only with _GNU_SOURCE (glibc's by
// default): a Linux host's TCP_INFO (sock_inet_list)
#define _GNU_SOURCE
#include "kernel/packet.h"
#include "kernel/netcap.h"
#include "kernel/inotify.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/real.h"

// TRACE() in this file routes to the `verbose` channel (debug.h's default),
// i.e. compiled out unless DEBUG_verbose is set.
#define DEFAULT_CHANNEL verbose
#include "fs/sock.h"
#include "fs/netlink.h"
#include "debug.h"

#define SOCKET_TYPE_MASK 0xf

const struct fd_ops socket_fdops;

// ---------------------------------------------------------------------------
// AF_NETLINK stub -- the socket half. fs/netlink.c is the other half: it
// turns a request into reply datagrams.
//
// Darwin has no AF_NETLINK and there is no Linux network stack under the
// guest, so nothing is forwarded. A netlink fd is a host socketpair whose
// far end this file writes replies into, which gives poll/epoll/close and
// refcounting for free. The rules it keeps:
//
//   * Every request is answered when it is SENT -- by write(), writev(),
//     send(), sendto() or sendmsg() alike -- so a reply is already queued by
//     the time the guest reads. (The stub used to answer only sendto and
//     sendmsg: busybox `ip` sends with write() and then waited for ever.)
//   * What the host can tell us is real: RTM_GETLINK and RTM_GETADDR come
//     from getifaddrs(). What it cannot (routes, rules, neighbours, any
//     change, any protocol but NETLINK_ROUTE) is refused with NLMSG_ERROR at
//     once. Nothing is left unanswered.
//   * A read never blocks without bound. MSG_PEEK, MSG_TRUNC, MSG_DONTWAIT
//     and O_NONBLOCK mean what they mean on Linux; an empty queue is EAGAIN
//     for a non-blocking reader, and a blocking reader waits in bounded
//     polls so a guest signal always gets through. A socket bound to
//     multicast groups (ip monitor, Go's netmon) waits the same way: no
//     event is ever delivered, exactly as on a Linux box where nothing
//     changes.
//
// On by default; ISH_NETLINK_STUB=0 disables it at runtime (AF_NETLINK is
// then EAFNOSUPPORT). ISH_NETLINK_ROUTE=empty answers a route dump with an
// empty dump instead of EOPNOTSUPP.
// ---------------------------------------------------------------------------

#define AF_NETLINK_ 16
#define NETLINK_ROUTE_ 0

// nlmsghdr, as the guest (Linux/arm64) sees it. Same layout on all Linux
// arches: 16 bytes, 4-byte aligned.
struct nlmsghdr_ {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};
#define NLMSG_NOOP_  1
#define NLMSG_ERROR_ 2
#define NLMSG_DONE_  3
#define NLMSG_ALIGN_(len) (((len) + 3) & ~3u)

// sockaddr_nl as the guest sees it (12 bytes on all Linux arches).
struct sockaddr_nl_ {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

bool ish_netlink_stub_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        // Built in and on by default (tailscale / tsnet die at startup on
        // EAFNOSUPPORT otherwise); ISH_NETLINK_STUB=0 turns it off.
        const char *v = getenv("ISH_NETLINK_STUB");
        cached = (v != NULL && v[0] == '0') ? 0 : 1;
    }
    return cached == 1;
}

// One line per kind of event for the life of the process, not one per call:
// the stub sits on paths Go programs hit in a loop.
static void netlink_stub_log_once(int *flag, const char *what, unsigned type, unsigned seq) {
    if (__sync_bool_compare_and_swap(flag, 0, 1))
        printk("NETLINK_STUB %s (pid=%d comm=%s type=%u seq=%u; logged once)\n",
               what, current->pid, current->comm, type, seq);
}

struct netlink_emit_ctx {
    struct fd *fd;
    bool overflow;
    bool refused;
    unsigned count;
};

// Queue one reply datagram into the socketpair. The far end is
// non-blocking: a full queue is recorded, never waited on.
static int netlink_stub_emit(void *opaque, const void *dgram, size_t len) {
    struct netlink_emit_ctx *ctx = opaque;
    int peer = ctx->fd->socket.netlink_peer_fd;
    if (peer < 0)
        return _EINVAL;
    if (len >= 20 && ((const struct nlmsghdr_ *) dgram)->nlmsg_type == NLMSG_ERROR_ &&
            ((const int32_t *) dgram)[4] != 0)
        ctx->refused = true;
    for (;;) {
        ssize_t w = send(peer, dgram, len, MSG_DONTWAIT);
        if (w >= 0) {
            ctx->count++;
            return 0;
        }
        if (errno == EINTR)
            continue;
        ctx->overflow = true;
        return _ENOBUFS;
    }
}

// Answer a request the guest just sent (by whatever syscall). Returns
// req_len: as on Linux, sending a request succeeds whatever its answer is.
static int netlink_stub_reply(struct fd *fd, const void *req, size_t req_len) {
    static int logged_refuse, logged_overflow;
    struct netlink_emit_ctx ctx = {.fd = fd};
    struct netlink_stub_req last = {0};
    netlink_stub_answer(fd->socket.protocol, req, req_len, netlink_stub_emit, &ctx, &last);

    lock(&fd->lock);
    // A dump that did not fit the queue still has to end: the reader is
    // owed an NLMSG_DONE, handed over when it finds the queue empty.
    fd->socket.netlink_owe_done = ctx.overflow && last.valid;
    fd->socket.netlink_owe_seq = last.seq;
    unlock(&fd->lock);

    if (ctx.overflow)
        netlink_stub_log_once(&logged_overflow, "reply queue full, dump cut short", last.type, last.seq);
    else if (ctx.refused)
        netlink_stub_log_once(&logged_refuse, "request refused with NLMSG_ERROR (not available on this host)", last.type, last.seq);
    TRACE("NETLINK_STUB request type=%u seq=%u flags=%#x -> %u datagram(s)%s%s\n",
          last.type, last.seq, last.flags, ctx.count,
          ctx.refused ? " (refused)" : "", ctx.overflow ? " (queue full)" : "");
    return (int) req_len;
}

// A guest iovec: 64-bit base and length.
typedef struct iovec64_ guest_iovec_t;

static int netlink_stub_sendmsg(struct fd *sock, addr_t msghdr_addr) {
    addr_t iov_addr; uint64_t iovlen;
    struct msghdr64_ m;
    if (user_get(msghdr_addr, m)) return _EFAULT;
    iov_addr = (addr_t) m.msg_iov; iovlen = m.msg_iovlen;
    char req[4096]; size_t total = 0;
    for (uint64_t i = 0; i < iovlen && total < sizeof(req); i++) {
        guest_iovec_t iv;
        if (user_get(iov_addr + i * sizeof(iv), iv)) return _EFAULT;
        size_t n = (size_t) iv.len;
        if (n > sizeof(req) - total) n = sizeof(req) - total;
        if (n && user_read((addr_t) iv.base, req + total, n)) return _EFAULT;
        total += n;
    }
    int r = netlink_stub_reply(sock, req, total);
    return r < 0 ? r : (int) total;
}

static long monotonic_ms(void);

// Take (or with MSG_PEEK_, look at) the next reply datagram. Returns its
// full length, which may exceed `cap`, or a negative errno. Never blocks
// without bound:
//   * a queued datagram is returned at once;
//   * an empty queue with a dump still owed its end yields that NLMSG_DONE;
//   * an empty queue is _EAGAIN for a non-blocking reader;
//   * a blocking reader waits in 1 s polls, checking for guest signals
//     between them (_EINTR), until something is queued or SO_RCVTIMEO runs
//     out.
static ssize_t netlink_stub_dequeue(struct fd *sock, int guest_flags, char *buf, size_t cap) {
    static int logged_owed, logged_wait;
    int host_flags = MSG_DONTWAIT | ((guest_flags & MSG_PEEK_) ? MSG_PEEK : 0);
    int fl = fcntl(sock->real_fd, F_GETFL, 0);
    bool nonblock = (guest_flags & MSG_DONTWAIT_) || (sock->flags & O_NONBLOCK_) ||
                    (fl >= 0 && (fl & O_NONBLOCK));
    long timeout_ms = sock->socket.netlink_rcvtimeo_ms;
    long start_ms = monotonic_ms();
    for (;;) {
        ssize_t n = recv(sock->real_fd, buf, cap, host_flags);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return errno_map();

        lock(&sock->lock);
        bool owed = sock->socket.netlink_owe_done;
        uint32_t seq = sock->socket.netlink_owe_seq;
        if (owed && !(guest_flags & MSG_PEEK_))
            sock->socket.netlink_owe_done = false;
        unlock(&sock->lock);
        if (owed && cap >= 20) {
            netlink_stub_log_once(&logged_owed, "empty queue with a dump unfinished: NLMSG_DONE synthesized", NLMSG_DONE_, seq);
            return (ssize_t) netlink_stub_done(buf, seq);
        }
        if (nonblock)
            return _EAGAIN;

        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            bool pending = task_signal_pending(current);
            unlock(&current->sighand->lock);
            if (pending)
                return _EINTR;
        }
        long waited = monotonic_ms() - start_ms;
        if (timeout_ms > 0 && waited >= timeout_ms)
            return _EAGAIN;
        if (waited >= 3000)
            netlink_stub_log_once(&logged_wait, "blocking read with nothing queued for 3s (no event is ever delivered)",
                                  0, sock->socket.netlink_groups);
        int poll_ms = 1000;
        if (timeout_ms > 0 && timeout_ms - waited < poll_ms)
            poll_ms = (int) (timeout_ms - waited);
        struct pollfd pfd = {.fd = sock->real_fd, .events = POLLIN};
        current->blocking = true;
        poll(&pfd, 1, poll_ms);
        current->blocking = false;
    }
}

static int netlink_stub_recvmsg(struct fd *sock, addr_t msghdr_addr, int flags) {
    addr_t iov_addr, name_addr; uint64_t iovlen; uint32_t namelen;
    struct msghdr64_ m;
    if (user_get(msghdr_addr, m)) return _EFAULT;
    iov_addr = (addr_t) m.msg_iov; iovlen = m.msg_iovlen;
    name_addr = (addr_t) m.msg_name; namelen = m.msg_namelen;
    char buf[NETLINK_STUB_DGRAM_MAX * 2];
    ssize_t n = netlink_stub_dequeue(sock, flags, buf, sizeof(buf));
    if (n < 0) return (int) n;
    size_t have_total = (size_t) n < sizeof(buf) ? (size_t) n : sizeof(buf);

    size_t off = 0;
    for (uint64_t i = 0; i < iovlen && off < have_total; i++) {
        guest_iovec_t iv;
        if (user_get(iov_addr + i * sizeof(iv), iv)) return _EFAULT;
        size_t want = (size_t) iv.len, have = have_total - off;
        if (want > have) want = have;
        if (want && user_write((addr_t) iv.base, buf + off, want)) return _EFAULT;
        off += want;
    }
    if (name_addr != 0 && namelen >= sizeof(struct sockaddr_nl_)) {
        struct sockaddr_nl_ nl = { .nl_family = AF_NETLINK_, .nl_pad = 0,
                                   .nl_pid = 0, .nl_groups = 0 };
        if (user_write(name_addr, &nl, sizeof(nl))) return _EFAULT;
        m.msg_namelen = sizeof(nl);
    } else {
        m.msg_namelen = 0;
    }
    // Written back as Linux does: who it came from, no ancillary data, and
    // MSG_TRUNC when the datagram did not fit.
    m.msg_controllen = 0;
    m.msg_flags = off < (size_t) n ? MSG_TRUNC_ : 0;
    if (user_put(msghdr_addr, m)) return _EFAULT;
    TRACE("NETLINK_STUB recvmsg flags=%#x -> %zd bytes (%zu copied)\n", flags, n, off);
    // With MSG_TRUNC the caller asks for the real length of the datagram.
    return (flags & MSG_TRUNC_) ? (int) n : (int) off;
}

// Sockets get inode numbers of their own, as from Linux's sockfs: fstat
// reports one, /proc/<pid>/fd shows socket:[inode], and /proc/net/tcp and udp
// list it, which is how netstat -p and ss -p find a socket's process.
static unsigned long sock_new_inode(void) {
    return adhoc_new_inode();
}

static fd_t netlink_stub_socket(dword_t type, dword_t protocol) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0)
        return errno_map();
    struct fd *fd = adhoc_fd_create(&socket_fdops);
    if (fd == NULL) {
        close(sv[0]);
        close(sv[1]);
        return _ENOMEM;
    }
    fd->stat.mode = S_IFSOCK | 0666;
    fd->stat.inode = sock_new_inode();
    fd->real_fd = sv[0];
    fd->socket.domain = AF_NETLINK_;
    fd->socket.type = type & SOCKET_TYPE_MASK;
    fd->socket.protocol = protocol;
    fd->socket.netlink_peer_fd = sv[1];
    fd->socket.netlink_groups = 0;
    fd->socket.netlink_owe_done = false;
    fd->socket.netlink_owe_seq = 0;
    fd->socket.netlink_rcvtimeo_ms = 0;
    // A full link + address dump is queued in one go, one message per
    // datagram, and a host AF_UNIX datagram socket holds only a few
    // kilobytes by default. Give the queue room; if the host refuses, the
    // dump is cut short and ended properly (netlink_owe_done).
    int space = 512 * 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &space, sizeof(space));
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &space, sizeof(space));
    TRACE("NETLINK_STUB socket(AF_NETLINK, %d, %d) -> stub fd\n", type, protocol);
    // [T-ish-host-fd-double-close] A failed f_install() has closed fd, and
    // sock_close() with it both sv[0] (real_fd) and sv[1] (netlink_peer_fd).
    return f_install(fd, type & ~SOCKET_TYPE_MASK);
}

static bool fd_is_netlink_stub(struct fd *fd) {
    return fd != NULL && fd->ops == &socket_fdops &&
           fd->socket.domain == AF_NETLINK_;
}

static lock_t peer_lock = LOCK_INITIALIZER;

// A unix stream connection the other end has not accepted yet. As on Linux,
// connect returns once the connection is queued; the two ends' peer pointers
// are linked when accept takes it. connect writes a unix_connect_header first
// thing on the connection, and accept reads it: the id to find this, and the
// credentials of the connecting process. File descriptors the
// connecting end sends meanwhile wait here for the accepted socket. Every
// field and the list are locked by peer_lock.
struct unix_pending {
    uint64_t id;
    struct fd *client; // NULL once it has closed
    struct list scm;
    struct list links;
};
static struct list unix_pending_list = LIST_INITIALIZER(unix_pending_list);
static uint64_t unix_pending_next_id = 1;

struct unix_connect_header {
    uint64_t id;
    struct ucred_ cred;
};

// Listening unix sockets, by their unix_listening. Locked by peer_lock.
static struct list unix_listeners = LIST_INITIALIZER(unix_listeners);

static void scm_free(struct scm *scm);

static void unix_pending_free(struct unix_pending *pending) {
    list_remove(&pending->links);
    struct scm *scm, *tmp;
    list_for_each_entry_safe(&pending->scm, scm, tmp, queue) {
        list_remove(&scm->queue);
        scm_free(scm);
    }
    free(pending);
}

// [T-ish-host-fd-double-close] Owns sock_fd from here on: on any failure it is
// already closed (f_install() closes the fd it could not install, and with it
// real_fd). Callers used to close() it again, which closes whatever the app
// opened under that number in between.
static fd_t sock_fd_create(int sock_fd, int domain, int type, int protocol) {
    struct fd *fd = adhoc_fd_create(&socket_fdops);
    if (fd == NULL) {
        close(sock_fd);
        return _ENOMEM;
    }
    fd->stat.mode = S_IFSOCK | 0666;
    fd->stat.inode = sock_new_inode();
    fd->real_fd = sock_fd;
    fd->socket.domain = domain;
    fd->socket.type = type & SOCKET_TYPE_MASK;
    fd->socket.protocol = protocol;
    fd->socket.netlink_peer_fd = -1; // [STAGE-0 PROBE] only netlink sets this
    // No peer credentials yet: as Linux reports them for a socket without
    fd->socket.unix_peer_cred = (struct ucred_) {.pid = 0, .uid = -1, .gid = -1};
    if (domain == AF_LOCAL_)
        list_init(&fd->socket.unix_scm);
    return f_install(fd, type & ~SOCKET_TYPE_MASK);
}

int_t sys_socket(dword_t domain, dword_t type, dword_t protocol) {
    STRACE("socket(%d, %d, %d)", domain, type, protocol);
    // [STAGE-0 PROBE] AF_NETLINK has no host equivalent (Darwin uses PF_ROUTE),
    // so it never reaches sock_family_to_real -- it is served entirely by the
    // stub above. On unless ISH_NETLINK_STUB=0.
    if (domain == AF_NETLINK_ && ish_netlink_stub_enabled())
        return netlink_stub_socket(type, protocol);
    if (domain == AF_PACKET_)
        return packet_socket(type, protocol);
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EAFNOSUPPORT;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return real_type;

    // this hack makes mtr work
    if (type == SOCK_RAW_ && protocol == IPPROTO_RAW)
        protocol = IPPROTO_ICMP;

    int sock = socket(real_domain, real_type, protocol);
    if (sock < 0)
        return errno_map();

#ifdef __APPLE__
    if (domain == AF_INET_ && type == SOCK_DGRAM_) {
        // in some cases, such as ICMP, datagram sockets on mac can default to
        // including the IP header like raw sockets
        int one = 1;
        setsockopt(sock, IPPROTO_IP, IP_STRIPHDR, &one, sizeof(one));
    }
#endif

    return sock_fd_create(sock, domain, type, protocol);
}

static void inode_release_if_exist(struct inode_data *inode) {
    if (inode != NULL)
        inode_release(inode);
}

static struct fd *sock_getfd(fd_t sock_fd) {
    struct fd *sock = f_get(sock_fd);
    if (sock == NULL || sock->ops != &socket_fdops)
        return NULL;
    return sock;
}

// What a socket call says of an fd sock_getfd gave none for, as Linux's
// sockfd_lookup: EBADF if there is no such fd, ENOTSOCK if it is no socket.
// It was EBADF for both (CPython's socket(fileno=) of a file,
// test_socket_fileno_requires_socket_fd).
static int sock_fd_error(fd_t sock_fd) {
    return f_get(sock_fd) == NULL ? _EBADF : _ENOTSOCK;
}

static uint32_t unix_socket_next_id(void) {
    static uint32_t next_id = 0;
    static lock_t next_id_lock = LOCK_INITIALIZER;
    lock(&next_id_lock);
    uint32_t id = ++next_id;
    unlock(&next_id_lock);
    return id;
}

static int unix_socket_get(const char *path_raw, struct fd *bind_fd, uint32_t *socket_id) {
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat;
    err = mount->fs->stat(mount, path, &stat);

    // If bind was called, there are some funny semantics.
    if (bind_fd != NULL) {
        // If the file exists, fail.
        if (err == 0) {
            err = _EADDRINUSE;
            goto out;
        }
        // If the file can't be found, try to create it as a socket.
        if (err < 0) {
            mode_t_ mode = 0777;
            struct fs_info *fs = current->fs;
            lock(&fs->lock);
            mode &= ~fs->umask;
            unlock(&fs->lock);
            err = mount->fs->mknod(mount, path, S_IFSOCK | mode, 0);
            if (err < 0)
                goto out;
            // made here, not by generic_mknodat: its inotify event too
            inotify_child(mount, path, IN_CREATE_, 0);
            err = mount->fs->stat(mount, path, &stat);
            if (err < 0)
                goto out;
        }
    }

    // If something other than bind was called, just do the obvious thing and
    // fail if stat failed.
    if (bind_fd == NULL && err < 0)
        goto out;

    if (!S_ISSOCK(stat.mode)) {
        err = _ENOTSOCK;
        goto out;
    }

    // Look up the socket ID for the inode number.
    struct inode_data *inode = inode_get(mount, stat.inode);
    if (inode == NULL) {
        err = _ENOMEM;
        goto out;
    }
    lock(&inode->lock);
    if (inode->socket_id == 0)
        inode->socket_id = unix_socket_next_id();
    unlock(&inode->lock);
    *socket_id = inode->socket_id;

    mount_release(mount);
    if (bind_fd != NULL)
        bind_fd->socket.unix_name_inode = inode;
    else
        inode_release(inode);
    return 0;

out:
    mount_release(mount);
    return err;
}

// Dan Bernstein's simple and decently effective hash function
static uint32_t bytes_hash(const char *bytes, size_t len) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = 33 * hash ^ bytes[i];
    }
    return hash;
}

// The abstract socket namespace is a lot simpler than it sounds: if the first
// byte of the path is a null byte, then it gets looked up in this hashtable
// instead of the filesystem. As on Linux, a name is the bytes after that null
// byte up to the address length, null bytes included: it is not a C string,
// and two names are the same only byte for byte.

struct unix_abstract {
    unsigned refcount;
    uint32_t hash;
    uint32_t socket_id;
    struct list links;
    size_t name_len;
    char name[];
};
#define ABSTRACT_HASH_SIZE 1024
static struct list abstract_hash[ABSTRACT_HASH_SIZE];
static lock_t unix_abstract_lock = LOCK_INITIALIZER;

static int unix_abstract_get(const char *name, size_t name_len, struct fd *bind_fd, uint32_t *socket_id) {
    uint32_t hash = bytes_hash(name, name_len);
    lock(&unix_abstract_lock);
    struct unix_abstract *sock_tmp;
    struct unix_abstract *sock = NULL;
    struct list *bucket = &abstract_hash[hash % ABSTRACT_HASH_SIZE];
    if (list_null(bucket))
        list_init(bucket);
    list_for_each_entry(bucket, sock_tmp, links) {
        if (sock_tmp->hash == hash && sock_tmp->name_len == name_len &&
                memcmp(sock_tmp->name, name, name_len) == 0) {
            sock = sock_tmp;
            break;
        }
    }

    if (bind_fd != NULL && sock != NULL) {
        unlock(&unix_abstract_lock);
        return _EEXIST;
    }
    if (bind_fd == NULL && sock == NULL) {
        unlock(&unix_abstract_lock);
        return _ENOENT;
    }

    if (sock == NULL) {
        sock = malloc(sizeof(struct unix_abstract) + name_len);
        sock->refcount = 0;
        sock->hash = hash;
        sock->name_len = name_len;
        memcpy(sock->name, name, name_len);
        sock->socket_id = unix_socket_next_id();
        list_add(bucket, &sock->links);
    }

    // Only the bound socket holds the name: a connect or sendto that looks it
    // up takes the id and no reference, so the name is free again once that
    // socket has closed. (Lookups used to take one each, never released, and
    // after a connect the name could not be bound again: EADDRINUSE.)
    *socket_id = sock->socket_id;
    if (bind_fd != NULL) {
        sock->refcount++;
        bind_fd->socket.unix_name_abstract = sock;
    }
    unlock(&unix_abstract_lock);
    return 0;
}

static void unix_abstract_release(struct unix_abstract *name) {
    lock(&unix_abstract_lock);
    if (--name->refcount == 0) {
        list_remove(&name->links);
        free(name);
    }
    unlock(&unix_abstract_lock);
}

const char *sock_tmp_prefix = "/tmp/ishsock";

// For a unix address, *unix_socket_id (if not NULL) is set to the id behind
// the name.
static int sockaddr_read_bind(addr_t sockaddr_addr, void *sockaddr, uint_t *sockaddr_len, struct fd *bind_fd,
                              uint32_t *unix_socket_id) {
    // Make sure we can read things without overflowing buffers
    if (*sockaddr_len < 2)
        return _EINVAL;
    if (*sockaddr_len > sizeof(struct sockaddr_max_))
        return _EINVAL;

    if (user_read(sockaddr_addr, sockaddr, *sockaddr_len))
        return _EFAULT;
    struct sockaddr *real_addr = sockaddr;
    struct sockaddr_ *fake_addr = sockaddr;
    real_addr->sa_family = sock_family_to_real(fake_addr->family);

    switch (real_addr->sa_family) {
        case PF_INET:
            if (*sockaddr_len < sizeof(struct sockaddr_in))
                return _EINVAL;
            break;
        case PF_INET6:
            if (*sockaddr_len < sizeof(struct sockaddr_in6))
                return _EINVAL;
            break;

        case PF_LOCAL: {
            // First pull out the path, being careful to not overflow anything.
            char path[SOCKADDR_DATA_MAX + 1];
            size_t path_size = *sockaddr_len - offsetof(struct sockaddr_, data);
            memcpy(path, fake_addr->data, path_size);
            path[path_size] = '\0';

            uint32_t socket_id;
            int err;
            if (path_size == 0) {
                // No name to bind: autobind, as Linux's unix_autobind, to a
                // name in the abstract namespace, five hex digits after its
                // null byte, the first not taken (ENOSPC if none is). It was
                // ENOENT (CPython's test_socket testAutobind). No name to
                // connect to is EINVAL, as Linux's unix_validate_addr.
                if (bind_fd == NULL)
                    return _EINVAL;
                static _Atomic uint32_t autobind_next;
                err = _EEXIST;
                for (uint32_t tries = 0; tries <= 0xfffff && err == _EEXIST; tries++) {
                    path[0] = '\0';
                    path_size = 1 + sprintf(path + 1, "%05x", (unsigned) (atomic_fetch_add(&autobind_next, 1) & 0xfffff));
                    err = unix_abstract_get(path + 1, path_size - 1, bind_fd, &socket_id);
                }
                if (err == _EEXIST)
                    return _ENOSPC;
            } else if (path[0] != '\0') {
                STRACE(" unix socket %s", path);
                err = unix_socket_get(path, bind_fd, &socket_id);
            } else {
                STRACE(" unix abstract socket %s", path + 1);
                err = unix_abstract_get(path + 1, path_size - 1, bind_fd, &socket_id);
            }
            if (err < 0)
                return err;
            if (unix_socket_id != NULL)
                *unix_socket_id = socket_id;
            if (bind_fd != NULL) {
                bind_fd->socket.unix_name_len = path_size;
                memcpy(bind_fd->socket.unix_name, path, path_size);
            }

            struct sockaddr_un *real_addr_un = sockaddr;
            size_t path_len = sprintf(real_addr_un->sun_path, "%s%d.%u", sock_tmp_prefix, getpid(), socket_id);
            // The call to real bind will fail if the backing socket already
            // exists from a previous run or something. We already checked that
            // the fake file doesn't exist in unix_socket_get, so try a simple
            // solution.
            if (bind_fd != NULL)
                unlink(real_addr_un->sun_path);
            *sockaddr_len = offsetof(struct sockaddr_un, sun_path) + path_len;
            break;
        }
        default:
            return _EINVAL;
    }
    return 0;
}

static int sockaddr_read(addr_t sockaddr_addr, void *sockaddr, uint_t *sockaddr_len) {
    struct inode_data *inode = NULL;
    int err = sockaddr_read_bind(sockaddr_addr, sockaddr, sockaddr_len, NULL, NULL);
    inode_release_if_exist(inode);
    return err;
}

static int sockaddr_write(addr_t sockaddr_addr, void *sockaddr, uint_t buffer_len, uint_t *sockaddr_len) {
    // No address: the host gives none for a message from an unnamed unix
    // socket (a socketpair, a connection whose end was never bound). As on
    // Linux, the length is 0 and nothing is written.
    if (*sockaddr_len == 0)
        return 0;
    struct sockaddr *real_addr = sockaddr;
    struct sockaddr_ *fake_addr = sockaddr;
    fake_addr->family = sock_family_from_real(real_addr->sa_family);
    switch (fake_addr->family) {
        case PF_LOCAL_: {
            // Most callers of sockaddr_write use it to return a peer name, and
            // since we don't know the peer name in this case, just return the
            // default peer name, which is the null address.
            static struct sockaddr_ unix_domain_null = {.family = PF_LOCAL_};
            sockaddr = &unix_domain_null;
            *sockaddr_len = sizeof(unix_domain_null);
            break;
        }
        case PF_INET_:
        case PF_INET6_:
            break;
        default:
            return _EINVAL;
    }

    if (buffer_len > *sockaddr_len)
        buffer_len = *sockaddr_len;
    // The address is supposed to be truncated if the specified length is too
    // short, instead of returning an error.
    if (user_write(sockaddr_addr, sockaddr, buffer_len))
        return _EFAULT;
    return 0;
}

int_t sys_bind(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("bind(%d, 0x%x, %d)", sock_fd, sockaddr_addr, sockaddr_len);
    struct fd *packet = f_get(sock_fd);
    if (fd_is_packet(packet))
        return packet_bind(packet, sockaddr_addr, sockaddr_len);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    // netlink bind: accepted. nl_groups is remembered for the log only: no
    // event is ever multicast, so a subscribed reader simply waits (in
    // bounded polls, see netlink_stub_dequeue).
    if (fd_is_netlink_stub(sock)) {
        struct sockaddr_nl_ nl = {0};
        if (sockaddr_len >= sizeof(nl) && user_read(sockaddr_addr, &nl, sizeof(nl)) == 0)
            sock->socket.netlink_groups = nl.nl_groups;
        TRACE("NETLINK_STUB bind() accepted (nl_groups=%#x, no events are delivered)\n", sock->socket.netlink_groups);
        return 0;
    }
    struct sockaddr_max_ sockaddr;
    struct inode_data *inode = NULL;
    uint32_t unix_socket_id = 0;
    int err = sockaddr_read_bind(sockaddr_addr, &sockaddr, &sockaddr_len, sock, &unix_socket_id);
    if (err < 0)
        return err;

    err = bind(sock->real_fd, (void *) &sockaddr, sockaddr_len);
    if (err < 0) {
        // sockaddr_read_bind() already stashed the freshly-retained inode (and/or
        // abstract socket) into sock->socket on success. When the real bind() then
        // fails (e.g. AF_UNIX bind on the sandboxed /tmp returns EPERM), we must
        // release those references AND clear the pointers — otherwise the socket
        // fd's destructor releases them a second time on close(), corrupting the
        // libmalloc freelist ("BUG IN CLIENT OF LIBMALLOC: memory corruption of
        // free block"). Clearing here makes the cleanup idempotent.
        inode_release_if_exist(sock->socket.unix_name_inode);
        sock->socket.unix_name_inode = NULL;
        if (sock->socket.unix_name_abstract != NULL) {
            unix_abstract_release(sock->socket.unix_name_abstract);
            sock->socket.unix_name_abstract = NULL;
        }
        return errno_map();
    }
    // On success the inode reference set by sockaddr_read_bind() stays on the fd
    // and is released by the socket destructor. Do NOT overwrite it with the
    // always-NULL local `inode` (doing so leaked the reference on every bind).
    (void) inode;
    sock->socket.unix_socket_id = unix_socket_id;
    return 0;
}

// The current process's credentials. As on Linux, the pid is the process's,
// whichever thread asks.
static void fill_cred(struct ucred_ *cred) {
    cred->pid = current->tgid;
    cred->uid = current->euid;
    cred->gid = current->egid;
}

int_t sys_connect(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "connect");
    STRACE("connect(%d, 0x%x, %d)", sock_fd, sockaddr_addr, sockaddr_len);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    struct sockaddr_max_ sockaddr;
    uint32_t unix_socket_id = 0;
    int err = sockaddr_read_bind(sockaddr_addr, &sockaddr, &sockaddr_len, NULL, &unix_socket_id);
    if (err < 0)
        return err;

    err = connect(sock->real_fd, (void *) &sockaddr, sockaddr_len);
    if (netcap_on()) {
        int saved_errno = errno;
        netcap_connect(sock, (void *) &sockaddr, err < 0 ? saved_errno : 0);
        errno = saved_errno;
    }
    if (err < 0)
        return errno_map();

    // A datagram socket has no other end to accept it: it only names where
    // its datagrams go.
    if (sock->socket.domain == AF_LOCAL_ && sock->socket.type != SOCK_DGRAM_) {
        assert(sock->socket.unix_peer == NULL && sock->socket.unix_pending == NULL);
        struct unix_pending *pending = malloc(sizeof(struct unix_pending));
        if (pending == NULL)
            return _ENOMEM;
        list_init(&pending->scm);
        pending->client = sock;
        lock(&peer_lock);
        pending->id = unix_pending_next_id++;
        list_add(&unix_pending_list, &pending->links);
        sock->socket.unix_pending = pending;
        // As on Linux, the connecting end's peer credentials are the
        // listener's, from its listen(). sys_listen holds peer_lock until the
        // listener is on the list, so a connect that got through finds it,
        // unless it has closed since and the connection with it.
        struct fd *listener;
        list_for_each_entry(&unix_listeners, listener, socket.unix_listening) {
            if (listener->socket.unix_socket_id == unix_socket_id) {
                sock->socket.unix_peer_cred = listener->socket.unix_peer_cred;
                // and its name, getpeername's, as Linux's (the listener's
                // bound one, not the host's socket iSH backs it with)
                sock->socket.unix_peer_name_len = listener->socket.unix_name_len;
                memcpy(sock->socket.unix_peer_name, listener->socket.unix_name, listener->socket.unix_name_len);
                break;
            }
        }
        unlock(&peer_lock);
        struct unix_connect_header header = {.id = pending->id};
        fill_cred(&header.cred);
        ssize_t res = write(sock->real_fd, &header, sizeof(header));
        if (res != sizeof(header)) {
            printk("unix connect(pid=%d comm=%s): writing connection header %llu returned %zd (errno %d)\n",
                   current->pid, current->comm, (unsigned long long) header.id, res, errno);
            lock(&peer_lock);
            sock->socket.unix_pending = NULL;
            unix_pending_free(pending);
            unlock(&peer_lock);
            return _ECONNREFUSED;
        }
    }

    return err;
}

int_t sys_listen(fd_t sock_fd, int_t backlog) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "listen");
    STRACE("listen(%d, %d)", sock_fd, backlog);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    // A unix socket goes on the list of listeners, with the credentials a
    // connecting end takes (on Linux, set by every listen()), before anyone
    // can connect: see sys_connect.
    lock(&peer_lock);
    int err = listen(sock->real_fd, backlog);
    if (err < 0) {
        err = errno_map();
        unlock(&peer_lock);
        return err;
    }
    if (sock->socket.domain == AF_LOCAL_) {
        fill_cred(&sock->socket.unix_peer_cred);
        if (list_null(&sock->socket.unix_listening))
            list_add(&unix_listeners, &sock->socket.unix_listening);
    }
    unlock(&peer_lock);
    sockrestart_begin_listen(sock);
    return 0;
}

int_t sys_accept(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "accept");
    STRACE("accept(%d, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    dword_t sockaddr_len = 0;
    if (sockaddr_addr != 0) {
        if (user_get(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    }

    char sockaddr[sockaddr_len];
    int client;
    do {
        sockrestart_begin_listen_wait(sock);
        errno = 0;
        client = accept(sock->real_fd,
                sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                sockaddr_addr != 0 ? &sockaddr_len : NULL);
        sockrestart_end_listen_wait(sock);
    } while (sockrestart_should_restart_listen_wait() && errno == EINTR);
    if (client < 0)
        return errno_map();

    if (sockaddr_addr != 0) {
        // [T-ish-host-fd-double-close] Neither failure may leak the accepted
        // host socket, and the first used to hand the HOST fd number to the
        // guest as if it were the result.
        int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
        if (err >= 0 && user_put(sockaddr_len_addr, sockaddr_len))
            err = _EFAULT;
        if (err < 0) {
            close(client);
            return err;
        }
    }

    fd_t client_f = sock_fd_create(client,
            sock->socket.domain, sock->socket.type, sock->socket.protocol);
    // [T-ish-host-fd-double-close] client is closed already; going on used to
    // f_get() the negative number below and write through the NULL fd.
    if (client_f < 0)
        return client_f;
    if (netcap_on() && sock->socket.domain != AF_LOCAL_) {
        struct fd *client_fd = f_get(client_f);
        if (client_fd != NULL)
            netcap_accept(client_fd);
    }

    if (sock->socket.domain == AF_LOCAL_) {
        // The connecting end writes its header as soon as its connect is
        // done, maybe just after this accept returned: wait for it, even on a
        // non-blocking socket (Darwin's accept inherits O_NONBLOCK), so it is
        // never left for the program to read as data. It is there even when
        // that end has closed since.
        struct unix_connect_header header;
        size_t got = 0;
        int flags = fcntl(client, F_GETFL);
        fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
        while (got < sizeof(header)) {
            ssize_t res = read(client, (char *) &header + got, sizeof(header) - got);
            if (res < 0 && errno == EINTR)
                continue;
            if (res <= 0) {
                printk("unix accept(pid=%d comm=%s): reading connection header returned %zd (errno %d) after %zu bytes\n",
                       current->pid, current->comm, res, errno, got);
                break;
            }
            got += res;
        }
        fcntl(client, F_SETFL, flags);

        lock(&peer_lock);
        struct fd *client_fd = f_get(client_f);
        // As on Linux, the credentials of the process that connected, taken
        // by its connect
        if (got == sizeof(header))
            client_fd->socket.unix_peer_cred = header.cred;
        // As on Linux, the accepted socket has the listener's address, for
        // getsockname and /proc/net/unix.
        client_fd->socket.unix_name_len = sock->socket.unix_name_len;
        memcpy(client_fd->socket.unix_name, sock->socket.unix_name, sock->socket.unix_name_len);
        // None is found when the connecting end closed before this accept
        // without sending file descriptors (sock_close frees it then); with
        // some, it is kept for this socket to receive them.
        struct unix_pending *pending = NULL, *p;
        if (got == sizeof(header)) {
            list_for_each_entry(&unix_pending_list, p, links) {
                if (p->id == header.id) {
                    pending = p;
                    break;
                }
            }
        }
        if (pending != NULL) {
            struct fd *peer = pending->client;
            if (peer != NULL) {
                client_fd->socket.unix_peer = peer;
                peer->socket.unix_peer = client_fd;
                peer->socket.unix_pending = NULL;
                // its other end's name: the connecting end's own, if it
                // bound one
                client_fd->socket.unix_peer_name_len = peer->socket.unix_name_len;
                memcpy(client_fd->socket.unix_peer_name, peer->socket.unix_name, peer->socket.unix_name_len);
            }
            lock(&client_fd->lock);
            struct scm *scm, *tmp;
            list_for_each_entry_safe(&pending->scm, scm, tmp, queue) {
                list_remove(&scm->queue);
                list_add_tail(&client_fd->socket.unix_scm, &scm->queue);
            }
            unlock(&client_fd->lock);
            unix_pending_free(pending);
        }
        unlock(&peer_lock);
    }

    return client_f;
}

int_t sys_accept4(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr, int_t flags) {
    STRACE("accept4(%d, 0x%x, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr, flags);
    if (flags & ~(SOCK_NONBLOCK_ | SOCK_CLOEXEC_))
        return _EINVAL;
    int_t client_f = sys_accept(sock_fd, sockaddr_addr, sockaddr_len_addr);
    if (client_f < 0)
        return client_f;
    struct fd *client_fd = f_get(client_f);
    if (client_fd != NULL) {
        if (flags & SOCK_NONBLOCK_)
            fd_setflags(client_fd, O_NONBLOCK_);
        if (flags & SOCK_CLOEXEC_) {
            lock(&current->files->lock);
            bit_set(client_f, current->files->cloexec);
            unlock(&current->files->lock);
        }
    }
    return client_f;
}

// The address of a name into a buffer of *sockaddr_len bytes, cut to fit
// (zeros after it), and its whole length, as Linux says when it is cut (the
// caller sees it was). A buffer shorter than the family took its length less
// the family's for the name's room, below 0 and so huge: memset past it.
static void copy_unix_name_of(char *sockaddr, dword_t *sockaddr_len, const char *name, size_t name_len) {
    struct sockaddr_max_ full = {};
    struct sockaddr_ *fake_addr = (void *) &full;
    fake_addr->family = PF_LOCAL_;
    memcpy(fake_addr->data, name, name_len);
    size_t full_len = offsetof(struct sockaddr_, data) + name_len;
    memset(sockaddr, 0, *sockaddr_len);
    memcpy(sockaddr, &full, *sockaddr_len < full_len ? *sockaddr_len : full_len);
    *sockaddr_len = full_len;
}

static void copy_unix_name(char *sockaddr, dword_t *sockaddr_len, struct fd *sock) {
    copy_unix_name_of(sockaddr, sockaddr_len, sock->socket.unix_name, sock->socket.unix_name_len);
}

int_t sys_getsockname(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    STRACE("getsockname(%d, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr);
    struct fd *packet = f_get(sock_fd);
    if (fd_is_packet(packet))
        return packet_getsockname(packet, sockaddr_addr, sockaddr_len_addr);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    char sockaddr[sockaddr_len];

    // [STAGE-0 PROBE] netlink: report a sockaddr_nl. Go's netlinkrib calls
    // getsockname() to learn the pid the kernel assigned, then matches it
    // against nlmsg_pid on replies. Reporting the backing AF_UNIX address
    // here is what made it fail with EINVAL. pid=0 matches the replies the
    // stub emits (it echoes the request's pid, normally 0).
    if (fd_is_netlink_stub(sock)) {
        struct sockaddr_nl_ nl = { .nl_family = AF_NETLINK_, .nl_pad = 0,
                                   .nl_pid = 0, .nl_groups = 0 };
        dword_t out_len = sizeof(nl);
        if (sockaddr_len < out_len)
            out_len = sockaddr_len;
        if (user_write(sockaddr_addr, &nl, out_len))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, out_len))
            return _EFAULT;
        TRACE("NETLINK_STUB getsockname -> nl_pid=0\n");
        return 0;
    }

    // if this is a unix socket, return the same string passed to bind
    if (sock->socket.domain == PF_LOCAL_) {
        copy_unix_name(sockaddr, &sockaddr_len, sock);
        if (user_write(sockaddr_addr, sockaddr, sizeof(sockaddr)))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        return 0;
    }

    int res = getsockname(sock->real_fd, (void *) sockaddr, &sockaddr_len);
    if (res < 0)
        return errno_map();

    int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    return res;
}

int_t sys_getpeername(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "getpeername");
    STRACE("getpeername(%d, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;

    char sockaddr[sockaddr_len];
    dword_t buffer_len = sockaddr_len;
    int res = getpeername(sock->real_fd, (void *) sockaddr, &sockaddr_len);
    // A connection its peer reset or closed: Darwin says EINVAL ("the socket
    // has been shut down"), Linux ENOTCONN, which CPython's ssl takes for not
    // connected and so reads what came before the TLS handshake (EINVAL it
    // raised as it was: test_ssl's TestPreHandshakeClose)
    if (res < 0 && errno == EINVAL)
        return _ENOTCONN;
    if (res < 0)
        return errno_map();

    // A unix socket's other end is the name it bound, as Linux says (into
    // the guest's buffer: the host's call set its length to the host's): the
    // host's is the socket iSH backs it with (or none), and the guest got
    // its own sockaddr's bytes, zeros for an abstract name (CPython's
    // test_socket testLinuxAbstractNamespace). The host says whether it is
    // connected (ENOTCONN), above.
    if (sock->socket.domain == PF_LOCAL_) {
        sockaddr_len = buffer_len;
        lock(&peer_lock);
        copy_unix_name_of(sockaddr, &sockaddr_len, sock->socket.unix_peer_name, sock->socket.unix_peer_name_len);
        unlock(&peer_lock);
        if (user_write(sockaddr_addr, sockaddr, sizeof(sockaddr)))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        return 0;
    }

    int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    return res;
}

int_t sys_socketpair(dword_t domain, dword_t type, dword_t protocol, addr_t sockets_addr) {
    STRACE("socketpair(%d, %d, %d, 0x%x)", domain, type, protocol, sockets_addr);
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EAFNOSUPPORT;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return real_type;

    int sockets[2];
    int err = socketpair(real_domain, real_type, protocol, sockets);
    if (err < 0)
        return errno_map();

    // [T-ish-host-fd-double-close] Each host end is closed exactly once: by
    // sock_fd_create() when it fails, by sys_close() once it is installed,
    // and here only while it has no guest fd yet.
    // [T-ish-socketpair-emfile-deadlock] peer_lock is taken only to link the
    // two ends. It used to be held across sock_fd_create(), whose failure
    // path (f_install() -> fd_close() -> sock_close()) takes peer_lock again:
    // a socketpair() at RLIMIT_NOFILE hung its thread forever on a
    // non-recursive mutex, and with it every later unix socket call.
    int fake_sockets[2];
    err = fake_sockets[0] = sock_fd_create(sockets[0], domain, type, protocol);
    if (fake_sockets[0] < 0) {
        close(sockets[1]);
        return err;
    }
    err = fake_sockets[1] = sock_fd_create(sockets[1], domain, type, protocol);
    if (fake_sockets[1] < 0)
        goto close_fake_0;
    struct fd *sock1 = f_get_retained(fake_sockets[0]);
    struct fd *sock2 = f_get_retained(fake_sockets[1]);
    if (sock1 != NULL && sock2 != NULL) {
        lock(&peer_lock);
        sock1->socket.unix_peer = sock2;
        sock2->socket.unix_peer = sock1;
        // as on Linux, both ends have their creator's credentials
        fill_cred(&sock1->socket.unix_peer_cred);
        fill_cred(&sock2->socket.unix_peer_cred);
        unlock(&peer_lock);
    }
    if (sock1 != NULL)
        fd_close(sock1);
    if (sock2 != NULL)
        fd_close(sock2);

    err = _EFAULT;
    if (user_put(sockets_addr, fake_sockets))
        goto close_fake_1;

    STRACE(" [%d, %d]", fake_sockets[0], fake_sockets[1]);
    return 0;

close_fake_1:
    sys_close(fake_sockets[1]);
close_fake_0:
    sys_close(fake_sockets[0]);
    return err;
}

// Darwin's send takes no MSG_DONTWAIT: sosend asks only whether the socket
// is non-blocking, and a send with the flag to a socket that is not blocked
// when what it sent did not fit, till the other end went away (CPython's
// test_socket, testSendmsgDontWait: EPIPE, not EAGAIN). For the guest's flag
// the host socket is made non-blocking for that one send, if it is not (a
// Linux host, which takes the flag, the same). Another thread's send without
// the flag on the socket in that moment, or its F_SETFL, would find it
// non-blocking. The host's flags before, or -1 for none changed.
static int send_dontwait_begin(struct fd *sock, int guest_flags) {
    if (!(guest_flags & MSG_DONTWAIT_) || (sock->flags & O_NONBLOCK_))
        return -1;
    int host_flags = fcntl(sock->real_fd, F_GETFL);
    fcntl(sock->real_fd, F_SETFL, host_flags | O_NONBLOCK);
    return host_flags;
}

// The send's errno kept
static void send_dontwait_end(struct fd *sock, int host_flags) {
    if (host_flags < 0)
        return;
    int saved_errno = errno;
    fcntl(sock->real_fd, F_SETFL, host_flags);
    errno = saved_errno;
}

int_t sys_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "sendto");
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    char *buffer = malloc(len + 1);
    if (user_read(buffer_addr, buffer, len))
        return _EFAULT;
    buffer[len] = '\0';
    STRACE("sendto(%d, \"%.100s\", %d, %d, 0x%x, %d)", sock_fd, buffer, len, flags, sockaddr_addr, sockaddr_len);
    // A netlink request is answered as it is sent; the replies are queued
    // for the guest's following read.
    if (fd_is_netlink_stub(sock)) {
        int nl = netlink_stub_reply(sock, buffer, len);
        free(buffer);
        return nl;
    }
    int real_flags = sock_flags_to_real(flags);
    int err = _EINVAL;
    if (real_flags < 0)
        goto error;
    struct sockaddr_max_ sockaddr;
    if (sockaddr_addr) {
        err = sockaddr_read(sockaddr_addr, &sockaddr, &sockaddr_len);
        if (err < 0)
            goto error;
    }

    int host_flags = send_dontwait_begin(sock, flags);
    ssize_t res = sendto(sock->real_fd, buffer, len, real_flags,
            sockaddr_addr ? (void *) &sockaddr : NULL, sockaddr_len);
    send_dontwait_end(sock, host_flags);
    if (netcap_on()) {
        int saved_errno = errno;
        if (res >= 0)
            netcap_send(sock, &(struct iovec) {buffer, res}, 1, res, sockaddr_addr ? (void *) &sockaddr : NULL);
        else
            netcap_error(sock, saved_errno);
        errno = saved_errno;
    }
    free(buffer);
    if (res < 0)
        return errno_map();
    return res;

error:
    free(buffer);
    return err;
}

// Read a socket timeout option as milliseconds. Returns 0 if unset or error.
static long sock_get_timeout_ms(int real_fd, int optname) {
    struct timeval tv = {0, 0};
    socklen_t len = sizeof(tv);
    if (getsockopt(real_fd, SOL_SOCKET, optname, &tv, &len) < 0)
        return 0;
    return (long)tv.tv_sec * 1000L + (long)tv.tv_usec / 1000L;
}

static long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)ts.tv_nsec / 1000000L;
}

// Wait for a socket fd to become ready (readable or writable), with a
// bounded per-poll timeout so guest signals can interrupt even if the host
// thread is briefly suspended (iOS). Honors SO_RCVTIMEO / SO_SNDTIMEO as
// an overall deadline when set.
//
// Returns 0 when ready, _EINTR on guest signal, _EAGAIN on timeout expiry,
// or a negative errno on poll() error.
static int sock_wait_for(int real_fd, short events, int timeout_opt) {
    long timeout_ms = sock_get_timeout_ms(real_fd, timeout_opt);
    long deadline_ms = (timeout_ms > 0) ? (monotonic_ms() + timeout_ms) : 0;
    for (;;) {
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            bool pending = task_signal_pending(current);
            unlock(&current->sighand->lock);
            if (pending)
                return _EINTR;
        }

        int poll_ms = 1000;
        if (deadline_ms > 0) {
            long remaining = deadline_ms - monotonic_ms();
            if (remaining <= 0)
                return _EAGAIN; // SO_RCVTIMEO / SO_SNDTIMEO ran out
            if (remaining < poll_ms)
                poll_ms = (int)remaining;
        }

        struct pollfd pfd = { .fd = real_fd, .events = events };
        current->blocking = true;
        int pr = poll(&pfd, 1, poll_ms);
        int saved_errno = errno;
        current->blocking = false;

        if (pr > 0)
            return 0;
        if (pr == 0)
            continue;
        if (saved_errno == EINTR)
            continue;
        errno = saved_errno;
        printk("NETDIAG sock_wait(pid=%d comm=%s fd=%d ev=%#x) poll err=%d\n",
               current->pid, current->comm, real_fd, events, saved_errno);
        return errno_map();
    }
}

static int sock_wait_readable(int real_fd) {
    return sock_wait_for(real_fd, POLLIN, SO_RCVTIMEO);
}

// A receive that failed: a capture's RST if it was reset, then the guest's
// error. The buffer goes.
static int recv_failed(struct fd *sock, char *buffer) {
    int saved_errno = errno;
    free(buffer);
    if (netcap_on())
        netcap_error(sock, saved_errno);
    errno = saved_errno;
    return errno_map();
}

int_t sys_recvfrom(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "recvfrom");
    STRACE("recvfrom(%d, 0x%x, %d, %d, 0x%x, 0x%x)", sock_fd, buffer_addr, len, flags, sockaddr_addr, sockaddr_len_addr);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    // The error queue is always empty (see IP_RECVERR in setsockopt).
    if (flags & MSG_ERRQUEUE_)
        return _EAGAIN;
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
    uint_t sockaddr_len = 0;
    if (sockaddr_len_addr != 0)
        if (user_get(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;

    // recv()/recvfrom() on a netlink fd: the same bounded read as recvmsg,
    // and the peer is reported as a sockaddr_nl (the generic path would
    // hand back the host socketpair's AF_UNIX address).
    if (fd_is_netlink_stub(sock)) {
        char nlbuf[NETLINK_STUB_DGRAM_MAX * 2];
        ssize_t n = netlink_stub_dequeue(sock, flags, nlbuf, sizeof(nlbuf));
        if (n < 0)
            return (int) n;
        size_t copy = (size_t) n < sizeof(nlbuf) ? (size_t) n : sizeof(nlbuf);
        if (copy > len)
            copy = len;
        if (copy && user_write(buffer_addr, nlbuf, copy))
            return _EFAULT;
        if (sockaddr_addr != 0 && sockaddr_len_addr != 0) {
            struct sockaddr_nl_ nl = { .nl_family = AF_NETLINK_ };
            if (sockaddr_len >= sizeof(nl) && user_write(sockaddr_addr, &nl, sizeof(nl)))
                return _EFAULT;
            uint_t out_len = sizeof(nl);
            if (user_put(sockaddr_len_addr, out_len))
                return _EFAULT;
        }
        return (flags & MSG_TRUNC_) ? (int) n : (int) copy;
    }

    char *buffer = malloc(len);
    char sockaddr[sockaddr_len];

    // Determine whether this call should wait for data:
    //   * guest flags MSG_DONTWAIT → never wait (explicit)
    //   * fd has O_NONBLOCK set   → never wait (fd-level nonblock)
    //
    // The second case is critical: musl's res_msend creates UDP sockets
    // with SOCK_NONBLOCK and then calls recv(fd, buf, len, 0). It relies
    // on the kernel returning EAGAIN immediately on empty buffer to
    // terminate its "drain all pending responses" loop. If we wait when
    // the fd itself is nonblock, musl loops forever and DNS resolution
    // appears to hang. We check both the iSH-level fd flags and the host
    // fd's real flags, in case they diverge (e.g. set via fcntl only).
    int host_flags = fcntl(sock->real_fd, F_GETFL, 0);
    bool host_nonblock = (host_flags >= 0) && (host_flags & O_NONBLOCK);
    bool guest_nonblock = (sock->flags & O_NONBLOCK_) != 0;
    bool should_wait = !(real_flags & MSG_DONTWAIT) && !guest_nonblock && !host_nonblock;

    // Try a nonblock recvfrom first (fast path — no host syscall overhead
    // when data is already buffered). Only if it returns EAGAIN and we
    // should wait do we fall back to the bounded-poll wait.
    ssize_t res;
    if (should_wait) {
        for (;;) {
            res = recvfrom(sock->real_fd, buffer, len, real_flags | MSG_DONTWAIT,
                    sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                    sockaddr_len_addr != 0 ? &sockaddr_len : NULL);
            if (res >= 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return recv_failed(sock, buffer);
            // EAGAIN: no data yet. Wait (bounded) for readability or a
            // guest signal, then retry.
            int wr = sock_wait_readable(sock->real_fd);
            if (wr < 0) {
                free(buffer);
                return wr;
            }
        }
    } else {
        res = recvfrom(sock->real_fd, buffer, len, real_flags,
                sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                sockaddr_len_addr != 0 ? &sockaddr_len : NULL);
        if (res < 0)
            return recv_failed(sock, buffer);
    }

    // what was received: not a peek, which leaves it there
    if (netcap_on() && !(flags & MSG_PEEK_))
        netcap_recv(sock, &(struct iovec) {buffer, (size_t) res < len ? (size_t) res : len}, 1,
                    (size_t) res < len ? (size_t) res : len, sockaddr_addr != 0 ? (void *) sockaddr : NULL);
    if (user_write(buffer_addr, buffer, len)) {
        free(buffer);
        return _EFAULT;
    }
    free(buffer);
    if (sockaddr_addr != 0) {
        int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
        if (err < 0)
            return err;
    }
    if (sockaddr_len_addr != 0)
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    return res;
}

int_t sys_send(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_sendto(sock_fd, buf, len, flags, 0, 0);
}

int_t sys_recv(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_recvfrom(sock_fd, buf, len, flags, 0, 0);
}

int_t sys_shutdown(fd_t sock_fd, dword_t how) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "shutdown");
    STRACE("shutdown(%d, %d)", sock_fd, how);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    int err = shutdown(sock->real_fd, how);
    if (err < 0)
        return errno_map();
    if (netcap_on())
        netcap_shutdown(sock, how);
    return 0;
}

#define DEFAULT_TCP_CONGESTION "cubic"

// A socket option iSH does not know fails with ENOPROTOOPT, as Linux fails one
// it does not know at that level: there is no telling what setting it would
// have meant. Programs take that for "not supported", and only that: the JDK
// probes TCP_QUICKACK and SO_INCOMING_NAPI_ID with getsockopt at its start and
// takes any other error for supported, so with EINVAL it offered both and
// setting TCP_QUICKACK failed with "Invalid argument". Said once per call,
// level and option, with what was asked, to add it from (telnet's SO_OOBINLINE
// failed with nothing but its own message on its screen).
static void sock_opt_unknown(const char *call, struct fd *sock, dword_t level, dword_t option,
                             const char *value, dword_t value_len) {
    static lock_t seen_lock = LOCK_INITIALIZER;
    static uint64_t seen[64];
    static unsigned seen_count;
    // setsockopt's has the value, which getsockopt's, said first, would hide
    uint64_t key = (uint64_t) level << 33 | (uint64_t) option << 1 | (call[0] == 's');
    lock(&seen_lock);
    bool first = true;
    for (unsigned i = 0; i < seen_count; i++) {
        if (seen[i] == key)
            first = false;
    }
    if (first && seen_count < sizeof(seen) / sizeof(seen[0]))
        seen[seen_count++] = key;
    unlock(&seen_lock);
    if (!first)
        return;
    char hex[3 * 32 + 1] = "";
    for (dword_t i = 0; i < value_len && i < 32; i++)
        sprintf(hex + 3 * i, "%02x ", (unsigned char) value[i]);
    printk("%s[%d]: %s level %u option %u (domain %d type %d) unknown, ENOPROTOOPT; value (%u bytes): %s\n",
           current->comm, current->pid, call, level, option, sock->socket.domain, sock->socket.type,
           value_len, hex);
}

int_t sys_setsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t value_len) {
    STRACE("setsockopt(%d, %d, %d, 0x%x, %d)", sock_fd, level, option, value_addr, value_len);
    struct fd *packet = f_get(sock_fd);
    if (fd_is_packet(packet)) {
        char packet_value[256];
        uint_t packet_len = value_len < sizeof(packet_value) ? value_len : sizeof(packet_value);
        if (user_read(value_addr, packet_value, packet_len))
            return _EFAULT;
        return packet_setsockopt(packet, level, option, packet_value, packet_len);
    }
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    char value[value_len];
    if (user_read(value_addr, value, value_len))
        return _EFAULT;

    // A netlink fd is a host socketpair: buffer sizes and SOL_NETLINK
    // options (iproute2 sets SO_SNDBUF/SO_RCVBUF and gives up if they fail)
    // mean nothing to it, so they are accepted and dropped. The receive
    // timeout is kept, for the bounded read to honour (netlink_stub_dequeue).
    // Both of Linux's numbers for it are taken: 20 is what a 64-bit guest's
    // libc uses, 66 the newer 64-bit-time one.
    if (fd_is_netlink_stub(sock)) {
        if (level == SOL_SOCKET_ && (option == SO_RCVTIMEO_OLD_ || option == SO_RCVTIMEO_) && value_len >= 16) {
            int64_t sec, usec;
            memcpy(&sec, value, 8);
            memcpy(&usec, value + 8, 8);
            if (sec < 0 || usec < 0 || usec >= 1000000)
                return _EDOM;
            sock->socket.netlink_rcvtimeo_ms = (long) (sec * 1000 + usec / 1000);
            // A timeout below a millisecond is still a timeout.
            if (sock->socket.netlink_rcvtimeo_ms == 0 && (sec != 0 || usec != 0))
                sock->socket.netlink_rcvtimeo_ms = 1;
        }
        return 0;
    }

    // ICMP6_FILTER can only be set on real SOCK_RAW
    if (level == IPPROTO_ICMPV6 && option == ICMP6_FILTER_)
        return 0;
    // IP_MTU_DISCOVER has no equivalent on Darwin
    if (level == IPPROTO_IP && option == IP_MTU_DISCOVER_)
        return 0;
    // Nor do IP_RECVERR / IPV6_RECVERR (the extended error queue). libuv sets
    // them on every UDP socket and gives up on the socket when they fail, so
    // bind 9.20's dig, host and nslookup could not send a query. Accepted,
    // the error queue stays empty (recv with MSG_ERRQUEUE says EAGAIN), and
    // an ICMP error on a connected socket still shows on its next send or
    // recv, as without the option.
    if ((level == IPPROTO_IP && option == IP_RECVERR_) ||
            (level == IPPROTO_IPV6 && option == IPV6_RECVERR_))
        return 0;
    // IP_MULTICAST_ALL 0: a socket gets the datagrams of only the groups it
    // joined itself, not of every group some socket on the host joined. That
    // is how Darwin delivers them, so 0 is accepted as it stands; 1 has no
    // Darwin equivalent and stays unknown below. Java clears it on every UDP
    // socket and gives up on the socket when that fails, so no DatagramSocket
    // could be made: Gradle died at its start, its file-lock listener being
    // one. On an IPv6 socket Java passes the same number at the IPv6 level,
    // which is Linux's IPV6_RECVPKTINFO, and 0 is that option's default; and
    // then IPV6_MULTICAST_ALL 0, IP_MULTICAST_ALL's for IPv6 (JDK 25:
    // IPV6_V6ONLY, 49, IPV6_MULTICAST_HOPS 1, IPV6_MULTICAST_ALL 0).
    static const int32_t off = 0;
    if (((level == IPPROTO_IP && option == IP_MULTICAST_ALL_) ||
            (level == IPPROTO_IPV6 && (option == IPV6_RECVPKTINFO_ || option == IPV6_MULTICAST_ALL_))) &&
            value_len == sizeof(off) && memcmp(value, &off, sizeof(off)) == 0)
        return 0;
    // [STAGE-0 PROBE] SO_BINDTODEVICE: bind this socket to a named interface.
    // iSH has no interface model at all (/proc/net/dev is hardcoded dummy
    // data), and Darwin's nearest equivalent is IP_BOUND_IF, which takes an
    // ifindex rather than a name. Go's netns code sets this on every outbound
    // dial, so rejecting it makes ALL dials fail with EINVAL. Accept and
    // ignore: with a single host network path there is nothing to choose
    // between, so unbound behaviour is what the caller wanted anyway.
    if (level == SOL_SOCKET_ && option == SO_BINDTODEVICE_) {
        // Prefix is deliberately NOT "NETLINK_STUB": this has nothing to do
        // with netlink and is NOT covered by ISH_NETLINK_STUB, so the old
        // name implied that disabling the stub would disable this too.
        // TRACE, not printk: Go sets this on EVERY outbound dial, and the
        // unconditional form produced ~512KB of log in a single proxy test.
        TRACE("SOCKOPT SO_BINDTODEVICE ignored (no iface model)\n");
        return 0;
    }
    // TCP_CONGESTION also has no equivalent on Darwin
#if defined(__APPLE__)
    if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        if (strncmp(value, DEFAULT_TCP_CONGESTION, sizeof(value)) == 0)
            return 0;
        return _ENOENT;
    }
#endif

    int real_opt = sock_opt_to_real(option, level);
    if (real_opt < 0) {
        sock_opt_unknown("setsockopt", sock, level, option, value, value_len);
        return _ENOPROTOOPT;
    }
    int real_level = sock_level_to_real(level);
    if (real_level < 0)
        return _EINVAL;

    // 0 means the option is not implemented, but things rely on it, so we
    // should just ignore attempts to set it.
    if (real_opt == 0)
        return 0;

    int err = setsockopt(sock->real_fd, real_level, real_opt, value, value_len);
    if (err < 0)
        return errno_map();
    return 0;
}

#if defined(__APPLE__)
// Darwin's TCPS_* connection states as Linux's TCP_*. They are listed in
// netinet/tcp_fsm.h, which only macOS has, not iOS. -1 for anything else.
static int tcp_state_from_darwin(unsigned state) {
    switch (state) {
        case 0: return 7;   // CLOSED -> CLOSE
        case 1: return 10;  // LISTEN
        case 2: return 2;   // SYN_SENT
        case 3: return 3;   // SYN_RECEIVED -> SYN_RECV
        case 4: return 1;   // ESTABLISHED
        case 5: return 8;   // CLOSE_WAIT
        case 6: return 4;   // FIN_WAIT_1 -> FIN_WAIT1
        case 7: return 11;  // CLOSING
        case 8: return 9;   // LAST_ACK
        case 9: return 5;   // FIN_WAIT_2 -> FIN_WAIT2
        case 10: return 6;  // TIME_WAIT
    }
    return -1;
}
#endif

int_t sys_getsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, addr_t len_addr) {
    STRACE("getsockopt(%d, %d, %d, %#x, %#x)", sock_fd, level, option, value_addr, len_addr);
    struct fd *packet = f_get(sock_fd);
    if (fd_is_packet(packet)) {
        uint_t packet_len;
        if (user_get(len_addr, packet_len))
            return _EFAULT;
        char packet_value[256] = {0};
        if (packet_len > sizeof(packet_value))
            packet_len = sizeof(packet_value);
        if (user_read(value_addr, packet_value, packet_len))
            return _EFAULT;
        int_t err = packet_getsockopt(packet, level, option, packet_value, &packet_len);
        if (err < 0)
            return err;
        if (user_write(value_addr, packet_value, packet_len) || user_put(len_addr, packet_len))
            return _EFAULT;
        return 0;
    }
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    dword_t value_len;
    if (user_get(len_addr, value_len))
        return _EFAULT;
    char value[value_len];
    if (user_read(value_addr, value, value_len))
        return _EFAULT;

    if (level == SOL_SOCKET_ && (option == SO_DOMAIN_ || option == SO_TYPE_ || option == SO_PROTOCOL_)) {
        dword_t *value_p = (dword_t *) value;
        if (value_len != sizeof(*value_p))
            return _EINVAL;
        if (option == SO_DOMAIN_)
            *value_p = sock->socket.domain;
        else if (option == SO_TYPE_)
            *value_p = sock->socket.type;
        else if (option == SO_PROTOCOL_)
            *value_p = sock->socket.protocol;
    } else if (level == SOL_SOCKET_ && option == SO_ACCEPTCONN_) {
        // Whether it listens, as Linux says of any socket. It was ENOPROTOOPT:
        // iSH's Linux CLI, run in iSH, asks it of the host for /proc/net/tcp,
        // and a server handed its socket (inetd, socket activation) asks it.
        // iSH's own word for it, sys_listen's: Darwin's getsockopt does not
        // know SO_ACCEPTCONN (ENOPROTOOPT of a TCP or UDP socket on iOS), and
        // said no of every unix socket.
        if (value_len != sizeof(dword_t))
            return _EINVAL;
        *(dword_t *) value = sockrestart_listening(sock);
    } else if (level == SOL_SOCKET_ && option == SO_PEERCRED_) {
        struct ucred_ *cred = (struct ucred_ *) value;
        if (value_len != sizeof(*cred))
            return _EINVAL;
        lock(&peer_lock);
        *cred = sock->socket.unix_peer_cred;
        unlock(&peer_lock);
    } else if (level == SOL_SOCKET_ && option == SO_ERROR_) {
        if (value_len != sizeof(dword_t))
            return _EINVAL;
        int real_error;
        socklen_t real_error_len = sizeof(real_error);
        int err = getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &real_error, &real_error_len);
        if (err < 0)
            return errno_map();
        *(dword_t *) value = real_error == 0 ? 0 : -err_map(real_error);
    } else if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        value_len = strlen(DEFAULT_TCP_CONGESTION);
        memcpy(value, DEFAULT_TCP_CONGESTION, value_len);
#if defined(__APPLE__)
    } else if (level == IPPROTO_TCP && option == TCP_INFO_) {
        // This one's fun. On Linux, the struct is not ABI dependent, so no
        // special handling is needed. On Darwin, the struct is completely
        // different and has a different sockopt name.
        struct tcp_connection_info conn_info;
        socklen_t conn_info_size = sizeof(conn_info);
        int err = getsockopt(sock->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &conn_info, &conn_info_size);
        if (err < 0)
            return errno_map();

        int state = tcp_state_from_darwin(conn_info.tcpi_state);
        if (state < 0) {
            printk("getsockopt(TCP_INFO): host reports unknown TCP state %u\n", conn_info.tcpi_state);
            return _EIO;
        }
        struct tcp_info_ info = {
            .state = state,
            .options = conn_info.tcpi_options,
            .snd_wscale = conn_info.tcpi_snd_wscale,
            .rcv_wscale = conn_info.tcpi_rcv_wscale,

            .rto = conn_info.tcpi_rto * 1000,
            .snd_mss = conn_info.tcpi_maxseg,

            .rtt = conn_info.tcpi_srtt * 1000,
            .rttvar = conn_info.tcpi_rttvar * 1000,
            .snd_ssthresh = conn_info.tcpi_snd_ssthresh,
            .snd_cwnd = conn_info.tcpi_snd_cwnd / conn_info.tcpi_maxseg,

            // https://lkml.org/lkml/2017/4/24/923
            .total_retrans = conn_info.tcpi_txretransmitpackets,
        };
        if (value_len > sizeof(struct tcp_info_))
            value_len = sizeof(struct tcp_info_);
        memcpy(value, &info, value_len);
#endif
    } else {
        int real_opt = sock_opt_to_real(option, level);
        if (real_opt < 0) {
            sock_opt_unknown("getsockopt", sock, level, option, NULL, 0);
            return _ENOPROTOOPT;
        }
        int real_level = sock_level_to_real(level);
        if (real_level < 0)
            return _EINVAL;

        int err = getsockopt(sock->real_fd, real_level, real_opt, value, &value_len);
        if (err < 0)
            return errno_map();
    }

    if (user_put(len_addr, value_len))
        return _EFAULT;
    if (user_put(value_addr, value))
        return _EFAULT;
    return 0;
}

static void scm_free(struct scm *scm) {
    for (unsigned i = 0; i < scm->num_fds; i++)
        fd_close(scm->fds[i]);
    free(scm);
}

// The control messages of an IP or IPv6 socket, Linux's and the host's:
// IPv6's hop limit, traffic class and packet info (RFC 3542), IPv4's TTL, TOS
// and packet info, and a receive's SO_TIMESTAMP. Their numbers differ, and
// the header's size and alignment (XNU's 12 bytes and 4, Linux's 16 and 8).
// They were dropped: a receive gave none (iputils' ping says each reply's
// TTL from IP_TTL; CPython's test_socket, its RFC 3542 tests), and a send's
// were not sent.

// A received one into the guest's buffer as Linux's put_cmsg puts one: whole,
// or as much as fits past its header, which says that much (MSG_CTRUNC), or
// none without room for a header (MSG_CTRUNC); the next at its aligned end.
struct guest_cmsgs {
    addr_t at;
    size_t room;
    size_t used;
    bool truncated;
};

static int guest_cmsg_put(struct guest_cmsgs *out, int level, int type, const void *data, size_t len) {
    size_t left = out->room - out->used;
    size_t cmlen = sizeof(struct cmsghdr_) + len;
    if (left < sizeof(struct cmsghdr_)) {
        out->truncated = true;
        return 0;
    }
    if (left < cmlen) {
        out->truncated = true;
        cmlen = left;
    }
    struct cmsghdr_ header = {.len = cmlen, .level = level, .type = type};
    if (user_write(out->at + out->used, &header, sizeof(header)))
        return _EFAULT;
    if (cmlen > sizeof(header) && user_write(out->at + out->used + sizeof(header), data, cmlen - sizeof(header)))
        return _EFAULT;
    size_t space = CMSG_SPACE_(len);
    out->used += space < left ? space : left;
    return 0;
}

// sizes Linux and XNU agree on: struct in_pktinfo (ifindex, spec_dst,
// addr) and struct in6_pktinfo (addr, ifindex)
#define IN_PKTINFO_SIZE 12
#define IN6_PKTINFO_SIZE 20

static int host_cmsgs_to_guest(struct msghdr *msg, struct guest_cmsgs *out) {
    // each at the aligned end of the last, as CMSG_NXTHDR (whose musl macro
    // compares signed and unsigned)
    uint8_t *at = msg->msg_control, *end = at + msg->msg_controllen;
    for (struct cmsghdr *c; at + sizeof(struct cmsghdr) <= end; at += CMSG_SPACE(c->cmsg_len - CMSG_LEN(0))) {
        c = (void *) at;
        if (c->cmsg_len < CMSG_LEN(0) || at + c->cmsg_len > end)
            break;
        const uint8_t *data = CMSG_DATA(c);
        size_t len = c->cmsg_len - CMSG_LEN(0);
        int err = 0;
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == HOST_IPV6_HOPLIMIT) {
            err = guest_cmsg_put(out, IPPROTO_IPV6, IPV6_HOPLIMIT_, data, sizeof(int));
        } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == HOST_IPV6_TCLASS) {
            err = guest_cmsg_put(out, IPPROTO_IPV6, IPV6_TCLASS_, data, sizeof(int));
        } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == HOST_IPV6_PKTINFO) {
            err = guest_cmsg_put(out, IPPROTO_IPV6, IPV6_PKTINFO_, data, IN6_PKTINFO_SIZE);
        } else if (c->cmsg_level == IPPROTO_IP && (c->cmsg_type == IP_TTL || c->cmsg_type == IP_RECVTTL)) {
            // an int, as Linux's: XNU's (IP_RECVTTL) is a byte
            int ttl = len == 1 ? data[0] : *(const int *) data;
            err = guest_cmsg_put(out, IPPROTO_IP, IP_TTL_, &ttl, sizeof(ttl));
        } else if (c->cmsg_level == IPPROTO_IP && (c->cmsg_type == IP_TOS || c->cmsg_type == IP_RECVTOS)) {
            uint8_t tos = data[0];
            err = guest_cmsg_put(out, IPPROTO_IP, IP_TOS_, &tos, sizeof(tos));
        } else if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            err = guest_cmsg_put(out, IPPROTO_IP, IP_PKTINFO_, data, IN_PKTINFO_SIZE);
        } else if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP) {
            // the guest's timeval, two 64-bit numbers (XNU's microseconds
            // are 32 bits)
            struct timeval tv;
            memcpy(&tv, data, sizeof(tv));
            int64_t guest_tv[2] = {tv.tv_sec, tv.tv_usec};
            err = guest_cmsg_put(out, SOL_SOCKET_, SO_TIMESTAMP_, guest_tv, sizeof(guest_tv));
        } else {
            static _Atomic bool said;
            if (!atomic_exchange(&said, true))
                printk("%s[%d]: recvmsg: the host's control message, level %d type %d (%zu bytes), unknown: "
                       "dropped\n", current->comm, current->pid, c->cmsg_level, c->cmsg_type, len);
        }
        if (err < 0)
            return err;
    }
    return 0;
}

// A send's, the guest's into the host's buffer: those above, and IPv4's TTL
// and TOS (the host's to take or refuse). Another, or one whose length does
// not fit the buffer, is EINVAL, as Linux says of one it has not; a buffer
// past the host's room ENOBUFS.
static int guest_cmsgs_to_host(uint8_t *guest, size_t guest_len, char *host, size_t host_room, struct msghdr *msg) {
    uint8_t *end = guest + guest_len;
    size_t used = 0;
    for (struct cmsghdr_ *g = (void *) guest; g != NULL; g = CMSG_NXTHDR_(g, end)) {
        if (g->len < sizeof(struct cmsghdr_) || (uint8_t *) g + g->len > end)
            return _EINVAL;
        size_t len = g->len - sizeof(struct cmsghdr_);
        int level = g->level, type;
        if (g->level == IPPROTO_IPV6 && g->type == IPV6_HOPLIMIT_)
            type = HOST_IPV6_HOPLIMIT;
        else if (g->level == IPPROTO_IPV6 && g->type == IPV6_TCLASS_)
            type = HOST_IPV6_TCLASS;
        else if (g->level == IPPROTO_IPV6 && g->type == IPV6_PKTINFO_)
            type = HOST_IPV6_PKTINFO;
        else if (g->level == IPPROTO_IP && g->type == IP_PKTINFO_)
            type = IP_PKTINFO;
        else if (g->level == IPPROTO_IP && g->type == IP_TTL_)
            type = IP_TTL;
        else if (g->level == IPPROTO_IP && g->type == IP_TOS_)
            type = IP_TOS;
        else {
            static _Atomic bool said;
            if (!atomic_exchange(&said, true))
                printk("%s[%d]: sendmsg: control message level %d type %d (%zu bytes) unknown, EINVAL\n",
                       current->comm, current->pid, g->level, g->type, len);
            return _EINVAL;
        }
        if (used + CMSG_SPACE(len) > host_room)
            return _ENOBUFS;
        struct cmsghdr *h = (void *) (host + used);
        h->cmsg_len = CMSG_LEN(len);
        h->cmsg_level = level;
        h->cmsg_type = type;
        memcpy(CMSG_DATA(h), g->data, len);
        used += CMSG_SPACE(len);
    }
    msg->msg_control = host;
    msg->msg_controllen = used;
    return 0;
}

int_t sys_sendmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "sendmsg");
    int err;
    STRACE("sendmsg(%d, %#x, %d)", sock_fd, msghdr_addr, flags);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    if (fd_is_netlink_stub(sock))
        return netlink_stub_sendmsg(sock, msghdr_addr);

    // Read the guest msghdr struct into our internal 32-bit representation
    struct msghdr msg;
    struct msghdr_ msg_fake;
    struct msghdr64_ msg_fake64;
    if (user_get(msghdr_addr, msg_fake64))
        return _EFAULT;
    msg_fake.msg_name = (addr_t)msg_fake64.msg_name;
    msg_fake.msg_namelen = msg_fake64.msg_namelen;
    msg_fake.msg_iov = (addr_t)msg_fake64.msg_iov;
    msg_fake.msg_iovlen = (uint_t)msg_fake64.msg_iovlen;
    msg_fake.msg_control = (addr_t)msg_fake64.msg_control;
    msg_fake.msg_controllen = (uint_t)msg_fake64.msg_controllen;
    msg_fake.msg_flags = msg_fake64.msg_flags;

    // msg_name
    struct sockaddr_max_ msg_name;
    if (msg_fake.msg_name != 0) {
        int err = sockaddr_read(msg_fake.msg_name, &msg_name, &msg_fake.msg_namelen);
        if (err < 0)
            return err;
        msg.msg_name = &msg_name;
        msg.msg_namelen = msg_fake.msg_namelen;
    } else {
        msg.msg_name = NULL;
    }

    // msg_iovec
    struct iovec64_ msg_iov_fake64[msg_fake.msg_iovlen];
    if (user_read(msg_fake.msg_iov, msg_iov_fake64, sizeof(msg_iov_fake64)))
        return _EFAULT;
    struct iovec msg_iov[msg_fake.msg_iovlen];
    memset(msg_iov, 0, sizeof(msg_iov));
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = msg_fake.msg_iovlen;
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov[i].iov_len = (size_t)msg_iov_fake64[i].len;
        msg_iov[i].iov_base = malloc(msg_iov[i].iov_len);
        err = _EFAULT;
        if (user_read((addr_t)msg_iov_fake64[i].base, msg_iov[i].iov_base, msg_iov[i].iov_len))
            goto out_free_iov;
    }

    // msg_control
    uint8_t msg_control_buf[2048];
    uint8_t *msg_control = NULL;
    if (msg_fake.msg_control != 0) {
        if (msg_fake.msg_controllen > sizeof(msg_control_buf)) {
            err = _EINVAL;
            goto out_free_iov;
        }
        msg_control = msg_control_buf;
        err = _EFAULT;
        if (user_read(msg_fake.msg_control, msg_control, msg_fake.msg_controllen))
            goto out_free_iov;
    }
    msg.msg_control = NULL;
    msg.msg_controllen = 0;

    struct scm *scm = NULL;
    char real_msg_control[CMSG_SPACE(sizeof(int))]; // only used if actually sending an fd
    if (sock->socket.domain == AF_LOCAL_ && msg_control != NULL && msg_fake.msg_controllen >= sizeof(struct cmsghdr_)) {
        // figure out how many file descriptors we're sending
        uint8_t *mhdr_end = msg_control + msg_fake.msg_controllen;
        unsigned num_fds = 0;
        struct cmsghdr_ *cmsg;
        for (cmsg = (void *) msg_control; cmsg != NULL; cmsg = CMSG_NXTHDR_(cmsg, mhdr_end)) {
            if (cmsg->level != SOL_SOCKET_)
                continue;
            // EINVAL by way of the cleanup: returned at once, the iovec's
            // buffers were never freed
            err = _EINVAL;
            if (cmsg->type != SCM_RIGHTS_)
                goto out_free_iov;
            num_fds += (cmsg->len - sizeof(struct cmsghdr_)) / sizeof(fd_t);
        }
        err = _EINVAL;
        if (num_fds > 253) // *magic*
            goto out_free_iov;

        if (num_fds > 0) {
            // send one (1) real fd and put the rest in a struct scm
            static int real_fd = -1;
            if (real_fd == -1) {
                real_fd = open(".", O_RDONLY);
                if (real_fd < 0)
                    ERRNO_DIE("no");
            }
            msg.msg_control = real_msg_control;
            msg.msg_controllen = sizeof(real_msg_control);
            struct cmsghdr *real_cmsg = CMSG_FIRSTHDR(&msg);
            real_cmsg->cmsg_level = SOL_SOCKET;
            real_cmsg->cmsg_type = SCM_RIGHTS;
            real_cmsg->cmsg_len = CMSG_LEN(sizeof(real_fd));
            memcpy(CMSG_DATA(real_cmsg), &real_fd, sizeof(real_fd));

            scm = malloc(sizeof(struct scm) + num_fds * sizeof(struct fd *));
            list_init(&scm->queue);
            scm->num_fds = num_fds;
            unsigned fd_i = 0;
            for (cmsg = (void *) msg_control; cmsg != NULL; cmsg = CMSG_NXTHDR_(cmsg, mhdr_end)) {
                if (cmsg->level != SOL_SOCKET_)
                    continue;
                fd_t *fds = (void *) cmsg->data;
                for (unsigned i = 0; i < (cmsg->len - sizeof(struct cmsghdr_)) / sizeof(fd_t); i++) {
                    STRACE(" sending fd %d", fds[i]);
                    // An fd that is not is EBADF, nothing sent, as Linux's
                    // scm_fp_copy. It was retained through NULL, and iSH
                    // crashed (CPython's test_socket,
                    // test_sendmsg_reentrant_ancillary_mutation).
                    struct fd *passed = f_get(fds[i]);
                    if (passed == NULL) {
                        scm->num_fds = fd_i;
                        scm_free(scm);
                        err = _EBADF;
                        goto out_free_iov;
                    }
                    scm->fds[fd_i++] = fd_retain(passed);
                }
            }
            lock(&peer_lock);
            struct fd *peer = sock->socket.unix_peer;
            if (peer != NULL) {
                lock(&peer->lock);
                list_add_tail(&peer->socket.unix_scm, &scm->queue);
                unlock(&peer->lock);
            } else if (sock->socket.unix_pending != NULL) {
                // not accepted yet: accept hands them to the accepted socket
                list_add_tail(&sock->socket.unix_pending->scm, &scm->queue);
            } else {
                unlock(&peer_lock);
                err = _EPIPE;
                goto out_free_scm;
            }
            unlock(&peer_lock);
        }
    }

    // an IP or IPv6 socket's control messages, the host's (above): they
    // were not sent. A buffer short of a header has none, as Linux's.
    char host_control[512] __attribute__((aligned(8)));
    if (sock->socket.domain != AF_LOCAL_ && msg_control != NULL &&
            msg_fake.msg_controllen >= sizeof(struct cmsghdr_)) {
        err = guest_cmsgs_to_host(msg_control, msg_fake.msg_controllen, host_control, sizeof(host_control), &msg);
        if (err < 0)
            goto out_free_scm;
    }

    msg.msg_flags = sock_flags_to_real(msg_fake.msg_flags);
    err = _EINVAL;
    if (msg.msg_flags < 0)
        goto out_free_scm;
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        goto out_free_scm;

    int host_flags = send_dontwait_begin(sock, flags);
    err = sendmsg(sock->real_fd, &msg, real_flags);
    send_dontwait_end(sock, host_flags);
    if (netcap_on()) {
        int saved_errno = errno;
        if (err >= 0)
            netcap_send(sock, msg.msg_iov, msg.msg_iovlen, err, msg.msg_name);
        else
            netcap_error(sock, saved_errno);
        errno = saved_errno;
    }
    if (err < 0) {
        err = errno_map();
        goto out_free_scm;
    }
    goto out_free_iov;

out_free_scm:
    if (scm != NULL) {
        lock(&peer_lock);
        struct fd *peer = sock->socket.unix_peer;
        if (peer != NULL) {
            lock(&peer->lock);
            list_remove_safe(&scm->queue);
            unlock(&peer->lock);
        } else {
            list_remove_safe(&scm->queue); // on its unix_pending, or on none
        }
        unlock(&peer_lock);
        scm_free(scm);
    }
out_free_iov:
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++)
        free(msg_iov[i].iov_base);
    return err;
}

int_t sys_recvmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    if (fd_is_packet(f_get(sock_fd)))
        return packet_unsupported(f_get(sock_fd), "recvmsg");
    STRACE("recvmsg(%d, %#x, %d)", sock_fd, msghdr_addr, flags);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return sock_fd_error(sock_fd);
    // The error queue is always empty (see IP_RECVERR in setsockopt).
    if (flags & MSG_ERRQUEUE_)
        return _EAGAIN;
    if (fd_is_netlink_stub(sock))
        return netlink_stub_recvmsg(sock, msghdr_addr, flags);

    // Read the guest msghdr struct into our internal 32-bit representation
    struct msghdr_ msg_fake;
    struct msghdr64_ msg_fake64;
    if (user_get(msghdr_addr, msg_fake64))
        return _EFAULT;
    msg_fake.msg_name = (addr_t)msg_fake64.msg_name;
    msg_fake.msg_namelen = msg_fake64.msg_namelen;
    msg_fake.msg_iov = (addr_t)msg_fake64.msg_iov;
    msg_fake.msg_iovlen = (uint_t)msg_fake64.msg_iovlen;
    msg_fake.msg_control = (addr_t)msg_fake64.msg_control;
    msg_fake.msg_controllen = (uint_t)msg_fake64.msg_controllen;
    msg_fake.msg_flags = msg_fake64.msg_flags;

    struct msghdr msg;

    // msg_name
    char msg_name[msg_fake.msg_namelen];
    if (msg_fake.msg_name != 0) {
        msg.msg_name = msg_name;
        msg.msg_namelen = sizeof(msg_name);
    } else {
        msg.msg_name = NULL;
        msg.msg_namelen = 0;
    }

    char real_msg_control[1024] __attribute__((aligned(8))) = {};
    // msg_control. On a unix socket, room for the one fd that stands for the
    // ones sent (sys_sendmsg), always: without it the host drops that fd,
    // and the batch it stands for would be taken by the next message. On
    // another, room for what the host gives, IP and IPv6's control messages
    // (host_cmsgs_to_guest), whatever the guest's room: what does not fit it
    // is MSG_CTRUNC, as on Linux.
    msg.msg_control = real_msg_control;
    msg.msg_controllen = sock->socket.domain == AF_LOCAL_ ? CMSG_SPACE(sizeof(int)) : sizeof(real_msg_control);

    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;

    // msg_iovec (no initial content)
    struct iovec64_ msg_iov_fake64[msg_fake.msg_iovlen];
    if (user_read(msg_fake.msg_iov, msg_iov_fake64, sizeof(msg_iov_fake64)))
        return _EFAULT;
    struct iovec msg_iov[msg_fake.msg_iovlen];
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = msg_fake.msg_iovlen;
    addr_t msg_iov_bases[msg_fake.msg_iovlen]; // save guest base addrs for writeback
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov_bases[i] = (addr_t)msg_iov_fake64[i].base;
        msg_iov[i].iov_len = (size_t)msg_iov_fake64[i].len;
        msg_iov[i].iov_base = malloc(msg_iov[i].iov_len);
    }

    // Same iOS-safe wait pattern as sys_recvfrom: avoid unbounded blocking
    // in host recvmsg() so guest signals can always be delivered. But only
    // wait when the caller actually wants blocking behavior — if the fd is
    // nonblock (SOCK_NONBLOCK set by guest or via fcntl), surface EAGAIN
    // immediately instead of waiting. This is critical for musl's
    // res_msend, which creates nonblock sockets and calls
    // recvmsg(fd, &mh, 0) — it relies on EAGAIN on empty buffer to end its
    // "drain pending responses" loop. Same bug that used to affect
    // sys_recvfrom before 76907b86.
    int host_flags_rm = fcntl(sock->real_fd, F_GETFL, 0);
    bool host_nonblock_rm = (host_flags_rm >= 0) && (host_flags_rm & O_NONBLOCK);
    bool guest_nonblock_rm = (sock->flags & O_NONBLOCK_) != 0;
    bool should_wait_rm = !(real_flags & MSG_DONTWAIT) && !guest_nonblock_rm && !host_nonblock_rm;

    ssize_t res = -1;
    int err = 0;
    if (should_wait_rm) {
        for (;;) {
            res = recvmsg(sock->real_fd, &msg, real_flags | MSG_DONTWAIT);
            if (res >= 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                err = errno_map();
                break;
            }
            int wr = sock_wait_readable(sock->real_fd);
            if (wr < 0) {
                err = wr;
                break;
            }
        }
    } else {
        res = recvmsg(sock->real_fd, &msg, real_flags);
        if (res < 0)
            err = errno_map();
    }
    // what was received: not a peek, which leaves it there
    if (netcap_on() && res >= 0 && !(flags & MSG_PEEK_))
        netcap_recv(sock, msg.msg_iov, msg.msg_iovlen, res, msg.msg_name);
    // don't return err quite yet, there are outstanding mallocs

    // msg_iovec (changed)
    // copy as many bytes as were received, according to the return value
    size_t n = res;
    if (res < 0)
        n = 0;
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        size_t chunk_size = msg_iov[i].iov_len;
        if (chunk_size > n)
            chunk_size = n;
        if (chunk_size > 0) {
            if (user_write(msg_iov_bases[i], msg_iov[i].iov_base, chunk_size))
                return _EFAULT;
        }
        n -= chunk_size;
        free(msg_iov[i].iov_base);
    }

    // msg_control (changed). As Linux's scm_detach_fds: as many of the file
    // descriptors as fit in the guest's buffer, the rest closed and
    // MSG_CTRUNC set; msg_controllen is the space the message took.
    size_t control_room = msg_fake.msg_controllen;
    msg_fake.msg_controllen = 0;
    bool control_truncated = false;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (sock->socket.domain == AF_LOCAL_ && cmsg != NULL &&
            cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int dummy_fd = ((int *) CMSG_DATA(cmsg))[0];
        close(dummy_fd);

        lock(&sock->lock);
        assert(!list_empty(&sock->socket.unix_scm));
        struct scm *scm = list_first_entry(&sock->socket.unix_scm, struct scm, queue);
        list_remove(&scm->queue);
        unlock(&sock->lock);

        if (res < 0) {
            scm_free(scm);
            return err;
        }

        unsigned fit = control_room < sizeof(struct cmsghdr_) ? 0 :
            (control_room - sizeof(struct cmsghdr_)) / sizeof(fd_t);
        if (fit > scm->num_fds)
            fit = scm->num_fds;
        uint8_t msg_control[sizeof(struct cmsghdr_) + fit * sizeof(fd_t)];
        struct cmsghdr_ *ctl = (void *) msg_control;
        fd_t *fds = (void *) ctl->data;
        unsigned installed = 0;
        for (; installed < fit; installed++) {
            fd_t f = f_install(scm->fds[installed], (flags & MSG_CMSG_CLOEXEC_) ? O_CLOEXEC_ : 0);
            if (f < 0)
                break; // out of descriptors: f_install closed this one
            fds[installed] = f;
            STRACE(" receiving fd %d", f);
        }
        unsigned next = installed < fit ? installed + 1 : installed;
        for (unsigned i = next; i < scm->num_fds; i++)
            fd_close(scm->fds[i]);
        control_truncated = installed < scm->num_fds;
        free(scm);
        if (installed > 0) {
            ctl->len = sizeof(struct cmsghdr_) + installed * sizeof(fd_t);
            ctl->level = SOL_SOCKET_;
            ctl->type = SCM_RIGHTS_;
            if (user_write(msg_fake.msg_control, ctl, ctl->len))
                return _EFAULT;
            size_t space = CMSG_SPACE_(installed * sizeof(fd_t));
            msg_fake.msg_controllen = space < control_room ? space : control_room;
        }
    }

    // by now the iovecs and scm have been freed so we can return
    if (res < 0)
        return err;

    if (sock->socket.domain != AF_LOCAL_) {
        struct guest_cmsgs out = {.at = msg_fake.msg_control, .room = control_room};
        int cmsg_err = host_cmsgs_to_guest(&msg, &out);
        if (cmsg_err < 0)
            return cmsg_err;
        msg_fake.msg_controllen = out.used;
        control_truncated = out.truncated;
    }

    // msg_name (changed)
    if (msg.msg_name != 0) {
        int err = sockaddr_write(msg_fake.msg_name, msg.msg_name, sizeof(msg_name), &msg.msg_namelen);
        if (err < 0)
            return err;
    }
    msg_fake.msg_namelen = msg.msg_namelen;

    // msg_flags (changed): what came of the receive only, as Linux's, not
    // the flags it was called with. XNU's recvmsg gives those back too, and
    // the host's was called with MSG_DONTWAIT (above), which the guest got,
    // 0x40, of every receive (CPython's test_socket testSendAndRecvFds).
    msg_fake.msg_flags = sock_flags_from_real(msg.msg_flags) & (MSG_OOB_ | MSG_TRUNC_ | MSG_CTRUNC_ | MSG_EOR_);
    if (control_truncated)
        msg_fake.msg_flags |= MSG_CTRUNC_;

    // Write back the updated msghdr to guest memory
    msg_fake64.msg_namelen = msg_fake.msg_namelen;
    msg_fake64.msg_controllen = msg_fake.msg_controllen;
    msg_fake64.msg_flags = msg_fake.msg_flags;
    if (user_put(msghdr_addr, msg_fake64))
        return _EFAULT;
    return res;
}

struct mmsghdr_ {
    struct msghdr_ hdr;
    uint_t len;
};

struct mmsghdr64_ {
    struct msghdr64_ hdr;
    uint32_t len;
    uint32_t _pad;
};

int_t sys_recvmmsg(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags, addr_t timeout_addr) {
    STRACE("recvmmsg(%d, %#x, %d, %d, %#x)", sock_fd, msg_vec, vec_len, flags, timeout_addr);
    // The error queue is always empty (see IP_RECVERR in setsockopt).
    if (flags & MSG_ERRQUEUE_)
        return _EAGAIN;
    // The timeout is checked after each datagram, as Linux does (recvmmsg(2),
    // BUGS: it does not end a wait for one), and what is left of it written
    // back.
    struct timespec deadline;
    if (timeout_addr != 0) {
        struct timespec_ timeout;
        if (user_get(timeout_addr, timeout))
            return _EFAULT;
        deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), convert_timespec(timeout));
    }
    int num_recv = 0;
    size_t mmsghdr_size = sizeof(struct mmsghdr64_);
    size_t len_offset = offsetof(struct mmsghdr64_, len);
    for (unsigned i = 0; i < vec_len; i++) {
        addr_t msghdr = msg_vec + i * mmsghdr_size;
        int_t res = sys_recvmsg(sock_fd, msghdr, flags);
        if (res >= 0) {
            addr_t msg_len_addr = msghdr + len_offset;
            if (user_put(msg_len_addr, res))
                res = _EFAULT;
        }
        if (res < 0) {
            if (num_recv > 0)
                break;
            return res;
        }
        num_recv++;
        if (res == 0)
            break;
        if (timeout_addr != 0 && !timespec_positive(timespec_subtract(deadline, timespec_now(CLOCK_MONOTONIC))))
            break;
    }
    if (timeout_addr != 0) {
        struct timespec left = timespec_subtract(deadline, timespec_now(CLOCK_MONOTONIC));
        if (!timespec_positive(left))
            left = (struct timespec) {0, 0};
        struct timespec_ timeout = {.sec = left.tv_sec, .nsec = left.tv_nsec};
        if (user_put(timeout_addr, timeout))
            return _EFAULT;
    }
    return num_recv;
}

int_t sys_sendmmsg(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags) {
    int num_sent = 0;
    size_t mmsghdr_size = sizeof(struct mmsghdr64_);
    size_t len_offset = offsetof(struct mmsghdr64_, len);
    for (unsigned i = 0; i < vec_len; i++) {
        addr_t msghdr = msg_vec + i * mmsghdr_size;
        int_t res = sys_sendmsg(sock_fd, msghdr, flags);
        if (res >= 0) {
            addr_t msg_len_addr = msghdr + len_offset;
            if (user_put(msg_len_addr, res))
                res = _EFAULT;
        }
        if (res < 0) {
            // From the man page:
            // If an error occurs after at least one message has been sent, the
            // call succeeds, and returns the number of messages sent.  The
            // error code is lost.
            if (num_sent > 0)
                break;
            return res;
        }
        num_sent++;
        if (res == 0) {
            // This means the socket is non-blocking and can't be written to anymore.
            break;
        }
    }
    return num_sent;
}

static void sock_translate_err(struct fd *fd, int *err) {
    // on ios, when the device goes to sleep, all connected sockets are killed.
    // reads/writes return ENOTCONN, which I'm pretty sure is a violation of
    // posix. so instead, detect this and return ECONNRESET.
    if (*err == _ENOTCONN) {
        struct sockaddr addr;
        socklen_t len = sizeof(addr);
        if (getpeername(fd->real_fd, &addr, &len) < 0 && errno == EINVAL) {
            *err = _ECONNRESET;
        }
    }
}

// sock_read / sock_write: behave exactly like realfs_read / realfs_write
// (a single host read/write call, with no poll and no flag change), except
// that EINTR is handled specially: if there is a guest signal pending, we
// surface EINTR back to the guest instead of blindly retrying.
//
// The original realfs_read/realfs_write does `while (errno == EINTR) retry`,
// which swallows the SIGUSR1 wake iSH uses to deliver guest signals into
// blocked host syscalls. Combined with iOS occasionally deferring signal
// delivery (thread suspension on backgrounding), this causes indefinite
// hangs on any blocking socket read/write (git HTTPS, nc, telnet, etc.).
//
// We intentionally do NOT poll, do NOT mess with nonblock flags, and do NOT
// switch to recv/send. This minimizes regression risk — the host I/O path
// is byte-for-byte identical to the original implementation for every case
// except "host syscall got EINTR AND guest has a pending signal", which is
// precisely the bug we need to fix.
static ssize_t sock_read(struct fd *fd, void *buf, size_t size) {
    // read() on a netlink fd is recv() with no flags: bounded, never a bare
    // blocking read() of the backing socketpair.
    if (fd_is_netlink_stub(fd)) {
        char nlbuf[NETLINK_STUB_DGRAM_MAX * 2];
        ssize_t n = netlink_stub_dequeue(fd, 0, nlbuf, sizeof(nlbuf));
        if (n < 0)
            return n;
        size_t copy = (size_t) n < sizeof(nlbuf) ? (size_t) n : sizeof(nlbuf);
        if (copy > size)
            copy = size;
        memcpy(buf, nlbuf, copy);
        return (ssize_t) copy;
    }
    int err;
    for (;;) {
        ssize_t res = read(fd->real_fd, buf, size);
        if (res >= 0) {
            err = (int) res;
            if (netcap_on())
                netcap_recv(fd, &(struct iovec) {buf, res}, 1, res, NULL);
            break;
        }
        if (errno != EINTR) {
            if (netcap_on()) {
                int saved_errno = errno;
                netcap_error(fd, saved_errno);
                errno = saved_errno;
            }
            err = errno_map();
            break;
        }
        // EINTR: only surface to guest if there is actually a pending signal
        // that the guest wants to handle. Otherwise retry (matches the
        // original realfs_read behavior for spurious EINTR).
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            bool pending = task_signal_pending(current);
            unlock(&current->sighand->lock);
            if (pending) { err = _EINTR; break; }
        }
        // no guest signal — retry the host read
    }
    sock_translate_err(fd, &err);
    return err;
}

static ssize_t sock_write(struct fd *fd, const void *buf, size_t size) {
    // write()/writev() on a netlink fd is a request like any other (busybox
    // `ip` sends its dump requests this way): answer it, do not push the
    // bytes into the socketpair where nobody reads them.
    if (fd_is_netlink_stub(fd))
        return netlink_stub_reply(fd, buf, size);
    int err;
    for (;;) {
        ssize_t res = write(fd->real_fd, buf, size);
        if (res >= 0) {
            err = (int) res;
            if (netcap_on())
                netcap_send(fd, &(struct iovec) {(void *) buf, res}, 1, res, NULL);
            break;
        }
        if (errno != EINTR) {
            if (netcap_on()) {
                int saved_errno = errno;
                netcap_error(fd, saved_errno);
                errno = saved_errno;
            }
            err = errno_map();
            break;
        }
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            bool pending = task_signal_pending(current);
            unlock(&current->sighand->lock);
            if (pending) { err = _EINTR; break; }
        }
    }
    sock_translate_err(fd, &err);
    return err;
}

static int sock_close(struct fd *fd) {
    if (fd->socket.netcap != NULL || netcap_on())
        netcap_close(fd);
    // [STAGE-0 PROBE] release the stub's socketpair write end.
    if (fd->socket.netlink_peer_fd >= 0) {
        close(fd->socket.netlink_peer_fd);
        fd->socket.netlink_peer_fd = -1;
    }
    sockrestart_end_listen(fd);
    // FIXME next 3 lines should go in a function like release_unix_names
    inode_release_if_exist(fd->socket.unix_name_inode);
    if (fd->socket.unix_name_abstract != NULL)
        unix_abstract_release(fd->socket.unix_name_abstract);
    lock(&peer_lock);
    list_remove_safe(&fd->socket.unix_listening);
    struct fd *peer = fd->socket.unix_peer;
    if (peer != NULL)
        peer->socket.unix_peer = NULL;
    struct unix_pending *pending = fd->socket.unix_pending;
    if (pending != NULL) {
        // Not accepted yet. Keep what it sent for the accepting end.
        pending->client = NULL;
        if (list_empty(&pending->scm))
            unix_pending_free(pending);
    }
    unlock(&peer_lock);
    if (fd->socket.domain == AF_LOCAL_) {
        lock(&fd->lock);
        struct scm *scm, *tmp;
        list_for_each_entry_safe(&fd->socket.unix_scm, scm, tmp, queue) {
            list_remove(&scm->queue);
            scm_free(scm);
        }
        unlock(&fd->lock);
    }
    return realfs_close(fd);
}

// ===== /proc/net/tcp, tcp6, udp, udp6 =====

// The host's TCP state of a socket that is not listening, as Linux's TCP_*
// number. -1 if the host cannot say (the socket went away meanwhile), -2 if it
// says something not in that list (logged with the socket's inode).
static int sock_tcp_state(struct fd *sock) {
    unsigned host_state;
#if defined(__APPLE__)
    struct tcp_connection_info info;
    socklen_t len = sizeof(info);
    if (getsockopt(sock->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &len) < 0)
        return -1;
    host_state = info.tcpi_state;
    int state = tcp_state_from_darwin(host_state);
#else
    struct tcp_info info;
    socklen_t len = sizeof(info);
    if (getsockopt(sock->real_fd, IPPROTO_TCP, TCP_INFO, &info, &len) < 0)
        return -1;
    host_state = info.tcpi_state;
    int state = host_state >= 1 && host_state <= 11 ? (int) host_state : -1;
#endif
    if (state < 0) {
        printk("/proc/net: skipping socket %lu, host TCP state %u is unknown\n", sock->stat.inode, host_state);
        return -2;
    }
    return state;
}

// Bytes waiting to be sent.
static unsigned sock_tx_queue(int real_fd) {
    int bytes = 0;
#if defined(__APPLE__)
    socklen_t len = sizeof(bytes);
    if (getsockopt(real_fd, SOL_SOCKET, SO_NWRITE, &bytes, &len) < 0)
        return 0;
#else
    if (ioctl(real_fd, TIOCOUTQ, &bytes) < 0)
        return 0;
#endif
    return bytes;
}

static void sockaddr_to_entry(const struct sockaddr_storage *addr, uint8_t ip[16], uint16_t *port) {
    memset(ip, 0, 16);
    *port = 0;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *) addr;
        memcpy(ip, &in->sin_addr, 4);
        *port = ntohs(in->sin_port);
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) addr;
        memcpy(ip, &in6->sin6_addr, 16);
        *port = ntohs(in6->sin6_port);
    }
}

// Fills everything but uid and inode. Returns false when the host cannot tell
// (the socket was closed meanwhile, or an unknown TCP state, which
// sock_tcp_state logs).
static bool sock_inet_query(struct fd *sock, struct sock_inet_entry *e) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(sock->real_fd, (struct sockaddr *) &addr, &len) < 0)
        return false;
    sockaddr_to_entry(&addr, e->local, &e->local_port);
    len = sizeof(addr);
    bool connected = getpeername(sock->real_fd, (struct sockaddr *) &addr, &len) == 0;
    if (connected)
        sockaddr_to_entry(&addr, e->remote, &e->remote_port);
    else
        sockaddr_to_entry(&(struct sockaddr_storage) {}, e->remote, &e->remote_port);

    // iSH's own word for it (sys_listen): Darwin's getsockopt does not know
    // SO_ACCEPTCONN
    if (sock->socket.type == SOCK_STREAM_ && sockrestart_listening(sock)) {
        e->state = 10; // LISTEN
        e->tx_queue = e->rx_queue = 0;
        return true;
    }
    if (sock->socket.type == SOCK_STREAM_) {
        e->state = sock_tcp_state(sock);
        if (e->state < 0)
            return false;
    } else {
        e->state = connected ? 1 : 7; // ESTABLISHED : CLOSE, as Linux shows UDP
    }
    int bytes = 0;
    e->rx_queue = ioctl(sock->real_fd, FIONREAD, &bytes) == 0 ? (unsigned) bytes : 0;
    e->tx_queue = sock_tx_queue(sock->real_fd);
    return true;
}

struct sock_open {
    struct fd *fd;
    uid_t_ uid; // effective uid of a process that has it open
};

// The open sockets of family and type (any type if type < 0), each once
// however many descriptors share it, retained: fd_close each. Returns how
// many (free() *socks) or a negative errno.
static int sock_collect(int family, int type, struct sock_open **socks_out) {
    // Collect the descriptor tables under pids_lock, then look into each with
    // only its own lock held: other paths take a table's lock before
    // pids_lock (see proc_pid_fd_readdir).
    struct table { struct fdtable *files; uid_t_ uid; } *tables = NULL;
    size_t n_tables = 0;
    int err = 0;
    lock(&pids_lock);
    for (pid_t_ pid = 1; pid < MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (task == NULL || task->files == NULL)
            continue;
        bool seen = false;
        for (size_t i = 0; i < n_tables && !seen; i++)
            seen = tables[i].files == task->files;
        if (seen)
            continue;
        struct table *grown = realloc(tables, (n_tables + 1) * sizeof(*tables));
        if (grown == NULL) {
            err = _ENOMEM;
            break;
        }
        tables = grown;
        task->files->refcount++;
        tables[n_tables++] = (struct table) {task->files, task->euid};
    }
    unlock(&pids_lock);

    struct sock_open *socks = NULL;
    size_t n_socks = 0;
    for (size_t t = 0; t < n_tables; t++) {
        struct fdtable *files = tables[t].files;
        lock(&files->lock);
        for (unsigned f = 0; f < files->size && err == 0; f++) {
            struct fd *fd = files->files[f];
            if (fd == NULL || fd->ops != &socket_fdops || fd->socket.domain != family ||
                    (type >= 0 && fd->socket.type != type))
                continue;
            bool seen = false;
            for (size_t i = 0; i < n_socks && !seen; i++)
                seen = socks[i].fd == fd;
            if (seen)
                continue;
            struct sock_open *grown = realloc(socks, (n_socks + 1) * sizeof(*socks));
            if (grown == NULL) {
                err = _ENOMEM;
                break;
            }
            socks = grown;
            socks[n_socks++] = (struct sock_open) {fd_retain(fd), tables[t].uid};
        }
        unlock(&files->lock);
        fdtable_release(files);
    }
    free(tables);
    if (err < 0) {
        for (size_t i = 0; i < n_socks; i++)
            fd_close(socks[i].fd);
        free(socks);
        return err;
    }
    *socks_out = socks;
    return (int) n_socks;
}

int sock_inet_list(int family, int type, struct sock_inet_entry **entries) {
    struct sock_open *socks = NULL;
    int n_socks = sock_collect(family, type, &socks);
    if (n_socks < 0)
        return n_socks;
    int err = 0;
    struct sock_inet_entry *list = n_socks > 0 ? calloc(n_socks, sizeof(*list)) : NULL;
    if (n_socks > 0 && list == NULL)
        err = _ENOMEM;
    int n = 0;
    for (int i = 0; i < n_socks; i++) {
        if (err == 0 && sock_inet_query(socks[i].fd, &list[n])) {
            if (type == SOCK_RAW_) {
                list[n].local_port = socks[i].fd->socket.protocol;
                list[n].remote_port = 0;
            }
            list[n].uid = socks[i].uid;
            list[n].inode = socks[i].fd->stat.inode;
            n++;
        }
        fd_close(socks[i].fd);
    }
    free(socks);
    if (err < 0) {
        free(list);
        return err;
    }
    *entries = list;
    return n;
}

int sock_unix_list(struct sock_unix_entry **entries) {
    struct sock_open *socks = NULL;
    int n_socks = sock_collect(AF_LOCAL_, -1, &socks);
    if (n_socks < 0)
        return n_socks;
    int err = 0;
    struct sock_unix_entry *list = n_socks > 0 ? calloc(n_socks, sizeof(*list)) : NULL;
    if (n_socks > 0 && list == NULL)
        err = _ENOMEM;
    for (int i = 0; i < n_socks; i++) {
        struct fd *sock = socks[i].fd;
        if (err == 0) {
            struct sock_unix_entry *e = &list[i];
            e->type = sock->socket.type;
            // iSH's own word for it, unix_listeners (sys_listen): the host's
            // SO_ACCEPTCONN said none of them listened on Darwin, and on a
            // Linux host that is itself an iSH of before it knew the option,
            // it failed
            lock(&peer_lock);
            e->listening = !list_null(&sock->socket.unix_listening);
            unlock(&peer_lock);
            struct sockaddr_storage addr;
            socklen_t len = sizeof(addr);
            e->connected = !e->listening &&
                getpeername(sock->real_fd, (struct sockaddr *) &addr, &len) == 0;
            e->inode = sock->stat.inode;
            e->name_len = sock->socket.unix_name_len;
            memcpy(e->name, sock->socket.unix_name, e->name_len);
        }
        fd_close(sock);
    }
    free(socks);
    if (err < 0) {
        free(list);
        return err;
    }
    *entries = list;
    return n_socks;
}

// ===== the interface ioctls =====

// Linux's struct ifreq: its union is as big as struct ifmap, of longs
struct ifreq_ {
    char name[16];
    union {
        struct {
            uint16_t family;
            uint8_t data[14];
        } hwaddr;
        // a struct sockaddr_in
        struct {
            uint16_t family;
            uint16_t port;
            uint8_t addr[4];
            uint8_t zero[8];
        } in;
        int16_t flags;
        int32_t ifindex;
        int32_t metric;
        int32_t mtu;
        int32_t qlen;
        uint64_t data;
        uint8_t size[24];
    };
};
// Linux's struct ifconf
struct ifconf_ {
    int32_t len;
    uint32_t pad;
    uint64_t buf;
};
static_assert(sizeof(struct ifreq_) == 40, "ifreq layout");
static_assert(sizeof(struct ifconf_) == 16, "ifconf layout");

#define SIOCGIFNAME_ 0x8910
#define SIOCGIFCONF_ 0x8912
#define SIOCGIFFLAGS_ 0x8913
#define SIOCGIFADDR_ 0x8915
#define SIOCGIFDSTADDR_ 0x8917
#define SIOCGIFBRDADDR_ 0x8919
#define SIOCGIFNETMASK_ 0x891b
#define SIOCGIFMETRIC_ 0x891d
#define SIOCGIFMTU_ 0x8921
#define SIOCGIFHWADDR_ 0x8927
#define SIOCGIFINDEX_ 0x8933
#define SIOCGIFTXQLEN_ 0x8942
#define SIOCETHTOOL_ 0x8946
#define SIOCGIFMAP_ 0x8970
// Linux's
#define IFF_LOOPBACK_ 0x8
#define ETHTOOL_GTSO_ 0x1e
#define ETHTOOL_GUFO_ 0x21
#define ETHTOOL_GGSO_ 0x23
#define ETHTOOL_GGRO_ 0x2b

ssize_t netif_ioctl_size(int cmd) {
    switch (cmd) {
        case SIOCGIFCONF_:
            return sizeof(struct ifconf_);
        case SIOCGIFNAME_:
        case SIOCGIFFLAGS_:
        case SIOCGIFADDR_:
        case SIOCGIFDSTADDR_:
        case SIOCGIFBRDADDR_:
        case SIOCGIFNETMASK_:
        case SIOCGIFMETRIC_:
        case SIOCGIFMTU_:
        case SIOCGIFHWADDR_:
        case SIOCGIFINDEX_:
        case SIOCGIFTXQLEN_:
        case SIOCETHTOOL_:
        case SIOCGIFMAP_:
            return sizeof(struct ifreq_);
    }
    return -1;
}

// Linux's dev_ifconf: an ifreq for each IPv4 address, its interface's name
// and the address, as many as there is room for; none, only the room they
// need, when there is no buffer
static int netif_conf(struct ifconf_ *ifc, const struct netlink_host_addr *addrs, size_t n_addrs) {
    int32_t total = 0;
    for (size_t i = 0; i < n_addrs; i++) {
        if (addrs[i].family != AF_INET_)
            continue;
        if (ifc->buf != 0) {
            if (ifc->len - total < (int32_t) sizeof(struct ifreq_))
                break;
            struct ifreq_ ifr = {0};
            strncpy(ifr.name, addrs[i].name, sizeof(ifr.name) - 1);
            ifr.in.family = AF_INET_;
            memcpy(ifr.in.addr, addrs[i].local, 4);
            if (user_write(ifc->buf + total, &ifr, sizeof(ifr)))
                return _EFAULT;
        }
        total += sizeof(struct ifreq_);
    }
    ifc->len = total;
    return 0;
}

// Linux's devinet_ioctl: the interface's first IPv4 address's. Its
// destination is its peer's on a point-to-point link, else its own.
static int netif_inet(int cmd, struct ifreq_ *ifr, const struct netlink_host_addr *a) {
    if (a == NULL)
        return _EADDRNOTAVAIL;
    memset(&ifr->in, 0, sizeof(ifr->in));
    ifr->in.family = AF_INET_;
    switch (cmd) {
        case SIOCGIFADDR_:
            memcpy(ifr->in.addr, a->local, 4);
            break;
        case SIOCGIFDSTADDR_:
            memcpy(ifr->in.addr, a->has_peer ? a->peer : a->local, 4);
            break;
        case SIOCGIFBRDADDR_:
            if (a->has_brd)
                memcpy(ifr->in.addr, a->brd, 4);
            break;
        case SIOCGIFNETMASK_: {
            uint32_t mask = a->prefixlen == 0 ? 0 : htonl(~0u << (32 - a->prefixlen));
            memcpy(ifr->in.addr, &mask, 4);
            break;
        }
    }
    return 0;
}

// The offloads are on: a made-up TCP segment is as big as a send
// (kernel/netcap.c), and libpcap sizes an Ethernet capture's frames by them.
// The rest are not here, as a driver may be without them.
static int netif_ethtool(addr_t data) {
    uint32_t cmd;
    if (user_get(data, cmd))
        return _EFAULT;
    switch (cmd) {
        case ETHTOOL_GTSO_:
        case ETHTOOL_GUFO_:
        case ETHTOOL_GGSO_:
        case ETHTOOL_GGRO_: {
            uint32_t value[2] = {cmd, 1};
            return user_put(data, value) ? _EFAULT : 0;
        }
    }
    return _EOPNOTSUPP;
}

// The interfaces as the guest knows them (fs/netlink.c): the host's, lo0 as lo
int netif_ioctl(int cmd, void *arg) {
    struct ifreq_ *ifr = arg;
    struct netlink_host_link *links;
    struct netlink_host_addr *addrs;
    size_t n_links, n_addrs;
    netlink_host_snapshot(&links, &n_links, &addrs, &n_addrs);
    if (cmd == SIOCGIFCONF_) {
        int err = netif_conf(arg, addrs, n_addrs);
        netlink_host_free(links, addrs);
        return err;
    }
    const struct netlink_host_link *link = NULL;
    for (size_t i = 0; i < n_links && link == NULL; i++) {
        if (cmd == SIOCGIFNAME_ ? (int) links[i].index == ifr->ifindex
                                : strncmp(links[i].name, ifr->name, sizeof(ifr->name)) == 0)
            link = &links[i];
    }
    int err = 0;
    if (link == NULL) {
        err = _ENODEV;
    } else {
        switch (cmd) {
            case SIOCGIFNAME_:
                memset(ifr->name, 0, sizeof(ifr->name));
                strncpy(ifr->name, link->name, sizeof(ifr->name) - 1);
                break;
            case SIOCGIFINDEX_:
                ifr->ifindex = link->index;
                break;
            case SIOCGIFFLAGS_:
                // the 16 bits there are room for, as Linux's
                ifr->flags = (int16_t) link->flags;
                break;
            case SIOCGIFMTU_:
                ifr->mtu = link->mtu;
                break;
            case SIOCGIFADDR_:
            case SIOCGIFDSTADDR_:
            case SIOCGIFBRDADDR_:
            case SIOCGIFNETMASK_: {
                const struct netlink_host_addr *a = NULL;
                for (size_t i = 0; i < n_addrs && a == NULL; i++)
                    if (addrs[i].index == link->index && addrs[i].family == AF_INET_)
                        a = &addrs[i];
                err = netif_inet(cmd, ifr, a);
                break;
            }
            // as Linux's: always 0
            case SIOCGIFMETRIC_:
                ifr->metric = 0;
                break;
            // a device's memory, I/O port, IRQ and DMA channel: none
            case SIOCGIFMAP_:
                memset(ifr->size, 0, sizeof(ifr->size));
                break;
            // as netlink says it (fs/netlink.c)
            case SIOCGIFTXQLEN_:
                ifr->qlen = (link->flags & IFF_LOOPBACK_) ? 1000 : 0;
                break;
            case SIOCGIFHWADDR_:
                memset(&ifr->hwaddr, 0, sizeof(ifr->hwaddr));
                ifr->hwaddr.family = link->arphrd;
                if (link->has_lladdr)
                    memcpy(ifr->hwaddr.data, link->lladdr, 6);
                break;
            case SIOCETHTOOL_:
                err = netif_ethtool(ifr->data);
                break;
        }
    }
    netlink_host_free(links, addrs);
    return err;
}

static ssize_t sock_ioctl_size(int cmd) {
    ssize_t size = netif_ioctl_size(cmd);
    return size >= 0 ? size : realfs_ioctl_size(cmd);
}

static int sock_ioctl(struct fd *fd, int cmd, void *arg) {
    if (netif_ioctl_size(cmd) >= 0)
        return netif_ioctl(cmd, arg);
    return realfs_ioctl(fd, cmd, arg);
}

const struct fd_ops socket_fdops = {
    .read = sock_read,
    .write = sock_write,
    .close = sock_close,
    .poll = realfs_poll,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
    .ioctl_size = sock_ioctl_size,
    .ioctl = sock_ioctl,
};

SYSCALL_CASTS_OK
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static struct socket_call {
    syscall_t func;
    int args;
} socket_calls[] = {
    {NULL},
    {(syscall_t) sys_socket, 3},
    {(syscall_t) sys_bind, 3},
    {(syscall_t) sys_connect, 3},
    {(syscall_t) sys_listen, 2},
    {(syscall_t) sys_accept, 3},
    {(syscall_t) sys_getsockname, 3},
    {(syscall_t) sys_getpeername, 3},
    {(syscall_t) sys_socketpair, 4},
    {(syscall_t) sys_send, 4}, // send
    {(syscall_t) sys_recv, 4}, // recv
    {(syscall_t) sys_sendto, 6},
    {(syscall_t) sys_recvfrom, 6},
    {(syscall_t) sys_shutdown, 2},
    {(syscall_t) sys_setsockopt, 5},
    {(syscall_t) sys_getsockopt, 5},
    {(syscall_t) sys_sendmsg, 3},
    {(syscall_t) sys_recvmsg, 3},
    {NULL}, // accept4
    {NULL}, // recvmmsg
    {(syscall_t) sys_sendmmsg, 4},
};

int_t sys_socketcall(dword_t call_num, addr_t args_addr) {
    STRACE("%d ", call_num);
    if (call_num < 1 || call_num >= sizeof(socket_calls)/sizeof(socket_calls[0]))
        return _EINVAL;
    struct socket_call call = socket_calls[call_num];
    if (call.func == NULL) {
        FIXME("socketcall %d", call_num);
        return _ENOSYS;
    }

    dword_t args[6];
    if (user_read(args_addr, args, sizeof(dword_t) * call.args))
        return _EFAULT;
    return call.func(args[0], args[1], args[2], args[3], args[4], args[5]);
}
