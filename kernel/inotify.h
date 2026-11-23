#ifndef KERNEL_INOTIFY_H
#define KERNEL_INOTIFY_H

// inotify (kernel/inotify.c). The filesystem operations report what they did
// to the inodes involved, and each watch on one of them gets the events it
// asked for, as Linux's fsnotify does. Watches are on inodes, a mount's own:
// a watched directory renamed is still watched.

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct mount;
struct fd;

#define IN_ACCESS_        0x00000001
#define IN_MODIFY_        0x00000002
#define IN_ATTRIB_        0x00000004
#define IN_CLOSE_WRITE_   0x00000008
#define IN_CLOSE_NOWRITE_ 0x00000010
#define IN_OPEN_          0x00000020
#define IN_MOVED_FROM_    0x00000040
#define IN_MOVED_TO_      0x00000080
#define IN_CREATE_        0x00000100
#define IN_DELETE_        0x00000200
#define IN_DELETE_SELF_   0x00000400
#define IN_MOVE_SELF_     0x00000800
#define IN_ALL_EVENTS_    0x00000fff
#define IN_Q_OVERFLOW_    0x00004000
#define IN_IGNORED_       0x00008000
#define IN_ISDIR_         0x40000000

// The events some watch asks for, all watches together: an operation reports
// nothing nobody watches, and costs this load then. 0: nothing is watched.
extern _Atomic uint32_t inotify_watched;
static inline bool inotify_wants(uint32_t events) {
    return atomic_load_explicit(&inotify_watched, memory_order_relaxed) & events;
}

// Paths below are a mount's own, as find_mount_and_trim_path() leaves them.
// The order of the events, and which carry IN_ISDIR, are Linux's (from what
// it gives: a directory's watches before the file's own; IN_DELETE_SELF and
// IN_MOVE_SELF never with IN_ISDIR).

// An event of path itself: with its name to the watches on its directory,
// then to those on it (IN_ACCESS, IN_MODIFY, IN_ATTRIB, ...)
void inotify_path(struct mount *mount, const char *path, uint32_t mask);
// path came or went in its directory (IN_CREATE, IN_DELETE, IN_MOVED_FROM,
// IN_MOVED_TO): to the directory's watches, with path's name. mask has
// IN_ISDIR if path is a directory.
void inotify_child(struct mount *mount, const char *path, uint32_t mask, uint32_t cookie);
// An event of an inode, to its own watches only (IN_MOVE_SELF, IN_ATTRIB of
// a link count, with IN_ISDIR for a directory)
void inotify_inode(struct mount *mount, ino_t ino, uint32_t mask);
// The inode is gone, its last link and last open file: IN_DELETE_SELF, then
// its watches end (IN_IGNORED). See inode_unlinked().
void inotify_inode_gone(struct mount *mount, ino_t ino);
// An event of what fd has open: to its directory's watches and its own
void inotify_fd(struct fd *fd, uint32_t mask);
// For the IN_MOVED_FROM and IN_MOVED_TO of one rename
uint32_t inotify_cookie(void);

#endif
