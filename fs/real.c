#include <string.h>
#include "util/verbosetrace.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/param.h>

#include "debug.h"
#include "fs/pipe.h"
#include "misc.h"
#include "kernel/errno.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/real.h"
#define ISH_INTERNAL
#include "fs/fake.h"
#include "fs/tty.h"
#include "util/fchdir.h"

// The longest path the host takes, its NUL with it: Darwin's 1024 bytes
// (MAXPATHLEN), where the guest's are 4096 (MAX_PATH) as Linux's are
#if defined(__APPLE__)
#define HOST_PATH_MAX MAXPATHLEN
#else
#define HOST_PATH_MAX PATH_MAX
#endif

int realfs_at_begin(int root_fd, const char *path, struct realfs_at *at) {
    at->root = root_fd;
    at->fd = root_fd;
    at->path = fix_path(path);
    while (strlen(at->path) >= HOST_PATH_MAX) {
        // the longest part of it that ends at a slash and the host takes
        const char *cut = NULL;
        for (const char *p = at->path; p < at->path + HOST_PATH_MAX - 1; p++) {
            if (*p == '/')
                cut = p;
        }
        if (cut == NULL) {
            realfs_at_end(at);
            return _ENAMETOOLONG;  // a name longer than the host's paths, which NAME_MAX refuses anyway
        }
        char dir[HOST_PATH_MAX];
        memcpy(dir, at->path, (size_t) (cut - at->path));
        dir[cut - at->path] = '\0';
#ifdef O_SEARCH
        // searched only, as a path's directories are: no read permission asked
        int fd = openat(at->fd, dir, O_SEARCH | O_CLOEXEC);
#else
        int fd = openat(at->fd, dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#endif
        if (fd < 0) {
            int err = errno_map();
            realfs_at_end(at);
            return err;
        }
        realfs_at_end(at);
        at->fd = fd;
        at->path = cut + 1;
    }
    return 0;
}

void realfs_at_end(struct realfs_at *at) {
    if (at->fd != at->root) {
        int saved = errno;
        close(at->fd);
        errno = saved;
    }
    at->fd = at->root;
}

#if defined(__APPLE__)
// The host path of directory fd, which F_GETPATH cannot say (ENOSPC: past
// MAXPATHLEN): its name in its parent, found there by its inode, and its
// parent's, and so on up to a directory whose path F_GETPATH says, as getcwd
// finds one. Right after a rename as F_GETPATH is. A file other than a
// directory has no parent to ask: ENAMETOOLONG.
static int getpath_by_parents(int fd, char *buf) {
    struct stat st;
    if (fstat(fd, &st) < 0)
        return errno_map();
    if (!S_ISDIR(st.st_mode))
        return _ENAMETOOLONG;
    // the names found, at the end of tail: "/a/b"
    char tail[MAX_PATH];
    size_t start = sizeof(tail) - 1;
    tail[start] = '\0';
    int dir = dup(fd);
    if (dir < 0)
        return errno_map();
    int err = 0;
    for (;;) {
        int parent = openat(dir, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) {
            err = errno_map();
            break;
        }
        int scan = dup(parent);
        DIR *entries = scan < 0 ? NULL : fdopendir(scan);
        if (entries == NULL) {
            err = errno_map();
            if (scan >= 0)
                close(scan);
            close(parent);
            break;
        }
        bool found = false;
        struct dirent *entry;
        while (!found && (entry = readdir(entries)) != NULL) {
            struct stat entry_st;
            if (entry->d_ino != st.st_ino || strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
                    fstatat(parent, entry->d_name, &entry_st, AT_SYMLINK_NOFOLLOW) < 0 ||
                    entry_st.st_dev != st.st_dev || entry_st.st_ino != st.st_ino)
                continue;
            size_t len = strlen(entry->d_name);
            if (len + 1 > start) {
                err = _ENAMETOOLONG;
                break;
            }
            start -= len;
            memcpy(tail + start, entry->d_name, len);
            tail[--start] = '/';
            found = true;
        }
        closedir(entries);
        close(dir);
        dir = parent;
        if (err < 0)
            break;
        if (!found) {
            err = _ENOENT;  // gone from its parent meanwhile
            break;
        }
        if (fstat(dir, &st) < 0) {
            err = errno_map();
            break;
        }
        if (fcntl(dir, F_GETPATH, buf) == 0) {
            size_t len = strlen(buf);
            if (len + (sizeof(tail) - 1 - start) >= MAX_PATH)
                err = _ENAMETOOLONG;
            else
                memcpy(buf + len, tail + start, sizeof(tail) - start);
            break;
        }
        if (errno != ENOSPC) {
            err = errno_map();
            break;
        }
    }
    close(dir);
    return err;
}
#endif

// The host's path of fd, or the host's error: it was its -1, which is
// -EPERM, and a directory whose host path F_GETPATH cannot say (past
// MAXPATHLEN, 1024 bytes with the app's container's) could be read none of:
// fakefs_readdir asks it, and every listdir in it said EPERM (CPython's
// test_os, test_getcwd_long_path, and its cleanup's rmtree). A directory's
// is found by its parents then (getpath_by_parents).
static int getpath(int fd, char *buf) {
#if defined(__linux__)
    char proc_fd[20];
    sprintf(proc_fd, "/proc/self/fd/%d", fd);
    ssize_t size = readlink(proc_fd, buf, MAX_PATH - 1);
    if (size < 0)
        return errno_map();
    buf[size] = '\0';
    return 0;
#elif defined(__APPLE__)
    if (fcntl(fd, F_GETPATH, buf) == 0)
        return 0;
    if (errno != ENOSPC)
        return errno_map();
    return getpath_by_parents(fd, buf);
#endif
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunreachable-code"
static int open_flags_real_from_fake(int flags) {
    int real_flags = 0;
    if (flags & O_RDONLY_) real_flags |= O_RDONLY;
    if (flags & O_WRONLY_) real_flags |= O_WRONLY;
    if (flags & O_RDWR_) real_flags |= O_RDWR;
    if (flags & O_CREAT_) real_flags |= O_CREAT;
    if (flags & O_EXCL_) real_flags |= O_EXCL;
    if (flags & O_TRUNC_) real_flags |= O_TRUNC;
    if (flags & O_APPEND_) real_flags |= O_APPEND;
    if (flags & O_NONBLOCK_) real_flags |= O_NONBLOCK;
    if (flags & O_NOFOLLOW_) real_flags |= O_NOFOLLOW;
#ifdef O_DIRECTORY
    // generic_openat says ENOTDIR of what the guest takes for no directory,
    // after this open; the host's says it before it opens the file, as
    // Linux's does. A FIFO's open waits for its other end: os.walk (scandir)
    // of one waited for good, where Linux says ENOTDIR at once (CPython's
    // test_walk_named_pipe). A guest's directory is the host's, in fakefs
    // too, and what is none is none on the host either.
    if (flags & O_DIRECTORY_) real_flags |= O_DIRECTORY;
#endif
    /* O_PATH/O_DIRECT have no Darwin equivalent (F_NOCACHE is the closest to
     * O_DIRECT and is advisory), so they stay guest-side only. */
    return real_flags;
}

static int open_flags_fake_from_real(int flags) {
    int fake_flags = 0;
    if (flags & O_RDONLY) fake_flags |= O_RDONLY_;
    if (flags & O_WRONLY) fake_flags |= O_WRONLY_;
    if (flags & O_RDWR) fake_flags |= O_RDWR_;
    if (flags & O_CREAT) fake_flags |= O_CREAT_;
    if (flags & O_EXCL) fake_flags |= O_EXCL_;
    if (flags & O_TRUNC) fake_flags |= O_TRUNC_;
    if (flags & O_APPEND) fake_flags |= O_APPEND_;
    if (flags & O_NONBLOCK) fake_flags |= O_NONBLOCK_;
    if (flags & O_NOFOLLOW) fake_flags |= O_NOFOLLOW_;
    return fake_flags;
}
#pragma clang diagnostic pop

// Map well-known /dev paths to device numbers. Returns 0 if not recognized.
static dev_t_ realfs_devnum_for_path(const char *path) {
    if (strncmp(path, "/dev/", 5) != 0)
        return 0;
    const char *devname = path + 5;
    if (strcmp(devname, "null") == 0)
        return dev_make(MEM_MAJOR, DEV_NULL_MINOR);
    if (strcmp(devname, "zero") == 0)
        return dev_make(MEM_MAJOR, DEV_ZERO_MINOR);
    if (strcmp(devname, "full") == 0)
        return dev_make(MEM_MAJOR, DEV_FULL_MINOR);
    if (strcmp(devname, "random") == 0)
        return dev_make(MEM_MAJOR, DEV_RANDOM_MINOR);
    if (strcmp(devname, "urandom") == 0)
        return dev_make(MEM_MAJOR, DEV_URANDOM_MINOR);
    if (strcmp(devname, "tty") == 0)
        return dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR);
    if (strcmp(devname, "console") == 0)
        return dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR);
    if (strcmp(devname, "ptmx") == 0)
        return dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR);
    return 0;
}

// What realfs makes has the mode asked for, which has the guest's umask
// applied already (sys_openat, sys_mkdirat, sys_mknodat): the host's umask,
// applied again, took bits from it (group write under an umask of 022:
// CPython's test_subprocess test_umask, through iSH's Linux CLI). fakefs,
// which makes its files through realfs with modes of its own (0666, 0777)
// and keeps the guest's in its database, keeps the host's umask on them.
static bool realfs_exact_mode(struct mount *mount) {
    return mount->fs == &realfs;
}

struct fd *realfs_open(struct mount *mount, const char *path, int flags, int mode) {
    int real_flags = open_flags_real_from_fake(flags);
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return ERR_PTR(err);
    int fd_no;
    if (flags & O_CREAT_ && realfs_exact_mode(mount)) {
        // made with O_EXCL to know it is made here, then given its mode; one
        // there already is opened as asked (made meanwhile by another, it
        // has the host's umask, as before)
        fd_no = openat(at.fd, at.path, real_flags | O_EXCL, mode);
        if (fd_no >= 0)
            fchmod(fd_no, mode & 07777);
        else if (errno == EEXIST && !(flags & O_EXCL_))
            fd_no = openat(at.fd, at.path, real_flags, mode);
    } else {
        fd_no = openat(at.fd, at.path, real_flags, mode);
    }
    realfs_at_end(&at);
    if (fd_no < 0)
        return ERR_PTR(errno_map());
    struct fd *fd = fd_create(&realfs_fdops);
    fd->real_fd = fd_no;
    fd->dir = NULL;

    /* Bind-mount change tracker: any open that may modify the file fires an
     * upsert event. The hook is intentionally agnostic about path layout —
     * we just emit whatever path realfs received. The Swift consumer
     * decides whether the path is one it cares about. */
    if (flags & (O_CREAT_ | O_TRUNC_ | O_WRONLY_ | O_RDWR_ | O_APPEND_)) {
        fakefs_record_change(path, FAKEFS_CHANGE_OP_WRITE);
    }
    return fd;
}

int realfs_close(struct fd *fd) {
    if (fd->dir != NULL)
        closedir(fd->dir);
    if (fd->pipe_size != NULL)
        pipe_size_release(fd);
    int proxy = atomic_load(&fd->flock_proxy);
    if (proxy != 0)
        close(proxy - 1);   // drops this description's flock, like closing the file would
    int err = close(fd->real_fd);
    if (err < 0)
        return errno_map();
    return 0;
}

static void copy_stat(struct statbuf *fake_stat, struct stat *real_stat) {
    fake_stat->dev = dev_fake_from_real(real_stat->st_dev);
    fake_stat->inode = real_stat->st_ino;
    fake_stat->mode = real_stat->st_mode;
    fake_stat->nlink = real_stat->st_nlink;
    fake_stat->uid = real_stat->st_uid;
    fake_stat->gid = real_stat->st_gid;
    fake_stat->rdev = dev_fake_from_real(real_stat->st_rdev);
    fake_stat->size = real_stat->st_size;
    fake_stat->blksize = real_stat->st_blksize;
    fake_stat->blocks = real_stat->st_blocks;
    fake_stat->atime = real_stat->st_atime;
    fake_stat->mtime = real_stat->st_mtime;
    fake_stat->ctime = real_stat->st_ctime;
#if __APPLE__
#define TIMESPEC(x) st_##x##timespec
#elif __linux__
#define TIMESPEC(x) st_##x##tim
#endif
    fake_stat->atime_nsec = real_stat->TIMESPEC(a).tv_nsec;
    fake_stat->mtime_nsec = real_stat->TIMESPEC(m).tv_nsec;
    fake_stat->ctime_nsec = real_stat->TIMESPEC(c).tv_nsec;
#undef TIMESPEC
}

int realfs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat) {
    struct stat real_stat;
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    int res = fstatat(at.fd, at.path, &real_stat, AT_SYMLINK_NOFOLLOW);
    realfs_at_end(&at);
    if (res < 0)
        return errno_map();
    copy_stat(fake_stat, &real_stat);
    // Override mode/rdev for well-known device paths backed by placeholder files
    dev_t_ devnum = realfs_devnum_for_path(path);
    if (devnum != 0) {
        fake_stat->mode = S_IFCHR | 0666;
        fake_stat->rdev = devnum;
    }
    return 0;
}

int realfs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    struct stat real_stat;
    if (fstat(fd->real_fd, &real_stat) < 0)
        return errno_map();
    copy_stat(fake_stat, &real_stat);
    return 0;
}

// A read or write that can wait for good, on a pipe or FIFO in blocking mode,
// ends with EINTR when the task has a signal to take, as on Linux (and
// receive_signals starts it again if no handler runs). The host call is made
// once poll says it is ready, poll waiting a second at a time and checking in
// between: the SIGUSR1 that wakes a thread for a signal interrupts only a
// host call it is already in. The call used to be retried on every EINTR, so
// no signal could end its wait: not SIGINT's handler (Python's Ctrl-C while
// reading a child's output), not SIGSTOP (Ctrl-Z). 0 when ready.
//
// An end not open the way it is used is EBADF, as Linux says of it before
// anything else: it would never be ready, and on a Linux host (iSH's Linux
// CLI) poll never says a pipe's read end writable, so a write to it waited
// for good (CPython's test_signal, test_wakeup_write_error). Asked only when
// it is not ready at once, not on the way of every read and write.
static int realfs_wait_ready(struct fd *fd, short events) {
    if (!S_ISFIFO(fd->type) || (fd->flags & O_NONBLOCK_) || current == NULL)
        return 0;
    struct pollfd ready = {.fd = fd->real_fd, .events = events};
    if (poll(&ready, 1, 0) > 0)
        return 0;
    if ((fcntl(fd->real_fd, F_GETFL) & O_ACCMODE) == (events & POLLOUT ? O_RDONLY : O_WRONLY))
        return _EBADF;
    for (;;) {
        if (task_signal_pending(current))
            return _EINTR;
        struct pollfd p = {.fd = fd->real_fd, .events = events};
        int n = poll(&p, 1, 1000);
        if (n > 0)
            return 0; // ready, or an error or hangup the call will tell
        if (n < 0 && errno != EINTR)
            return errno_map();
    }
}

// Darwin's poll never says a FIFO's read end ready once its last writer has
// gone: no POLLIN, no POLLHUP, where read says 0. A blocking read of one,
// made once poll said ready, waited for good at the end of the data: cat of
// a FIFO, CPython's test_logging (test_should_not_rollover_named_pipe). So
// it is read without blocking, and while there is nothing, waited for in
// poll a tenth of a second at a time and read again: the end comes as it
// is, 0, within that. The host's fd is made non-blocking for each such read
// (its flags are its open file's, which another reader of it shares, hence
// the lock). Signals end the wait as realfs_wait_ready's. A pipe's poll says
// its end (pipe(2), an adhoc fd): it is read as before.
static lock_t fifo_read_lock = LOCK_INITIALIZER;

static ssize_t realfs_fifo_read(struct fd *fd, void *buf, size_t bufsize) {
    for (;;) {
        lock(&fifo_read_lock);
        int host_flags = fcntl(fd->real_fd, F_GETFL);
        fcntl(fd->real_fd, F_SETFL, host_flags | O_NONBLOCK);
        ssize_t res = read(fd->real_fd, buf, bufsize);
        int read_errno = errno;
        fcntl(fd->real_fd, F_SETFL, host_flags);
        unlock(&fifo_read_lock);
        if (res >= 0)
            return res;
        if (read_errno != EAGAIN && read_errno != EINTR) {
            errno = read_errno;
            return errno_map();
        }
        if (task_signal_pending(current))
            return _EINTR;
        struct pollfd p = {.fd = fd->real_fd, .events = POLLIN};
        poll(&p, 1, 100);
    }
}

ssize_t realfs_read(struct fd *fd, void *buf, size_t bufsize) {
    if (S_ISFIFO(fd->type) && !is_adhoc_fd(fd) && !(fd->flags & O_NONBLOCK_) && current != NULL)
        return realfs_fifo_read(fd, buf, bufsize);
    for (;;) {
        int err = realfs_wait_ready(fd, POLLIN);
        if (err < 0)
            return err;
        ssize_t res = read(fd->real_fd, buf, bufsize);
        if (res >= 0)
            return res;
        if (errno != EINTR)
            return errno_map();
    }
}

ssize_t realfs_write(struct fd *fd, const void *buf, size_t bufsize) {
    for (;;) {
        int err = realfs_wait_ready(fd, POLLOUT);
        if (err < 0)
            return err;
        ssize_t res = write(fd->real_fd, buf, bufsize);
        if (res >= 0)
            return res;
        if (errno != EINTR)
            return errno_map();
    }
}

ssize_t realfs_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    ssize_t res;
    do {
        res = pread(fd->real_fd, buf, bufsize, off);
    } while (res < 0 && errno == EINTR);
    if (res < 0)
        return errno_map();
    return res;
}

ssize_t realfs_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    ssize_t res;
    do {
        res = pwrite(fd->real_fd, buf, bufsize, off);
    } while (res < 0 && errno == EINTR);
    if (res < 0)
        return errno_map();
    return res;
}

void realfs_opendir(struct fd *fd) {
    if (fd->dir == NULL) {
        int dirfd = dup(fd->real_fd);
        if (dirfd < 0) return;
        fd->dir = fdopendir(dirfd);
        if (fd->dir == NULL) {
            // fdopendir failed (fd may not be a directory, or was closed).
            // Close the dup'd fd and leave fd->dir NULL for callers to handle.
            close(dirfd);
        }
    }
}

int realfs_readdir(struct fd *fd, struct dir_entry *entry) {
    realfs_opendir(fd);
    if (fd->dir == NULL) return _EIO;
    // Darwin filenames (APFS/HFS+) can be up to 255 UTF-16 units, which in
    // UTF-8 may exceed Linux NAME_MAX (255 bytes). An unchecked strcpy into
    // entry->name[NAME_MAX + 1] smashes the caller's stack-allocated
    // dir_entry (see sys_getdents64 in fs/dir.c) and trips __stack_chk_fail.
    // Skip any entry whose name doesn't fit so the guest can continue walking.
    for (;;) {
        errno = 0;
        struct dirent *dirent = readdir(fd->dir);
        if (dirent == NULL) {
            if (errno != 0)
                return errno_map();
            return 0;
        }
        size_t namelen = strlen(dirent->d_name);
        if (namelen > NAME_MAX) {
            FIXME("realfs_readdir: skipping entry with name longer than NAME_MAX (%zu bytes)", namelen);
            continue;
        }
        entry->inode = dirent->d_ino;
        memcpy(entry->name, dirent->d_name, namelen + 1);
        return 1;
    }
}

unsigned long realfs_telldir(struct fd *fd) {
    realfs_opendir(fd);
    if (fd->dir == NULL) return 0;
    return telldir(fd->dir);
}

void realfs_seekdir(struct fd *fd, unsigned long ptr) {
    realfs_opendir(fd);
    if (fd->dir == NULL) return;
    // 0, the guest's start (rewinddir is lseek to 0): Darwin's telldir there
    // is 1, and its seekdir(0) goes elsewhere (from the 100th entry of 202 it
    // stayed there; a guest's rewinddir lost "." or ".."). rewinddir() is the
    // start everywhere.
    if (ptr == 0)
        rewinddir(fd->dir);
    else
        seekdir(fd->dir, ptr);
}

off_t realfs_lseek(struct fd *fd, off_t offset, int whence) {
    if (fd->dir != NULL && whence == LSEEK_SET) {
        realfs_seekdir(fd, offset);
        return offset;
    }

    if (whence == LSEEK_SET)
        whence = SEEK_SET;
    else if (whence == LSEEK_CUR)
        whence = SEEK_CUR;
    else if (whence == LSEEK_END)
        whence = SEEK_END;
    else
        return _EINVAL;
    off_t res = lseek(fd->real_fd, offset, whence);
    if (res < 0)
        return errno_map();
    return res;
}

int realfs_poll(struct fd *fd) {
    // A regular file or a directory is always ready, as Linux has it
    // (DEFAULT_POLLMASK, whatever it was opened for). Darwin's poll of a
    // directory says nothing, and a poll of one waited out its timeout.
    // POLLIN and POLLOUT are the same bits on both.
    if (S_ISREG(fd->type) || S_ISDIR(fd->type))
        return POLLIN | POLLOUT;
    struct pollfd p = {.fd = fd->real_fd, .events = POLLPRI};
    // prevent POLLNVAL
    int flags = fcntl(fd->real_fd, F_GETFL, 0);
    if ((flags & O_ACCMODE) != O_WRONLY)
        p.events |= POLLIN;
    if ((flags & O_ACCMODE) != O_RDONLY)
        p.events |= POLLOUT;
    int hup = 0;
#if defined(__APPLE__)
    // A FIFO's read end its writers have closed: Darwin's poll of one says
    // nothing (no POLLIN, no POLLHUP), where Linux's says POLLHUP, and a
    // program's poll of a FIFO for its end waited for good; nor does its
    // kqueue (no EV_EOF, as tried on macOS 15.6: kq_fifo, 2026-11-26), which
    // the build before asked. iSH counts the writers (fifo_hangup), and the
    // last one's close wakes the readers' polls. A pipe's poll (pipe(2), an
    // adhoc fd) says it itself; a Linux host's says it of a FIFO too.
    if (fd->pipe_size != NULL && !is_adhoc_fd(fd) && fifo_hangup(fd))
        hup = POLLHUP;
#endif
    if (poll(&p, 1, 0) <= 0)
        return hup;

#if defined(__APPLE__)
    // this is the "WTF is apple smoking" section

    // https://github.com/apple/darwin-xnu/blob/a449c6a3b8014d9406c2ddbdc81795da24aa7443/bsd/kern/sys_generic.c#L1856
    if (p.revents & POLLHUP)
        p.revents |= POLLOUT;
    // apparently you can sometimes get POLLPRI on a pipe??? please ignore how much of a mess this condition is
    if (is_adhoc_fd(fd) && S_ISFIFO(fd->stat.mode))
        p.revents &= ~POLLPRI;

    if (p.revents & POLLNVAL) {
        printk("pollnval %d flags %d events %d revents %d\n", fd->real_fd, flags, p.events, p.revents);
        // Seriously, fuck Darwin. I just want to poll on POLLIN|POLLOUT|POLLPRI.
        // But if there's almost any kind of error, you just get POLLNVAL back,
        // and no information about the bits that are in fact set. So ask for each
        // separately and ignore a POLLNVAL.
        // This is no longer atomic but I don't really know what to do about that.
        int events = 0;
        static const int pollbits[] = {POLLIN, POLLOUT, POLLPRI};
        for (unsigned i = 0; i < sizeof(pollbits)/sizeof(pollbits[0]); i++) {
            p.events = pollbits[i];
            if (poll(&p, 1, 0) > 0 && !(p.revents & POLLNVAL))
                events |= p.revents;
        }
        assert(!(events & POLLNVAL));
        return events | hup;
    }
#endif

    assert(!(p.revents & POLLNVAL));
    return p.revents | hup;
}

// What pt_map_chunked maps a file's chunks with
struct file_chunks {
    int fd;
    int prot;
    int flags;
    off_t offset;
};

static void *map_file_chunk(size_t from, size_t size, void *ctx) {
    struct file_chunks *chunks = ctx;
    return mmap(NULL, size, chunks->prot, chunks->flags, chunks->fd, chunks->offset + (off_t) from);
}

// Where the host page the file's end falls in ends. A mapping as far as there
// is the file's own: the host zero-fills what lies past the end in that page,
// as POSIX has it. Only beyond it is there no file to map.
static off_t file_pages_end(off_t file_size) {
    return (file_size + real_page_size - 1) / real_page_size * real_page_size;
}

// size bytes of a file from offset, of which the file has file_bytes: an
// anonymous mapping (zeros past the file's end, as for BSS), the file mapped
// over it as far as whole host pages go, the rest read in. *anonymous is how
// much the file is not mapped over, for anon_mmap_count once it is mapped.
static void *map_past_end(int fd, size_t size, off_t offset, size_t file_bytes, int prot, int flags,
                          size_t *anonymous) {
    char *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return MAP_FAILED;
    size_t file_map_size = (file_bytes / real_page_size) * real_page_size;
    if (file_map_size > 0) {
        char *file_map = mmap(memory, file_map_size, prot, flags | MAP_FIXED, fd, offset);
        if (file_map == MAP_FAILED)
            file_map_size = 0;
    }
    *anonymous = size - file_map_size;
    if (file_map_size < file_bytes) {
        size_t remaining = file_bytes - file_map_size;
        size_t total_read = 0;
        while (total_read < remaining) {
            ssize_t n = pread(fd, memory + file_map_size + total_read, remaining - total_read,
                              offset + (off_t) (file_map_size + total_read));
            if (n <= 0) break;
            total_read += n;
        }
    }
    if (!(prot & PROT_WRITE) && file_map_size < size)
        mprotect(memory + file_map_size, size - file_map_size, prot);
    return memory;
}

// A chunk of a mapping that runs past the file's end: the file's own where it
// has the whole chunk, as map_file_chunk maps it, map_past_end's where its end
// falls in the chunk or before it
struct past_end_chunks {
    int fd;
    int prot;
    int flags;
    off_t offset;
    off_t file_size;
    // map_past_end's: its anonymous host mmaps, and how much of them the file
    // is not mapped over, counted once all the chunks are mapped
    uint64_t anonymous_mmaps;
    size_t anonymous;
};

static void *map_past_end_chunk(size_t from, size_t size, void *ctx) {
    struct past_end_chunks *chunks = ctx;
    off_t at = chunks->offset + (off_t) from;
    if (at + (off_t) size <= file_pages_end(chunks->file_size))
        return mmap(NULL, size, chunks->prot, chunks->flags, chunks->fd, at);
    size_t file_bytes = chunks->file_size > at ? (size_t) (chunks->file_size - at) : 0;
    size_t anonymous;
    void *memory = map_past_end(chunks->fd, size, at, file_bytes, chunks->prot, chunks->flags, &anonymous);
    if (memory != MAP_FAILED) {
        chunks->anonymous_mmaps++;
        chunks->anonymous += anonymous;
    }
    return memory;
}

#if defined(__APPLE__)
#define STAT_MTIME(st) ((st).st_mtimespec)
#else
#define STAT_MTIME(st) ((st).st_mtim)
#endif

// A read-only mapping of a file, shared by every process that maps the same
// bytes of the same file the same way. Each JVM maps the JDK's 140MB
// lib/modules and libjvm's 19MB of code, each claude.exe (Claude Code, and
// every grep it runs) 128MB of its own, and each took address space of its
// own for them: the app has a few GB of it in all, and a Gradle build, its
// daemon with a test JVM and kotlinter's workers, ran it out.
//
// Only what no process can write: a private mapping's pages are copy on
// write, as after fork, so a process that makes one writable writes a copy
// of its own; a shared one only from a file opened read-only, which Linux
// does not let be made writable. The file is known by its device, inode, size
// and modification time, so one written since is mapped anew. The table holds
// no reference: the mapping goes when its last page does (data->release), and
// a lookup takes one only while it has others (shared_map_get).
struct shared_map {
    dev_t dev;
    ino_t ino;
    off_t file_size;
    struct timespec mtime;
    off_t offset;
    size_t size;
    int flags; // the host's MAP_SHARED or MAP_PRIVATE
    struct data *data;
    struct shared_map *next;
};
static lock_t shared_maps_lock = LOCK_INITIALIZER;
static struct shared_map *shared_maps;
static _Atomic uint64_t shared_maps_reused, shared_maps_reused_bytes;

static bool shared_map_is(const struct shared_map *map, const struct stat *st, off_t offset, size_t size, int flags) {
    return map->dev == st->st_dev && map->ino == st->st_ino && map->file_size == st->st_size &&
        map->mtime.tv_sec == STAT_MTIME(*st).tv_sec && map->mtime.tv_nsec == STAT_MTIME(*st).tv_nsec &&
        map->offset == offset && map->size == size && map->flags == flags;
}

// The mapping of these bytes some process has, with a reference taken, or NULL
static struct data *shared_map_get(const struct stat *st, off_t offset, size_t size, int flags) {
    struct data *data = NULL;
    lock(&shared_maps_lock);
    for (struct shared_map *map = shared_maps; map != NULL; map = map->next) {
        if (!shared_map_is(map, st, offset, size, flags))
            continue;
        // None left means it is going (data_unref is in shared_map_release, or
        // on its way there): not to be taken back
        unsigned refs = atomic_load(&map->data->refcount);
        while (refs != 0 && !atomic_compare_exchange_weak(&map->data->refcount, &refs, refs + 1))
            ;
        if (refs != 0)
            data = map->data;
        break;
    }
    unlock(&shared_maps_lock);
    return data;
}

// data->release: its last page is gone
static void shared_map_release(struct data *data) {
    lock(&shared_maps_lock);
    for (struct shared_map **map = &shared_maps; *map != NULL; map = &(*map)->next) {
        if ((*map)->data == data) {
            struct shared_map *gone = *map;
            *map = gone->next;
            free(gone);
            break;
        }
    }
    unlock(&shared_maps_lock);
    munmap(data->data, data->size);
    if (data->fd != NULL)
        fd_close(data->fd);
    free(data);
}

void realfs_shared_maps_stats(uint64_t *maps, uint64_t *bytes, uint64_t *reused, uint64_t *reused_bytes) {
    *maps = *bytes = 0;
    lock(&shared_maps_lock);
    for (struct shared_map *map = shared_maps; map != NULL; map = map->next) {
        (*maps)++;
        *bytes += map->size;
    }
    unlock(&shared_maps_lock);
    *reused = atomic_load(&shared_maps_reused);
    *reused_bytes = atomic_load(&shared_maps_reused_bytes);
}

// The host's mapping of the file, memory, made a shared one and mapped. Its
// file is set here, as pt_set_file would set it, before another process can
// find it: two processes' pt_set_file, each under its own lock, would both
// take a reference to a file of their own, and one be lost. It is in the
// table before its pages are mapped, with a reference of its own until they
// are: a process mapping the same file at the same time (two JVMs started
// together, as Gradle starts its workers) finds it while this one is still
// mapping its 35000 pages of lib/modules, not a mapping of its own.
static int shared_map_add(struct mem *mem, page_t start, pages_t pages, void *memory, size_t correction,
                          unsigned flags, struct fd *fd, off_t file_offset, const struct stat *st, off_t offset,
                          size_t size, int map_flags) {
    struct data *data = calloc(1, sizeof(*data));
    struct shared_map *map = calloc(1, sizeof(*map));
    if (data == NULL || map == NULL) {
        free(data);
        free(map);
        munmap(memory, size);
        return _ENOMEM;
    }
    *data = (struct data) {
        .data = memory,
        .size = size,
        .refcount = 1,
        .fd = fd_retain(fd),
        .file_offset = file_offset,
        .release = shared_map_release,
    };
    *map = (struct shared_map) {
        .dev = st->st_dev,
        .ino = st->st_ino,
        .file_size = st->st_size,
        .mtime = STAT_MTIME(*st),
        .offset = offset,
        .size = size,
        .flags = map_flags,
        .data = data,
    };
    lock(&shared_maps_lock);
    map->next = shared_maps;
    shared_maps = map;
    unlock(&shared_maps_lock);
    pt_map_data(mem, start, pages, data, correction, flags);
    data_unref(data); // the pages hold theirs
    return 0;
}

int realfs_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    int mmap_flags = 0;
    if (flags & MMAP_PRIVATE) mmap_flags |= MAP_PRIVATE;
    if (flags & MMAP_SHARED) mmap_flags |= MAP_SHARED;
    int mmap_prot = PROT_READ;
    if (prot & P_WRITE) mmap_prot |= PROT_WRITE;

    off_t real_offset = (offset / real_page_size) * real_page_size;
    off_t correction = offset - real_offset;
    size_t map_size = (pages * PAGE_SIZE) + correction;

    struct stat st;
    int have_stat = (fstat(fd->real_fd, &st) == 0);

    // What no process can write is shared (struct shared_map): a private
    // mapping's pages copy on write, so one made writable is copied first.
    // Also one past the file's end: what lies there is zeros, and the table
    // knows the file by its size and modification time, so the same bytes.
    // musl maps a library so, its code, data and BSS in one mapping from the
    // file before it maps its data and BSS over it: libjvm's 19MB of code.
    bool shareable = have_stat && !(prot & P_WRITE) &&
        ((mmap_flags & MAP_PRIVATE) || (fd->flags & O_ACCMODE_) == O_RDONLY_);
    unsigned shared_flags = prot | ((mmap_flags & MAP_PRIVATE) ? P_COW : 0);
    if (shareable) {
        struct data *data = shared_map_get(&st, real_offset, map_size, mmap_flags);
        if (data != NULL) {
            pt_map_data(mem, start, pages, data, correction, shared_flags);
            data_unref(data); // shared_map_get's: the pages hold theirs
            atomic_fetch_add(&shared_maps_reused, 1);
            atomic_fetch_add(&shared_maps_reused_bytes, map_size);
            return 0;
        }
    }

    // A mapping beyond the host page the file's end falls in. One that ends in
    // that page is the file's (file_pages_end), as most are whose file is not
    // whole pages long: the JVM's of lib/modules, a library's segments.
    if (have_stat && (off_t)(real_offset + map_size) > file_pages_end(st.st_size)) {
        // For MAP_SHARED writable mappings, use a direct file-backed mmap.
        // The host kernel handles beyond-EOF correctly: writes within the
        // file are flushed on munmap, and the zero-filled region between
        // file end and page boundary is discarded. This is what apk needs
        // for its posix_fallocate + mmap(MAP_SHARED) extraction pattern.
        // We must NOT extend the file via ftruncate, because apk doesn't
        // truncate it back, leaving trailing null bytes that corrupt files.
        if ((mmap_flags & MAP_SHARED) && (mmap_prot & PROT_WRITE)) {
            char *memory = mmap(NULL, map_size,
                    mmap_prot, mmap_flags, fd->real_fd, real_offset);
            if (memory != MAP_FAILED) {
                return pt_map(mem, start, pages, memory, correction, prot);
            }
            // mmap failed — fall through to anonymous path
        }

        size_t file_bytes = 0;
        if (st.st_size > real_offset)
            file_bytes = st.st_size - real_offset;
        if (file_bytes > map_size)
            file_bytes = map_size;
        size_t anonymous;
        char *memory = map_past_end(fd->real_fd, map_size, real_offset, file_bytes, mmap_prot, mmap_flags,
                                    &anonymous);
        // In chunks when the host has no free range of the whole, as a mapping
        // within the file is below: a file whose length is not whole pages,
        // mapped whole, comes here, as each JVM maps the JDK's 140MB
        // lib/modules at its start. With no 140MB free that was ENOMEM, and
        // the JVM died of SIGSEGV before saying a word.
        if (memory == MAP_FAILED) {
            struct past_end_chunks chunks = {
                .fd = fd->real_fd,
                .prot = mmap_prot,
                .flags = mmap_flags,
                .offset = real_offset,
                .file_size = st.st_size,
                .anonymous_mmaps = 0,
                .anonymous = 0,
            };
            int err = pt_map_chunked(mem, start, pages, correction, prot, map_past_end_chunk, &chunks);
            if (err >= 0)
                anon_mmap_count(chunks.anonymous_mmaps, chunks.anonymous / PAGE_SIZE);
            return err;
        }
        int err = shareable
            ? shared_map_add(mem, start, pages, memory, correction, shared_flags, fd, offset, &st, real_offset,
                             map_size, mmap_flags)
            : pt_map(mem, start, pages, memory, correction, prot);
        if (err >= 0)
            anon_mmap_count(1, anonymous / PAGE_SIZE);
        return err;
    }

    char *memory = mmap(NULL, map_size,
            mmap_prot, mmap_flags, fd->real_fd, real_offset);
    if (memory == MAP_FAILED) {
        struct file_chunks chunks = {fd->real_fd, mmap_prot, mmap_flags, real_offset};
        return pt_map_chunked(mem, start, pages, correction, prot, map_file_chunk, &chunks);
    }

    if (shareable)
        return shared_map_add(mem, start, pages, memory, correction, shared_flags, fd, offset, &st, real_offset,
                              map_size, mmap_flags);
    return pt_map(mem, start, pages, memory, correction, prot);
}

ssize_t realfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    ssize_t size = readlinkat(at.fd, at.path, buf, bufsize);
    realfs_at_end(&at);
    if (size < 0)
        return errno_map();
    return size;
}

// Whether path is prefix, of length len, or a path under it: "/a/data2" is not
// under "/a/data"
static bool path_under(const char *path, const char *prefix, size_t len) {
    return strncmp(path, prefix, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

int realfs_getpath(struct fd *fd, char *buf) {
    int err = getpath(fd->real_fd, buf);
    if (err < 0)
        return err;

    /* For bind-mounted dirs, F_GETPATH resolves symlinks and returns the
     * host persistent path (e.g. /Users/.../MinisChat/minis/<sid>/attachments).
     * This won't start with mount->source, so the normal prefix strip fails.
     * Detect and translate back to the Linux path. */
    char linux_path[MAX_PATH];
    if (fakefs_bind_mount_resolve_path(buf, linux_path, sizeof(linux_path))) {
        /* [T-ios-log-verbose-tier] Per-resolve trace; see util/verbosetrace.h. */
        if (ish_verbose_trace_enabled && strstr(buf, "minis") != NULL)
            fprintf(stderr, "realfs_getpath: bind_mount_resolve OK: \"%s\" -> \"%s\"\n", buf, linux_path);
        strlcpy(buf, linux_path, MAX_PATH);
        return 0;
    }
    if (ish_verbose_trace_enabled && strstr(buf, "minis") != NULL)
        fprintf(stderr, "realfs_getpath: bind_mount_resolve MISS: F_GETPATH=\"%s\" source=\"%s\"\n", buf, fd->mount->source);

    if (strcmp(fd->mount->source, "/") != 0 || strcmp(buf, "/") == 0) {
        size_t source_len = strlen(fd->mount->source);
        /* Verify buf actually starts with mount->source before stripping.
         * F_GETPATH resolves symlinks, so bind-mounted paths may point outside
         * mount->source when the bind mount table has been cleared (e.g. after
         * app restart before mountMinis re-registers the mounts). */
        if (path_under(buf, fd->mount->source, source_len)) {
            memmove(buf, buf + source_len, MAX_PATH - source_len);
        } else if (strncmp(fd->mount->source, "/var/", 5) == 0 &&
                   strncmp(buf, "/private", 8) == 0 &&
                   path_under(buf + 8, fd->mount->source, source_len)) {
            /* F_GETPATH returns /private/var/... but mount->source is /var/...
             * Strip /private prefix + mount->source from buf. */
            memmove(buf, buf + 8 + source_len, MAX_PATH - 8 - source_len);
        } else {
            // Outside the mount, and no bind mount's (a bind mount's link
            // whose table was cleared: the app restarted, mountMinis yet to
            // register it again): the file has no path in the guest. ENOENT,
            // as Linux's getcwd of a directory out of the root's reach. It
            // was "/", and every call by the fd's path acted on the root
            // instead, saying nothing: futimens set the root's times, a file
            // made by openat from it as a dirfd was made in /. Logged, with
            // what it takes to see why.
            printk("realfs_getpath: fd %d's host path \"%s\" is not under its mount's source \"%s\" (at \"%s\"), nor a bind mount's\n",
                    fd->real_fd, buf, fd->mount->source, fd->mount->point);
            return _ENOENT;
        }
    }
    return 0;
}

int realfs_link(struct mount *mount, const char *src, const char *dst) {
    struct realfs_at src_at, dst_at;
    int err = realfs_at_begin(mount->root_fd, src, &src_at);
    if (err < 0)
        return err;
    err = realfs_at_begin(mount->root_fd, dst, &dst_at);
    if (err < 0) {
        realfs_at_end(&src_at);
        return err;
    }
    int res = linkat(src_at.fd, src_at.path, dst_at.fd, dst_at.path, 0);
    realfs_at_end(&src_at);
    realfs_at_end(&dst_at);
    if (res < 0)
        return errno_map();
    return res;
}

int realfs_unlink(struct mount *mount, const char *path) {
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    int res = unlinkat(at.fd, at.path, 0);
    realfs_at_end(&at);
    if (res < 0)
        return errno_map();
    fakefs_record_change(path, FAKEFS_CHANGE_OP_UNLINK);
    return res;
}

int realfs_rmdir(struct mount *mount, const char *path) {
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    err = unlinkat(at.fd, at.path, AT_REMOVEDIR);
    realfs_at_end(&at);
    if (err < 0)
        return errno_map();
    fakefs_record_change(path, FAKEFS_CHANGE_OP_UNLINK);
    return 0;
}

int realfs_rename(struct mount *mount, const char *src, const char *dst) {
    struct realfs_at src_at, dst_at;
    int err = realfs_at_begin(mount->root_fd, src, &src_at);
    if (err < 0)
        return err;
    err = realfs_at_begin(mount->root_fd, dst, &dst_at);
    if (err < 0) {
        realfs_at_end(&src_at);
        return err;
    }
    err = renameat(src_at.fd, src_at.path, dst_at.fd, dst_at.path);
    realfs_at_end(&src_at);
    realfs_at_end(&dst_at);
    if (err < 0)
        return errno_map();
    /* Source is gone, destination appeared. Emit two events; the Swift
     * consumer filters paths it doesn't care about. */
    fakefs_record_change(src, FAKEFS_CHANGE_OP_UNLINK);
    fakefs_record_change(dst, FAKEFS_CHANGE_OP_RENAME);
    return err;
}

int realfs_symlink(struct mount *mount, const char *target, const char *link) {
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, link, &at);
    if (err < 0)
        return err;
    err = symlinkat(target, at.fd, at.path);
    realfs_at_end(&at);
    if (err < 0)
        return errno_map();
    return err;
}

// The name of a unix socket bound on this filesystem. iSH's socket itself is
// a host one in /tmp (sockaddr_read_bind); this file only has to stat as a
// socket, and on Darwin only bind() makes one (mknod would need root). A host
// path can be longer than sun_path (104 bytes on Darwin), so it is bound by
// its last component, from its directory.
static int realfs_make_socket(struct mount *mount, const char *path, mode_t_ mode) {
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    const char *rel = at.path;
    const char *slash = strrchr(rel, '/');
    const char *name = slash != NULL ? slash + 1 : rel;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(name) >= sizeof(addr.sun_path)) {
        realfs_at_end(&at);
        return _ENAMETOOLONG;
    }
    strcpy(addr.sun_path, name);

    int dirfd = at.fd;
    if (slash != NULL) {
        char dir[MAX_PATH];
        size_t len = (size_t) (slash - rel);
        memcpy(dir, rel, len);
        dir[len] = '\0';
        dirfd = openat(at.fd, dir, O_RDONLY | O_DIRECTORY);
        if (dirfd < 0) {
            err = errno_map();
            realfs_at_end(&at);
            return err;
        }
    }
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        err = errno_map();
        goto out;
    }
    lock_fchdir(dirfd);
    int bound = bind(sock, (struct sockaddr *) &addr, sizeof(addr));
    int bind_errno = errno;
    unlock_fchdir();
    close(sock);
    if (bound < 0) {
        // mknod's error for a name already there
        errno = bind_errno == EADDRINUSE ? EEXIST : bind_errno;
        err = errno_map();
        goto out;
    }
    // bound with the host's umask: the mode asked for
    if (fchmodat(dirfd, name, mode & 07777, 0) < 0)
        err = errno_map();
out:
    if (dirfd != at.fd)
        close(dirfd);
    realfs_at_end(&at);
    return err;
}

int realfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ UNUSED(dev)) {
    if (S_ISSOCK(mode))
        return realfs_make_socket(mount, path, mode);
    if (!S_ISFIFO(mode) && !S_ISREG(mode))
        return _EPERM;
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    if (S_ISFIFO(mode)) {
        lock_fchdir(at.fd);
        err = mkfifo(at.path, mode & ~S_IFMT);
        unlock_fchdir();
    } else {
        err = openat(at.fd, at.path, O_CREAT|O_EXCL|O_RDONLY, mode & ~S_IFMT);
        if (err >= 0)
            err = close(err);
    }
    // the mode asked for, not less the host's umask (realfs_exact_mode)
    if (err >= 0 && realfs_exact_mode(mount))
        err = fchmodat(at.fd, at.path, mode & 07777, 0);
    realfs_at_end(&at);
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_truncate(struct mount *mount, const char *path, off_t_ size) {
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    int fd = openat(at.fd, at.path, O_RDWR);
    realfs_at_end(&at);
    if (fd < 0)
        return errno_map();
    if (ftruncate(fd, size) < 0)
        err = errno_map();
    close(fd);
    if (err == 0)
        fakefs_record_change(path, FAKEFS_CHANGE_OP_TRUNCATE);
    return err;
}

int realfs_setattr(struct mount *mount, const char *path, struct attr attr) {
    if (attr.type == attr_size)
        return realfs_truncate(mount, path, attr.size);
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    switch (attr.type) {
        // The path is the file's: generic_setattrat followed its symlinks, or
        // not, as asked. The host following it again, lchown of a symlink
        // chowned what it points to (a dangling one's ENOENT: CPython's
        // test_lchown). A gid was set as the owner.
        case attr_uid:
            err = fchownat(at.fd, at.path, attr.uid, -1, AT_SYMLINK_NOFOLLOW);
            if (err < 0 && errno == EPERM)
                err = 0; // silently ignore, we're not root on host
            break;
        case attr_gid:
            err = fchownat(at.fd, at.path, -1, attr.gid, AT_SYMLINK_NOFOLLOW);
            if (err < 0 && errno == EPERM)
                err = 0;
            break;
        case attr_mode:
            err = fchmodat(at.fd, at.path, attr.mode, 0);
            break;
        default:
            TODO("other attrs");
    }
    realfs_at_end(&at);
    if (err < 0)
        return errno_map();
    return err;
}

int realfs_fsetattr(struct fd *fd, struct attr attr) {
    int real_fd = fd->real_fd;
    int err;
    switch (attr.type) {
        case attr_uid:
            err = fchown(real_fd, attr.uid, -1);
            if (err < 0 && errno == EPERM)
                return 0;
            break;
        case attr_gid:
            // the group, not the owner (it set the owner to the gid)
            err = fchown(real_fd, -1, attr.gid);
            if (err < 0 && errno == EPERM)
                return 0;
            break;
        case attr_mode:
            err = fchmod(real_fd, attr.mode);
            break;
        case attr_size:
            err = ftruncate(real_fd, attr.size);
            break;
        default: abort();
    }
    if (err < 0)
        return errno_map();
    return err;
}

// The path is the file's: generic_utime followed its symlinks, or not, as
// asked. The host following it again, utimensat with AT_SYMLINK_NOFOLLOW set
// the times of what a symlink points to (a dangling one's ENOENT: CPython's
// shutil.copytree(symlinks=True), whose copystat sets a copied link's).
int realfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime) {
    struct timespec times[2] = {atime, mtime};
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    err = utimensat(at.fd, at.path, times, AT_SYMLINK_NOFOLLOW);
    realfs_at_end(&at);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    struct realfs_at at;
    int err = realfs_at_begin(mount->root_fd, path, &at);
    if (err < 0)
        return err;
    err = mkdirat(at.fd, at.path, mode);
    // the mode asked for, not less the host's umask (realfs_exact_mode); a
    // directory's mode bits and its sticky bit, as Linux's mkdir takes
    if (err >= 0 && realfs_exact_mode(mount))
        err = fchmodat(at.fd, at.path, mode & 01777, 0);
    realfs_at_end(&at);
    if (err < 0)
        return errno_map();
    return 0;
}

// [T-ish-flock-proxy] A guest flock() must not lock the real file: on iOS
// the rootfs lives in the App Group container, and runningboard kills a
// suspended app that still holds a lock outside its own data container
// (0xdead10cc "locked files not in allowed directories"). codex holds a flock
// on ~/.codex/tmp/arg0/*/.lock for its whole run, so iSH died every time it
// went to the background during `codex login`. Lock a stand-in file in the
// app's private temp directory instead, one per real (dev, inode), opened once
// per open file description: host flock() semantics between descriptions,
// processes, fork and close stay exactly the same.
static char flock_dir[PATH_MAX];

static void flock_proxy_dir_init(void) {
    const char *tmp = getenv("TMPDIR");
#if defined(__APPLE__)
    char buf[PATH_MAX];
    if (tmp == NULL && confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf)) > 0)
        tmp = buf;
#endif
    if (tmp == NULL)
        tmp = "/tmp";
    snprintf(flock_dir, sizeof(flock_dir), "%s/ish-flock", tmp);
    mkdir(flock_dir, 0700);
}

static const char *flock_proxy_dir(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, flock_proxy_dir_init);
    return flock_dir;
}

static int flock_proxy_fd(struct fd *fd) {
    int have = atomic_load(&fd->flock_proxy);
    if (have != 0)
        return have - 1;
    struct stat st;
    if (fstat(fd->real_fd, &st) < 0)
        return -1;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%llx-%llx", flock_proxy_dir(),
             (unsigned long long) st.st_dev, (unsigned long long) st.st_ino);
    int pfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (pfd < 0)
        return -1;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&fd->flock_proxy, &expected, pfd + 1)) {
        close(pfd);   // another thread of this description got there first
        return expected - 1;
    }
    return pfd;
}

int realfs_flock(struct fd *fd, int operation) {
    int real_op = 0;
    if (operation & LOCK_SH_) real_op |= LOCK_SH;
    if (operation & LOCK_EX_) real_op |= LOCK_EX;
    if (operation & LOCK_UN_) real_op |= LOCK_UN;
    if (operation & LOCK_NB_) real_op |= LOCK_NB;
    int pfd = flock_proxy_fd(fd);
    if (pfd < 0)
        return errno_map();
    if (flock(pfd, real_op) < 0)
        return errno_map();
    return 0;
}

int realfs_statfs(struct mount *mount, struct statfsbuf *stat) {
    struct statvfs vfs = {};
    fstatvfs(mount->root_fd, &vfs);
    stat->bsize = vfs.f_bsize;
    stat->blocks = vfs.f_blocks;
    stat->bfree = vfs.f_bfree;
    stat->bavail = vfs.f_bavail;
    stat->files = vfs.f_files;
    stat->ffree = vfs.f_ffree;
    stat->namelen = vfs.f_namemax;
    stat->frsize = vfs.f_frsize;
    return 0;
}

int realfs_mount(struct mount *mount) {
    char *source_realpath = realpath(mount->source, NULL);
    if (source_realpath == NULL)
        return errno_map();
    // musl's realpath keeps a leading "//", whose meaning POSIX leaves to the
    // system; Linux and Darwin take it for "/", and the path of an fd they
    // give (getpath), which realfs_getpath strips this from, never has it. A
    // root of "//dir" (a musl host's ish -f //dir) matched no fd's path, and
    // every fd's path came out "/": futimens set the root's times, fexecve
    // ran the root
    if (source_realpath[0] == '/' && source_realpath[1] == '/')
        memmove(source_realpath, source_realpath + 1, strlen(source_realpath));
    free((void *) mount->source);
    mount->source = source_realpath;

    mount->root_fd = open(mount->source, O_DIRECTORY);
    if (mount->root_fd < 0)
        return errno_map();
    return 0;
}

int realfs_fsync(struct fd *fd) {
    int err = fsync(fd->real_fd);
    if (err < 0)
        return errno_map();
    return 0;
}

int realfs_getflags(struct fd *fd) {
    int flags = fcntl(fd->real_fd, F_GETFL);
    if (flags < 0)
        return errno_map();
    return open_flags_fake_from_real(flags);
}

int realfs_setflags(struct fd *fd, dword_t flags) {
    int ret = fcntl(fd->real_fd, F_SETFL, open_flags_real_from_fake(flags));
    if (ret < 0)
        return errno_map();
    // and the guest's, which realfs_wait_ready goes by: a pipe made
    // non-blocking (fcntl, pipe2, FIONBIO) still waited when full or empty
    fd->flags = (fd->flags & ~(O_APPEND_ | O_NONBLOCK_)) | (flags & (O_APPEND_ | O_NONBLOCK_));
    return 0;
}

ssize_t realfs_ioctl_size(int cmd) {
    if (cmd == FIONREAD_)
        return sizeof(dword_t);
    if (cmd == TCGETS_)
        return sizeof(struct termios_);
    if (cmd == TCSETS_ || cmd == TCSETSW_ || cmd == TCSETSF_)
        return sizeof(struct termios_);
    if (cmd == TIOCGWINSZ_)
        return sizeof(struct winsize_);
    return -1;
}

int realfs_ioctl(struct fd *fd, int cmd, void *arg) {
    int err;
    size_t nread;
    switch (cmd) {
        case FIONREAD_:
            err = ioctl(fd->real_fd, FIONREAD, &nread);
            if (err < 0)
                return errno_map();
            *(dword_t *) arg = nread;
            return 0;
        case TCGETS_:
            // For piped stdio fds backed by a real host TTY, return a
            // plausible termios so that musl isatty() succeeds.
            if (isatty(fd->real_fd)) {
                struct termios host_termios;
                if (tcgetattr(fd->real_fd, &host_termios) == 0) {
                    struct termios_ *guest = (struct termios_ *)arg;
                    memset(guest, 0, sizeof(*guest));
                    guest->iflags = host_termios.c_iflag;
                    guest->oflags = host_termios.c_oflag;
                    guest->cflags = host_termios.c_cflag;
                    guest->lflags = host_termios.c_lflag;
                    return 0;
                }
            }
            return _ENOTTY;
        case TIOCGWINSZ_: {
            // libuv calls TIOCGWINSZ during uv_tty_init to get terminal size.
            if (isatty(fd->real_fd)) {
                struct winsize host_ws;
                if (ioctl(fd->real_fd, TIOCGWINSZ, &host_ws) == 0) {
                    struct winsize_ *guest_ws = (struct winsize_ *)arg;
                    guest_ws->row = host_ws.ws_row;
                    guest_ws->col = host_ws.ws_col;
                    guest_ws->xpixel = host_ws.ws_xpixel;
                    guest_ws->ypixel = host_ws.ws_ypixel;
                    return 0;
                }
            }
            return _ENOTTY;
        }
        case TCSETS_:
        case TCSETSW_:
        case TCSETSF_:
            if (ish_exec_trace())
                fprintf(stderr, "TCSETS_DBG: fd=%d cmd=0x%x real_fd=%d isatty=%d\n",
                        fd->real_fd, cmd, fd->real_fd, isatty(fd->real_fd));
            // Node 22's ResetStdio() calls tcsetattr() on any fd it
            // previously saw as a TTY (via uv_guess_handle → TCGETS).
            // For piped stdio backed by a host TTY, apply the termios to
            // the host fd so terminal mode restoration actually takes
            // effect. For anything else, succeed silently — Node will
            // CHECK() on errors other than 0 / -EPERM.
            if (isatty(fd->real_fd)) {
                struct termios_ *guest = (struct termios_ *)arg;
                struct termios host_termios = {0};
                if (tcgetattr(fd->real_fd, &host_termios) == 0) {
                    host_termios.c_iflag = guest->iflags;
                    host_termios.c_oflag = guest->oflags;
                    host_termios.c_cflag = guest->cflags;
                    host_termios.c_lflag = guest->lflags;
                    int how = (cmd == TCSETSW_) ? TCSADRAIN
                            : (cmd == TCSETSF_) ? TCSAFLUSH
                            : TCSANOW;
                    (void)tcsetattr(fd->real_fd, how, &host_termios);
                }
                return 0;
            }
            return 0;
    }
    return _ENOTTY;
}

const struct fs_ops realfs = {
    .name = "real", .magic = 0x7265616c,
    .mount = realfs_mount,
    .statfs = realfs_statfs,

    .open = realfs_open,
    .readlink = realfs_readlink,
    .link = realfs_link,
    .unlink = realfs_unlink,
    .rmdir = realfs_rmdir,
    .rename = realfs_rename,
    .symlink = realfs_symlink,
    .mknod = realfs_mknod,

    .close = realfs_close,
    .stat = realfs_stat,
    .fstat = realfs_fstat,
    .setattr = realfs_setattr,
    .fsetattr = realfs_fsetattr,
    .utime = realfs_utime,
    .getpath = realfs_getpath,
    .flock = realfs_flock,

    .mkdir = realfs_mkdir,
};

const struct fd_ops realfs_fdops = {
    .read = realfs_read,
    .write = realfs_write,
    .pread = realfs_pread,
    .pwrite = realfs_pwrite,
    .readdir = realfs_readdir,
    .telldir = realfs_telldir,
    .seekdir = realfs_seekdir,
    .lseek = realfs_lseek,
    .mmap = realfs_mmap,
    .poll = realfs_poll,
    .ioctl_size = realfs_ioctl_size,
    .ioctl = realfs_ioctl,
    .fsync = realfs_fsync,
    .close = realfs_close,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
};
