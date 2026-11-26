#ifndef FS_PIPE_H
#define FS_PIPE_H
#include <stdbool.h>
#include "misc.h"
struct fd;

// F_GETPIPE_SZ (set false) and F_SETPIPE_SZ of fd: its capacity, EBADF if it
// is no pipe or FIFO
int_t pipe_fcntl_size(struct fd *fd, bool set, dword_t arg);
// Lets go of fd's reference to its pipe's capacity (realfs_close)
void pipe_size_release(struct fd *fd);

#endif
