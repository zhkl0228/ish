// signalfd: pending signals, read from a file descriptor (signalfd4). A read
// takes the signals of the fd's mask queued for the reading thread, then for
// its process, as sigtimedwait does, and poll says whether there is one. They
// are the signals of whoever reads or polls, not of whoever made the fd, as on
// Linux: a child of fork reads its own.
//
// A process that polls a signalfd watches it from its sighand, as Linux's
// pollers wait on their sighand's signalfd_wqh: a signal queued for a process
// wakes the pollers of the signalfds its sighand watches whose mask has it,
// and no others. Woken for every signal of the mask anywhere, s6's servers
// polled again and again for nothing (POLL-SPIN). A watch stays until the fd
// or the sighand goes: a process that stopped polling is woken for nothing at
// most.
//
// The wakeup itself, poll_wakeup, takes a poll's locks, and a signal is
// queued holding a sighand's lock, often pids_lock too: so the signalfd is
// queued for the waker, a thread of its own that holds no other lock, which
// wakes its pollers. signalfd_lock is taken with any lock held and takes no
// other: a sighand's under poll's lock (fd->ops->poll), poll's locks only
// with no lock held. Woken with pids_lock held, under a poll's lock, a
// poller waiting for pids_lock (a tty's under it) made a cycle.

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
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

// A sighand watching a signalfd
struct signalfd_watch {
    struct list for_fd;
    struct list for_sighand;
    struct fd *fd;
    struct sighand *sighand;
};

// The watches, and the waker's queue of signalfds to wake the pollers of
static lock_t signalfd_lock = LOCK_INITIALIZER;
static struct list signalfd_pending = LIST_INITIALIZER(signalfd_pending);
static cond_t signalfd_pending_cond = COND_INITIALIZER;
// the signalfds there are: signals are queued for nothing else to look at
// while there are none
static _Atomic unsigned signalfd_count;

static void *signalfd_waker(void *arg) {
    (void) arg;
    lock(&signalfd_lock);
    for (;;) {
        while (list_empty(&signalfd_pending))
            wait_for_ignore_signals(&signalfd_pending_cond, &signalfd_lock, NULL);
        struct fd *fd = list_first_entry(&signalfd_pending, struct fd, signalfd.pending_link);
        list_remove(&fd->signalfd.pending_link);
        fd->signalfd.pending = false;
        unlock(&signalfd_lock);
        poll_wakeup(fd, POLL_READ);
        // the queue's reference
        fd_close(fd);
        lock(&signalfd_lock);
    }
    return NULL;
}

static void start_waker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, signalfd_waker, NULL) != 0)
        die("signalfd: no waker thread: %s", strerror(errno));
    pthread_detach(thread);
}

// A reference unless the fd's last went, its close under way
static bool fd_retain_unless_closing(struct fd *fd) {
    unsigned refs = atomic_load(&fd->refcount);
    do {
        if (refs == 0)
            return false;
    } while (!atomic_compare_exchange_weak(&fd->refcount, &refs, refs + 1));
    return true;
}

void signalfd_notify(struct sighand *sighand, int sig) {
    if (atomic_load_explicit(&signalfd_count, memory_order_relaxed) == 0)
        return;
    lock(&signalfd_lock);
    struct signalfd_watch *watch;
    list_for_each_entry(&sighand->signalfd_watches, watch, for_sighand) {
        struct fd *fd = watch->fd;
        if (fd->signalfd.pending || !sigset_has(atomic_load(&fd->signalfd.mask), sig))
            continue;
        if (!fd_retain_unless_closing(fd))
            continue;
        fd->signalfd.pending = true;
        list_add_tail(&signalfd_pending, &fd->signalfd.pending_link);
        notify(&signalfd_pending_cond);
    }
    unlock(&signalfd_lock);
}

// A poll of the signalfd by current: its sighand watches it, from now on
static void watch_signalfd(struct fd *fd) {
    struct sighand *sighand = current->sighand;
    lock(&signalfd_lock);
    struct signalfd_watch *watch;
    list_for_each_entry(&fd->signalfd.watches, watch, for_fd) {
        if (watch->sighand == sighand)
            goto out;
    }
    watch = malloc(sizeof(*watch));
    watch->fd = fd;
    watch->sighand = sighand;
    list_add(&fd->signalfd.watches, &watch->for_fd);
    list_add(&sighand->signalfd_watches, &watch->for_sighand);
out:
    unlock(&signalfd_lock);
}

static void watch_free(struct signalfd_watch *watch) {
    list_remove(&watch->for_fd);
    list_remove(&watch->for_sighand);
    free(watch);
}

void signalfd_sighand_gone(struct sighand *sighand) {
    lock(&signalfd_lock);
    struct signalfd_watch *watch, *tmp;
    list_for_each_entry_safe(&sighand->signalfd_watches, watch, tmp, for_sighand)
        watch_free(watch);
    unlock(&signalfd_lock);
}

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

    static pthread_once_t waker_once = PTHREAD_ONCE_INIT;
    pthread_once(&waker_once, start_waker);
    struct fd *fd = adhoc_fd_create(&signalfd_ops);
    if (fd == NULL)
        return _ENOMEM;
    atomic_store(&fd->signalfd.mask, mask);
    list_init(&fd->signalfd.watches);
    signalfd_count++;
    fd_t installed = f_install(fd, flags);
    if (installed < 0)
        fd_close(fd);
    return installed;
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
    // before looking: a signal queued after it wakes the poller
    watch_signalfd(fd);
    sigset_t_ mask = atomic_load(&fd->signalfd.mask);
    lock(&current->sighand->lock);
    bool pending = (current->pending | current->group->shared_pending) & mask;
    unlock(&current->sighand->lock);
    return pending ? POLL_READ : 0;
}

// Not while it waits for the waker: the queue holds a reference
static int signalfd_close(struct fd *fd) {
    lock(&signalfd_lock);
    struct signalfd_watch *watch, *tmp;
    list_for_each_entry_safe(&fd->signalfd.watches, watch, tmp, for_fd)
        watch_free(watch);
    signalfd_count--;
    unlock(&signalfd_lock);
    return 0;
}

static struct fd_ops signalfd_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .close = signalfd_close,
};
