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
    uint64_t address_space; // the address space its mappings take
    uint64_t largest_free;  // its largest range of address space that no mapping takes
};
struct host_memory get_host_memory(void);

// A range of the host's address space: a mapping, or one no mapping takes
struct host_range {
    uint64_t start, size;
    char prot[4];        // a mapping's protection, "rwx" as in /proc/*/maps
    unsigned tag;        // what made the mapping: Darwin's user tag (VM_MEMORY_*), 0 on Linux
    unsigned share_mode; // Darwin's SM_*, 0 on Linux
    bool submap;         // a nested map (Darwin: the shared region's)
};
#define HOST_RANGES_SHOWN 8
// Where the host's address space went, for /proc/ish/host: whether what is
// free ran out or broke up
struct host_ranges {
    struct host_range mapped[HOST_RANGES_SHOWN]; // the largest mappings, largest first
    struct host_range free[HOST_RANGES_SHOWN];   // the largest free ranges, largest first
    uint64_t free_total, free_count; // all of the address space no mapping takes, in how many ranges
    uint64_t lowest, highest;        // the address space the task may have (Linux: its lowest and highest mapping)
};
void get_host_ranges(struct host_ranges *ranges);

// Keeps range among the largest in top, largest first
static inline void host_ranges_keep(struct host_range *top, struct host_range range) {
    if (range.size <= top[HOST_RANGES_SHOWN - 1].size)
        return;
    int i = HOST_RANGES_SHOWN - 1;
    while (i > 0 && top[i - 1].size < range.size) {
        top[i] = top[i - 1];
        i--;
    }
    top[i] = range;
}

static inline void host_ranges_free(struct host_ranges *ranges, uint64_t start, uint64_t size) {
    ranges->free_total += size;
    ranges->free_count++;
    host_ranges_keep(ranges->free, (struct host_range) {.start = start, .size = size});
}

struct uptime_info {
    uint64_t uptime_ticks;
    uint64_t load_1m, load_5m, load_15m;
};
struct uptime_info get_uptime(void);

#endif
