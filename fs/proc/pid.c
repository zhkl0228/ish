#include <string.h>
#include <sys/stat.h>
#include "kernel/memory.h"
#include "kernel/calls.h"
#include "fs/proc.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/fs.h"
#include "kernel/vdso.h"
#include "util/sync.h"

static void proc_pid_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->pid);
}

static struct task *proc_get_task(struct proc_entry *entry) {
    lock(&pids_lock);
    struct task *task = pid_get_task(entry->pid);
    // Also reject tasks that are mid-exit: sighand/group may already be freed.
    if (task != NULL && (task->exiting || task->sighand == NULL || task->group == NULL))
        task = NULL;
    if (task == NULL)
        unlock(&pids_lock);
    return task;
}
static void proc_put_task(struct task *UNUSED(task)) {
    unlock(&pids_lock);
}

// The task's memory in pages, by walking its page table: all mapped, resident
// (backed by real data, not a PROT_NONE reservation or lazy anonymous memory)
// and resident shared. Caller holds task->general_lock, which keeps task->mem
// alive (do_exit unpublishes the mm under it).
static void task_mem_pages(struct task *task, unsigned long *total, unsigned long *resident, unsigned long *shared) {
    *total = *resident = *shared = 0;
    struct mem *mem = task->mem;
    if (mem == NULL)
        return;
    read_wrlock(&mem->lock);
    page_t page = 0;
    while (page < MEM_PAGES) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt != NULL) {
            (*total)++;
            if (pt->data != NULL) {
                (*resident)++;
                if (pt->flags & P_SHARED)
                    (*shared)++;
            }
        }
        mem_next_page(mem, &page);
    }
    read_wrunlock(&mem->lock);
}

// Its state, as Linux's letters: inside a system call, where a task waits,
// it is taken for sleeping (S), as near as iSH can tell; the task reading this
// is running.
static char task_state(struct task *task) {
    if (task->zombie)
        return 'Z';
    if (task->group->stopped)
        return 'T';
    if (task != current && atomic_load(&task->in_syscall))
        return 'S';
    return 'R';
}

// in USER_HZ ticks, as /proc/<pid>/stat has times
static unsigned long timeval_ticks(struct timeval_ tv) {
    return (unsigned long) tv.sec * 100 + (unsigned long) tv.usec / 10000;
}

static int proc_pid_stat_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // Before the group and sighand locks: the page walk takes the memory's
    // lock, which signal delivery takes under sighand's.
    lock(&task->general_lock);
    unsigned long pages, resident, shared;
    task_mem_pages(task, &pages, &resident, &shared);
    // before the group's lock, which this takes
    struct rusage_ rusage = rusage_of_group(task->group);
    lock(&task->group->lock);
    struct rusage_ children = task->group->children_rusage;
    lock(&task->sighand->lock);

    proc_printf(buf, "%d ", task->pid);
    proc_printf(buf, "(%.16s) ", task->comm);
    proc_printf(buf, "%c ", task_state(task));
    proc_printf(buf, "%d ", task->parent ? task->parent->pid : 0);
    proc_printf(buf, "%d ", task->group->pgid);
    proc_printf(buf, "%d ", task->group->sid);
    struct tty *tty = task->group->tty;
    proc_printf(buf, "%d ", tty ? dev_make(tty->driver->major, tty->num) : 0);
    proc_printf(buf, "%d ", tty ? tty->fg_group : 0);
    proc_printf(buf, "%u ", 0); // flags

    // page faults (no data available)
    proc_printf(buf, "%lu ", 0l); // minor faults
    proc_printf(buf, "%lu ", 0l); // children minor faults
    proc_printf(buf, "%lu ", 0l); // major faults
    proc_printf(buf, "%lu ", 0l); // children major faults

    // the process's CPU time (ps's TIME), and its waited-for children's
    proc_printf(buf, "%lu ", timeval_ticks(rusage.utime));
    proc_printf(buf, "%lu ", timeval_ticks(rusage.stime));
    proc_printf(buf, "%ld ", timeval_ticks(children.utime));
    proc_printf(buf, "%ld ", timeval_ticks(children.stime));

    proc_printf(buf, "%ld ", 20l); // priority (not adjustable)
    proc_printf(buf, "%ld ", 0l); // nice (also not adjustable)
    proc_printf(buf, "%ld ", list_size(&task->group->threads));
    proc_printf(buf, "%ld ", 0l); // itimer value (deprecated, always 0)
    proc_printf(buf, "%llu ", (unsigned long long) task->start_ticks); // start, in ticks since boot

    proc_printf(buf, "%lu ", pages * PAGE_SIZE); // vsize, in bytes
    proc_printf(buf, "%lu ", resident); // rss, in pages
    proc_printf(buf, "%lu ", 0l); // rss limit

    // bunch of shit that can only be accessed by a debugger
    proc_printf(buf, "%lu ", 0l); // startcode
    proc_printf(buf, "%lu ", 0l); // endcode
    proc_printf(buf, "%lu ", task->mm ? task->mm->stack_start : 0);
    proc_printf(buf, "%lu ", 0l); // kstkesp
    proc_printf(buf, "%lu ", 0l); // kstkeip

    proc_printf(buf, "%lu ", (unsigned long) task->pending & 0xffffffff);
    proc_printf(buf, "%lu ", (unsigned long) task->blocked & 0xffffffff);
    // signal n is bit n - 1, as in the masks above (it was bit n)
    uint32_t ignored = 0;
    uint32_t caught = 0;
    for (int i = 1; i <= 32; i++) {
        if (task->sighand->action[i].handler == SIG_IGN_)
            ignored |= 1u << (i - 1);
        else if (task->sighand->action[i].handler != SIG_DFL_)
            caught |= 1u << (i - 1);
    }
    proc_printf(buf, "%lu ", (unsigned long) ignored);
    proc_printf(buf, "%lu ", (unsigned long) caught);

    proc_printf(buf, "%lu ", 0l); // wchan (wtf)
    proc_printf(buf, "%lu ", 0l); // nswap
    proc_printf(buf, "%lu ", 0l); // cnswap
    proc_printf(buf, "%d ", task->exit_signal);
    proc_printf(buf, "%d", 0); // processor
    // that's enough for now
    proc_printf(buf, "\n");

    unlock(&task->sighand->lock);
    unlock(&task->group->lock);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

// What iSH knows of Linux's status file. ps takes VSZ and RSS from here
// (procps reads VmSize and VmRSS): with no file they were 0.
static int proc_pid_status_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // the page walk first, as in proc_pid_stat_show
    lock(&task->general_lock);
    unsigned long pages, resident, shared;
    task_mem_pages(task, &pages, &resident, &shared);
    char comm[sizeof(task->comm) + 1] = "";
    strncpy(comm, task->comm, sizeof(task->comm));
    unlock(&task->general_lock);

    proc_printf(buf, "Name:\t%s\n", comm);
    if (task->fs != NULL) {
        lock(&task->fs->lock);
        proc_printf(buf, "Umask:\t%04o\n", task->fs->umask);
        unlock(&task->fs->lock);
    }
    lock(&task->group->lock);
    char state = task_state(task);
    unlock(&task->group->lock);
    proc_printf(buf, "State:\t%c (%s)\n", state,
                state == 'S' ? "sleeping" : state == 'T' ? "stopped" : state == 'Z' ? "zombie" : "running");
    proc_printf(buf, "Tgid:\t%d\n", task->tgid);
    proc_printf(buf, "Ngid:\t0\n");
    proc_printf(buf, "Pid:\t%d\n", task->pid);
    proc_printf(buf, "PPid:\t%d\n", task->parent ? task->parent->tgid : 0);
    lock(&task->ptrace.lock);
    bool traced = task->ptrace.traced;
    unlock(&task->ptrace.lock);
    // iSH's tracer is the parent (PTRACE_TRACEME)
    proc_printf(buf, "TracerPid:\t%d\n", traced && task->parent ? task->parent->tgid : 0);
    // the filesystem ids follow the effective ones: iSH has no setfsuid
    proc_printf(buf, "Uid:\t%u\t%u\t%u\t%u\n", task->uid, task->euid, task->suid, task->euid);
    proc_printf(buf, "Gid:\t%u\t%u\t%u\t%u\n", task->gid, task->egid, task->sgid, task->egid);
    if (task->files != NULL) {
        lock(&task->files->lock);
        proc_printf(buf, "FDSize:\t%u\n", task->files->size);
        unlock(&task->files->lock);
    }
    proc_printf(buf, "Groups:\t");
    for (unsigned i = 0; i < task->ngroups; i++)
        proc_printf(buf, "%u ", task->groups[i]);
    proc_printf(buf, "\n");
    proc_printf(buf, "NStgid:\t%d\n", task->tgid);
    proc_printf(buf, "NSpid:\t%d\n", task->pid);
    proc_printf(buf, "NSpgid:\t%d\n", task->group->pgid);
    proc_printf(buf, "NSsid:\t%d\n", task->group->sid);
    proc_printf(buf, "VmSize:\t%8lu kB\n", pages * (PAGE_SIZE / 1024));
    proc_printf(buf, "VmRSS:\t%8lu kB\n", resident * (PAGE_SIZE / 1024));
    proc_printf(buf, "RssShmem:\t%8lu kB\n", shared * (PAGE_SIZE / 1024));
    proc_printf(buf, "Threads:\t%d\n", list_size(&task->group->threads));
    lock(&task->sighand->lock);
    uint64_t ignored = 0, caught = 0;
    for (int i = 1; i < NUM_SIGS; i++) {
        if (task->sighand->action[i].handler == SIG_IGN_)
            ignored |= 1ull << (i - 1);
        else if (task->sighand->action[i].handler != SIG_DFL_)
            caught |= 1ull << (i - 1);
    }
    proc_printf(buf, "SigPnd:\t%016llx\n", (unsigned long long) task->pending);
    proc_printf(buf, "ShdPnd:\t%016llx\n", (unsigned long long) task->group->shared_pending);
    proc_printf(buf, "SigBlk:\t%016llx\n", (unsigned long long) task->blocked);
    proc_printf(buf, "SigIgn:\t%016llx\n", (unsigned long long) ignored);
    proc_printf(buf, "SigCgt:\t%016llx\n", (unsigned long long) caught);
    unlock(&task->sighand->lock);

    proc_put_task(task);
    return 0;
}

static int proc_pid_statm_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;

    // Reporting all-zeros here breaks runtimes (e.g. Bun/JSC) that derive
    // pool/lock sizes from the reported VM size and reject a value of 0
    // ("Invalid value for lock: 0").
    unsigned long total, resident, shared; // in pages
    // [T-ish-exit-mm-general-lock] general_lock is what keeps task->mem alive
    // while we walk it (do_exit unpublishes the mm under it); cmdline/environ
    // already did this, statm/maps/mem did not.
    lock(&task->general_lock);
    task_mem_pages(task, &total, &resident, &shared);
    unlock(&task->general_lock);

    proc_printf(buf, "%lu ", total);    // total program size
    proc_printf(buf, "%lu ", resident); // resident set size
    proc_printf(buf, "%lu ", shared);   // resident shared pages
    proc_printf(buf, "%lu ", 0);        // text
    proc_printf(buf, "%lu ", 0);        // lib (always 0 since linux 2.6)
    proc_printf(buf, "%lu ", resident); // data + stack
    proc_printf(buf, "%lu ", 0);        // dirty (always 0 since linux 2.6)
    proc_printf(buf, "\n");

    proc_put_task(task);
    return 0;
}

static int proc_pid_auxv_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    int err = 0;
    lock(&task->general_lock);
    if (task->mm == NULL)
        goto out_free_task;

    size_t size = task->mm->auxv_end - task->mm->auxv_start;
    char *data = malloc(size);
    if (data == NULL) {
        err = _ENOMEM;
        goto out_free_task;
    }
    if (user_read_task(task, task->mm->auxv_start, data, size) == 0)
        proc_buf_append(buf, data, size);
    free(data);

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_cmdline_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    int err = 0;
    lock(&task->general_lock);
    if (task->mm == NULL)
        goto out_free_task;

    size_t size = task->mm->argv_end - task->mm->argv_start;
    char *data = malloc(size);
    if (data == NULL) {
        err = _ENOMEM;
        goto out_free_task;
    }
    if (user_read_task(task, task->mm->argv_start, data, size) == 0)
        proc_buf_append(buf, data, size);
    free(data);

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_mem_show(struct proc_entry *entry, struct proc_data *buf) {
    // mem_show does not need to do any data set-up. The /proc/mem read/write 
    // operations do not rely on this set-up as they call user_read/write 
    // operations directly.

    (void)entry;
    (void)buf;
    return 0;
}

// The /proc/pid/smaps fields iSH can tell from its page table, in pages. A
// page is shared while it is copy-on-write after a fork or mapped MAP_SHARED,
// and private otherwise. Anonymous memory has no backing to be clean against,
// so its pages count as dirty and file pages as clean.
struct smaps_counts {
    unsigned long shared_clean, shared_dirty, private_clean, private_dirty, anonymous;
};

static void smaps_count_page(struct smaps_counts *counts, unsigned flags) {
    bool anonymous = flags & P_ANONYMOUS;
    if (flags & (P_COW | P_SHARED)) {
        if (anonymous)
            counts->shared_dirty++;
        else
            counts->shared_clean++;
    } else {
        if (anonymous)
            counts->private_dirty++;
        else
            counts->private_clean++;
    }
    if (anonymous)
        counts->anonymous++;
}

static void smaps_print(struct proc_data *buf, page_t pages, const struct smaps_counts *c, unsigned flags) {
    unsigned long kb = PAGE_SIZE / 1024;
    unsigned long shared = c->shared_clean + c->shared_dirty;
    unsigned long private = c->private_clean + c->private_dirty;
    // A shared page is split between the two sides of the fork that shared it;
    // iSH does not count how many processes still map it.
    unsigned long pss = (private * kb) + (shared * kb) / 2;
    proc_printf(buf, "%-16s%8lu kB\n", "Size:", (unsigned long) pages * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "KernelPageSize:", kb);
    proc_printf(buf, "%-16s%8lu kB\n", "MMUPageSize:", kb);
    proc_printf(buf, "%-16s%8lu kB\n", "Rss:", (unsigned long) pages * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "Pss:", pss);
    proc_printf(buf, "%-16s%8lu kB\n", "Shared_Clean:", c->shared_clean * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "Shared_Dirty:", c->shared_dirty * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "Private_Clean:", c->private_clean * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "Private_Dirty:", c->private_dirty * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "Referenced:", (unsigned long) pages * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "Anonymous:", c->anonymous * kb);
    proc_printf(buf, "%-16s%8lu kB\n", "LazyFree:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "AnonHugePages:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "ShmemPmdMapped:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "FilePmdMapped:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "Shared_Hugetlb:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "Private_Hugetlb:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "Swap:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "SwapPss:", 0ul);
    proc_printf(buf, "%-16s%8lu kB\n", "Locked:", 0ul);
    proc_printf(buf, "VmFlags:%s%s%s%s%s\n",
            flags & P_READ ? " rd" : "",
            flags & P_WRITE ? " wr" : "",
            flags & P_EXEC ? " ex" : "",
            flags & P_SHARED ? " sh" : "",
            flags & P_GROWSDOWN ? " gd" : "");
}

// /proc/pid/maps, and with smaps the /proc/pid/smaps fields after each line.
static void proc_maps_show_regions(struct task *task, struct proc_data *buf, bool smaps) {
    struct mem *mem = task->mem;
    if (mem == NULL)
        return;

    read_wrlock(&mem->lock);
    page_t page = 0;
    while (page < MEM_PAGES) {
        // find a region
        while (page < MEM_PAGES && mem_pt(mem, page) == NULL) {
            mem_next_page(mem, &page);
        }
        if (page >= MEM_PAGES)
            break;
        page_t start = page;
        struct pt_entry *start_pt = mem_pt(mem, start);
        struct data *data = start_pt->data;

        // find the end of said region
        struct smaps_counts counts = {0};
        while (page < MEM_PAGES) {
            struct pt_entry *pt = mem_pt(mem, page);
            if (pt == NULL)
                break;
            if ((pt->flags & P_RWX) != (start_pt->flags & P_RWX))
                break;
            // region continues if data is the same or both are anonymous
            if (!(pt->data == data || (pt->flags & P_ANONYMOUS && start_pt->flags & P_ANONYMOUS)))
                break;
            smaps_count_page(&counts, pt->flags);
            mem_next_page(mem, &page);
        }
        page_t end = page;

        // output info
        char path[MAX_PATH] = "";
        if (start_pt->flags & P_GROWSDOWN) {
            strcpy(path, "[stack]");
        } else if (data->name != NULL) {
            strcpy(path, data->name);
        } else if (data->fd != NULL) {
            generic_getpath(start_pt->data->fd, path);
        }
#ifdef GUEST_ARM64
        proc_printf(buf, "%012llx-%012llx %c%c%c%c %08lx 00:00 %-10d %s\n",
                (unsigned long long)(start << PAGE_BITS),
                (unsigned long long)(end << PAGE_BITS),
#else
        proc_printf(buf, "%08x-%08x %c%c%c%c %08lx 00:00 %-10d %s\n",
                start << PAGE_BITS, end << PAGE_BITS,
#endif
                start_pt->flags & P_READ ? 'r' : '-',
                start_pt->flags & P_WRITE ? 'w' : '-',
                start_pt->flags & P_EXEC ? 'x' : '-',
                start_pt->flags & P_SHARED ? '-' : 'p',
                (unsigned long) data->file_offset, // offset
                0, // inode
                path);
        if (smaps)
            smaps_print(buf, end - start, &counts, start_pt->flags);
    }
    read_wrunlock(&mem->lock);
}

void proc_maps_dump(struct task *task, struct proc_data *buf) {
    proc_maps_show_regions(task, buf, false);
}

static int proc_pid_maps_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
    proc_maps_dump(task, buf);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

static int proc_pid_smaps_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
    proc_maps_show_regions(task, buf, true);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

static ssize_t proc_pid_mem_pread(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
    int result = user_read_task(task, (addr_t)offset, buf->data, buf->size);
    unlock(&task->general_lock);
    proc_put_task(task);
    return result ? -1 : buf->size;
}

static ssize_t proc_pid_mem_pwrite(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
    int result = user_write_task_ptrace(task, (addr_t)offset, buf->data, buf->size);
    unlock(&task->general_lock);
    proc_put_task(task);
    return result ? -1 : buf->size;
}


static struct proc_dir_entry proc_pid_fd;

static bool proc_pid_fd_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = proc_get_task(entry);
    // Task exited while its /proc/<pid>/fd dir was being iterated (e.g.
    // `find /proc` racing short-lived children). This function returns bool —
    // the old `return _ESRCH` here (-3) read as TRUE, so proc_readdir consumed
    // a next_entry that was never filled in (meta == NULL) and crashed in
    // proc_entry_getname (EXC_BAD_ACCESS at 0x10). No task = no more entries.
    if (task == NULL)
        return false;
    // Retain the fdtable, then drop pids_lock BEFORE taking files->lock, to
    // avoid the pids_lock -> files->lock ordering (proc_get_task holds
    // pids_lock). Other paths take files->lock and then pids_lock (e.g. via
    // proc_entry_stat); holding both here in the opposite order deadlocked
    // claude-cli startup when 20+ bun workers scan /proc/self/fd at once.
    struct fdtable *files = task->files;
    files->refcount++;
    proc_put_task(task); // releases pids_lock
    lock(&files->lock);
    while (*index < files->size && files->files[*index] == NULL)
        (*index)++;
    fd_t f = (*index)++;
    bool any_left = (unsigned) f < files->size;
    unlock(&files->lock);
    fdtable_release(files);
    *next_entry = (struct proc_entry) {&proc_pid_fd, .pid = entry->pid, .fd = f};
    return any_left;
}

static void proc_pid_fd_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->fd);
}

static int proc_pid_fd_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // Retain the fd under files->lock, then drop BOTH files->lock and
    // pids_lock (held by proc_get_task) before calling generic_getpath.
    // getpath can take other subsystem locks (mounts, real fs), and holding
    // pids_lock across it created a lock-ordering hazard against paths that
    // take those locks first and then pids_lock — an intermittent deadlock
    // during claude-cli startup when 20+ bun worker threads all scan
    // /proc/self/fd concurrently. Also: fd may be NULL if another thread
    // closed the descriptor (return ENOENT, not a host crash).
    lock(&task->files->lock);
    struct fd *fd = fdtable_get(task->files, entry->fd);
    if (fd != NULL)
        fd = fd_retain(fd);
    unlock(&task->files->lock);
    proc_put_task(task); // releases pids_lock
    if (fd == NULL)
        return _ENOENT;
    int err = generic_getpath(fd, buf);
    fd_close(fd);
    return err;
}

static int proc_pid_exe_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock);
    int err;
    if (task->mm == NULL || task->mm->exefile == NULL)
        err = _ENOENT;
    else
        err = generic_getpath(task->mm->exefile, buf);
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static void proc_pid_task_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->pid);
}

static int proc_pid_task_readlink(struct proc_entry *entry, char *buf) {
    sprintf(buf, "/proc/%d", entry->pid);
    return 0;
}

static struct proc_dir_entry proc_pid_task;

static bool proc_pid_task_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    // TODO: Expose all threads
    *next_entry = (struct proc_entry) {&proc_pid_task, .pid = entry->pid};
    return !(*index)++;
}

static int proc_pid_cwd_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->fs->lock);
    int err = generic_getpath(task->fs->pwd, buf);
    unlock(&task->fs->lock);
    proc_put_task(task);
    return err;
}


struct proc_children proc_pid_children = PROC_CHILDREN({
    {"auxv", .show = proc_pid_auxv_show},
    {"cmdline", .show = proc_pid_cmdline_show},
    {"cwd", S_IFLNK, .readlink = proc_pid_cwd_readlink},
    {"exe", S_IFLNK, .readlink = proc_pid_exe_readlink},
    {"fd", S_IFDIR, .readdir = proc_pid_fd_readdir},
    {"maps", .show = proc_pid_maps_show},
    {"smaps", .show = proc_pid_smaps_show},
    {"mem", .pread = proc_pid_mem_pread, .pwrite = proc_pid_mem_pwrite, .show = proc_pid_mem_show},
    {"stat", .show = proc_pid_stat_show},
    {"statm", .show = proc_pid_statm_show},
    {"status", .show = proc_pid_status_show},
    {"task", S_IFDIR, .readdir = proc_pid_task_readdir},
});

struct proc_dir_entry proc_pid = {NULL, S_IFDIR,
    .children = &proc_pid_children, .getname = proc_pid_getname};

static struct proc_dir_entry proc_pid_fd = {NULL, S_IFLNK,
    .getname = proc_pid_fd_getname, .readlink = proc_pid_fd_readlink};

static struct proc_dir_entry proc_pid_task = {NULL, S_IFLNK,
    .getname = proc_pid_task_getname, .readlink = proc_pid_task_readlink};
