#include <stddef.h>
#include <stdbool.h>

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

// The app's name and version ("iSH 2.1.0 (806)"), which it sets at its start
// (AppDelegate.m); "" in the command line tool
extern const char *proc_ish_version;
