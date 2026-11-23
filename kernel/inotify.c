// inotify: inotify_init1, inotify_add_watch, inotify_rm_watch, and the events
// the filesystem operations report (kernel/inotify.h).
//
// A watch is on an inode, (mount, inode number), as Linux's are: a watched
// directory renamed is still watched. The watches are kept in a table by
// inode, so that an event costs a lookup however many there are (a file
// watcher has one per directory of a tree). An instance's events queue up to
// Linux's default max_queued_events; then IN_Q_OVERFLOW ends the queue. An
// event is merged into the last one queued if they are the same, as Linux
// does.
//
// inotify_lock covers the table, every instance's watches and events. It is
// taken by nothing else's code: the operations that report events stat what
// they need before taking it, and an instance's poll reads its count without
// it, so that waking a poll under it cannot deadlock against a poll that
// asks this instance while holding its own lock.

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/inotify.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/poll.h"
#include "fs/stat.h"
#include "util/list.h"
#include "util/sync.h"

#define IN_CLOEXEC_ O_CLOEXEC_
#define IN_NONBLOCK_ O_NONBLOCK_
// inotify_add_watch flags
#define IN_ONLYDIR_ 0x01000000
#define IN_DONT_FOLLOW_ 0x02000000
#define IN_EXCL_UNLINK_ 0x04000000
#define IN_MASK_CREATE_ 0x10000000
#define IN_MASK_ADD_ 0x20000000
#define IN_ONESHOT_ 0x80000000

// Linux's default /proc/sys/fs/inotify/max_queued_events
#define MAX_QUEUED_EVENTS 16384

struct inotify_event_ {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len; // of the name after it, NUL-padded to a multiple of this struct's size
};

struct queued_event {
    struct list link;
    struct inotify_event_ event;
    char name[];
};

// An inotify instance: what its fd has
struct inotify {
    struct fd *fd;
    struct list watches;
    int32_t last_wd;
    struct list events;
    _Atomic unsigned nevents; // read by inotify_poll without the lock
    size_t bytes;             // of the events as read gives them (FIONREAD)
    bool overflowed;          // IN_Q_OVERFLOW is queued
};

struct watch {
    struct list bucket;       // in watch_table
    struct list link;         // in its inotify's watches
    struct inotify *inotify;
    int32_t wd;
    struct mount *mount;      // compared, never followed: no reference
    ino_t ino;
    uint32_t mask;            // events, IN_ONESHOT_, IN_EXCL_UNLINK_
};

#define WATCH_BUCKETS 1024
static struct list watch_table[WATCH_BUCKETS];
static lock_t inotify_lock = LOCK_INITIALIZER;
_Atomic uint32_t inotify_watched;
static unsigned watching[32]; // per event bit, the watches asking for it
static _Atomic uint32_t last_cookie;

static struct fd_ops inotify_ops;

static struct list *bucket_of(struct mount *mount, ino_t ino) {
    struct list *bucket = &watch_table[(((uintptr_t) mount >> 6) ^ ino) % WATCH_BUCKETS];
    if (list_null(bucket))
        list_init(bucket);
    return bucket;
}

// A watch's events counted in or out of inotify_watched
static void count_events(uint32_t mask, int delta) {
    uint32_t watched = 0;
    for (int bit = 0; bit < 32; bit++) {
        if (mask & IN_ALL_EVENTS_ & (1u << bit))
            watching[bit] += delta;
        if (watching[bit] != 0)
            watched |= 1u << bit;
    }
    atomic_store(&inotify_watched, watched);
}

static void queue_event(struct inotify *in, int32_t wd, uint32_t mask, uint32_t cookie, const char *name) {
    uint32_t len = 0;
    if (name != NULL) {
        size_t size = sizeof(struct inotify_event_);
        len = (uint32_t) ((strlen(name) + 1 + size - 1) / size * size);
    }
    if (!list_empty(&in->events)) {
        struct queued_event *last = list_entry(in->events.prev, struct queued_event, link);
        if (last->event.wd == wd && last->event.mask == mask && last->event.cookie == cookie &&
                last->event.len == len && (len == 0 || strcmp(last->name, name) == 0))
            return;
    }
    if (in->nevents >= MAX_QUEUED_EVENTS) {
        if (in->overflowed)
            return;
        in->overflowed = true;
        wd = -1;
        mask = IN_Q_OVERFLOW_;
        cookie = 0;
        len = 0;
    }
    struct queued_event *qe = calloc(1, sizeof(*qe) + len);
    if (qe == NULL)
        return;
    qe->event = (struct inotify_event_) {.wd = wd, .mask = mask, .cookie = cookie, .len = len};
    if (len != 0)
        strcpy(qe->name, name);
    list_add_tail(&in->events, &qe->link);
    in->nevents++;
    in->bytes += sizeof(qe->event) + len;
    notify(&in->fd->cond);
    poll_wakeup(in->fd, POLL_READ);
}

// The watch ends: IN_IGNORED, and it is gone
static void watch_end(struct watch *watch) {
    queue_event(watch->inotify, watch->wd, IN_IGNORED_, 0, NULL);
    list_remove(&watch->bucket);
    list_remove(&watch->link);
    count_events(watch->mask, -1);
    free(watch);
}

// The event to every watch on (mount, ino) that asks for it
static void deliver(struct mount *mount, ino_t ino, uint32_t mask, uint32_t cookie, const char *name) {
    struct list *bucket = bucket_of(mount, ino);
    struct watch *watch, *tmp;
    list_for_each_entry_safe(bucket, watch, tmp, bucket) {
        if (watch->mount != mount || watch->ino != ino || !(watch->mask & mask & IN_ALL_EVENTS_))
            continue;
        queue_event(watch->inotify, watch->wd, mask, cookie, name);
        if (watch->mask & IN_ONESHOT_)
            watch_end(watch);
    }
}

// Whether some watch on (mount, ino) asks for an event in mask
static bool is_watched(struct mount *mount, ino_t ino, uint32_t mask) {
    struct watch *watch;
    list_for_each_entry(bucket_of(mount, ino), watch, bucket) {
        if (watch->mount == mount && watch->ino == ino && (watch->mask & mask & IN_ALL_EVENTS_))
            return true;
    }
    return false;
}

// path's directory and its name in it; false for a mount's root, which has
// none in this mount
static bool split_path(const char *path, char *dir, const char **name) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0')
        return false;
    memcpy(dir, path, slash - path);
    dir[slash - path] = '\0';
    *name = slash + 1;
    return true;
}

void inotify_path(struct mount *mount, const char *path, uint32_t mask) {
    if (!inotify_wants(mask))
        return;
    struct statbuf stat, dir_stat;
    if (mount->fs->stat(mount, path, &stat) < 0)
        return;
    if (S_ISDIR(stat.mode))
        mask |= IN_ISDIR_;
    char dir[MAX_PATH];
    const char *name;
    bool has_dir = split_path(path, dir, &name) && mount->fs->stat(mount, dir, &dir_stat) >= 0;
    lock(&inotify_lock);
    if (has_dir)
        deliver(mount, dir_stat.inode, mask, 0, name);
    deliver(mount, stat.inode, mask, 0, NULL);
    unlock(&inotify_lock);
}

void inotify_child(struct mount *mount, const char *path, uint32_t mask, uint32_t cookie) {
    if (!inotify_wants(mask))
        return;
    char dir[MAX_PATH];
    const char *name;
    struct statbuf dir_stat;
    if (!split_path(path, dir, &name) || mount->fs->stat(mount, dir, &dir_stat) < 0)
        return;
    lock(&inotify_lock);
    deliver(mount, dir_stat.inode, mask, cookie, name);
    unlock(&inotify_lock);
}

void inotify_inode(struct mount *mount, ino_t ino, uint32_t mask) {
    if (!inotify_wants(mask))
        return;
    lock(&inotify_lock);
    deliver(mount, ino, mask, 0, NULL);
    unlock(&inotify_lock);
}

void inotify_inode_gone(struct mount *mount, ino_t ino) {
    if (atomic_load(&inotify_watched) == 0)
        return;
    lock(&inotify_lock);
    deliver(mount, ino, IN_DELETE_SELF_, 0, NULL);
    // every watch on it ends, whatever it asked for
    struct watch *watch, *tmp;
    list_for_each_entry_safe(bucket_of(mount, ino), watch, tmp, bucket) {
        if (watch->mount == mount && watch->ino == ino)
            watch_end(watch);
    }
    unlock(&inotify_lock);
}

void inotify_fd(struct fd *fd, uint32_t mask) {
    if (!inotify_wants(mask) || fd->inode == NULL)
        return;
    if (S_ISDIR(fd->type))
        mask |= IN_ISDIR_;
    struct mount *mount = fd->inode->mount;
    ino_t ino = fd->inode->number;
    // Its directory, from its path now (it may have been renamed since it
    // was opened). A path that is no longer this file (it was unlinked) has
    // no directory to tell.
    char path[MAX_PATH], dir[MAX_PATH];
    const char *name;
    struct statbuf dir_stat, stat;
    bool has_dir = mount->fs->getpath != NULL && mount->fs->getpath(fd, path) >= 0 &&
            split_path(path, dir, &name) && mount->fs->stat(mount, dir, &dir_stat) >= 0;
    if (has_dir) {
        lock(&inotify_lock);
        has_dir = is_watched(mount, dir_stat.inode, mask);
        unlock(&inotify_lock);
    }
    has_dir = has_dir && mount->fs->stat(mount, path, &stat) >= 0 && stat.inode == ino;
    lock(&inotify_lock);
    if (has_dir)
        deliver(mount, dir_stat.inode, mask, 0, name);
    deliver(mount, ino, mask, 0, NULL);
    unlock(&inotify_lock);
}

uint32_t inotify_cookie(void) {
    uint32_t cookie;
    do
        cookie = atomic_fetch_add(&last_cookie, 1) + 1;
    while (cookie == 0);
    return cookie;
}

fd_t sys_inotify_init1(int_t flags) {
    STRACE("inotify_init1(%#x)", flags);
    if (flags & ~(IN_CLOEXEC_|IN_NONBLOCK_))
        return _EINVAL;
    struct inotify *in = calloc(1, sizeof(*in));
    if (in == NULL)
        return _ENOMEM;
    struct fd *fd = adhoc_fd_create(&inotify_ops);
    if (fd == NULL) {
        free(in);
        return _ENOMEM;
    }
    list_init(&in->watches);
    list_init(&in->events);
    in->fd = fd;
    fd->data = in;
    return f_install(fd, flags);
}

fd_t sys_inotify_init(void) {
    return sys_inotify_init1(0);
}

int_t sys_inotify_add_watch(fd_t inotify_f, addr_t path_addr, uint_t mask) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("inotify_add_watch(%d, \"%s\", %#x)", inotify_f, path, mask);
    struct fd *fd = f_get(inotify_f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &inotify_ops)
        return _EINVAL;
    if (!(mask & IN_ALL_EVENTS_) || ((mask & IN_MASK_ADD_) && (mask & IN_MASK_CREATE_)))
        return _EINVAL;

    char normal[MAX_PATH];
    int err = path_normalize(AT_PWD, path, normal, mask & IN_DONT_FOLLOW_ ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(normal);
    struct statbuf stat;
    err = mount->fs->stat(mount, normal, &stat);
    mount_release(mount);
    if (err < 0)
        return err;
    if ((mask & IN_ONLYDIR_) && !S_ISDIR(stat.mode))
        return _ENOTDIR;
    err = access_check(&stat, AC_R);
    if (err < 0)
        return err;

    struct inotify *in = fd->data;
    uint32_t kept = mask & (IN_ALL_EVENTS_ | IN_ONESHOT_ | IN_EXCL_UNLINK_);
    lock(&inotify_lock);
    struct watch *watch;
    list_for_each_entry(&in->watches, watch, link) {
        if (watch->mount == mount && watch->ino == stat.inode) {
            int_t wd = watch->wd;
            if (mask & IN_MASK_CREATE_) {
                wd = _EEXIST;
            } else {
                count_events(watch->mask, -1);
                watch->mask = mask & IN_MASK_ADD_ ? watch->mask | kept : kept;
                count_events(watch->mask, +1);
            }
            unlock(&inotify_lock);
            return wd;
        }
    }
    watch = malloc(sizeof(*watch));
    if (watch == NULL) {
        unlock(&inotify_lock);
        return _ENOMEM;
    }
    *watch = (struct watch) {
        .inotify = in,
        .wd = ++in->last_wd,
        .mount = mount,
        .ino = stat.inode,
        .mask = kept,
    };
    list_add_tail(bucket_of(mount, stat.inode), &watch->bucket);
    list_add_tail(&in->watches, &watch->link);
    count_events(kept, +1);
    int_t wd = watch->wd;
    unlock(&inotify_lock);
    return wd;
}

int_t sys_inotify_rm_watch(fd_t inotify_f, int_t wd) {
    STRACE("inotify_rm_watch(%d, %d)", inotify_f, wd);
    struct fd *fd = f_get(inotify_f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &inotify_ops)
        return _EINVAL;
    struct inotify *in = fd->data;
    lock(&inotify_lock);
    struct watch *watch;
    list_for_each_entry(&in->watches, watch, link) {
        if (watch->wd == wd) {
            watch_end(watch);
            unlock(&inotify_lock);
            return 0;
        }
    }
    unlock(&inotify_lock);
    return _EINVAL;
}

// As many whole events as fit; EINVAL if not even the first does
static ssize_t inotify_read(struct fd *fd, void *buf, size_t bufsize) {
    struct inotify *in = fd->data;
    lock(&inotify_lock);
    while (list_empty(&in->events)) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&inotify_lock);
            return _EAGAIN;
        }
        if (wait_for(&fd->cond, &inotify_lock, NULL)) {
            unlock(&inotify_lock);
            return _EINTR;
        }
    }
    size_t n = 0;
    while (!list_empty(&in->events)) {
        struct queued_event *qe = list_first_entry(&in->events, struct queued_event, link);
        size_t size = sizeof(qe->event) + qe->event.len;
        if (n + size > bufsize)
            break;
        memcpy((char *) buf + n, &qe->event, sizeof(qe->event));
        memcpy((char *) buf + n + sizeof(qe->event), qe->name, qe->event.len);
        n += size;
        if (qe->event.mask & IN_Q_OVERFLOW_)
            in->overflowed = false;
        in->nevents--;
        in->bytes -= size;
        list_remove(&qe->link);
        free(qe);
    }
    unlock(&inotify_lock);
    return n == 0 ? _EINVAL : (ssize_t) n;
}

static int inotify_poll(struct fd *fd) {
    struct inotify *in = fd->data;
    return atomic_load(&in->nevents) != 0 ? POLL_READ : 0;
}

static ssize_t inotify_ioctl_size(int cmd) {
    return cmd == FIONREAD_ ? (ssize_t) sizeof(dword_t) : -1;
}

static int inotify_ioctl(struct fd *fd, int cmd, void *arg) {
    struct inotify *in = fd->data;
    switch (cmd) {
        case FIONREAD_:
            lock(&inotify_lock);
            *(dword_t *) arg = (dword_t) in->bytes;
            unlock(&inotify_lock);
            return 0;
    }
    return _ENOTTY;
}

static int inotify_close(struct fd *fd) {
    struct inotify *in = fd->data;
    lock(&inotify_lock);
    struct watch *watch, *tmp_watch;
    list_for_each_entry_safe(&in->watches, watch, tmp_watch, link) {
        list_remove(&watch->bucket);
        count_events(watch->mask, -1);
        free(watch);
    }
    struct queued_event *qe, *tmp_event;
    list_for_each_entry_safe(&in->events, qe, tmp_event, link)
        free(qe);
    unlock(&inotify_lock);
    free(in);
    return 0;
}

static struct fd_ops inotify_ops = {
    .read = inotify_read,
    .poll = inotify_poll,
    .ioctl_size = inotify_ioctl_size,
    .ioctl = inotify_ioctl,
    .close = inotify_close,
};
