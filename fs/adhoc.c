#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "kernel/errno.h"

static struct mount adhoc_mount;

struct fd *adhoc_fd_create(const struct fd_ops *ops) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return NULL;
    mount_retain(&adhoc_mount);
    fd->mount = &adhoc_mount;
    fd->stat = (struct statbuf) {};
    return fd;
}

// A memfd is the ad-hoc fd of a regular file (memfd_create, below): a host
// file, made and unlinked. Its size, blocks and times are the host file's,
// and its ftruncate the host's; the rest is the fd's own, as a pipe's.
bool fd_is_memfd(struct fd *fd) {
    return fd->mount == &adhoc_mount && S_ISREG(fd->stat.mode);
}

static int adhoc_fstat(struct fd *fd, struct statbuf *stat) {
    *stat = fd->stat;
    if (fd_is_memfd(fd)) {
        struct statbuf host;
        int err = realfs_fstat(fd, &host);
        if (err < 0)
            return err;
        stat->size = host.size;
        stat->blksize = host.blksize;
        stat->blocks = host.blocks;
        stat->atime = host.atime;
        stat->atime_nsec = host.atime_nsec;
        stat->mtime = host.mtime;
        stat->mtime_nsec = host.mtime_nsec;
        stat->ctime = host.ctime;
        stat->ctime_nsec = host.ctime_nsec;
    }
    return 0;
}

static int adhoc_fsetattr(struct fd *fd, struct attr attr) {
    if (attr.type == attr_size && fd_is_memfd(fd))
        return ftruncate(fd->real_fd, attr.size) < 0 ? errno_map() : 0;
    switch (attr.type) {
        case attr_uid:
            fd->stat.uid = attr.uid;
            break;
        case attr_gid:
            fd->stat.gid = attr.gid;
            break;
        case attr_mode:
            fd->stat.mode = (fd->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            return _EINVAL;
    }
    return 0;
}

static _Atomic unsigned long adhoc_last_inode = 9999;

unsigned long adhoc_new_inode(void) {
    return atomic_fetch_add(&adhoc_last_inode, 1) + 1;
}

// What /proc/<pid>/fd links to: socket:[inode] and pipe:[inode] as on Linux,
// which netstat -p and lsof match against /proc/net and each other.
static int adhoc_getpath(struct fd *fd, char *buf) {
    // a memfd's, as Linux's: its name, the file being no file's
    if (fd_is_memfd(fd)) {
        sprintf(buf, "/memfd:%s (deleted)", fd->memfd.name);
        return 0;
    }
    const char *type = "unknown"; // TODO allow this to be customized
    if (S_ISSOCK(fd->stat.mode))
        type = "socket";
    else if (S_ISFIFO(fd->stat.mode))
        type = "pipe";
    if (fd->stat.inode == 0)
        sprintf(buf, "anon_inode:[%s]", type);
    else
        sprintf(buf, "%s:[%lu]", type, (unsigned long) fd->stat.inode);
    return 0;
}

bool is_adhoc_fd(struct fd *fd) {
    return fd->mount == &adhoc_mount;
}

static const struct fs_ops adhoc_fs = {
    .magic = 0x09041934, // FIXME wrong for pipes and sockets
    .fstat = adhoc_fstat,
    .fsetattr = adhoc_fsetattr,
    .getpath = adhoc_getpath,
};

static struct mount adhoc_mount = {
    .fs = &adhoc_fs,
    .point = "",
};

// memfd_create: a file of memory, no file's, as Linux's. It was ENOSYS
// (CPython's test_os, test_memfd_create; programs that share memory by an
// fd). A host file in the app's temporary directory (where the host sockets
// of unix sockets go, sock_tmp_prefix), unlinked once made, its fd an
// ad-hoc one: read, write, lseek, ftruncate, fstat and mmap, shared too,
// are the host file's. Its name is /proc/<pid>/fd's, "/memfd:name
// (deleted)"; one past 249 bytes is EINVAL. Sealing is not: a memfd has
// F_SEAL_SEAL, as one made without MFD_ALLOW_SEALING (F_GET_SEALS, and
// F_ADD_SEALS is EPERM), and MFD_ALLOW_SEALING and MFD_NOEXEC_SEAL, which
// ask for it, are EINVAL, said once (callers fall back, to shm_open or a
// file). MFD_HUGETLB is EINVAL, there being no huge pages.
#define MFD_CLOEXEC_ 0x1
#define MFD_ALLOW_SEALING_ 0x2
#define MFD_HUGETLB_ 0x4
#define MFD_NOEXEC_SEAL_ 0x8
#define MFD_EXEC_ 0x10

int_t sys_memfd_create(addr_t name_addr, uint_t flags) {
    char name[250];
    if (user_read_string(name_addr, name, sizeof(name)))
        return _EFAULT;
    if (memchr(name, '\0', sizeof(name)) == NULL)
        return _EINVAL;
    STRACE("memfd_create(\"%s\", %#x)", name, flags);
    if (flags & ~(MFD_CLOEXEC_ | MFD_ALLOW_SEALING_ | MFD_NOEXEC_SEAL_ | MFD_EXEC_))
        return _EINVAL;
    if (flags & (MFD_ALLOW_SEALING_ | MFD_NOEXEC_SEAL_)) {
        static _Atomic bool said;
        if (!atomic_exchange(&said, true))
            printk("%s[%d]: memfd_create(\"%s\", %#x): sealing is not implemented, EINVAL\n", current->comm,
                   current->pid, name, flags);
        return _EINVAL;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%smemfd.XXXXXX", sock_tmp_prefix);
    int real_fd = mkstemp(path);
    if (real_fd < 0)
        return errno_map();
    unlink(path);
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        close(real_fd);
        return _ENOMEM;
    }
    fd->real_fd = real_fd;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    fd->stat.mode = S_IFREG | 0777;
    fd->stat.inode = adhoc_new_inode();
    fd->stat.nlink = 1;
    fd->stat.uid = current->uid;
    fd->stat.gid = current->gid;
    fd->stat.blksize = 4096;
    strcpy(fd->memfd.name, name);
    return f_install(fd, flags & MFD_CLOEXEC_ ? O_CLOEXEC_ : 0);
}
