#ifndef FS_PROC_ISH_H
#define FS_PROC_ISH_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct user_default_key {
    char *name;
    char *underlying_name;
};

extern char **(*get_all_defaults_keys)(void);
extern char *(*get_friendly_name)(const char *name);
extern char *(*get_underlying_name)(const char *name);
extern bool (*get_user_default)(const char *name, char **buffer, size_t *size);
extern bool (*set_user_default)(const char *name, char *buffer, size_t size);
extern bool (*remove_user_default)(const char *name);
extern char *(*get_documents_directory)(void);
// A program's request to open a URL, the bytes written to /proc/ish/open_url:
// the app asks whether to, and opens it if allowed. Returns once that is
// answered: 0 if it was opened, else the error the write gives. NULL in the
// command line tool, which has no one to ask.
extern int (*open_url)(const char *url, size_t size);
// What the screen shows of the app now, a PNG (/proc/ish/screenshot): 0 with
// *png malloc'd for the caller to free, or the error the read gives, _EAGAIN
// while no window of the app is in front, where iOS draws none. NULL in the
// command line tool, which has no windows.
extern int (*take_screenshot)(void **png, size_t *size);
// An action in the app's window (/proc/ish/ui, ish-ui), the bytes written:
// done without animation, it returns once the window shows it, 0, or the
// error the write gives (UIActions.m). NULL in the command line tool.
extern int (*do_ui_action)(const char *action, size_t size);
// The image on the clipboard, a PNG (/proc/ish/clipboard_image): 0 with *png
// malloc'd for the caller to free, or the error the read gives, _ENODATA with
// none there, _EACCES if pasting it was not allowed. NULL in the command line
// tool.
extern int (*get_clipboard_image)(void **png, size_t *size);
// The device's network and power now (/proc/ish/network, /proc/ish/power),
// as text, malloc'd for the caller to free; the network's NULL until iOS has
// said what it is. NULL in the command line tool.
extern char *(*get_network_report)(void);
extern char *(*get_power_report)(void);
// The device and the window of the terminal in front (/proc/ish/device), as
// text in *report, malloc'd for the caller to free: 0, or the error the read
// gives, _EAGAIN while no terminal window is in front. NULL in the command
// line tool.
extern int (*get_device_report)(char **report);
// The processes the meters' details list, those taking the most of the CPUs
// (app/MonitorSampler.m). Each process (its leader) but init and the zombies:
// its pid, when it started (a pid used again is another process: start_ticks
// says which one the others take), its CPU time, all its threads', user and
// system, and its name. Fills up to max; returns how many there are.
struct ish_process {
    int pid;
    uint64_t start_ticks;
    uint64_t cpu_us;
    char comm[16 + 1];
};
size_t ish_processes(struct ish_process *out, size_t max);
// The memory of the process pid in use (resident, as /proc/<pid>/status's
// VmRSS) if it is the one started at start_ticks: bytes, 0 if it is gone
uint64_t ish_process_resident(int pid, uint64_t start_ticks);
// Asks the process pid to end, if it is the one started at start_ticks:
// SIGTERM, as kill does, or forced, SIGKILL. 0, or _ESRCH if it is gone.
int ish_process_end(int pid, uint64_t start_ticks, bool force);

// iOS's thermal state (ProcessInfo's), 0 nominal to 3 critical, which
// /proc/ish/thermal names: a long job can pause while the device is hot. NULL
// in the command line tool, which has none.
extern int (*get_thermal_state)(void);
// The built-in JIT's state as the app has it (app/BuiltinJIT.m), which
// /proc/ish/jit shows after the JIT's own: the device's pairing file, whether
// it is enabled as iSH starts, what it is doing or what came of its last
// attempt, and that attempt's steps. Lines, malloc'd. NULL in the command
// line tool, which has none.
extern char *(*get_builtin_jit_report)(void);

// The app's name and version ("iSH 2.1.0 (806)"), which it sets at its start
// (AppDelegate.m); "" in the command line tool
extern const char *proc_ish_version;

#endif
