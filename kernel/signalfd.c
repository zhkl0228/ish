// signalfd: pending signals, read from a file descriptor (signalfd4). A read
// takes the signals of the fd's mask queued for the reading thread, then for
// its process, as sigtimedwait does, and poll says whether there is one. They
// are the signals of whoever reads or polls, not of whoever made the fd, as on
// Linux: a child of fork reads its own.
//
// A signal sent wakes those polling a signalfd whose mask has it, all of them:
// the fd does not know whose signals its pollers wait for, and a poller woken
// for another's polls again and finds nothing.
//
// Lock order: a poll's lock (around fd->ops->poll), then a sighand's. So the
// senders wake the pollers after dropping the sighand lock, and the mask is
// read without signalfd_lock.

#include <stdatomic.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "fs/fd.h"
#include "fs/poll.h"

// Linux's signalfd_siginfo
struct signalfd_siginfo_ {
    uint32_t signo;
    int32_t errno_;
    int32_t code;
    uint32_t pid;
    uint32_t uid;
    int32_t fd;
    uint32_t tid;
    uint32_t band;
    uint32_t overrun;
    uint32_t trapno;
    int32_t status;
    int32_t int_;
    uint64_t ptr;
    uint64_t utime;
    uint64_t stime;
    uint64_t addr;
    uint16_t addr_lsb;
    uint16_t pad2;
    int32_t syscall;
    uint64_t call_addr;
    uint32_t arch;
    uint8_t pad[28];
};
static_assert(sizeof(struct signalfd_siginfo_) == 128, "signalfd_siginfo layout");

static struct fd_ops signalfd_ops;

// The signalfds, for signalfd_wakeup
static lock_t signalfd_lock = LOCK_INITIALIZER;
static struct list signalfds = LIST_INITIALIZER(signalfds);
static _Atomic unsigned signalfd_count;

int_t sys_signalfd4(fd_t f, addr_t mask_addr, uint_t size, int_t flags) {
    STRACE("signalfd4(%d, %#llx, %u, %#x)", f, (unsigned long long) mask_addr, size, flags);
    if (flags & ~(O_CLOEXEC_ | O_NONBLOCK_) || size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    // never pending: they are never blocked
    sigset_del(&mask, SIGKILL_);
    sigset_del(&mask, SIGSTOP_);

    if (f != -1) {
        struct fd *fd = f_get(f);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops != &signalfd_ops)
            return _EINVAL;
        atomic_store(&fd->signalfd.mask, mask);
        poll_wakeup(fd, POLL_READ);
        return f;
    }

    struct fd *fd = adhoc_fd_create(&signalfd_ops);
    if (fd == NULL)
        return _ENOMEM;
    atomic_store(&fd->signalfd.mask, mask);
    lock(&signalfd_lock);
    list_add(&signalfds, &fd->signalfd.links);
    signalfd_count++;
    unlock(&signalfd_lock);
    fd_t installed = f_install(fd, flags);
    if (installed < 0)
        fd_close(fd);
    return installed;
}

void signalfd_wakeup(int sig) {
    if (atomic_load_explicit(&signalfd_count, memory_order_relaxed) == 0)
        return;
    lock(&signalfd_lock);
    struct fd *fd;
    list_for_each_entry(&signalfds, fd, signalfd.links) {
        if (sigset_has(atomic_load(&fd->signalfd.mask), sig))
            poll_wakeup(fd, POLL_READ);
    }
    unlock(&signalfd_lock);
}

// What Linux gives of info (signalfd_copyinfo): the fields of its layout,
// which its signal and code tell (siginfo_layout)
static struct signalfd_siginfo_ signalfd_info(const struct siginfo_ *info) {
    struct signalfd_siginfo_ out = {
        .signo = info->sig,
        .errno_ = info->sig_errno,
        .code = info->code,
    };
    if (info->code > SI_USER_ && info->code < SI_KERNEL_) {
        // a code of the signal's own
        switch (info->sig) {
            case SIGILL_: case SIGFPE_: case SIGSEGV_: case SIGBUS_: case SIGTRAP_:
                out.addr = info->fault.addr;
                return out;
            case SIGCHLD_:
                out.pid = info->child.pid;
                out.uid = info->child.uid;
                out.status = info->child.status;
                out.utime = info->child.utime;
                out.stime = info->child.stime;
                return out;
        }
        // SIGIO's and SIGSYS's, which iSH sends none of
        FIXME("signalfd: signal %d with code %d: its fields not given", info->sig, info->code);
        return out;
    }
    if (info->code == SI_TIMER_) {
        out.tid = info->timer.timer;
        out.overrun = info->timer.overrun;
        out.int_ = info->timer.value.sv_int;
        out.ptr = info->timer.value.sv_ptr;
        return out;
    }
    // kill's; with a negative code sigqueue's or tkill's, with a value where
    // a timer's is
    out.pid = info->kill.pid;
    out.uid = info->kill.uid;
    if (info->code < 0) {
        out.int_ = info->timer.value.sv_int;
        out.ptr = info->timer.value.sv_ptr;
    }
    return out;
}

// As many signals as fit, waiting for the first unless O_NONBLOCK
static ssize_t signalfd_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(struct signalfd_siginfo_))
        return _EINVAL;
    sigset_t_ mask = atomic_load(&fd->signalfd.mask);
    struct signalfd_siginfo_ *out = buf;
    size_t count = 0, max = bufsize / sizeof(*out);
    lock(&current->sighand->lock);
    while (count < max) {
        struct siginfo_ info;
        if (dequeue_signal(mask, &info)) {
            out[count++] = signalfd_info(&info);
            continue;
        }
        if (count > 0 || (fd->flags & O_NONBLOCK_))
            break;
        // woken for one of them, as sigtimedwait is: they are blocked
        assert(current->waiting == 0);
        current->waiting = mask;
        int err = wait_for(&current->pause, &current->sighand->lock, NULL);
        current->waiting = 0;
        if (err < 0) {
            unlock(&current->sighand->lock);
            return err;
        }
    }
    unlock(&current->sighand->lock);
    return count > 0 ? (ssize_t) (count * sizeof(*out)) : _EAGAIN;
}

static int signalfd_poll(struct fd *fd) {
    sigset_t_ mask = atomic_load(&fd->signalfd.mask);
    lock(&current->sighand->lock);
    bool pending = (current->pending | current->group->shared_pending) & mask;
    unlock(&current->sighand->lock);
    return pending ? POLL_READ : 0;
}

static int signalfd_close(struct fd *fd) {
    lock(&signalfd_lock);
    list_remove(&fd->signalfd.links);
    signalfd_count--;
    unlock(&signalfd_lock);
    return 0;
}

static struct fd_ops signalfd_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .close = signalfd_close,
};
