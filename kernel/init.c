#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/personality.h"

int mount_root(const struct fs_ops *fs, const char *source) {
    char source_realpath[MAX_PATH + 1];
    if (realpath(source, source_realpath) == NULL)
        return errno_map();
    int err = do_mount(fs, source_realpath, "", "", 0);
    if (err < 0)
        return err;
    return 0;
}

static void establish_signal_handlers(void) {
    extern void sigusr1_handler(int sig);
    struct sigaction sigact;
    sigact.sa_handler = sigusr1_handler;
    sigact.sa_flags = 0;
    sigemptyset(&sigact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGUSR1);
    sigaction(SIGUSR1, &sigact, NULL);
    signal(SIGPIPE, SIG_IGN);
}

// copied from include/asm-generic/resource.h in the kernel
static struct rlimit_ init_rlimits[16] = {
    [RLIMIT_CPU_]        = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_FSIZE_]      = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_DATA_]       = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_STACK_]      = {128*1024*1024, RLIM_INFINITY_},
    [RLIMIT_CORE_]       = {0, RLIM_INFINITY_},
    [RLIMIT_RSS_]        = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_NPROC_]      = {1024, 1024},
    [RLIMIT_NOFILE_]     = {1024, 4096},
    [RLIMIT_MEMLOCK_]    = {64*1024, 64*1024},
    [RLIMIT_AS_]         = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_LOCKS_]      = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_SIGPENDING_] = {1024, 1024},
    [RLIMIT_MSGQUEUE_]   = {819200, 819200},
    [RLIMIT_NICE_]       = {0, 0},
    [RLIMIT_RTPRIO_]     = {0, 0},
    [RLIMIT_RTTIME_]     = {RLIM_INFINITY_, RLIM_INFINITY_},
};

// TODO error propagation
static struct task *construct_task(struct task *parent) {
    struct task *task = task_create_(parent);

    struct tgroup *group = malloc(sizeof(struct tgroup));
    *group = (struct tgroup) {};
    atomic_fetch_add_explicit(&ish_live_guest_procs, 1, memory_order_relaxed);   // [T-ish-freeze-watch]
    list_init(&group->threads);
    list_init(&group->shared_queue);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    memcpy(group->limits, init_rlimits, sizeof(init_rlimits));
    group->leader = task;
    group->personality = ADDR_NO_RANDOMIZE_;
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        atomic_store_explicit(&group->last_progress_ns,
            (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec,
            memory_order_relaxed);
    }
    list_add(&group->threads, &task->group_links);
    task->group = group;
    task->tgid = task->pid;
    task_setsid(task);

    task_set_mm(task, mm_new());
    task->sighand = sighand_new();
    task->files = fdtable_new(3); // why is there a 3 here

    task->fs = fs_info_new();
    task->fs->umask = 0022;
    // we'll need to have current set to do the open call
    struct task *old_current = current;
    current = task;
    task->fs->root = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(task->fs->root))
        return ERR_PTR(task->fs->root);
    task->fs->pwd = fd_retain(task->fs->root);
    current = old_current;

    return task;
}

int become_first_process(void) {
    // now seems like a nice time
    establish_signal_handlers();

    struct task *task = construct_task(NULL);
    if (IS_ERR(task))
        return PTR_ERR(task);

    current = task;
    return 0;
}

int become_new_init_child(void) {
    // locking? who needs locking?!
    struct task *init = pid_get_task(1);
    assert(init != NULL);

    struct task *task = construct_task(init);
    if (IS_ERR(task))
        return PTR_ERR(task);

    // these are things we definitely don't want to inherit
    task->clear_tid = 0;
    task->vfork = NULL;
    task->blocked = task->pending = task->waiting = 0;
    list_init(&task->queue);
    // init (pid 1) itself has no exit signal, but init must be told when this
    // child dies or it never reaps the zombie, which keeps a reference to the
    // child's controlling terminal forever.
    task->exit_signal = SIGCHLD_;
    // TODO: think about whether it would be a good idea to inherit fs_info

    current = task;
    return 0;
}

extern int console_major;
extern int console_minor;
void set_console_device(int major, int minor) {
    console_major = major;
    console_minor = minor;
}

// Creates a character device node unless one is there already. One that a
// filesystem imported on iOS before fakefs_import stored device numbers in
// iSH's encoding has the host's: Darwin's makedev, major << 24 | minor, which
// reads as major 0 and fails to open with ENXIO. The arm64 rootfs comes with
// every node below, so such a node of exactly the device it should be is
// replaced; anything else there is left alone.
static void make_char_device(const char *path, int major, int minor) {
    struct statbuf stat;
    if (generic_statat(AT_PWD, path, &stat, false) == 0 && S_ISCHR(stat.mode) &&
            stat.rdev == ((uint32_t) major << 24 | (uint32_t) minor)) {
        printk("%s has device number %#llx, %d,%d in Darwin's encoding: recreating it\n",
                path, (unsigned long long) stat.rdev, major, minor);
        generic_unlinkat(AT_PWD, path);
    }
    generic_mknodat(AT_PWD, path, S_IFCHR|0666, dev_make(major, minor));
}

void create_some_device_nodes(void) {
    // create some device nodes
    // this will do nothing if they already exist
    make_char_device("/dev/tty1", TTY_CONSOLE_MAJOR, 1);
    make_char_device("/dev/tty2", TTY_CONSOLE_MAJOR, 2);
    make_char_device("/dev/tty3", TTY_CONSOLE_MAJOR, 3);
    make_char_device("/dev/tty4", TTY_CONSOLE_MAJOR, 4);
    make_char_device("/dev/tty5", TTY_CONSOLE_MAJOR, 5);
    make_char_device("/dev/tty6", TTY_CONSOLE_MAJOR, 6);
    make_char_device("/dev/tty7", TTY_CONSOLE_MAJOR, 7);

    make_char_device("/dev/tty", TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR);
    make_char_device("/dev/console", TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR);
    make_char_device("/dev/ptmx", TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR);

    make_char_device("/dev/null", MEM_MAJOR, DEV_NULL_MINOR);
    make_char_device("/dev/zero", MEM_MAJOR, DEV_ZERO_MINOR);
    make_char_device("/dev/full", MEM_MAJOR, DEV_FULL_MINOR);
    make_char_device("/dev/random", MEM_MAJOR, DEV_RANDOM_MINOR);
    make_char_device("/dev/urandom", MEM_MAJOR, DEV_URANDOM_MINOR);

    generic_mkdirat(AT_PWD, "/dev/pts", 0755);
    // POSIX shared memory and named semaphores (shm_open, sem_open) are files
    // in /dev/shm, a tmpfs on Linux that programs take for granted: without
    // it, Python's multiprocessing could make no lock, queue or pool. Here it
    // is a directory of the root filesystem, so what a program leaves there
    // outlives a restart. Its mode is set every boot, as tmpfs gets it at each
    // mount: mkdir's would be cut by the host's umask on realfs.
    int err = generic_mkdirat(AT_PWD, "/dev/shm", 01777);
    if (err == 0 || err == _EEXIST)
        generic_setattrat(AT_PWD, "/dev/shm", make_attr(mode, 01777), false);
}

int create_stdio(const char *file, int major, int minor) {
    struct fd *fd = generic_open(file, O_RDWR_, 0);
    if (!IS_ERR(fd) && !S_ISCHR(fd->stat.mode)) {
        // opened a regular file instead of a char device (e.g. realfs
        // can't create device nodes), so close it and use fallback
        fd_close(fd);
        fd = ERR_PTR(_ENOENT);
    }
    if (IS_ERR(fd)) {
        // fallback to adhoc files for stdio
        fd = adhoc_fd_create(NULL);
        fd->stat.rdev = dev_make(major, minor);
        fd->stat.mode = S_IFCHR | S_IRUSR;
        fd->flags = O_RDWR_;
        int err = dev_open(major, minor, DEV_CHAR, fd);
        if (err < 0)
            return err;
    }

    fd->refcount = 0;
    current->files->files[0] = fd_retain(fd);
    current->files->files[1] = fd_retain(fd);
    current->files->files[2] = fd_retain(fd);
    return 0;
}

static struct fd *open_fd_from_actual_fd(int fd_no) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        return NULL;
    }
    fd->real_fd = fd_no;
    fd->dir = NULL;
    // Populate stat from host fd so guest fstat() returns valid metadata.
    // Node.js (libuv) calls fstat on stdout/stderr to determine if it's a
    // TTY, pipe, or file — all-zero stat causes console.log to silently fail.
    struct stat real_stat;
    if (fstat(fd_no, &real_stat) == 0) {
        fd->stat.mode = real_stat.st_mode;
        fd->stat.rdev = dev_fake_from_real(real_stat.st_rdev);
        fd->stat.inode = real_stat.st_ino;
        fd->stat.size = real_stat.st_size;
        // On macOS, piped/redirected fds may appear as sockets (S_IFSOCK).
        // Guest programs (especially Node.js/libuv) don't recognize sockets
        // as valid stdio types and fail to initialize stdout/stderr handles.
        // Report sockets as character devices (S_IFCHR) which is what Linux
        // shows for TTY fds. This allows node's libuv uv_guess_handle() to
        // detect them as TTY-like (when combined with isatty/TCGETS support)
        // or fall back to a generic stream.
        // Make guest see TTY-like char device when host fd is a socket
        // (macOS piped stdio) or an actual TTY (terminal).
        if (S_ISSOCK(fd->stat.mode) || isatty(fd_no)) {
            fd->stat.mode = S_IFCHR | 0620;
            fd->stat.rdev = dev_make(TTY_PSEUDO_SLAVE_MAJOR, 0);
        }
    }
    return fd;
}

int create_piped_stdio(void) {
    if (!(current->files->files[0] = open_fd_from_actual_fd(STDIN_FILENO))) {
        return -1;
    }
    if (!(current->files->files[1] = open_fd_from_actual_fd(STDOUT_FILENO))) {
        return -1;
    }
    if (!(current->files->files[2] = open_fd_from_actual_fd(STDERR_FILENO))) {
        return -1;
    }
    return 0;
}
