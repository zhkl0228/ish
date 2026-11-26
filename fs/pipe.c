#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/pipe.h"
#include "fs/real.h"
#include "debug.h"
#include "util/list.h"
#include "util/sync.h"

// A pipe's capacity, as Linux's F_GETPIPE_SZ and F_SETPIPE_SZ have it: the
// host's pipes have none to ask or set, and fcntl was EINVAL of both
// (CPython's test_fcntl, and test_subprocess's pipesize). One for a pipe,
// shared by its two ends (pipe2); one for a FIFO, by the host's device and
// inode, shared by its opens as they ask of it. It goes with the last.
//
// What the host's pipe holds is its own: a Darwin pipe takes up to 64KB
// whatever it is set to, so a smaller one takes more before a write waits
// than Linux's would, and one set below what it holds is not EBUSY. More
// than 64KB it cannot hold: EPERM, as a Linux whose pipe-max-size is that.
struct pipe_size {
    unsigned refs;
    unsigned size;
    // a FIFO's, on fifo_sizes; a pipe's is on no list
    dev_t dev;
    ino_t ino;
    struct list links;
};

#define PIPE_SIZE_DEFAULT (16 * PAGE_SIZE) // Linux's, 64KB
#define PIPE_SIZE_MAX 65536 // a Darwin pipe's at most

static lock_t pipe_sizes_lock = LOCK_INITIALIZER;
static struct list fifo_sizes = LIST_INITIALIZER(fifo_sizes);

void pipe_size_release(struct fd *fd) {
    struct pipe_size *ps = fd->pipe_size;
    fd->pipe_size = NULL;
    lock(&pipe_sizes_lock);
    if (--ps->refs == 0) {
        list_remove_safe(&ps->links);
        free(ps);
    }
    unlock(&pipe_sizes_lock);
}

// A FIFO's, found by the host's device and inode, or made; its reference
// taken. Under pipe_sizes_lock.
static int fifo_size_attach(struct fd *fd) {
    struct stat st;
    if (fstat(fd->real_fd, &st) < 0)
        return errno_map();
    struct pipe_size *ps;
    list_for_each_entry(&fifo_sizes, ps, links) {
        if (ps->dev == st.st_dev && ps->ino == st.st_ino) {
            ps->refs++;
            fd->pipe_size = ps;
            return 0;
        }
    }
    ps = malloc(sizeof(*ps));
    if (ps == NULL)
        return _ENOMEM;
    *ps = (struct pipe_size) {.refs = 1, .size = PIPE_SIZE_DEFAULT, .dev = st.st_dev, .ino = st.st_ino};
    list_add(&fifo_sizes, &ps->links);
    fd->pipe_size = ps;
    return 0;
}

int_t pipe_fcntl_size(struct fd *fd, bool set, dword_t arg) {
    // what is not a pipe or FIFO, as Linux's pipe_fcntl
    if (fd->type != S_IFIFO)
        return _EBADF;
    lock(&pipe_sizes_lock);
    if (fd->pipe_size == NULL) {
        int err = fifo_size_attach(fd);
        if (err < 0) {
            unlock(&pipe_sizes_lock);
            return err;
        }
    }
    int_t result;
    if (!set) {
        result = fd->pipe_size->size;
    } else if (arg > PIPE_SIZE_MAX) {
        result = _EPERM;
    } else {
        // as Linux's round_pipe_size: a page at least, a power of two pages
        unsigned size = PAGE_SIZE;
        while (size < arg)
            size <<= 1;
        fd->pipe_size->size = size;
        result = size;
    }
    unlock(&pipe_sizes_lock);
    return result;
}

// [T-ish-host-fd-double-close] Owns pipe_fd from here on: on any failure it
// is already closed (f_install() closes the fd it could not install, and with
// it real_fd), so a caller must never close() it again - by then the number
// may belong to another thread of the app (CFNetwork, boringssl, AVAudio).
static fd_t pipe_f_create(int pipe_fd, unsigned long inode, struct pipe_size *ps, int flags) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        close(pipe_fd);
        return _ENOMEM;
    }
    fd->real_fd = pipe_fd;
    // the pipe's capacity, its reference this end's (realfs_close lets go)
    lock(&pipe_sizes_lock);
    ps->refs++;
    unlock(&pipe_sizes_lock);
    fd->pipe_size = ps;
    fd->type = S_IFIFO; // realfs_read and _write wait for a signal on it too
    fd->stat.mode = S_IFIFO | 0660;
    // the pipe's, both ends': fstat, /proc/<pid>/fd's pipe:[inode], and
    // diff, which takes two of the same as one file
    fd->stat.inode = inode;
    // the rest as Linux's pipefs has them
    fd->stat.nlink = 1;
    fd->stat.blksize = 4096;
    fd->stat.atime = fd->stat.mtime = fd->stat.ctime = time(NULL);
    fd->stat.uid = current->uid;
    fd->stat.gid = current->gid;
    return f_install(fd, flags);
}

int_t sys_pipe2(addr_t pipe_addr, int_t flags) {
    STRACE("pipe2(%#x, %#x)", pipe_addr, flags);
    if (flags & ~(O_CLOEXEC_|O_NONBLOCK_)) {
        FIXME("unsupported pipe2 flags");
        return _EINVAL;
    }

    int p[2];
    int err = pipe(p);
    if (err < 0)
        return err;

    // [T-ish-host-fd-double-close] Each host end is closed exactly once:
    // by pipe_f_create() when it fails, by f_close() once it is installed,
    // and here only while it has no guest fd yet.
    int fp[2];
    unsigned long inode = adhoc_new_inode();
    // the ends' references to it are theirs, taken in pipe_f_create
    struct pipe_size *ps = malloc(sizeof(*ps));
    if (ps == NULL) {
        close(p[0]);
        close(p[1]);
        return _ENOMEM;
    }
    // on no list: links null
    *ps = (struct pipe_size) {.refs = 1, .size = PIPE_SIZE_DEFAULT};
    err = fp[0] = pipe_f_create(p[0], inode, ps, flags);
    if (fp[0] < 0) {
        close(p[1]);
        goto release;
    }
    err = fp[1] = pipe_f_create(p[1], inode, ps, flags);
    if (fp[1] < 0)
        goto close_fake_0;

    err = _EFAULT;
    if (user_put(pipe_addr, fp))
        goto close_fake_1;
    STRACE(" [%d %d]", fp[0], fp[1]);
    err = 0;
    goto release;

close_fake_1:
    f_close(fp[1]);
close_fake_0:
    f_close(fp[0]);
release:
    // this function's reference: the ends hold theirs
    lock(&pipe_sizes_lock);
    if (--ps->refs == 0)
        free(ps);
    unlock(&pipe_sizes_lock);
    return err;
}

int_t sys_pipe(addr_t pipe_addr) {
    return sys_pipe2(pipe_addr, 0);
}
