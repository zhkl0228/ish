#include <TargetConditionals.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <sys/sysctl.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include <CoreFoundation/CoreFoundation.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "platform/platform.h"
#include "debug.h"

// mach_host_self() adds a uref to the host port name on every call; calling it
// per-read leaks refs and can saturate the name's uref count under guests that
// poll /proc/meminfo or /proc/stat at high frequency (e.g. node/V8), after
// which host_info()/host_statistics64() start failing. Resolve it once.
static host_t cached_host_self(void) {
    static host_t host = HOST_NULL;
    if (host == HOST_NULL)
        host = mach_host_self();
    return host;
}

// The CPUs' ticks summed, as Linux's cpu line is its cpuN lines'. Not
// HOST_CPU_LOAD_INFO's: the host's counts are 32 bits for all its CPUs, and a
// 20-CPU Mac's idle count wrapped every 25 days (an iPhone's, of 6, every 82),
// where the cpu line fell far below its CPUs' (2^33 on a Mac up long enough).
// One CPU's count wraps after 497 days. All 0 when the host does not tell.
struct cpu_usage get_cpu_usage(void) {
    struct cpu_usage cpus[256], usage = {};
    unsigned n = get_per_cpu_usage(cpus, sizeof(cpus) / sizeof(cpus[0]));
    for (unsigned i = 0; i < n; i++) {
        usage.user_ticks += cpus[i].user_ticks;
        usage.system_ticks += cpus[i].system_ticks;
        usage.idle_ticks += cpus[i].idle_ticks;
        usage.nice_ticks += cpus[i].nice_ticks;
    }
    return usage;
}

uint64_t get_context_switches(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (uint64_t) usage.ru_nvcsw + (uint64_t) usage.ru_nivcsw;
}

unsigned get_per_cpu_usage(struct cpu_usage *usage, unsigned max) {
    natural_t cpus = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t info_count = 0;
    kern_return_t status = host_processor_info(cached_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                               &cpus, &info, &info_count);
    if (status != KERN_SUCCESS) {
        // /proc/stat goes without its per-CPU lines, which Linux tools
        // tolerate (CPUs can go offline)
        static bool logged;
        if (!logged) {
            logged = true;
            printk("host_processor_info(PROCESSOR_CPU_LOAD_INFO) failed: %d; /proc/stat has no per-CPU lines\n", status);
        }
        return 0;
    }
    processor_cpu_load_info_t load = (processor_cpu_load_info_t) info;
    unsigned n = cpus < max ? cpus : max;
    for (unsigned i = 0; i < n; i++) {
        usage[i].user_ticks = load[i].cpu_ticks[CPU_STATE_USER];
        usage[i].system_ticks = load[i].cpu_ticks[CPU_STATE_SYSTEM];
        usage[i].idle_ticks = load[i].cpu_ticks[CPU_STATE_IDLE];
        usage[i].nice_ticks = load[i].cpu_ticks[CPU_STATE_NICE];
    }
    vm_deallocate(mach_task_self(), (vm_address_t) info, info_count * sizeof(*info));
    return n;
}

struct host_memory get_host_memory(void) {
    struct host_memory m = {};
#if TARGET_OS_IPHONE
    m.available = os_proc_available_memory();
#endif
    size_t physical_size = sizeof(m.physical);
    sysctlbyname("hw.memsize", &m.physical, &physical_size, NULL, 0);
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &vm, &count) == KERN_SUCCESS)
        m.footprint = vm.phys_footprint;
    // vm_region_64 from each region's end to the next, as vmmap counts them;
    // what lies between two, and between the last and the end of the task's
    // address space, no mapping takes
    vm_address_t end = (vm_address_t) vm.min_address;
    for (vm_address_t address = 0;;) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t) &info,
                         &info_count, &object) != KERN_SUCCESS)
            break;
        if (address > end && address - end > m.largest_free)
            m.largest_free = address - end;
        m.mappings++;
        m.address_space += size;
        address += size;
        end = address;
    }
    if (vm.max_address > end && vm.max_address - end > m.largest_free)
        m.largest_free = vm.max_address - end;
    malloc_statistics_t stats = {};
    malloc_zone_statistics(NULL, &stats);
    m.malloc_in_use = stats.size_in_use;
    return m;
}

// The same walk, with what each region is: vm_region_recurse_64 at depth 0
// has the user tag, and its short info counts no pages (the extended info
// would visit every page of a region hundreds of GB large).
void get_host_ranges(struct host_ranges *ranges) {
    *ranges = (struct host_ranges) {};
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &vm, &count);
    ranges->lowest = vm.min_address;
    ranges->highest = vm.max_address;
    vm_address_t end = (vm_address_t) vm.min_address;
    for (vm_address_t address = 0;;) {
        vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_short_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t) &info,
                                 &info_count) != KERN_SUCCESS)
            break;
        if (address > end)
            host_ranges_free(ranges, end, address - end);
        struct host_range mapped = {
            .start = address,
            .size = size,
            .tag = info.user_tag,
            .share_mode = info.share_mode,
            .submap = info.is_submap,
        };
        mapped.prot[0] = info.protection & VM_PROT_READ ? 'r' : '-';
        mapped.prot[1] = info.protection & VM_PROT_WRITE ? 'w' : '-';
        mapped.prot[2] = info.protection & VM_PROT_EXECUTE ? 'x' : '-';
        host_ranges_keep(ranges->mapped, mapped);
        address += size;
        end = address;
    }
    if (vm.max_address > end)
        host_ranges_free(ranges, end, vm.max_address - end);
}

struct mem_usage get_mem_usage(void) {
#if TARGET_OS_IPHONE
    // An app has a memory limit of its own, well under the device's memory,
    // and iOS kills it past that: the guest's memory is that limit (what the
    // app takes now and what it may still take), its free memory what it may
    // still take. Programs that size themselves by the memory (Java's default
    // heap, a fourth of it) stay under the limit. os_proc_available_memory()
    // is 0 for a process with no limit (the simulator, an iPad app on a Mac):
    // the device's memory then, below.
    size_t available = os_proc_available_memory();
    if (available != 0) {
        task_vm_info_data_t vm = {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &vm, &count);
        struct mem_usage app = {
            .total = vm.phys_footprint + available,
            .free = available,
            .active = vm.phys_footprint,
            .inactive = 0,
            .app_limit = true,
        };
        return app;
    }
#endif
    // Last successful reading; served as a fallback so a transient (or
    // OS-beta-induced) host_info failure degrades /proc/meminfo instead of
    // aborting the whole app. Benign data race: stale/torn stats are fine here.
    static struct mem_usage last_good;
    static bool have_last_good = false;

    host_basic_info_data_t basic = {};
    mach_msg_type_number_t fuck = HOST_BASIC_INFO_COUNT;
    kern_return_t status = host_info(cached_host_self(), HOST_BASIC_INFO, (host_info_t) &basic, &fuck);
    vm_statistics64_data_t vm = {};
    if (status == KERN_SUCCESS) {
        fuck = HOST_VM_INFO64_COUNT;
        status = host_statistics64(cached_host_self(), HOST_VM_INFO64, (host_info_t) &vm, &fuck);
    }
    if (status != KERN_SUCCESS) {
        printk("WARNING: get_mem_usage: host_info/host_statistics64 failed (kr=%d), using fallback\n", status);
        if (have_last_good)
            return last_good;
        struct mem_usage fallback = {};
        uint64_t memsize = 0;
        size_t size = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &size, NULL, 0) != 0 || memsize == 0)
            memsize = 4ULL * 1024 * 1024 * 1024;
        fallback.total = memsize;
        fallback.free = memsize / 4;
        fallback.active = memsize / 4;
        fallback.inactive = memsize / 4;
        return fallback;
    }

    struct mem_usage usage = {};
    usage.total = basic.max_mem;
    usage.free = vm.free_count * vm_page_size;
    usage.active = vm.active_count * vm_page_size;
    usage.inactive = vm.inactive_count * vm_page_size;
    last_good = usage;
    have_last_good = true;
    return usage;
}

struct uptime_info get_uptime(void) {
    struct timeval boottime;
    size_t size = sizeof(boottime);
    sysctlbyname("kern.boottime", &boottime, &size, NULL, 0);
    struct timeval now;
    gettimeofday(&now, NULL);

    struct {
        uint32_t ldavg[3];
        long scale;
    } vm_loadavg;
    size = sizeof(vm_loadavg);
    sysctlbyname("vm.loadavg", &vm_loadavg, &size, NULL, 0);

    // linux wants the scale to be 16 bits
    for (int i = 0; i < 3; i++) {
        if (FSHIFT < 16)
            vm_loadavg.ldavg[i] <<= 16 - FSHIFT;
        else
            vm_loadavg.ldavg[i] >>= FSHIFT - 16;
    }

    struct uptime_info uptime = {
        // in hundredths of a second, as /proc/uptime expects
        .uptime_ticks = (now.tv_sec - boottime.tv_sec) * 100 + (now.tv_usec - boottime.tv_usec) / 10000,
        .load_1m = vm_loadavg.ldavg[0],
        .load_5m = vm_loadavg.ldavg[1],
        .load_15m = vm_loadavg.ldavg[2],
    };
    return uptime;
}

// Security's SecTask, as <Security/SecTask.h> declares it (macOS's SDK; not
// iOS's, though its Security has them): the process's entitlements as its
// code signature has them, XML or DER, read by the system. Linked by the
// app (app/AppARM64.xcconfig) and by meson's CLI.
typedef struct __SecTask *SecTaskRef;
extern SecTaskRef SecTaskCreateFromSelf(CFAllocatorRef allocator);
extern CFTypeRef SecTaskCopyValueForEntitlement(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error);

// A CFString's UTF-8 into text, cut where it would not fit
static void cf_string_text(CFStringRef string, char *text, size_t size) {
    CFIndex used = 0;
    CFStringGetBytes(string, CFRangeMake(0, CFStringGetLength(string)), kCFStringEncodingUTF8, '?', false,
                     (UInt8 *) text, size - 1, &used);
    text[used] = '\0';
}

// Whether an array's values are all strings
static bool strings_only(CFArrayRef array) {
    for (CFIndex i = 0; i < CFArrayGetCount(array); i++) {
        if (CFGetTypeID(CFArrayGetValueAtIndex(array, i)) != CFStringGetTypeID())
            return false;
    }
    return true;
}

enum entitlement_state host_entitlement(const char *name, char *why, size_t why_size) {
    why[0] = '\0';
    SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
    if (task == NULL) {
        snprintf(why, why_size, "SecTaskCreateFromSelf failed");
        return ENTITLEMENT_UNKNOWN;
    }
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    CFErrorRef error = NULL;
    CFTypeRef value = SecTaskCopyValueForEntitlement(task, key, &error);
    CFRelease(key);
    CFRelease(task);
    if (error != NULL) {
        CFStringRef description = CFErrorCopyDescription(error);
        cf_string_text(description, why, why_size);
        CFRelease(description);
        CFRelease(error);
        return ENTITLEMENT_UNKNOWN;
    }
    if (value == NULL)
        return ENTITLEMENT_NO; // the signature has no such key
    // A boolean, or an array of strings (the network extensions, the app
    // groups, the fonts): yes, its strings in why. Another value is one not
    // seen, shown as it is.
    enum entitlement_state state = ENTITLEMENT_UNKNOWN;
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        state = CFBooleanGetValue(value) ? ENTITLEMENT_YES : ENTITLEMENT_NO;
    } else if (CFGetTypeID(value) == CFArrayGetTypeID() && strings_only(value)) {
        state = ENTITLEMENT_YES;
        for (CFIndex i = 0; i < CFArrayGetCount(value); i++) {
            char item[160];
            cf_string_text(CFArrayGetValueAtIndex(value, i), item, sizeof(item));
            if (i > 0)
                strlcat(why, ", ", why_size);
            strlcat(why, item, why_size);
        }
    } else {
        int prefix = snprintf(why, why_size, "not a boolean or strings: ");
        CFStringRef description = CFCopyDescription(value);
        cf_string_text(description, why + prefix, why_size - prefix);
        CFRelease(description);
    }
    CFRelease(value);
    return state;
}

enum entitlement_state host_developer_mode(char *why, size_t why_size) {
    why[0] = '\0';
    int status;
    size_t size = sizeof(status);
    if (sysctlbyname("security.mac.amfi.developer_mode_status", &status, &size, NULL, 0) != 0) {
        snprintf(why, why_size, "security.mac.amfi.developer_mode_status: %s", strerror(errno));
        return ENTITLEMENT_UNKNOWN;
    }
    if (status == 0 || status == 1)
        return status ? ENTITLEMENT_YES : ENTITLEMENT_NO;
    snprintf(why, why_size, "security.mac.amfi.developer_mode_status is %d, not 0 or 1", status);
    return ENTITLEMENT_UNKNOWN;
}
