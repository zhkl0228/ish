#include <sys/sysinfo.h>
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

struct mem_usage get_mem_usage() {
    struct mem_usage usage;
    char buf[1234];

    read_proc_line("/proc/meminfo", "MemTotal:", buf);
    sscanf(buf, "MemTotal: %"PRIu64" kB\n", &usage.total);
    read_proc_line("/proc/meminfo", "MemFree:", buf);
    sscanf(buf, "MemFree: %"PRIu64" kB\n", &usage.free);
    read_proc_line("/proc/meminfo", "Active:", buf);
    sscanf(buf, "Active: %"PRIu64" kB\n", &usage.active);
    read_proc_line("/proc/meminfo", "Inactive:", buf);
    sscanf(buf, "Inactive: %"PRIu64" kB\n", &usage.inactive);

    return usage;
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
