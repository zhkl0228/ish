#ifndef FS_REAL_H
#define FS_REAL_H

#include "kernel/fs.h"

extern const struct fd_ops realfs_fdops;
extern const struct fs_ops realfs;

// A path under a mount's root as the host's *at calls take it: from the
// root's fd (root), or, longer than the host's paths (Darwin's 1024 bytes; the
// guest's are 4096), from a directory down it (fd), each directory on the way
// opened in turn, and the rest of it (path). realfs_at_begin returns 0, or an
// error and nothing to end; realfs_at_end closes what it opened, errno kept.
struct realfs_at {
    int root;
    int fd;
    const char *path;
};
int realfs_at_begin(int root_fd, const char *path, struct realfs_at *at);
void realfs_at_end(struct realfs_at *at);

struct fd *realfs_open(struct mount *mount, const char *path, int flags, int mode);

ssize_t realfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize);
int realfs_link(struct mount *mount, const char *src, const char *dst);
int realfs_unlink(struct mount *mount, const char *path);
int realfs_rmdir(struct mount *mount, const char *path);
int realfs_rename(struct mount *mount, const char *src, const char *dst);
int realfs_symlink(struct mount *mount, const char *target, const char *link);
int realfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ UNUSED(dev));

int realfs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat);
int realfs_statfs(struct mount *mount, struct statfsbuf *stat);
int realfs_fstat(struct fd *fd, struct statbuf *fake_stat);
int realfs_setattr(struct mount *mount, const char *path, struct attr attr);
int realfs_fsetattr(struct fd *fd, struct attr attr);

int realfs_mkdir(struct mount *mount, const char *path, mode_t_ mode);

int realfs_truncate(struct mount *mount, const char *path, off_t_ size);
int realfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime);

int realfs_statfs(struct mount *mount, struct statfsbuf *stat);
int realfs_flock(struct fd *fd, int operation);
int realfs_getpath(struct fd *fd, char *buf);
ssize_t realfs_read(struct fd *fd, void *buf, size_t bufsize);
ssize_t realfs_write(struct fd *fd, const void *buf, size_t bufsize);

int realfs_readdir(struct fd *fd, struct dir_entry *entry);
unsigned long realfs_telldir(struct fd *fd);
void realfs_seekdir(struct fd *fd, unsigned long ptr);

off_t realfs_lseek(struct fd *fd, off_t offset, int whence);

int realfs_poll(struct fd *fd);
int realfs_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);
// The read-only file mappings processes share: how many and their bytes, and
// how many times and how many bytes one was mapped again from the table
void realfs_shared_maps_stats(uint64_t *maps, uint64_t *bytes, uint64_t *reused, uint64_t *reused_bytes);
int realfs_fsync(struct fd *fd);
int realfs_getflags(struct fd *fd);
int realfs_setflags(struct fd *fd, dword_t arg);
ssize_t realfs_ioctl_size(int cmd);
int realfs_ioctl(struct fd *fd, int cmd, void *arg);
int realfs_close(struct fd *fd);

#endif
