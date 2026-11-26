#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "kernel/memory.h"
#include "kernel/calls.h"
#include "fs/proc.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/fs.h"
#include "fs/proc/ish.h"
#include "kernel/vdso.h"
#include "util/sync.h"

static void proc_pid_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->pid);
}

// The task an entry is of, with pids_lock held (which keeps an exited one from
// being reaped and freed), or NULL. One that has exited but is not gone is
// too, as Linux shows it (state Z): a zombie its parent has not waited for,
// and a leader that left before its process's other threads. /proc had
// neither: ps and top showed no <defunct>, and a process whose main thread
// had called pthread_exit had no stat (ESRCH) while its threads ran. Such a
// task's memory, files, fs and signal handlers are gone (NULL), and so may a
// live one's files and fs be as it exits: each file says what Linux says
// without them, mostly nothing.
static struct task *proc_get_task(struct proc_entry *entry) {
    lock(&pids_lock);
    struct task *task = pid_get_task_zombie(entry->pid);
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

size_t ish_processes(struct ish_process *out, size_t max) {
    size_t count = 0;
    lock(&pids_lock);
    for (pid_t_ pid = 2; pid <= MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (task == NULL || task->exiting || task->tgid != pid)
            continue;
        if (count < max) {
            struct rusage_ rusage = rusage_of_group(task->group);
            struct ish_process *process = &out[count];
            process->pid = pid;
            process->start_ticks = task->start_ticks;
            process->cpu_us = (uint64_t) (rusage.utime.sec + rusage.stime.sec) * 1000000 +
                (uint64_t) (rusage.utime.usec + rusage.stime.usec);
            memcpy(process->comm, task->comm, sizeof(task->comm));
            process->comm[sizeof(task->comm)] = '\0';
        }
        count++;
    }
    unlock(&pids_lock);
    return count;
}

// The live process pid, if it is the one started at start_ticks, with
// pids_lock held; NULL, and the lock let go, if it is not
static struct task *process_started_at(int pid, uint64_t start_ticks) {
    lock(&pids_lock);
    struct task *task = pid_get_task(pid);
    if (task == NULL || task->exiting || task->tgid != pid || task->start_ticks != start_ticks) {
        unlock(&pids_lock);
        return NULL;
    }
    return task;
}

uint64_t ish_process_resident(int pid, uint64_t start_ticks) {
    struct task *task = process_started_at(pid, start_ticks);
    if (task == NULL)
        return 0;
    // as /proc/<pid>/status does it, pids_lock then the task's general_lock
    lock(&task->general_lock);
    unsigned long pages, resident, shared;
    task_mem_pages(task, &pages, &resident, &shared);
    unlock(&task->general_lock);
    unlock(&pids_lock);
    return (uint64_t) resident * PAGE_SIZE;
}

int ish_process_end(int pid, uint64_t start_ticks, bool force) {
    struct task *task = process_started_at(pid, start_ticks);
    if (task == NULL)
        return _ESRCH;
    send_process_signal(task->group, force ? SIGKILL_ : SIGTERM_, SIGINFO_NIL);
    unlock(&pids_lock);
    return 0;
}

// Its state, as Linux's letters: inside a system call, where a task waits,
// it is taken for sleeping (S), as near as iSH can tell; the task reading this
// is running.
static char task_state(struct task *task) {
    if (task->zombie || task->exiting)
        return 'Z';
    if (task->group->stopped)
        return 'T';
    if (task != current && atomic_load(&task->in_syscall))
        return 'S';
    return 'R';
}

// The process's threads, as Linux counts them: a leader that has exited, which
// is no longer on the list, until the process is gone
static long group_threads(struct tgroup *group) {
    return list_size(&group->threads) + (group->leader->exiting ? 1 : 0);
}

// The signals ignored and handled, as bits (signal n is bit n - 1), of those
// below max; none once the task has exited and let its handlers go
static void signal_dispositions(struct task *task, int max, uint64_t *ignored, uint64_t *caught) {
    *ignored = *caught = 0;
    if (task->sighand == NULL)
        return;
    lock(&task->sighand->lock);
    for (int i = 1; i < max; i++) {
        if (task->sighand->action[i].handler == SIG_IGN_)
            *ignored |= 1ull << (i - 1);
        else if (task->sighand->action[i].handler != SIG_DFL_)
            *caught |= 1ull << (i - 1);
    }
    unlock(&task->sighand->lock);
}

// in USER_HZ ticks, as /proc/<pid>/stat has times
static unsigned long timeval_ticks(struct timeval_ tv) {
    return (unsigned long) tv.sec * 100 + (unsigned long) tv.usec / 10000;
}

// The system call the thread is in, as Linux's /proc/<pid>/syscall says it:
// its number, its six arguments, the stack pointer and the pc (all but the
// number in hex). A thread that is not in one is "running", as Linux says of
// one running: it runs guest code, whose pc iSH has only as of its last
// leaving translated code (stat's kstkeip has that). A hung program's threads,
// waiting in futex, poll or read, say where they wait.
static int proc_pid_syscall_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (!task->zombie && atomic_load(&task->in_syscall)) {
        const struct cpu_state *cpu = &task->cpu;
        proc_printf(buf, "%d 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx\n", (int) cpu->regs[8],
                    (unsigned long long) cpu->regs[0], (unsigned long long) cpu->regs[1],
                    (unsigned long long) cpu->regs[2], (unsigned long long) cpu->regs[3],
                    (unsigned long long) cpu->regs[4], (unsigned long long) cpu->regs[5],
                    (unsigned long long) cpu->sp, (unsigned long long) cpu->pc);
    } else {
        proc_printf(buf, "running\n");
    }
    proc_put_task(task);
    return 0;
}

// stat of the process (whole), or of one thread (/proc/<pid>/task/<tid>/stat),
// as Linux's do_task_stat: the same but for the CPU time, the thread's own
static int proc_stat_show(struct proc_entry *entry, struct proc_data *buf, bool whole) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // Before the group and sighand locks: the page walk takes the memory's
    // lock, which signal delivery takes under sighand's.
    lock(&task->general_lock);
    unsigned long pages, resident, shared;
    task_mem_pages(task, &pages, &resident, &shared);
    // before the group's lock, which this takes
    struct rusage_ rusage = whole ? rusage_of_group(task->group) : rusage_of_thread(task);
    lock(&task->group->lock);
    struct rusage_ children = task->group->children_rusage;

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

    // the nice setpriority gave it (pids_lock held, as there), and the
    // priority of a task with it, as Linux's for a task that is not real
    // time: they were 20 and 0 of every task, and ps and top said NI 0 of
    // what nice or renice had made 5
    proc_printf(buf, "%d ", 20 + task->nice); // priority
    proc_printf(buf, "%d ", task->nice); // nice
    proc_printf(buf, "%ld ", group_threads(task->group));
    proc_printf(buf, "%ld ", 0l); // itimer value (deprecated, always 0)
    proc_printf(buf, "%llu ", (unsigned long long) task->start_ticks); // start, in ticks since boot

    proc_printf(buf, "%lu ", pages * PAGE_SIZE); // vsize, in bytes
    proc_printf(buf, "%lu ", resident); // rss, in pages
    proc_printf(buf, "%lu ", 0l); // rss limit

    // bunch of shit that can only be accessed by a debugger
    proc_printf(buf, "%lu ", 0l); // startcode
    proc_printf(buf, "%lu ", 0l); // endcode
    proc_printf(buf, "%lu ", task->mm ? task->mm->stack_start : 0);
    // where the thread's stack and code were when it last left translated
    // code (a system call, a block's end): Linux has them 0 now, but ps -o
    // esp,eip reads them, and a thread running guest code has no other
    // (/proc/<pid>/syscall says "running" of it)
    proc_printf(buf, "%llu ", task->zombie ? 0ull : (unsigned long long) task->cpu.sp); // kstkesp
    proc_printf(buf, "%llu ", task->zombie ? 0ull : (unsigned long long) task->cpu.pc); // kstkeip

    proc_printf(buf, "%lu ", (unsigned long) task->pending & 0xffffffff);
    proc_printf(buf, "%lu ", (unsigned long) task->blocked & 0xffffffff);
    // signal n is bit n - 1, as in the masks above (it was bit n)
    uint64_t ignored, caught;
    signal_dispositions(task, 33, &ignored, &caught);
    proc_printf(buf, "%lu ", (unsigned long) ignored);
    proc_printf(buf, "%lu ", (unsigned long) caught);

    proc_printf(buf, "%lu ", 0l); // wchan (wtf)
    proc_printf(buf, "%lu ", 0l); // nswap
    proc_printf(buf, "%lu ", 0l); // cnswap
    proc_printf(buf, "%d ", task->exit_signal);
    proc_printf(buf, "%d", 0); // processor
    // that's enough for now
    proc_printf(buf, "\n");

    unlock(&task->group->lock);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

static int proc_pid_stat_show(struct proc_entry *entry, struct proc_data *buf) {
    return proc_stat_show(entry, buf, true);
}

static int proc_tid_stat_show(struct proc_entry *entry, struct proc_data *buf) {
    return proc_stat_show(entry, buf, false);
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
    proc_printf(buf, "Threads:\t%ld\n", group_threads(task->group));
    uint64_t ignored, caught;
    signal_dispositions(task, NUM_SIGS, &ignored, &caught);
    proc_printf(buf, "SigPnd:\t%016llx\n", (unsigned long long) task->pending);
    proc_printf(buf, "ShdPnd:\t%016llx\n", (unsigned long long) task->group->shared_pending);
    proc_printf(buf, "SigBlk:\t%016llx\n", (unsigned long long) task->blocked);
    proc_printf(buf, "SigIgn:\t%016llx\n", (unsigned long long) ignored);
    proc_printf(buf, "SigCgt:\t%016llx\n", (unsigned long long) caught);

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

    // the fields with a space between them, as Linux's: there was one after
    // the last
    proc_printf(buf, "%lu ", total);    // total program size
    proc_printf(buf, "%lu ", resident); // resident set size
    proc_printf(buf, "%lu ", shared);   // resident shared pages
    proc_printf(buf, "%lu ", 0);        // text
    proc_printf(buf, "%lu ", 0);        // lib (always 0 since linux 2.6)
    proc_printf(buf, "%lu ", resident); // data + stack
    proc_printf(buf, "%lu", 0);         // dirty (always 0 since linux 2.6)
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

// The environment the process was started with, NUL after each string, as
// cmdline its arguments: there was no environ, and nothing could say what a
// running process had been given (tr '\0' '\n' < /proc/<pid>/environ). Only its
// owner reads it, as on Linux (mode 0400).
static int proc_pid_environ_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    int err = 0;
    lock(&task->general_lock);
    if (task->mm == NULL)
        goto out_free_task;

    size_t size = task->mm->env_end - task->mm->env_start;
    char *data = malloc(size);
    if (data == NULL) {
        err = _ENOMEM;
        goto out_free_task;
    }
    if (user_read_task(task, task->mm->env_start, data, size) == 0)
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

// Whether a page is in the region of a mapped page before it, as maps tells
// regions: the same permissions, and the same data or both anonymous
static bool proc_maps_same_region(struct pt_entry *start_pt, struct pt_entry *pt) {
    if ((pt->flags & P_RWX) != (start_pt->flags & P_RWX))
        return false;
    return pt->data == start_pt->data || (pt->flags & P_ANONYMOUS && start_pt->flags & P_ANONYMOUS);
}

// The end of the region that starts at start, a mapped page; its pages
// counted for smaps in counts. Under mem->lock.
static page_t proc_maps_region_end(struct mem *mem, page_t start, struct smaps_counts *counts) {
    struct pt_entry *start_pt = mem_pt(mem, start);
    page_t page = start;
    while (page < MEM_PAGES) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt == NULL || !proc_maps_same_region(start_pt, pt))
            break;
        smaps_count_page(counts, pt->flags);
        // mem_next_page skips page tables that hold nothing, so where it
        // goes further than the next page, there was a gap: the region
        // ends here. Without this an anonymous mapping ran on across the
        // gap into the next one with its permissions: a JVM's hs_err map
        // had one rw-p line of 15TB, up to where its CDS archive starts.
        page_t next = page;
        mem_next_page(mem, &next);
        if (next != page + 1) {
            page++;
            break;
        }
        page = next;
    }
    return page;
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
        page = proc_maps_region_end(mem, start, &counts);
        page_t end = page;

        // output info
        char path[MAX_PATH] = "";
        if (start_pt->flags & P_GROWSDOWN) {
            strcpy(path, "[stack]");
        } else if (data->name != NULL) {
            strcpy(path, data->name);
        } else if (data->fd != NULL) {
            // a file with no path in the guest (realfs_getpath logs why)
            // is listed with none, not the host's path left in path
            if (generic_getpath(start_pt->data->fd, path) < 0)
                path[0] = '\0';
        }
        proc_printf(buf, "%012llx-%012llx %c%c%c%c %08lx 00:00 %-10d %s\n",
                (unsigned long long)(start << PAGE_BITS),
                (unsigned long long)(end << PAGE_BITS),
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
    // none once it has exited: nothing, as Linux's mem_rw
    ssize_t result = task->mem == NULL ? 0 :
        user_read_task(task, (addr_t)offset, buf->data, buf->size) ? -1 : (ssize_t) buf->size;
    unlock(&task->general_lock);
    proc_put_task(task);
    return result;
}

static ssize_t proc_pid_mem_pwrite(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
    ssize_t result = task->mem == NULL ? 0 :
        user_write_task_ptrace(task, (addr_t)offset, buf->data, buf->size) ? -1 : (ssize_t) buf->size;
    unlock(&task->general_lock);
    proc_put_task(task);
    return result;
}


static struct proc_dir_entry proc_pid_fd, proc_tid_fd;

// The entries of a fd directory, each of meta: an entry's meta has one parent
// directory, so /proc/<pid>/fd's are proc_pid_fd and task/<tid>/fd's
// proc_tid_fd. None once the task has exited, as Linux's.
static bool fd_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry,
                       struct proc_dir_entry *meta) {
    struct task *task = proc_get_task(entry);
    // Task exited while its /proc/<pid>/fd dir was being iterated (e.g.
    // `find /proc` racing short-lived children). This function returns bool —
    // the old `return _ESRCH` here (-3) read as TRUE, so proc_readdir consumed
    // a next_entry that was never filled in (meta == NULL) and crashed in
    // proc_entry_getname (EXC_BAD_ACCESS at 0x10). No task = no more entries.
    if (task == NULL)
        return false;
    if (task->files == NULL) {
        proc_put_task(task);
        return false;
    }
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
    *next_entry = (struct proc_entry) {meta, .pid = entry->pid, .fd = f};
    return any_left;
}

static bool proc_pid_fd_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return fd_readdir(entry, index, next_entry, &proc_pid_fd);
}

static bool proc_tid_fd_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    return fd_readdir(entry, index, next_entry, &proc_tid_fd);
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
    if (task->files == NULL) {
        proc_put_task(task);
        return _ENOENT;
    }
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

// The file a /proc/<pid>/fd/N link is to, when it has no path the link's
// text could be followed to: a pipe ("pipe:[N]"), which bash's process
// substitution, <(...) and >(...), opens as /dev/fd/N. Linux's links are to
// the file itself (generic_fd_link). NULL if the entry is no such link, or
// its file has a path (or is none here: logged).
struct fd *proc_pid_fd_file(struct proc_entry *entry) {
    if (entry->meta != &proc_pid_fd && entry->meta != &proc_tid_fd)
        return NULL;
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return NULL;
    // another user's only to root, as Linux's ptrace access check
    if (!superuser() && task->uid != current->euid) {
        proc_put_task(task);
        return ERR_PTR(_EACCES);
    }
    if (task->files == NULL) {
        proc_put_task(task);
        return NULL;
    }
    lock(&task->files->lock);
    struct fd *fd = fdtable_get(task->files, entry->fd);
    if (fd != NULL)
        fd = fd_retain(fd);
    unlock(&task->files->lock);
    proc_put_task(task); // releases pids_lock
    if (fd == NULL)
        return NULL;
    if (is_adhoc_fd(fd) && fd->type == S_IFIFO)
        return fd;
    if (is_adhoc_fd(fd)) {
        char link[MAX_PATH];
        if (generic_getpath(fd, link) < 0)
            strcpy(link, "?");
        FIXME("/proc/%d/fd/%d (%s) as its file not implemented", entry->pid, entry->fd, link);
    }
    fd_close(fd);
    return NULL;
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

// The task's name, as Linux's comm: musl's pthread_getname_np and
// pthread_setname_np of another thread read and write
// /proc/self/task/<tid>/comm. There was none (ENOENT).
static int proc_pid_comm_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock);
    proc_printf(buf, "%.16s\n", task->comm);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

// As Linux's comm_write: a thread of the writer's process takes its first 15
// bytes for its name; another's is EINVAL.
static ssize_t proc_pid_comm_pwrite(struct proc_entry *entry, struct proc_data *data, off_t UNUSED(off)) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    ssize_t res = (ssize_t) data->size;
    if (task->group != current->group) {
        res = _EINVAL;
    } else {
        lock(&task->general_lock);
        size_t len = data->size < sizeof(task->comm) - 1 ? data->size : sizeof(task->comm) - 1;
        memcpy(task->comm, data->data, len);
        task->comm[len] = '\0';
        unlock(&task->general_lock);
    }
    proc_put_task(task);
    return res;
}

static void proc_pid_task_getname(struct proc_entry *entry, char *buf) {
    sprintf(buf, "%d", entry->pid);
}

static struct proc_dir_entry proc_pid_task;

// Every thread of the process, by tid, each a directory of that thread
// (proc_tid_children). There was its leader only: /proc/self/task/<tid> of
// any other thread was ENOENT. index is the last tid said, so a thread that
// comes or goes between calls neither repeats one nor skips one.
static bool proc_pid_task_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return false;
    pid_t_ next = 0;
    struct task *thread;
    list_for_each_entry(&task->group->threads, thread, group_links) {
        if ((unsigned long) thread->pid > *index && (next == 0 || thread->pid < next))
            next = thread->pid;
    }
    // a leader that has exited is off the list, and listed till its process is gone
    struct task *leader = task->group->leader;
    if (leader->exiting && (unsigned long) leader->pid > *index && (next == 0 || leader->pid < next))
        next = leader->pid;
    proc_put_task(task);
    if (next == 0)
        return false;
    *next_entry = (struct proc_entry) {&proc_pid_task, .pid = next};
    *index = next;
    return true;
}

// /proc/<pid>/map_files: a symlink for each region of maps that maps a file,
// named by its range ("%lx-%lx", as Linux names them), to the file. Gradle's
// file-events library lists it to tell musl from glibc: a link to a file
// named "-musl-" (ld-musl-aarch64.so.1) is musl. There was none, and on
// Alpine it loaded its glibc build, which failed (Gradle 9 ran only with
// org.gradle.native=false).
static struct proc_dir_entry proc_pid_map_file;

// The names of the regions that map a file, in order, NULL after them
static char **proc_map_files_names(struct task *task) {
    size_t count = 0, capacity = 16;
    char **names = malloc(capacity * sizeof(*names));
    struct mem *mem = task->mem;
    if (mem != NULL) {
        read_wrlock(&mem->lock);
        page_t page = 0;
        while (page < MEM_PAGES) {
            while (page < MEM_PAGES && mem_pt(mem, page) == NULL)
                mem_next_page(mem, &page);
            if (page >= MEM_PAGES)
                break;
            page_t start = page;
            struct smaps_counts counts = {0};
            page = proc_maps_region_end(mem, start, &counts);
            if (mem_pt(mem, start)->data->fd == NULL)
                continue;
            if (count + 1 >= capacity) {
                capacity *= 2;
                names = realloc(names, capacity * sizeof(*names));
            }
            char name[40];
            snprintf(name, sizeof(name), "%lx-%lx", (unsigned long) start << PAGE_BITS,
                     (unsigned long) page << PAGE_BITS);
            names[count++] = strdup(name);
        }
        read_wrunlock(&mem->lock);
    }
    names[count] = NULL;
    return names;
}

static bool proc_pid_map_files_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    if (*index == 0 || entry->child_names == NULL) {
        if (entry->child_names != NULL)
            free_string_array(entry->child_names);
        entry->child_names = NULL;
        struct task *task = proc_get_task(entry);
        if (task == NULL)
            return false;
        lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
        entry->child_names = proc_map_files_names(task);
        unlock(&task->general_lock);
        proc_put_task(task);
    }
    if (entry->child_names[*index] == NULL)
        return false;
    *next_entry = (struct proc_entry) {&proc_pid_map_file, .pid = entry->pid,
        .name = strdup(entry->child_names[*index])};
    (*index)++;
    return true;
}

// A name, found without listing them all (every entry is looked up, and a
// listing walks the whole page table): the range of a region that maps a
// file, from its first page to its end, as readdir names it
static bool proc_pid_map_files_lookup(struct proc_entry *entry, const char *name, unsigned long *index,
                                      struct proc_entry *next_entry) {
    char *dash, *rest;
    unsigned long start = strtoul(name, &dash, 16);
    unsigned long end = strtoul(dash + (*dash == '-'), &rest, 16);
    char spelled[40];
    snprintf(spelled, sizeof(spelled), "%lx-%lx", start, end);
    if (strcmp(spelled, name) != 0 || start % PAGE_SIZE != 0 || end % PAGE_SIZE != 0 ||
            start >= end || end >> PAGE_BITS > MEM_PAGES)
        return false;
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return false;
    bool found = false;
    lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
    struct mem *mem = task->mem;
    if (mem != NULL) {
        read_wrlock(&mem->lock);
        page_t first = start >> PAGE_BITS;
        struct pt_entry *pt = mem_pt(mem, first);
        struct pt_entry *before = first > 0 ? mem_pt(mem, first - 1) : NULL;
        struct smaps_counts counts = {0};
        found = pt != NULL && pt->data->fd != NULL && (before == NULL || !proc_maps_same_region(before, pt)) &&
                proc_maps_region_end(mem, first, &counts) == end >> PAGE_BITS;
        read_wrunlock(&mem->lock);
    }
    unlock(&task->general_lock);
    proc_put_task(task);
    if (!found)
        return false;
    *next_entry = (struct proc_entry) {&proc_pid_map_file, .pid = entry->pid, .name = strdup(name)};
    *index = 0;
    return true;
}

static void proc_pid_map_file_getname(struct proc_entry *entry, char *buf) {
    strcpy(buf, entry->name);
}

// The file mapped at the region's first page; ENOENT once it maps none
static int proc_pid_map_file_readlink(struct proc_entry *entry, char *buf) {
    page_t first = strtoul(entry->name, NULL, 16) >> PAGE_BITS;
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    int err = _ENOENT;
    lock(&task->general_lock);   // [T-ish-exit-mm-general-lock]
    struct mem *mem = task->mem;
    if (mem != NULL) {
        read_wrlock(&mem->lock);
        struct pt_entry *pt = mem_pt(mem, first);
        if (pt != NULL && pt->data->fd != NULL)
            err = generic_getpath(pt->data->fd, buf);
        read_wrunlock(&mem->lock);
    }
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_cwd_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (task->fs == NULL) {
        proc_put_task(task);
        return _ENOENT;
    }
    lock(&task->fs->lock);
    int err = generic_getpath(task->fs->pwd, buf);
    unlock(&task->fs->lock);
    proc_put_task(task);
    return err;
}


struct proc_children proc_pid_children = PROC_CHILDREN({
    {"auxv", .show = proc_pid_auxv_show},
    {"cmdline", .show = proc_pid_cmdline_show},
    {"comm", S_IFREG | 0644, .show = proc_pid_comm_show, .pwrite = proc_pid_comm_pwrite},
    {"cwd", S_IFLNK, .readlink = proc_pid_cwd_readlink},
    {"environ", S_IFREG | 0400, .show = proc_pid_environ_show},
    {"exe", S_IFLNK, .readlink = proc_pid_exe_readlink},
    {"fd", S_IFDIR, .readdir = proc_pid_fd_readdir},
    {"map_files", S_IFDIR, .readdir = proc_pid_map_files_readdir, .lookup = proc_pid_map_files_lookup},
    {"maps", .show = proc_pid_maps_show},
    {"smaps", .show = proc_pid_smaps_show},
    {"mem", .pread = proc_pid_mem_pread, .pwrite = proc_pid_mem_pwrite, .show = proc_pid_mem_show},
    {"stat", .show = proc_pid_stat_show},
    {"statm", .show = proc_pid_statm_show},
    {"status", .show = proc_pid_status_show},
    {"syscall", S_IFREG | 0400, .show = proc_pid_syscall_show},
    {"task", S_IFDIR, .readdir = proc_pid_task_readdir},
});

struct proc_dir_entry proc_pid = {NULL, S_IFDIR,
    .children = &proc_pid_children, .getname = proc_pid_getname};

static struct proc_dir_entry proc_pid_fd = {NULL, S_IFLNK,
    .getname = proc_pid_fd_getname, .readlink = proc_pid_fd_readlink};
static struct proc_dir_entry proc_tid_fd = {NULL, S_IFLNK,
    .getname = proc_pid_fd_getname, .readlink = proc_pid_fd_readlink};

// /proc/<pid>/task/<tid>: one thread's, as Linux's tid_base_stuff. Its stat
// has the thread's own CPU time, where /proc/<pid>/stat (and /proc/<tid>/stat)
// has the process's: task/<tid> was a symlink to /proc/<tid>, so every thread
// said the process's, and top -H and ps -L could not tell which thread took
// it. The rest are /proc/<pid>'s, which go by the task they name: its name,
// state and signals in status and comm, its process's memory and files. Its
// fd's links are proc_tid_fd, /proc/<pid>/fd's of the same files: an entry's
// meta has one parent. There was no fd (ENOENT).
static struct proc_children proc_tid_children = PROC_CHILDREN({
    {"auxv", .show = proc_pid_auxv_show},
    {"cmdline", .show = proc_pid_cmdline_show},
    {"comm", S_IFREG | 0644, .show = proc_pid_comm_show, .pwrite = proc_pid_comm_pwrite},
    {"cwd", S_IFLNK, .readlink = proc_pid_cwd_readlink},
    {"environ", S_IFREG | 0400, .show = proc_pid_environ_show},
    {"exe", S_IFLNK, .readlink = proc_pid_exe_readlink},
    {"fd", S_IFDIR, .readdir = proc_tid_fd_readdir},
    {"maps", .show = proc_pid_maps_show},
    {"smaps", .show = proc_pid_smaps_show},
    {"mem", .pread = proc_pid_mem_pread, .pwrite = proc_pid_mem_pwrite, .show = proc_pid_mem_show},
    {"stat", .show = proc_tid_stat_show},
    {"statm", .show = proc_pid_statm_show},
    {"status", .show = proc_pid_status_show},
    {"syscall", S_IFREG | 0400, .show = proc_pid_syscall_show},
});

static struct proc_dir_entry proc_pid_task = {NULL, S_IFDIR,
    .getname = proc_pid_task_getname, .children = &proc_tid_children};

static struct proc_dir_entry proc_pid_map_file = {NULL, S_IFLNK,
    .getname = proc_pid_map_file_getname, .readlink = proc_pid_map_file_readlink};
