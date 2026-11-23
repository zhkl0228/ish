#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/dev.h"
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/inotify.h"

struct mount *find_mount_and_trim_path(char *path) {
    struct mount *mount = mount_find(path);
    char *dst = path;
    const char *src = path + strlen(mount->point);
    while (*src != '\0')
        *dst++ = *src++;
    *dst = '\0';
    return mount;
}

bool contains_mount_point(const char *path) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        int n = strlen(path);
        if (strncmp(path, mount->point, n) == 0 &&
                (mount->point[n] == '\0' || mount->point[n] == '/'))
            return true;
    }
    return false;
}

static struct fd *generic_openat_path(struct fd *at, const char *path_raw, int flags, int mode, bool *via_procfs);

// Linux's MAXSYMLINKS
#define FD_LINK_MAX_LINKS 40

struct fd *generic_fd_link(struct fd *at, const char *path_raw) {
    char cur[MAX_PATH];
    if (strlen(path_raw) >= sizeof(cur))
        return NULL;
    strcpy(cur, path_raw);
    for (int links = 0; links < FD_LINK_MAX_LINKS; links++) {
        char path[MAX_PATH];
        if (path_normalize(at, cur, path, N_SYMLINK_NOFOLLOW) < 0)
            return NULL;
        char dir[MAX_PATH];
        strcpy(dir, path);
        *strrchr(dir, '/') = '\0';
        struct mount *mount = find_mount_and_trim_path(path);
        struct fd *file = mount->fs == &procfs ? proc_fd_link_file(path) : NULL;
        char target[MAX_PATH];
        ssize_t n = _EINVAL;
        if (file == NULL && mount->fs->readlink != NULL)
            n = mount->fs->readlink(mount, path, target, sizeof(target) - 1);
        mount_release(mount);
        if (file != NULL)
            return file;
        // not a link: no fd link on the way
        if (n < 0)
            return NULL;
        target[n] = '\0';
        if (target[0] == '/')
            strcpy(cur, target);
        else if ((size_t) snprintf(cur, sizeof(cur), "%s/%s", dir, target) >= sizeof(cur))
            return NULL;
        at = AT_PWD;
    }
    return NULL;
}

struct fd *generic_openat(struct fd *at, const char *path_raw, int flags, int mode) {
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return ERR_PTR(_EINVAL);
    bool via_procfs;
    struct fd *fd = generic_openat_path(at, path_raw, flags, mode, &via_procfs);
    if (!IS_ERR(fd) || !via_procfs)
        return fd;
    // only when the path did not open, through a procfs link: no cost to
    // the others
    struct fd *file = generic_fd_link(at, path_raw);
    if (file == NULL || IS_ERR(file))
        return file != NULL ? file : fd;
    // a pipe (the only file there is here): the end the access mode asks
    // for, shared. Linux opens the pipe anew, either end.
    int end = fcntl(file->real_fd, F_GETFL) & O_ACCMODE;
    int want = flags & O_RDWR_ ? O_RDWR : flags & O_WRONLY_ ? O_WRONLY : O_RDONLY;
    if (end == want)
        return file;
    FIXME("open of %s, a pipe's %s end, with flags %#x not implemented", path_raw, end == O_RDONLY ? "read" : "write",
          flags);
    fd_close(file);
    return fd;
}

static struct fd *generic_openat_path(struct fd *at, const char *path_raw, int flags, int mode, bool *via_procfs) {
    // TODO really, really, seriously reconsider what I'm doing with the strings
    char path[MAX_PATH];
    // O_NOFOLLOW has to be honored here rather than by handing the flag to the
    // host open(): path_normalize resolves the final component itself, so by
    // the time the fs open() runs the symlink is already gone. Leaving it
    // unresolved makes the S_ISLNK check below return ELOOP, as Linux does.
    int err = path_normalize_via(at, path_raw, path,
            (flags & O_NOFOLLOW_ ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW) |
            (flags & O_CREAT_ ? N_PARENT_DIR_WRITE : 0), via_procfs);
    if (err < 0)
        return ERR_PTR(err);
    struct mount *mount = find_mount_and_trim_path(path);
    // For inotify: whether the open makes the file (IN_CREATE) or empties
    // one that was there (IN_MODIFY)
    bool existed = true;
    if ((flags & O_CREAT_) && inotify_wants(IN_CREATE_ | IN_MODIFY_)) {
        struct statbuf before;
        existed = mount->fs->stat(mount, path, &before) >= 0;
    }
    struct fd *fd = mount->fs->open(mount, path, flags, mode);
    if (IS_ERR(fd)) {
        // if an error happens after this point, fd_close will release the
        // mount, but right now we need to do it manually
        mount_release(mount);
        return fd;
    }
    fd->mount = mount;
    if (!existed)
        inotify_child(mount, path, IN_CREATE_, 0);

    struct statbuf stat;
    if (fd->inode != NULL) {
        // [T-ish-openat-inode-early-ref] The filesystem already took the
        // inode reference inside open() (fakefs does), so no concurrent
        // path removal can orphan the row under us and fstat needs no lock.
        // This is the hot path of a fork+exec storm: holding inodes_lock
        // across fakefs_fstat's SQLite read was the convoy that parked 26
        // threads here and 6 in inode_release (bt all, 2026-09-19) and
        // 1484 in the 2026-09-18 IPS.
        err = fd->mount->fs->fstat(fd, &stat);
        if (err < 0)
            goto error;
    } else {
        // Filesystems that do not pre-acquire keep d57b6d26's guarantee:
        // "read the stat row" and "take the inode ref" are atomic against
        // inode_release / inode_check_orphaned under inodes_lock.
        lock(&inodes_lock); // TODO: don't do this
        err = fd->mount->fs->fstat(fd, &stat);
        if (err < 0) {
            unlock(&inodes_lock);
            goto error;
        }
        fd->inode = inode_get_unlocked(mount, stat.inode);
        if (fd->inode == NULL) {
            unlock(&inodes_lock);
            err = _ENOMEM;
            goto error;
        }
        unlock(&inodes_lock);
    }
    // [T-ish-exec-fewer-meta-txns] Whatever the fs did with its open-time
    // stat cache, it must not outlive this open: a later fstat has to see
    // current metadata. Keep the stat itself for exec.
    fd->fake_open_stat.valid = false;
    fd->open_stat = stat;
    fd->type = stat.mode & S_IFMT;
    fd->flags = flags;

    int accmode;
    if (flags & O_RDWR_) accmode = AC_R | AC_W;
    else if (flags & O_WRONLY_) accmode = AC_W;
    else accmode = AC_R;
    err = access_check(&stat, accmode);
    if (err < 0)
        goto error;

    if (S_ISLNK(fd->type)) {
        // path_normalize should have resolved symlinks, but if it didn't,
        // return an error instead of crashing the whole app.
        err = _ELOOP;
        goto error;
    }
    if (S_ISBLK(fd->type) || S_ISCHR(fd->type)) {
        int type;
        if (S_ISBLK(fd->type))
            type = DEV_BLOCK;
        else
            type = DEV_CHAR;
        err = dev_open(dev_major(stat.rdev), dev_minor(stat.rdev), type, fd);
        if (err < 0)
            goto error;
    }
    err = _ENXIO;
    if (S_ISSOCK(fd->type))
        goto error;
    err = _EISDIR;
    if (S_ISDIR(fd->type) && flags & (O_RDWR_ | O_WRONLY_))
        goto error;
    err = _ENOTDIR;
    if (!S_ISDIR(fd->type) && flags & O_DIRECTORY_)
        goto error;
    fd->inotify_opened = true;
    inotify_fd(fd, IN_OPEN_);
    if (existed && (flags & O_TRUNC_) && S_ISREG(fd->type) && (flags & (O_WRONLY_ | O_RDWR_)))
        inotify_fd(fd, IN_MODIFY_);
    return fd;

error:
    fd_close(fd);
    return ERR_PTR(err);
}

struct fd *generic_open(const char *path, int flags, int mode) {
    return generic_openat(AT_PWD, path, flags, mode);
}

int generic_getpath(struct fd *fd, char *buf) {
    int err = fd->mount->fs->getpath(fd, buf);
    if (err < 0)
        return err;
    if (strlen(buf) + strlen(fd->mount->point) >= MAX_PATH)
        return _ENAMETOOLONG;
    memmove(buf + strlen(fd->mount->point), buf, strlen(buf) + 1);
    memcpy(buf, fd->mount->point, strlen(fd->mount->point));
    if (buf[0] == '\0')
        strcpy(buf, "/");
    return 0;
}

int generic_accessat(struct fd *dirfd, const char *path_raw, int mode) {
    char path[MAX_PATH];
    int err = path_normalize(dirfd, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    mount_release(mount);
    if (err < 0)
        return err;
    return access_check(&stat, mode);
}

int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->link == NULL)
        err = _EPERM;
    else
        err = mount->fs->link(mount, src, dst);
    if (err >= 0 && mount == dst_mount) {
        struct statbuf linked;
        if (inotify_wants(IN_ATTRIB_) && mount->fs->stat(mount, dst, &linked) >= 0)
            inotify_inode(mount, linked.inode, IN_ATTRIB_);   // its link count
        inotify_child(mount, dst, IN_CREATE_, 0);
    }
    path_cache_invalidate();
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_unlinkat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    /* Include N_PARENT_DIR_WRITE so path_normalize checks write permission
     * on the parent directory. This makes read-only bind mounts (whose
     * top-level directory is mode 0555 in meta.db) reject `rm` with EACCES
     * without needing any special case in fakefs. */
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    // For inotify, what goes: watches on it end with its last link
    struct statbuf gone;
    bool watched = atomic_load(&inotify_watched) != 0 && mount->fs->stat(mount, path, &gone) >= 0;
    err = _EPERM;
    if (mount->fs->unlink)
        err = mount->fs->unlink(mount, path);
    // A directory is EISDIR on Linux, but the host's unlink(2) says EPERM for
    // it (POSIX allows either), and remove(3) goes on to rmdir(2) only for
    // EISDIR, in musl and glibc alike: so Java's File.delete(), which is
    // remove(), could not delete a directory, and Maven's cleanups (plexus
    // FileUtils, the protobuf plugin's) failed with "unable to be deleted".
    // Only a failure pays for the stat.
    if (err == _EPERM) {
        struct statbuf victim;
        if (mount->fs->stat(mount, path, &victim) >= 0 && S_ISDIR(victim.mode))
            err = _EISDIR;
    }
    if (err >= 0 && watched) {
        inotify_inode(mount, gone.inode, IN_ATTRIB_);   // its link count
        if (gone.nlink <= 1)
            inode_unlinked(mount, gone.inode);
        inotify_child(mount, path, IN_DELETE_, 0);
    }
    path_cache_invalidate();
    mount_release(mount);
    return err;
}

int generic_renameat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    /* Rename removes the src from its parent directory, so we also need
     * write permission on src's parent — not just dst's. */
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(src))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    // For inotify: what moves, and what it replaces. One file under both
    // names is no rename at all.
    struct statbuf from, to;
    bool watched = atomic_load(&inotify_watched) != 0 && mount == dst_mount &&
            mount->fs->stat(mount, src, &from) >= 0;
    bool replaces = watched && mount->fs->stat(mount, dst, &to) >= 0;
    if (replaces && to.inode == from.inode)
        watched = replaces = false;
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->rename == NULL)
        err = _EPERM;
    else
        err = mount->fs->rename(mount, src, dst);
    if (err >= 0 && watched) {
        uint32_t isdir = S_ISDIR(from.mode) ? IN_ISDIR_ : 0;
        uint32_t cookie = inotify_cookie();
        inotify_child(mount, src, IN_MOVED_FROM_ | isdir, cookie);
        inotify_child(mount, dst, IN_MOVED_TO_ | isdir, cookie);
        inotify_inode(mount, from.inode, IN_MOVE_SELF_);
        if (replaces) {
            inotify_inode(mount, to.inode, IN_ATTRIB_ | (S_ISDIR(to.mode) ? IN_ISDIR_ : 0));
            if (S_ISDIR(to.mode) || to.nlink <= 1)
                inode_unlinked(mount, to.inode);
        }
    }
    path_cache_invalidate();
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_symlinkat(const char *target, struct fd *at, const char *link_raw) {
    char link[MAX_PATH];
    int err = path_normalize(at, link_raw, link, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(link);
    err = _EPERM;
    if (mount->fs->symlink)
        err = mount->fs->symlink(mount, target, link);
    if (err >= 0)
        inotify_child(mount, link, IN_CREATE_, 0);
    path_cache_invalidate();
    mount_release(mount);
    return err;
}

int generic_mknodat(struct fd *at, const char *path_raw, mode_t_ mode, dev_t_ dev) {
    if (S_ISDIR(mode) || S_ISLNK(mode))
        return _EINVAL;
    if (!superuser() && (S_ISBLK(mode) || S_ISCHR(mode)))
        return _EPERM;

    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mknod)
        err = mount->fs->mknod(mount, path, mode, dev);
    if (err >= 0)
        inotify_child(mount, path, IN_CREATE_, 0);
    path_cache_invalidate();
    mount_release(mount);
    return err;
}

int generic_setattrat(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->setattr)
        err = mount->fs->setattr(mount, path, attr);
    if (err >= 0)
        inotify_path(mount, path, attr.type == attr_size ? IN_MODIFY_ : IN_ATTRIB_);
    mount_release(mount);
    return err;
}

int generic_utime(struct fd *at, const char *path_raw, struct timespec atime, struct timespec mtime, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->utime)
        err = mount->fs->utime(mount, path, atime, mtime);
    if (err >= 0)
        inotify_path(mount, path, IN_ATTRIB_);
    mount_release(mount);
    return err;
}

ssize_t generic_readlinkat(struct fd *at, const char *path_raw, char *buf, size_t bufsize) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EINVAL;
    if (mount->fs->readlink)
        err = mount->fs->readlink(mount, path, buf, bufsize);
    mount_release(mount);
    return err;
}

int generic_mkdirat(struct fd *at, const char *path_raw, mode_t_ mode) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_FOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mkdir)
        err = mount->fs->mkdir(mount, path, mode);
    if (err >= 0)
        inotify_child(mount, path, IN_CREATE_ | IN_ISDIR_, 0);
    path_cache_invalidate();
    mount_release(mount);
    return err;
}

int generic_rmdirat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_FOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(path))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf gone;
    bool watched = atomic_load(&inotify_watched) != 0 && mount->fs->stat(mount, path, &gone) >= 0;
    err = _EPERM;
    if (mount->fs->rmdir)
        err = mount->fs->rmdir(mount, path);
    if (err >= 0 && watched) {
        inode_unlinked(mount, gone.inode);
        inotify_child(mount, path, IN_DELETE_ | IN_ISDIR_, 0);
    }
    path_cache_invalidate();
    mount_release(mount);
    return err;
}

int generic_seek(struct fd *fd, off_t_ off, int whence, size_t size) {
    off_t_ new_off = fd->offset;
    if (whence == LSEEK_SET) {
        fd->offset = off;
    } else if (whence == LSEEK_CUR) {
        if (__builtin_add_overflow(new_off, off, &new_off) || new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else if (whence == LSEEK_END) {
        new_off = size + off;
        if (new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else {
        return _EINVAL;
    }
    return 0;
}
