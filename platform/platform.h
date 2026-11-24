#ifndef PLATFORM_H
#define PLATFORM_H
#include "misc.h"

// for some reason a tick is always 10ms
struct cpu_usage {
    uint64_t user_ticks;
    uint64_t system_ticks;
    uint64_t idle_ticks;
    uint64_t nice_ticks;
};
struct cpu_usage get_cpu_usage(void);
// The same for each CPU, as many as fit in usage (max): returns how many.
unsigned get_per_cpu_usage(struct cpu_usage *usage, unsigned max);

struct mem_usage {
    uint64_t total;
    uint64_t free;
    uint64_t active;
    uint64_t inactive;
};
struct mem_usage get_mem_usage(void);

// The host's memory as host_out_of_memory reports it, read without allocating:
// whether the app met the OS's limit on its memory or ran out of address space
struct host_memory {
    uint64_t available;     // what the OS still lets the app take (iOS: os_proc_available_memory), 0 for no limit
    uint64_t footprint;     // the app's memory: its physical footprint (Darwin), resident set (Linux)
    uint64_t mappings;      // its host mappings (VM regions)
    uint64_t malloc_in_use; // malloc's bytes in use
};
struct host_memory get_host_memory(void);

struct uptime_info {
    uint64_t uptime_ticks;
    uint64_t load_1m, load_5m, load_15m;
};
struct uptime_info get_uptime(void);

#endif
