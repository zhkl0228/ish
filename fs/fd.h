#ifndef FD_H
#define FD_H
#include <dirent.h>
#include "kernel/memory.h"
#include "util/list.h"
#include "util/sync.h"
#include "util/bits.h"
#include "fs/stat.h"
#include "fs/proc.h"
#include "fs/sockrestart.h"

// FIXME almost everything that uses the structs in this file does so without any kind of sane locking

#define NAME_MAX 255
struct dir_entry {
    qword_t inode;
    char name[NAME_MAX + 1];
};

struct fd {
    atomic_uint refcount;
    unsigned flags;
    mode_t_ type; // just the S_IFMT part, it can't change
    const struct fd_ops *ops;
    struct list poll_fds;
    lock_t poll_lock;
    unsigned long offset;
    // Links this fd into the global pidfd registry (pidfd_open only).
    struct list pidfd_links;

    // fd data
    union {
        // pidfd (pidfd_open): references a process by pid
        struct {
            pid_t_ pid;
            // Set true by pidfd_notify_exit when `pid` becomes a zombie. Read
            // lock-free by pidfd_poll so polling never takes pids_lock (which
            // would create a poll->lock <-> pids_lock ABBA against the exit
            // path that wakes pidfd pollers while holding pids_lock).
            _Atomic bool exited;
        } pidfd;
        // tty
        struct {
            struct tty *tty;
            // links together fds pointing to the same tty
            // locked by the tty
            struct list tty_other_fds;
        };
        struct {
            struct poll *poll;
        } epollfd;
        struct {
            uint64_t val;
            bool semaphore; // EFD_SEMAPHORE: a read takes 1
        } eventfd;
        // signalfd (kernel/signalfd.c): the signals it reads (a sigset_t_),
        // the sighands of those polling it, and whether a wakeup of its
        // pollers is waiting, in the waker's queue. Locked by signalfd_lock.
        struct {
            _Atomic uint64_t mask;
            struct list watches;
            struct list pending_link;
            bool pending;
        } signalfd;
        struct {
            struct timer *timer;
            uint64_t expirations;
        } timerfd;
        struct {
            int domain;
            int type;
            int protocol;

            // These are only used as strong references, to keep the inode
            // alive while there is a listener.
            struct inode_data *unix_name_inode;
            struct unix_abstract *unix_name_abstract;
            uint8_t unix_name_len;
            char unix_name[108];
            // the other end's name, getpeername's: the listener's for a
            // connecting end (connect), the connecting end's for an
            // accepted one (accept)
            uint8_t unix_peer_name_len;
            char unix_peer_name[108];
            // The id behind the name bind() gave (inode->socket_id or the
            // abstract name's), 0 before. Connect finds the listener by it.
            uint32_t unix_socket_id;
            // On the list of listening unix sockets (sys_listen). Locked by
            // peer_lock.
            struct list unix_listening;
            struct fd *unix_peer; // locked by peer_lock, for simplicity
            // A connection the other end has not accepted yet (sys_connect),
            // or NULL. Locked by peer_lock.
            struct unix_pending *unix_pending;
            // AF_NETLINK stub: the write end of the backing socketpair
            // (-1 for every non-netlink socket), the groups bind() asked
            // for, and whether a dump whose replies did not all fit the
            // queue is still owed its NLMSG_DONE, and the receive timeout.
            // See fs/sock.c.
            int netlink_peer_fd;
            uint32_t netlink_groups;
            uint32_t netlink_owe_seq;
            bool netlink_owe_done;
            long netlink_rcvtimeo_ms; // SO_RCVTIMEO; 0 = wait
            // Queue of struct scm for sending file descriptors
            // locked by fd->lock
            struct list unix_scm;
            // What SO_PEERCRED gives: as on Linux, taken when the connection
            // is made and kept. Locked by peer_lock.
            struct ucred_ {
                pid_t_ pid;
                uid_t_ uid;
                uid_t_ gid;
            } unix_peer_cred;
            // A TCP connection's packets as /dev/netcap makes them up
            // (kernel/netcap.c), from its first while capturing. Locked by
            // netcap_lock.
            struct netcap_flow *netcap;
        } socket;

        // /dev/netcap (kernel/netcap.c): the capture
        struct {
            struct netcap_reader *reader;
        } netcap;
        // an AF_PACKET socket (kernel/packet.c)
        struct {
            struct packet_sock *sock;
        } packet;

        // See app/Pasteboard.m
        struct {
            // UIPasteboard.changeCount
            uint64_t generation;
            // Buffer for written data
            void* buffer;
            // its capacity
            size_t buffer_cap;
            // length of actual data stored in the buffer
            size_t buffer_len;
        } clipboard;

        // memfd (memfd_create, fs/adhoc.c): its name, /proc/<pid>/fd's
        struct {
            char name[250];
        } memfd;

        // can fit anything in here
        void *data;
    };
    // fs data
    union {
        struct {
            struct proc_entry entry;
            unsigned dir_index;
            struct proc_data data;
        } proc;
        struct {
            int num;
        } devpts;
        struct {
            struct tmp_dirent *dirent;
            struct tmp_dirent *dir_pos;
        } tmpfs;
        void *fs_data;
    };

    // fs/inode data
    struct mount *mount;
    int real_fd; // seeks on this fd require the lock TODO think about making a special lock just for that
    // [T-ish-flock-proxy] host fd + 1 of this open file's flock proxy (see
    // realfs_flock), 0 if it never took a flock
    _Atomic int flock_proxy;
    DIR *dir;
    // getdents: an entry already read from the directory that did not fit in
    // the caller's buffer. The next getdents returns it first, so the
    // directory position never moves backwards: seekdir() back to a position
    // taken before entries were deleted is unspecified by POSIX and does not
    // work on every host filesystem. Dropped by lseek on the directory.
    struct {
        bool valid;
        struct dir_entry entry;
        off_t_ offset; // d_off reported with it
    } dirent_pending;
    struct inode_data *inode;
    ino_t fake_inode;
    // [T-ish-exec-fewer-meta-txns] fakefs_open reads the stat row inside the
    // transaction it already holds for the path lookup and parks it here; the
    // first fstat after open (generic_openat's own) consumes it instead of
    // opening a second meta.db transaction. One-shot, so any later fstat sees
    // chmod/chown done by other tasks. Cleared by generic_openat regardless.
    struct { uint32_t mode, uid, gid, rdev; bool valid; } fake_open_stat;
    // The stat generic_openat obtained at open time. exec used to fstat the
    // fd again right after open, which on fakefs is a third meta.db
    // transaction per exec for a value it already had.
    struct statbuf open_stat;
    // generic_openat opened it and reported IN_OPEN: its last close reports
    // IN_CLOSE_WRITE or IN_CLOSE_NOWRITE (kernel/inotify.h)
    bool inotify_opened;
    struct statbuf stat; // for adhoc fs
    // A pipe's or FIFO's capacity, as F_GETPIPE_SZ and F_SETPIPE_SZ have it
    // (fs/pipe.c), shared with its other ends; NULL for another file, and for
    // a FIFO until it is first asked of. A reference, let go of on close.
    struct pipe_size *pipe_size;
    struct fd_sockrestart sockrestart; // argh

    // these are used for a variety of things related to the fd
    lock_t lock;
    cond_t cond;
};

typedef sdword_t fd_t;
#define AT_FDCWD_ -100

struct fd *fd_create(const struct fd_ops *ops);
struct fd *fd_retain(struct fd *fd);
int fd_close(struct fd *fd);

int fd_getflags(struct fd *fd);
int fd_setflags(struct fd *fd, int flags);


#define LSEEK_SET 0
#define LSEEK_CUR 1
#define LSEEK_END 2

struct fd_ops {
    // required for files
    // TODO make optional for non-files
    ssize_t (*read)(struct fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct fd *fd, const void *buf, size_t bufsize);
    ssize_t (*pread)(struct fd *fd, void *buf, size_t bufsize, off_t off);
    ssize_t (*pwrite)(struct fd *fd, const void *buf, size_t bufsize, off_t off);
    off_t_ (*lseek)(struct fd *fd, off_t_ off, int whence);

    // Reads a directory entry from the stream
    // required for directories
    int (*readdir)(struct fd *fd, struct dir_entry *entry);
    // Return an opaque value representing the current point in the directory stream
    // optional, fd->offset will be used instead
    unsigned long (*telldir)(struct fd *fd);
    // Seek to the location represented by a pointer returned from telldir
    // optional, fd->offset will be used instead
    void (*seekdir)(struct fd *fd, unsigned long ptr);

    // map the file
    int (*mmap)(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);

    // returns a bitmask of operations that won't block
    int (*poll)(struct fd *fd);

    // returns the size needed for the output of ioctl, 0 if the arg is not a
    // pointer, -1 for invalid command
    ssize_t (*ioctl_size)(int cmd);
    // if ioctl_size returns non-zero, arg must point to ioctl_size valid bytes
    int (*ioctl)(struct fd *fd, int cmd, void *arg);

    int (*fsync)(struct fd *fd);
    int (*close)(struct fd *fd);

    // handle F_GETFL, i.e. return open flags for this fd
    int (*getflags)(struct fd *fd);
    // handle F_SETFL, i.e. set O_NONBLOCK
    int (*setflags)(struct fd *fd, dword_t arg);
};

struct fdtable {
    atomic_uint refcount;
    unsigned size;
    struct fd **files;
    bits_t *cloexec;
    lock_t lock;
};

struct fdtable *fdtable_new(int size);
void fdtable_release(struct fdtable *table);
struct fdtable *fdtable_copy(struct fdtable *table);
void fdtable_free(struct fdtable *table);
void fdtable_do_cloexec(struct fdtable *table);
struct fd *fdtable_get(struct fdtable *table, fd_t f);

struct fd *f_get(fd_t f);
struct fd *f_get_retained(fd_t f);
// steals a reference to the fd, gives it to the table on success and destroys it on error
// flags is checked for O_CLOEXEC and O_NONBLOCK
fd_t f_install(struct fd *fd, int flags);
int f_close(fd_t f);

#endif
