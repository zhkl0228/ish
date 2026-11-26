#include <sys/sysinfo.h>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "platform/platform.h"
#include "debug.h"

static void read_proc_line(const char *file, const char *name, char *buf) {
    FILE *f = fopen(file, "r");
    if (f == NULL) ERRNO_DIE(file);
    do {
        fgets(buf, 1234, f);
        if (feof(f))
            die("could not find proc line %s", name);
    } while (!(strncmp(name, buf, strlen(name)) == 0 && buf[strlen(name)] == ' '));
    fclose(f);
}

struct cpu_usage get_cpu_usage() {
    struct cpu_usage usage = {};
    char buf[1234];
    read_proc_line("/proc/stat", "cpu", buf);
    // Linux's order: user nice system idle (this read them as user system
    // idle nice, and the guest's /proc/stat had the host's idle time as nice)
    sscanf(buf, "cpu %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64"\n", &usage.user_ticks, &usage.nice_ticks, &usage.system_ticks, &usage.idle_ticks);
    return usage;
}

uint64_t get_context_switches(void) {
    char buf[1234];
    read_proc_line("/proc/stat", "ctxt", buf);
    uint64_t ctxt;
    sscanf(buf, "ctxt %"SCNu64"\n", &ctxt);
    return ctxt;
}

unsigned get_per_cpu_usage(struct cpu_usage *usage, unsigned max) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL) ERRNO_DIE("/proc/stat");
    char buf[1234];
    unsigned n = 0;
    while (n < max && fgets(buf, sizeof(buf), f) != NULL) {
        // "cpu0 ...", not the "cpu  ..." summary
        if (strncmp(buf, "cpu", 3) != 0 || buf[3] < '0' || buf[3] > '9')
            continue;
        struct cpu_usage *u = &usage[n++];
        sscanf(buf, "cpu%*u %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64"\n",
               &u->user_ticks, &u->nice_ticks, &u->system_ticks, &u->idle_ticks);
    }
    fclose(f);
    return n;
}

// In bytes, as on Darwin (the host's /proc/meminfo has kB: the guest's showed
// 7.9 MB of memory for 7.9 GB)
struct mem_usage get_mem_usage() {
    struct mem_usage usage = {};
    char buf[1234];

    read_proc_line("/proc/meminfo", "MemTotal:", buf);
    sscanf(buf, "MemTotal: %"PRIu64" kB\n", &usage.total);
    read_proc_line("/proc/meminfo", "MemFree:", buf);
    sscanf(buf, "MemFree: %"PRIu64" kB\n", &usage.free);
    read_proc_line("/proc/meminfo", "Active:", buf);
    sscanf(buf, "Active: %"PRIu64" kB\n", &usage.active);
    read_proc_line("/proc/meminfo", "Inactive:", buf);
    sscanf(buf, "Inactive: %"PRIu64" kB\n", &usage.inactive);
    usage.total *= 1024;
    usage.free *= 1024;
    usage.active *= 1024;
    usage.inactive *= 1024;

    return usage;
}

struct host_memory get_host_memory(void) {
    struct host_memory m = {};
    m.physical = (uint64_t) sysconf(_SC_PHYS_PAGES) * (uint64_t) sysconf(_SC_PAGESIZE);
    char buf[4096];
    // the resident set, statm's second field, in pages
    int fd = open("/proc/self/statm", O_RDONLY);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        unsigned long long size, resident;
        sscanf(buf, "%llu %llu", &size, &resident);
        m.footprint = resident * (uint64_t) sysconf(_SC_PAGESIZE);
    }
    // a line of /proc/self/maps per mapping, "start-end ...", in address order:
    // what lies between two no mapping takes. x86-64's [vsyscall] is in the
    // kernel's half, not the process's address space, and left out.
    uint64_t start = 0, end = 0, last_end = 0;
    int field = 0; // 0 start, 1 end, 2 the rest of the line
    fd = open("/proc/self/maps", O_RDONLY);
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (start < 0xffff000000000000) {
                    if (last_end != 0 && start - last_end > m.largest_free)
                        m.largest_free = start - last_end;
                    m.mappings++;
                    m.address_space += end - start;
                    last_end = end;
                }
                start = end = 0;
                field = 0;
            } else if (field == 0 && c == '-') {
                field = 1;
            } else if (field == 1 && c == ' ') {
                field = 2;
            } else if (field == 0) {
                start = start << 4 | (uint64_t) (c <= '9' ? c - '0' : c - 'a' + 10);
            } else if (field == 1) {
                end = end << 4 | (uint64_t) (c <= '9' ? c - '0' : c - 'a' + 10);
            }
        }
    }
    close(fd);
#if defined(__GLIBC__)
    m.malloc_in_use = mallinfo2().uordblks;
#else
    // musl (an Alpine host) has no mallinfo: /proc/ish/host says 0
#endif
    return m;
}

// /proc/self/maps again, read as get_host_memory reads it (an allocation would
// be a mapping more, read while it is there), with each mapping's protection,
// "start-end rwxp ..."; Linux has no tags. The address space runs from the
// lowest mapping to the highest here.
void get_host_ranges(struct host_ranges *ranges) {
    *ranges = (struct host_ranges) {};
    char buf[4096];
    struct host_range mapped = {};
    uint64_t stop = 0, end = 0;
    int field = 0, perm = 0; // 0 start, 1 end, 2 the protection, 3 the rest of the line
    int fd = open("/proc/self/maps", O_RDONLY);
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (mapped.start < 0xffff000000000000) {
                    if (end == 0)
                        ranges->lowest = mapped.start;
                    else if (mapped.start > end)
                        host_ranges_free(ranges, end, mapped.start - end);
                    mapped.size = stop - mapped.start;
                    host_ranges_keep(ranges->mapped, mapped);
                    end = stop;
                }
                mapped = (struct host_range) {};
                stop = 0;
                field = perm = 0;
            } else if (field == 0 && c == '-') {
                field = 1;
            } else if (field == 1 && c == ' ') {
                field = 2;
            } else if (field == 0) {
                mapped.start = mapped.start << 4 | (uint64_t) (c <= '9' ? c - '0' : c - 'a' + 10);
            } else if (field == 1) {
                stop = stop << 4 | (uint64_t) (c <= '9' ? c - '0' : c - 'a' + 10);
            } else if (field == 2) {
                mapped.prot[perm++] = c;
                if (perm == 3)
                    field = 3;
            }
        }
    }
    close(fd);
    ranges->highest = end;
}

struct uptime_info get_uptime() {
    struct sysinfo info;
    sysinfo(&info);
    // sysinfo's uptime is whole seconds; /proc/uptime shows hundredths, from
    // the clock Linux reads it from.
    struct timespec boot;
    clock_gettime(CLOCK_BOOTTIME, &boot);
    struct uptime_info uptime = {
        .uptime_ticks = (uint64_t) boot.tv_sec * 100 + boot.tv_nsec / 10000000, // hundredths of a second
        .load_1m = info.loads[0],
        .load_5m = info.loads[1],
        .load_15m = info.loads[2],
    };
    return uptime;
}

enum entitlement_state host_entitlement(const char *UNUSED(name), char *why, size_t why_size) {
    snprintf(why, why_size, "a Linux host has no code signature");
    return ENTITLEMENT_UNKNOWN;
}

enum entitlement_state host_developer_mode(char *why, size_t why_size) {
    snprintf(why, why_size, "a Linux host has no Developer Mode");
    return ENTITLEMENT_UNKNOWN;
}
