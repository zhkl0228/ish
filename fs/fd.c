#include "kernel/inotify.h"
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/pipe.h"

struct fd *fd_create(const struct fd_ops *ops) {
    struct fd *fd = malloc(sizeof(struct fd));
    if (fd == NULL)
        return NULL;
    *fd = (struct fd) {};
    fd->ops = ops;
    fd->refcount = 1;
    fd->flags = 0;
    fd->mount = NULL;
    fd->offset = 0;
    list_init(&fd->poll_fds);
    list_init(&fd->pidfd_links);
    lock_init(&fd->poll_lock);
    lock_init(&fd->lock);
    cond_init(&fd->cond);
    return fd;
}

struct fd *fd_retain(struct fd *fd) {
    fd->refcount++;
    return fd;
}

int fd_close(struct fd *fd) {
    int err = 0;
    if (--fd->refcount == 0) {
        if (fd->inotify_opened)
            inotify_fd(fd, fd->flags & (O_WRONLY_ | O_RDWR_) ? IN_CLOSE_WRITE_ : IN_CLOSE_NOWRITE_);
        poll_cleanup_fd(fd);
        if (fd->ops->close)
            err = fd->ops->close(fd);
        // see comment in close in kernel/fs.h
        if (fd->mount && fd->mount->fs->close && fd->mount->fs->close != fd->ops->close) {
            int new_err = fd->mount->fs->close(fd);
            if (new_err < 0)
                err = new_err;
        }

        if (fd->inode)
            inode_release(fd->inode);
        if (fd->mount)
            mount_release(fd->mount);
        free(fd);
    }
    return err;
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(int size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    lock_init(&fdt->lock);
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

static int fdtable_close(struct fdtable *table, fd_t f);

// FIXME this looks like it has the classic refcount UAF
void fdtable_release(struct fdtable *table) {
    lock(&table->lock);
    if (--table->refcount == 0) {
        for (fd_t f = 0; (unsigned) f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        unlock(&table->lock);
        free(table);
    } else {
        unlock(&table->lock);
    }
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock);
    int size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            new_table->files[f]->refcount++;
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    unlock(&table->lock);
    return new_table;
}

static int fdtable_expand(struct fdtable *table, fd_t max) {
    unsigned size = max + 1;
    if (size > rlimit(RLIMIT_NOFILE_))
        return _EMFILE;
    if (table->size >= size)
        return 0;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    if (f < 0 || (unsigned) f >= current->files->size)
        return NULL;
    return table->files[f];
}

struct fd *f_get(fd_t f) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, f);
    unlock(&current->files->lock);
    return fd;
}

// [T-ish-poll-member-race] f_get() plus a reference taken while files->lock
// is still held, so a concurrent close() of the number cannot free the fd
// while the caller uses it. Release with fd_close().
struct fd *f_get_retained(fd_t f) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, f);
    if (fd != NULL)
        fd_retain(fd);
    unlock(&current->files->lock);
    return fd;
}

static fd_t f_install_start(struct fd *fd, fd_t start) {
    assert(start >= 0);
    struct fdtable *table = current->files;
    unsigned size = rlimit(RLIMIT_NOFILE_);
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; (unsigned) f < size; f++)
        if (table->files[f] == NULL)
            break;
    if ((unsigned) f >= size) {
        int err = fdtable_expand(table, f);
        if (err < 0)
            f = err;
    }

    if (f >= 0) {
        table->files[f] = fd;
        bit_clear(f, table->cloexec);
    } else {
        fd_close(fd);
    }
    return f;
}

fd_t f_install(struct fd *fd, int flags) {
    lock(&current->files->lock);
    fd_t f = f_install_start(fd, 0);
    if (f >= 0) {
        if (flags & O_CLOEXEC_)
            bit_set(f, current->files->cloexec);
        if (flags & O_NONBLOCK_)
            fd_setflags(fd, O_NONBLOCK_);
    }
    unlock(&current->files->lock);
    return f;
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->inode != NULL) // temporary hack for files like sockets that right now don't have inodes but will eventually
        file_lock_remove_owned_by(fd, table);
    int err = fd_close(fd);
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    return err;
}

int f_close(fd_t f) {
    lock(&current->files->lock);
    int err = fdtable_close(current->files, f);
    unlock(&current->files->lock);
    return err;
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock);
    for (fd_t f = 0; (unsigned) f < table->size; f++)
        if (bit_test(f, table->cloexec))
            fdtable_close(table, f);
    unlock(&table->lock);
}

#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4

#define F_GETLK_ 5
#define F_SETLK_ 6
#define F_SETLKW_ 7
#define F_GETLK64_ 12
#define F_SETLK64_ 13
#define F_SETLKW64_ 14

#define F_DUPFD_CLOEXEC_ 1030
#define F_SETPIPE_SZ_ 1031
#define F_GETPIPE_SZ_ 1032
#define F_ADD_SEALS_ 1033
#define F_GET_SEALS_ 1034
#define F_SEAL_SEAL_ 0x1

dword_t sys_dup(fd_t f) {
    STRACE("dup(%d)", f);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    fd->refcount++;
    return f_install(fd, 0);
}

// f's file at new_f, what new_f had closed, as Linux's do_dup2. A new_f
// below 0 or past RLIMIT_NOFILE is EBADF: -1 was taken for a table that
// needs no growing, and the file written at files[-1], over malloc's header
// of the table: iSH's heap was broken, and it crashed freeing the table at
// exit (CPython's test_os test_dup2_negative_fd; the -1 returned said EPERM).
// Under the table's lock, as f_install: another thread's open took the slot
// meanwhile. new_f's close-on-exec is O_CLOEXEC's, cleared without it (it
// kept the one of the file it had).
static dword_t dup_to(fd_t f, fd_t new_f, int_t flags) {
    struct fdtable *table = current->files;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL || new_f < 0 || (unsigned) new_f >= rlimit(RLIMIT_NOFILE_)) {
        unlock(&table->lock);
        return _EBADF;
    }
    int err = fdtable_expand(table, new_f);
    if (err < 0) {
        unlock(&table->lock);
        return err;
    }
    fd_retain(fd);
    fdtable_close(table, new_f);
    table->files[new_f] = fd;
    if (flags & O_CLOEXEC_)
        bit_set(new_f, table->cloexec);
    else
        bit_clear(new_f, table->cloexec);
    unlock(&table->lock);
    return new_f;
}

// dup3 of an fd to itself, or with a flag but O_CLOEXEC, is EINVAL
dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags) {
    STRACE("dup3(%d, %d, %d)", f, new_f, flags);
    if (flags & ~O_CLOEXEC_ || f == new_f)
        return _EINVAL;
    return dup_to(f, new_f, flags);
}

// dup2 of an fd to itself is the fd, if it is one
dword_t sys_dup2(fd_t f, fd_t new_f) {
    STRACE("dup2(%d, %d)", f, new_f);
    if (f == new_f)
        return f_get(f) != NULL ? (dword_t) f : (dword_t) _EBADF;
    return dup_to(f, new_f, 0);
}

int fd_getflags(struct fd *fd) {
    if (fd->ops->getflags)
        return fd->ops->getflags(fd);
    return fd->flags;
}

#define FD_ALLOWED_FLAGS (O_APPEND_ | O_NONBLOCK_)
int fd_setflags(struct fd *fd, int flags) {
    if (fd->ops->setflags)
        return fd->ops->setflags(fd, flags);
    fd->flags = (fd->flags & ~FD_ALLOWED_FLAGS) | (flags & FD_ALLOWED_FLAGS);
    return 0;
}

dword_t sys_fcntl(fd_t f, dword_t cmd, addr_t arg) {
    struct fdtable *table = current->files;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    struct flock_ flock;
    fd_t new_f;
    int err;
    switch (cmd) {
        // The lowest fd it may take at or past RLIMIT_NOFILE (a negative one,
        // taken unsigned) is EINVAL, as Linux's f_dupfd: f_install_start
        // asserted it at least 0, and iSH aborted. Under the table's lock, as
        // f_install. Close-on-exec is set on the fd made only: a failure's
        // error was set as a bit, before the bitmap.
        case F_DUPFD_:
        case F_DUPFD_CLOEXEC_:
            STRACE("fcntl(%d, %s, %d)", f, cmd == F_DUPFD_ ? "F_DUPFD" : "F_DUPFD_CLOEXEC", arg);
            if (arg >= rlimit(RLIMIT_NOFILE_))
                return _EINVAL;
            lock(&table->lock);
            fd->refcount++;
            new_f = f_install_start(fd, (fd_t) arg);
            if (new_f >= 0 && cmd == F_DUPFD_CLOEXEC_)
                bit_set(new_f, table->cloexec);
            unlock(&table->lock);
            return new_f;

        // A memfd's seals: F_SEAL_SEAL, as one made without
        // MFD_ALLOW_SEALING (memfd_create takes no sealing), and adding one
        // is EPERM. Another file has none to say: EINVAL, as Linux's.
        case F_GET_SEALS_:
            STRACE("fcntl(%d, F_GET_SEALS)", f);
            return fd_is_memfd(fd) ? F_SEAL_SEAL_ : _EINVAL;
        case F_ADD_SEALS_:
            STRACE("fcntl(%d, F_ADD_SEALS, %#x)", f, arg);
            return fd_is_memfd(fd) ? _EPERM : _EINVAL;

        case F_GETFD_:
            STRACE("fcntl(%d, F_GETFD)", f);
            return bit_test(f, table->cloexec);
        case F_SETFD_:
            STRACE("fcntl(%d, F_SETFD, 0x%x)", f, arg);
            if (arg & 1)
                bit_set(f, table->cloexec);
            else
                bit_clear(f, table->cloexec);
            return 0;

        case F_GETPIPE_SZ_:
            STRACE("fcntl(%d, F_GETPIPE_SZ)", f);
            return pipe_fcntl_size(fd, false, 0);
        case F_SETPIPE_SZ_:
            STRACE("fcntl(%d, F_SETPIPE_SZ, %u)", f, arg);
            return pipe_fcntl_size(fd, true, arg);
        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            return fd_getflags(fd);
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            return fd_setflags(fd, arg);

        // A 64-bit guest's struct flock, arm64's. F_GETLK64 and the others
        // are a 32-bit kernel's: EINVAL (default). The 32-bit layout read
        // here before put the offsets in the wrong places: a lock of a range
        // (Python's fcntl.lockf) locked another, and F_GETLK's answer was
        // read wrong.
        case F_GETLK_: {
            STRACE("fcntl(%d, F_GETLK, %#x)", f, arg);
            struct flock_arm64_ flock64;
            if (user_read(arg, &flock64, sizeof(flock64)))
                return _EFAULT;
            flock = (struct flock_) {flock64.type, flock64.whence, flock64.start, flock64.len, flock64.pid};
            err = fcntl_getlk(fd, &flock);
            if (err >= 0) {
                flock64.type = flock.type;
                flock64.whence = flock.whence;
                flock64.start = flock.start;
                flock64.len = flock.len;
                flock64.pid = flock.pid;
                if (user_write(arg, &flock64, sizeof(flock64)))
                    return _EFAULT;
            }
            return err;
        }

        case F_SETLK_:
        case F_SETLKW_: {
            STRACE("fcntl(%d, F_SETLK%*s, %#x)", f, cmd == F_SETLKW_, "W", arg);
            struct flock_arm64_ flock64;
            if (user_read(arg, &flock64, sizeof(flock64)))
                return _EFAULT;
            flock = (struct flock_) {flock64.type, flock64.whence, flock64.start, flock64.len, flock64.pid};
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW_);
        }

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            return _EINVAL;
    }
}

dword_t sys_fcntl32(fd_t fd, dword_t cmd, addr_t arg) {
    switch (cmd) {
        case F_GETLK64_:
        case F_SETLK64_:
        case F_SETLKW64_:
            return _EINVAL;
    }
    return sys_fcntl(fd, cmd, arg);
}

// close_range(unsigned first, unsigned last, unsigned flags) — Linux 5.9+ syscall #436.
// Closes all open fds in [first, last]. Used by Anthropic claude-cli, openssh,
// systemd, etc.; without this, claude-cli loops on missing-syscall ENOSYS.
// CLOSE_RANGE_UNSHARE (0x02) and CLOSE_RANGE_CLOEXEC (0x04) are defined in
// kernel headers but we treat them as no-ops here — guests that need the
// CLOEXEC variant will fall back to F_SETFD anyway.
#define CLOSE_RANGE_CLOEXEC_ 0x04
dword_t sys_close_range(uint32_t first, uint32_t last, uint32_t flags) {
    STRACE("close_range(%u, %u, 0x%x)", first, last, flags);
    if (first > last)
        return _EINVAL;
    lock(&current->files->lock);
    fd_t lim = current->files->size;
    if ((fd_t)last >= lim) last = lim - 1;
    int err = 0;
    for (fd_t f = (fd_t)first; f <= (fd_t)last; f++) {
        if (current->files->files[f] == NULL) continue;
        if (flags & CLOSE_RANGE_CLOEXEC_) {
            // Mark CLOEXEC instead of actually closing.
            bit_set(f, current->files->cloexec);
            continue;
        }
        int e = fdtable_close(current->files, f);
        if (e != 0 && err == 0) err = e;
    }
    unlock(&current->files->lock);
    return 0;  // Linux always returns 0 on success even if some close failed
}
