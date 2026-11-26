#include "fs/proc.h"
#include "fs/proc/ish.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "fs/poll.h"
#include "platform/platform.h"
#include "fs/real.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "asbestos/asbestos.h"
#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
#endif
#include "ish-commit.h"
#include "ish-commit-date.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *proc_ish_version = "";
char **(*get_all_defaults_keys)(void);
char *(*get_friendly_name)(const char *name);
char *(*get_underlying_name)(const char *name);
bool (*get_user_default)(const char *name, char **buffer, size_t *size);
bool (*set_user_default)(const char *name, char *buffer, size_t size);
bool (*remove_user_default)(const char *name);
char *(*get_documents_directory)(void);
int (*open_url)(const char *url, size_t size);
char *(*get_builtin_jit_report)(void);

static int proc_ish_show_colors(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf,
                "\x1B[30m" "iSH" "\x1B[39m "
                "\x1B[31m" "iSH" "\x1B[39m "
                "\x1B[32m" "iSH" "\x1B[39m "
                "\x1B[33m" "iSH" "\x1B[39m "
                "\x1B[34m" "iSH" "\x1B[39m "
                "\x1B[35m" "iSH" "\x1B[39m "
                "\x1B[36m" "iSH" "\x1B[39m "
                "\x1B[37m" "iSH" "\x1B[39m" "\n\x1B[7m"
                "\x1B[40m" "iSH" "\x1B[39m "
                "\x1B[41m" "iSH" "\x1B[39m "
                "\x1B[42m" "iSH" "\x1B[39m "
                "\x1B[43m" "iSH" "\x1B[39m "
                "\x1B[44m" "iSH" "\x1B[39m "
                "\x1B[45m" "iSH" "\x1B[39m "
                "\x1B[46m" "iSH" "\x1B[39m "
                "\x1B[47m" "iSH" "\x1B[39m" "\x1B[0m\x1B[1m\n"
                "\x1B[90m" "iSH" "\x1B[39m "
                "\x1B[91m" "iSH" "\x1B[39m "
                "\x1B[92m" "iSH" "\x1B[39m "
                "\x1B[93m" "iSH" "\x1B[39m "
                "\x1B[94m" "iSH" "\x1B[39m "
                "\x1B[95m" "iSH" "\x1B[39m "
                "\x1B[96m" "iSH" "\x1B[39m "
                "\x1B[97m" "iSH" "\x1B[39m" "\n\x1B[7m"
                "\x1B[100m" "iSH" "\x1B[39m "
                "\x1B[101m" "iSH" "\x1B[39m "
                "\x1B[102m" "iSH" "\x1B[39m "
                "\x1B[103m" "iSH" "\x1B[39m "
                "\x1B[104m" "iSH" "\x1B[39m "
                "\x1B[105m" "iSH" "\x1B[39m "
                "\x1B[106m" "iSH" "\x1B[39m "
                "\x1B[107m" "iSH" "\x1B[39m" "\x1B[0m\n"
                );
    return 0;
}

static int proc_ish_show_documents(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    if (get_documents_directory == NULL) {
        proc_printf(buf, "\n");
        return 0;
    }
    char *directory = get_documents_directory();
    proc_printf(buf, "%s\n", directory);
    free(directory);
    return 0;
}

static void proc_ish_defaults_getname(struct proc_entry *entry, char *buf) {
    strcpy(buf, entry->name);
}

static int proc_ish_defaults_readlink(struct proc_entry *entry, char *buf) {
    char *name = get_underlying_name(entry->name);
    sprintf(buf, "../.defaults/%s", name);
    free(name);
    return 0;
}

static int proc_ish_underlying_defaults_show(struct proc_entry *entry, struct proc_data *data) {
    size_t size;
    char *buffer;
    if (get_user_default == NULL || !get_user_default(entry->name, &buffer, &size))
        return _EIO;
    proc_buf_append(data, buffer, size);
    free(buffer);
    return 0;
}

static int proc_ish_underlying_defaults_update(struct proc_entry *entry, struct proc_data *data) {
    if (set_user_default == NULL || !set_user_default(entry->name, data->data, data->size))
        return _EIO;
    return 0;
}

static int proc_ish_underlying_defaults_unlink(struct proc_entry *entry) {
    return (remove_user_default != NULL && remove_user_default(entry->name)) ? 0 : _EIO;
}

static int proc_ish_defaults_unlink(struct proc_entry *entry) {
    char *name = get_underlying_name(entry->name);
    int err = remove_user_default(name) ? 0 : _EIO;
    free(name);
    return err;
}

struct proc_dir_entry proc_ish_underlying_defaults_fd = { NULL,
    .getname = proc_ish_defaults_getname,
    .show = proc_ish_underlying_defaults_show,
    .update = proc_ish_underlying_defaults_update,
    .unlink = proc_ish_underlying_defaults_unlink,
};
struct proc_dir_entry proc_ish_defaults_fd = { NULL, S_IFLNK,
    .getname = proc_ish_defaults_getname,
    .readlink = proc_ish_defaults_readlink,
    .unlink = proc_ish_defaults_unlink,
};

static void get_child_names(struct proc_entry *entry, unsigned long index) {
    if (index == 0 || entry->child_names == NULL) {
        if (entry->child_names != NULL)
            free_string_array(entry->child_names);
        if (get_all_defaults_keys == NULL) {
            entry->child_names = NULL;
        } else {
            entry->child_names = get_all_defaults_keys();
        }
    }
}

static bool proc_ish_underlying_defaults_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    get_child_names(entry, *index);
    if (entry->child_names == NULL || entry->child_names[*index] == NULL)
        return false;
    next_entry->meta = &proc_ish_underlying_defaults_fd;
    next_entry->name = strdup(entry->child_names[*index]);
    (*index)++;
    return true;
}

static bool proc_ish_defaults_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    get_child_names(entry, *index);
    char *friendly_name;
    do {
        const char *name = NULL;
        if (entry->child_names != NULL) {
            name = entry->child_names[*index];
        }
        if (name == NULL)
            return false;
        friendly_name = get_friendly_name(name);
        (*index)++;
    } while (friendly_name == NULL);
    next_entry->meta = &proc_ish_defaults_fd;
    next_entry->name = friendly_name;
    return true;
}

// The build: the app's version (the first line, as it was alone before), the
// commit and its date, and the JIT's abi, which an AOT image must match
static int proc_ish_show_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s\n", ish_build_name());
    proc_printf(buf, "commit: %s (%s)\n", ISH_COMMIT, ISH_COMMIT_DATE);
#ifdef ISH_JIT
    proc_printf(buf, "abi: %08x (JIT_CODE_VERSION %u)\n", jit_abi(), jit_code_version());
#else
    proc_printf(buf, "abi: none (no JIT)\n");
#endif
    return 0;
}

// The host's memory, as host_out_of_memory reports it when an allocation
// fails (platform/platform.h): to watch it while the app runs, how near its
// limit and how many host mappings
static int proc_ish_show_host(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct host_memory m = get_host_memory();
    proc_printf(buf, "available: %.1f MB (what the OS still lets the app take: 0 is its limit met, or a host "
                "with none)\n", m.available / 1048576.0);
    proc_printf(buf, "footprint: %.1f MB\n", m.footprint / 1048576.0);
    // The app's limit, past which the OS ends it (iOS's jetsam): what it
    // takes and what it may still take. Not the device's memory, which it
    // shares
    if (m.available != 0)
        proc_printf(buf, "limit: %.1f MB (footprint and available: past it the OS ends the app)\n",
                    (m.footprint + m.available) / 1048576.0);
    proc_printf(buf, "device memory: %.1f MB\n", m.physical / 1048576.0);
    // Each entitlement app/iSH.entitlements asks for, as the signature that
    // installed the app has it (an entitlements file asking is not enough: a
    // profile without one signs without it): the two that raise the memory
    // limit and the address space; get-task-allow, which only a development
    // profile signs with, and the packet tunnel, which the built-in JIT's
    // debugger attaches by and is reached through (app/BuiltinJIT.m); the app
    // group the File Provider shares; the fonts. And Developer Mode, which
    // the debugger needs too. The JIT's settings said of these only on their
    // own screen, as it was to start.
    static const char *const entitlements[] = {
        "com.apple.developer.kernel.increased-memory-limit",
        "com.apple.developer.kernel.extended-virtual-addressing",
        "get-task-allow",
        "com.apple.developer.networking.networkextension",
        "com.apple.security.application-groups",
        "com.apple.developer.user-fonts",
    };
    for (size_t i = 0; i < sizeof(entitlements) / sizeof(entitlements[0]); i++) {
        char why[200];
        enum entitlement_state state = host_entitlement(entitlements[i], why, sizeof(why));
        proc_printf(buf, "entitlement %s: %s%s%s\n", entitlements[i],
                    state == ENTITLEMENT_YES ? "yes" : state == ENTITLEMENT_NO ? "no" : "unknown",
                    why[0] != '\0' ? ", " : "", why);
    }
    char why[200];
    enum entitlement_state developer = host_developer_mode(why, sizeof(why));
    proc_printf(buf, "developer mode: %s%s%s\n",
                developer == ENTITLEMENT_YES ? "on" : developer == ENTITLEMENT_NO ? "off" : "unknown",
                why[0] != '\0' ? ", " : "", why);
    proc_printf(buf, "mappings: %llu\n", (unsigned long long) m.mappings);
    proc_printf(buf, "malloc in use: %.1f MB\n", m.malloc_in_use / 1048576.0);
    proc_printf(buf, "address space: %.1f MB in the mappings, %.1f MB the largest range none takes\n",
                m.address_space / 1048576.0, m.largest_free / 1048576.0);
    // Where it went: what is free in all and in how many pieces, the largest
    // of them, and the largest mappings with what made them
    struct host_ranges r;
    get_host_ranges(&r);
    proc_printf(buf, "free: %.1f MB in %llu ranges, of %#llx-%#llx\n", r.free_total / 1048576.0,
                (unsigned long long) r.free_count, (unsigned long long) r.lowest, (unsigned long long) r.highest);
    proc_printf(buf, "largest free ranges:\n");
    for (int i = 0; i < HOST_RANGES_SHOWN && r.free[i].size != 0; i++)
        proc_printf(buf, "  %#014llx-%#014llx %12.1f MB\n", (unsigned long long) r.free[i].start,
                    (unsigned long long) (r.free[i].start + r.free[i].size), r.free[i].size / 1048576.0);
    proc_printf(buf, "largest mappings:\n");
    for (int i = 0; i < HOST_RANGES_SHOWN && r.mapped[i].size != 0; i++)
        proc_printf(buf, "  %#014llx-%#014llx %12.1f MB %s tag %u share mode %u%s\n",
                    (unsigned long long) r.mapped[i].start,
                    (unsigned long long) (r.mapped[i].start + r.mapped[i].size), r.mapped[i].size / 1048576.0,
                    r.mapped[i].prot, r.mapped[i].tag, r.mapped[i].share_mode, r.mapped[i].submap ? " submap" : "");
    // What sharing read-only file mappings saves (fs/real.c)
    uint64_t shared, shared_bytes, reused, reused_bytes;
    realfs_shared_maps_stats(&shared, &shared_bytes, &reused, &reused_bytes);
    proc_printf(buf, "shared file mappings: %llu, %.1f MB; mapped again from them %llu times, %.1f MB\n",
                (unsigned long long) shared, shared_bytes / 1048576.0, (unsigned long long) reused,
                reused_bytes / 1048576.0);
    // Where the host mappings and the app's malloc go. Since the app started:
    // the anonymous host mmaps that became guest memory, each a mapping,
    // PROT_NONE ones too, and of them the commits of a lazy reservation's page
    // on its first touch, each a host page (whole) or a guest page, and the
    // copies on write, likewise; MADV_DONTNEED's pages zeroed
    // in place, which stay the app's. Now: the translated blocks of every
    // address space.
    uint64_t anon_mmaps, anon_pages, cow_copies, cow_whole, cow_pages, calls, whole, single, pages_committed;
    ish_anon_mmap_stats(&anon_mmaps, &anon_pages, &cow_copies, &cow_whole, &cow_pages);
    ish_cluster_stats(&calls, &whole, &single, NULL, NULL, NULL, NULL, &pages_committed);
    extern _Atomic uint64_t st_dontneed_zeroed;
    // A copy on write is a host mapping of its own, a host page: 16KB on iOS,
    // of memory as of address space (the host cannot give less; see
    // pt_map_cluster), which holds the guest pages of that host page
    // (cow_cluster) or the one guest page written: what they copied, and what
    // they take
    proc_printf(buf, "anonymous host mmaps since the app started: %llu, for %.1f MB; of them lazy commits %llu "
                "(%llu host pages, %llu guest pages), %.1f MB; copies on write %llu (%llu host pages, %llu guest "
                "pages), %.1f MB, a host page each: %.1f MB\n",
                (unsigned long long) anon_mmaps, anon_pages * PAGE_SIZE / 1048576.0, (unsigned long long) calls,
                (unsigned long long) whole, (unsigned long long) single, pages_committed * PAGE_SIZE / 1048576.0,
                (unsigned long long) cow_copies, (unsigned long long) cow_whole,
                (unsigned long long) (cow_copies - cow_whole), cow_pages * PAGE_SIZE / 1048576.0,
                cow_copies * real_page_size / 1048576.0);
    proc_printf(buf, "MADV_DONTNEED zeroed in place since the app started: %llu pages, %.1f MB\n",
                (unsigned long long) atomic_load(&st_dontneed_zeroed),
                atomic_load(&st_dontneed_zeroed) * PAGE_SIZE / 1048576.0);
    proc_printf(buf, "translated blocks now: %llu, %.1f MB\n", (unsigned long long) atomic_load(&st_fiber_blocks),
                atomic_load(&st_fiber_bytes) / 1048576.0);
    return 0;
}

#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
// Whether the JIT / AOT images are in use (asbestos/guest-arm64/jit.h).
static int proc_ish_show_jit(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    size_t size = 16384;
    char *text = malloc(size);
    if (text == NULL)
        return _ENOMEM;
    jit_describe(text, size);
    proc_printf(buf, "%s", text);
    free(text);
    if (get_builtin_jit_report != NULL) {
        char *report = get_builtin_jit_report();
        proc_printf(buf, "built-in JIT:\n%s", report);
        free(report);
    }
    return 0;
}
// pwrite, not update: an unknown command fails the write (EINVAL), and
// "region" with no code region to be had (EIO; /proc/ish/jit says why).
static ssize_t proc_ish_write_jit(struct proc_entry *UNUSED(entry), struct proc_data *data, off_t UNUSED(off)) {
    switch (jit_control(data->data, data->size)) {
    case 0:
        return (ssize_t) data->size;
    case -2:
        return _EIO;
    default:
        return _EINVAL;
    }
}
#endif

// The translated blocks of every address space now, and what memory pressure
// has freed of idle ones (kernel/mmap.c); "reclaim" written frees those idle
// now, as the brake does
static int proc_ish_show_translations(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "blocks now: %llu, %.1f MB\n", (unsigned long long) atomic_load(&st_fiber_blocks),
                atomic_load(&st_fiber_bytes) / 1048576.0);
    uint64_t runs, spaces, blocks, bytes;
    ish_reclaim_describe(&runs, &spaces, &blocks, &bytes);
    proc_printf(buf, "reclaimed: %llu blocks, %.1f MB, of %llu idle address spaces, in %llu reclaims\n",
                (unsigned long long) blocks, bytes / 1048576.0, (unsigned long long) spaces,
                (unsigned long long) runs);
    uint64_t shared_forks, mapping, private_blocks, written;
    mem_asbestos_describe(&shared_forks, &mapping, &private_blocks, &written);
    proc_printf(buf, "shared with forks: %llu; left: %llu for a mapping, %llu for a block of their own, %llu for code "
                "written\n", (unsigned long long) shared_forks, (unsigned long long) mapping,
                (unsigned long long) private_blocks, (unsigned long long) written);
    return 0;
}

static ssize_t proc_ish_write_translations(struct proc_entry *UNUSED(entry), struct proc_data *data,
                                           off_t UNUSED(off)) {
    size_t n = data->size;
    if (n > 0 && data->data[n - 1] == '\n')
        n--;
    if (n != strlen("reclaim") || memcmp(data->data, "reclaim", n) != 0)
        return _EINVAL;
    ish_reclaim_translations();
    return data->size;
}

// [T-ish-poll-spin-guard] Poll wakeup counters and guard switches (fs/poll.c).
static int proc_ish_show_poll_spin(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char text[512];
    poll_spin_describe(text, sizeof(text));
    proc_printf(buf, "%s", text);
    return 0;
}
static ssize_t proc_ish_write_poll_spin(struct proc_entry *UNUSED(entry), struct proc_data *data, off_t UNUSED(off)) {
    return poll_spin_control(data->data, data->size) == 0 ? (ssize_t) data->size : _EINVAL;
}

// What clone and futex waits cost (kernel/task.h's thread_stats_describe)
static int proc_ish_show_threads(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char text[768];
    thread_stats_describe(text, sizeof(text));
    proc_printf(buf, "%s", text);
    return 0;
}

// A URL to open (xdg-open writes it), one write for the whole of it. The
// write returns once the app has asked and been answered.
static ssize_t proc_ish_write_open_url(struct proc_entry *UNUSED(entry), struct proc_data *data, off_t UNUSED(off)) {
    if (open_url == NULL)
        return _ENODEV;
    int err = open_url(data->data, data->size);
    return err < 0 ? err : (ssize_t) data->size;
}

struct proc_children proc_ish_children = PROC_CHILDREN({
    {"colors", .show = proc_ish_show_colors},
    {".defaults", S_IFDIR, .readdir = proc_ish_underlying_defaults_readdir},
    {"defaults", S_IFDIR, .readdir = proc_ish_defaults_readdir},
    {"documents", .show = proc_ish_show_documents},
    {"host", .show = proc_ish_show_host},
#ifdef ISH_JIT
    {"jit", .show = proc_ish_show_jit, .pwrite = proc_ish_write_jit},
#endif
    {"open_url", S_IFREG | 0222, .pwrite = proc_ish_write_open_url},
    {"poll_spin", .show = proc_ish_show_poll_spin, .pwrite = proc_ish_write_poll_spin},
    {"threads", .show = proc_ish_show_threads},
    {"translations", S_IFREG | 0644, .show = proc_ish_show_translations, .pwrite = proc_ish_write_translations},
    {"version", .show = proc_ish_show_version},
});
