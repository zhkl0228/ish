#ifndef FS_PIPE_H
#define FS_PIPE_H
#include <stdbool.h>
#include "misc.h"
struct fd;

// F_GETPIPE_SZ (set false) and F_SETPIPE_SZ of fd: its capacity, EBADF if it
// is no pipe or FIFO
int_t pipe_fcntl_size(struct fd *fd, bool set, dword_t arg);
// Lets go of fd's reference to its pipe's or FIFO's state (realfs_close): a
// FIFO's last writer gone, its readers' polls are woken (fifo_hangup)
void pipe_size_release(struct fd *fd);

// A FIFO's writers, as Linux's pipe_poll has them: Darwin's poll says nothing
// of the last one gone (no POLLHUP, no POLLIN), nor does its kqueue (no
// EV_EOF, registered before the close or after), so iSH counts them. The
// count of writers' opens of any FIFO now, which an open takes as it begins
// (generic_openat): a reader's open waits in the host's open for a writer,
// which opens meanwhile.
uint64_t fifo_open_epoch(void);
// A FIFO of realfs's (fakefs's too) opened with flags, the open begun at
// epoch: counted, a reader put on its readers
int fifo_opened(struct fd *fd, int flags, uint64_t epoch);
// Whether a FIFO's read end is hung up, as Linux's pipe_poll says EPOLLHUP:
// no writer now, and one has opened since the reader began to open
bool fifo_hangup(struct fd *fd);

#endif
