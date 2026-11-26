#include <sys/stat.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "fs/proc.h"
#include "platform/platform.h"

static int proc_show_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts;
    do_uname(&uts);
    proc_printf(buf, "%s version %s %s\n", uts.system, uts.release, uts.version);
    return 0;
}

// Tasks that have not begun to exit, and of them those running: as
// /proc/<pid>/stat has it, those not in a system call (where a task waits),
// and the one reading this.
static unsigned live_tasks(unsigned *running) {
    unsigned tasks = 0;
    *running = 0;
    for (pid_t_ pid = 1; pid <= MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (task == NULL || task->exiting)
            continue;
        tasks++;
        if (task == current || !atomic_load(&task->in_syscall))
            (*running)++;
    }
    return tasks;
}

static int proc_show_stat(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct cpu_usage usage = get_cpu_usage();
    // All ten of Linux's fields: procps-ng 4 refuses a cpu line with fewer
    // than eight, and ps then fails with "Unable to get system boot time".
    // iSH counts no iowait, irq, softirq, steal or guest time.
    proc_printf(buf, "cpu  %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" 0 0 0 0 0 0\n",
                usage.user_ticks, usage.nice_ticks, usage.system_ticks, usage.idle_ticks);
    // the host's CPUs, each (top's per-CPU view, htop and psutil count them here)
    struct cpu_usage cpus[256];
    unsigned ncpus = get_per_cpu_usage(cpus, sizeof(cpus) / sizeof(cpus[0]));
    for (unsigned i = 0; i < ncpus; i++)
        proc_printf(buf, "cpu%u %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" 0 0 0 0 0 0\n",
                    i, cpus[i].user_ticks, cpus[i].nice_ticks, cpus[i].system_ticks, cpus[i].idle_ticks);
    // no interrupts in iSH
    proc_printf(buf, "intr 0\n");
    // Context switches: the host's, as the cpu lines are, where it tells.
    // iSH's own getrusage was it on every host: in an iSH that runs in iSH
    // (whose getrusage keeps times only) it was 0.
    proc_printf(buf, "ctxt %"PRIu64"\n", get_context_switches());

    // calculate btime (boot time in seconds since epoch) by subtracting uptime from current time
    struct uptime_info uptime = get_uptime();
    struct timespec uptime_ts = {.tv_sec = uptime.uptime_ticks / 100, .tv_nsec = uptime.uptime_ticks % 100 * 10000000};
    struct timespec boot_time = timespec_subtract(timespec_now(CLOCK_REALTIME), uptime_ts);
    proc_printf(buf, "btime %ld\n", boot_time.tv_sec);

    // forks since boot, threads included, as Linux's total_forks
    proc_printf(buf, "processes %"PRIu64"\n", (uint64_t) atomic_load(&ish_guest_forks));
    // none is ever in uninterruptible sleep (D)
    unsigned running;
    lock(&pids_lock);
    live_tasks(&running);
    unlock(&pids_lock);
    proc_printf(buf, "procs_running %u\n", running);
    proc_printf(buf, "procs_blocked 0\n");
    proc_printf(buf, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");

    return 0;
}

static int proc_show_cpuinfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    unsigned cpus = sysconf(_SC_NPROCESSORS_ONLN);
    { const char *e = getenv("ISH_FAKE_NCPU"); if (e) { int n = atoi(e); if (n > 0) cpus = n; } }
    for (unsigned i = 0; i < cpus; i++) {
        proc_printf(buf, "processor\t: %u\n", i);
        // ARM64 format
        proc_printf(buf, "BogoMIPS\t: 48.00\n");
        // Include crypto features that iSH ARM64 emulates
        proc_printf(buf, "Features\t: fp asimd evtstrm aes pmull atomics\n");
        proc_printf(buf, "CPU implementer\t: 0x00\n");
        proc_printf(buf, "CPU architecture: 8\n");
        proc_printf(buf, "CPU variant\t: 0x0\n");
        proc_printf(buf, "CPU part\t: 0x000\n");
        proc_printf(buf, "CPU revision\t: 0\n");
        proc_printf(buf, "\n");
    }
    return 0;
}

// value in bytes; Linux's kB is 1024 of them
static void show_kb(struct proc_data *buf, const char *name, uint64_t value) {
    proc_printf(buf, "%s%8"PRIu64" kB\n", name, value / 1024);
}

static int proc_show_meminfo(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct mem_usage usage = get_mem_usage();
    // Cap reported memory to match sys_sysinfo limit.
    // Reporting full host RAM (e.g. 24GB) causes V8 to set heap_size_limit=4GB
    // which exhausts the emulator's limited address space.
    // The app's own limit (iOS) is shown as it is: it is all the app may take
    // (6 GB with the increased-memory-limit entitlement, V8's heap a fourth of
    // it), and capped, the guest saw 4 GB of it, all of it free.
    #define MEMINFO_MAX_RAM (4ULL * 1024 * 1024 * 1024)
    if (!usage.app_limit && usage.total > MEMINFO_MAX_RAM)
        usage.total = MEMINFO_MAX_RAM;
    if (!usage.app_limit && usage.free > MEMINFO_MAX_RAM)
        usage.free = MEMINFO_MAX_RAM;
    // no more active or inactive than there is (the host's, said of a total
    // capped above)
    if (usage.active > usage.total)
        usage.active = usage.total;
    if (usage.inactive > usage.total - usage.active)
        usage.inactive = usage.total - usage.active;
    show_kb(buf, "MemTotal:       ", usage.total);
    show_kb(buf, "MemFree:        ", usage.free);
    show_kb(buf, "MemAvailable:   ", usage.free + usage.inactive);
    show_kb(buf, "MemShared:      ", usage.free);
    // a bunch of crap busybox top needs to see or else it gets stack garbage
    show_kb(buf, "Shmem:          ", 0);
    show_kb(buf, "Buffers:        ", 0);
    show_kb(buf, "Cached:         ", 0);
    // Every Linux has them, and programs read them: iSH's Linux CLI died
    // without Active ("could not find proc line Active:", get_mem_usage)
    show_kb(buf, "Active:         ", usage.active);
    show_kb(buf, "Inactive:       ", usage.inactive);
    show_kb(buf, "SwapTotal:      ", 0);
    show_kb(buf, "SwapFree:       ", 0);
    show_kb(buf, "Dirty:          ", 0);
    show_kb(buf, "Writeback:      ", 0);
    show_kb(buf, "AnonPages:      ", 0);
    show_kb(buf, "Mapped:         ", 0);
    show_kb(buf, "Slab:           ", 0);
    return 0;
}

// The host's load averages (uptime, top and w show them), as Linux prints
// them: two decimals, then running/all tasks and the last pid given out.
static int proc_show_loadavg(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uptime_info uptime = get_uptime();
    uint64_t loads[] = {uptime.load_1m, uptime.load_5m, uptime.load_15m};
    for (int i = 0; i < 3; i++) {
        // in 16-bit fixed point, as sysinfo's
        uint64_t hundredths = (loads[i] * 100 + (1 << 15)) >> 16;
        proc_printf(buf, "%"PRIu64".%02"PRIu64" ", hundredths / 100, hundredths % 100);
    }
    unsigned running;
    lock(&pids_lock);
    unsigned tasks = live_tasks(&running);
    pid_t_ last_pid = pid_last_allocated();
    unlock(&pids_lock);
    proc_printf(buf, "%u/%u %d\n", running, tasks, last_pid);
    return 0;
}

// What vmstat reads from it (si, so, bi, bo): iSH neither swaps nor pages to
// a disk of its own. Without the file vmstat failed: "Unable to create vmstat
// structure".
static int proc_show_vmstat(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "pgpgin 0\npgpgout 0\npswpin 0\npswpout 0\n");
    return 0;
}

static int proc_show_uptime(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uptime_info uptime_info = get_uptime();
    unsigned long uptime = uptime_info.uptime_ticks;
    proc_printf(buf, "%lu.%02lu %lu.%02lu\n", uptime / 100, uptime % 100, uptime / 100, uptime % 100);
    return 0;
}

// /proc/self names the process (thread group), not the calling thread.
static int proc_readlink_self(struct proc_entry *UNUSED(entry), char *buf) {
    sprintf(buf, "%d", current->tgid);
    return 0;
}

static void proc_print_escaped(struct proc_data *buf, const char *str) {
    for (size_t i = 0; str[i]; i++) {
        switch (str[i]) {
            case '\t': case ' ': case '\\':
                proc_printf(buf, "\\%03o", str[i]);
                break;
            default:
                proc_printf(buf, "%c", str[i]);
        }
    }
}

#define proc_printf_comma(buf, at_start, format, ...) do { \
    proc_printf((buf), "%s" format, *(at_start) ? "" : ",", ##__VA_ARGS__); \
    *(at_start) = false; \
} while (0)

static int proc_show_mounts(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        const char *point = mount->point;
        if (point[0] == '\0')
            point = "/";

        proc_print_escaped(buf, mount->source);
        proc_printf(buf, " ");
        proc_print_escaped(buf, point);
        proc_printf(buf, " %s ", mount->fs->name);
        bool at_start = true;
        proc_printf_comma(buf, &at_start, "%s", mount->flags & MS_READONLY_ ? "ro" : "rw");
        if (mount->flags & MS_NOSUID_)
            proc_printf_comma(buf, &at_start, "nosuid");
        if (mount->flags & MS_NODEV_)
            proc_printf_comma(buf, &at_start, "nodev");
        if (mount->flags & MS_NOEXEC_)
            proc_printf_comma(buf, &at_start, "noexec");
        if (strcmp(mount->info, "") != 0)
            proc_printf_comma(buf, &at_start, "%s", mount->info);
        proc_printf(buf, " 0 0\n");
    };
    return 0;
}

// Forward declaration for /proc/net
extern struct proc_children proc_net_children;

// in alphabetical order
struct proc_dir_entry proc_root_entries[] = {
    {"cpuinfo", .show = proc_show_cpuinfo},
    {"ish", S_IFDIR, .children = &proc_ish_children},
    {"loadavg", .show = proc_show_loadavg},
    {"meminfo", .show = proc_show_meminfo},
    {"mounts", .show = proc_show_mounts},
    {"net", S_IFDIR, .children = &proc_net_children},
    {"self", S_IFLNK, .readlink = proc_readlink_self},
    {"stat", .show = proc_show_stat},
    {"uptime", .show = proc_show_uptime},
    {"version", .show = proc_show_version},
    {"vmstat", .show = proc_show_vmstat},
};
#define PROC_ROOT_LEN sizeof(proc_root_entries)/sizeof(proc_root_entries[0])

static bool proc_root_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_ROOT_LEN) {
        *next_entry = (struct proc_entry) {&proc_root_entries[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }

    pid_t_ pid = *index - PROC_ROOT_LEN;
    if (pid <= MAX_PID) {
        // Processes only, as Linux lists them: a thread is in its process's
        // task/, and found by its tid (/proc/<tid>) without being listed.
        // Listed, top and ps -e showed every thread as a process. A leader
        // that exits before its threads keeps its pid till the last, and a
        // zombie till it is waited for (proc_get_task).
        lock(&pids_lock);
        for (pid++; pid <= MAX_PID; pid++) {
            struct task *task = pid_get_task_zombie(pid);
            if (task != NULL && task_is_leader(task))
                break;
        }
        unlock(&pids_lock);
        if (pid > MAX_PID)
            return false;
        *next_entry = (struct proc_entry) {&proc_pid, .pid = pid};
        *index = pid + PROC_ROOT_LEN;
        return true;
    }

    return false;
}

// A task by its id, a thread's too, which readdir does not list: as Linux
// finds /proc/<tid>. The id as readdir would name it, no other spelling.
static bool proc_root_lookup(struct proc_entry *UNUSED(entry), const char *name, unsigned long *index,
                             struct proc_entry *next_entry) {
    char *end;
    unsigned long id = strtoul(name, &end, 10);
    char spelled[24];
    snprintf(spelled, sizeof(spelled), "%lu", id);
    if (strcmp(spelled, name) != 0 || id > MAX_PID)
        return false;
    lock(&pids_lock);
    bool exists = pid_get_task_zombie(id) != NULL;
    unlock(&pids_lock);
    if (!exists)
        return false;
    *next_entry = (struct proc_entry) {&proc_pid, .pid = id};
    *index = id + PROC_ROOT_LEN;
    return true;
}

struct proc_dir_entry proc_root = {NULL, S_IFDIR, .readdir = proc_root_readdir, .lookup = proc_root_lookup};
