#ifndef FS_FAKEFSIFY_H
#define FS_FAKEFSIFY_H
#include <stdbool.h>

struct fakefsify_error {
    int line;
    enum {
        ERR_ARCHIVE,
        ERR_SQLITE,
        ERR_POSIX,
        ERR_CANCELLED,
    } type;
    int code;
    char *message;
};

struct progress {
    void *cookie;
    void (*callback)(void *cookie, double progress, const char *message, bool *cancel_out);
};

bool fakefs_import(const char *archive_path, const char *fs, struct fakefsify_error *err_out, struct progress progress);
bool fakefs_export(const char *fs, const char *archive_path, struct fakefsify_error *err_out, struct progress progress);
// Carry directories over from one root to another, as resetting a root does
// with the home directories: each of dirs (absolute paths, NULL-terminated),
// with all below it, takes the place of new_fs's. Their files are moved,
// old_fs being about to go, and their metadata copied. A dir old_fs lacks is
// left as new_fs has it. On failure old_fs is as it was.
bool fakefs_carry_over(const char *old_fs, const char *new_fs, const char *const dirs[], struct fakefsify_error *err_out);

#endif
