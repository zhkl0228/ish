// Regression test for the path walk on a fakefs root (run.sh runs it as the
// /init of a fakefs of its own).
//
// The walk (path_normalize) asks each directory of a path whether it is a
// symlink, and if not, whether it is a directory the caller may search. On a
// fakefs that was a readlink and then a stat, two meta.db transactions and a
// host fstatat for each directory; now it is one transaction
// (readlink_or_stat), which answers both from meta.db.
//
// Checked, on paths no walk has resolved before (the path cache keeps what one
// resolved for 100 ms):
// - through symlinks to a directory: absolute, relative, with .. in the
//   target, and a trailing slash after one;
// - a dangling symlink in the middle: ENOENT, the link itself there (lstat);
// - a regular file in the middle: ENOTDIR;
// - a loop of symlinks: ELOOP;
// - a directory without search permission in the middle, for a user who is
//   not root: EACCES, though stat of the directory itself works;
// - a directory renamed: the old path ENOENT through the symlink to it, the
//   new path found;
// - mkdir with a trailing slash.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_fakefs_walk regress_fakefs_walk.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed;

static void check(const char *name, int ok, const char *detail) {
    if (ok) {
        passed++;
        printf("PASS %s\n", name);
    } else {
        failed++;
        printf("FAIL %s: %s\n", name, detail);
    }
    fflush(stdout);
}

// stat(path): 0 or the errno, and the size and mode
static int st(const char *path, off_t *size, mode_t *mode) {
    struct stat s;
    if (stat(path, &s) < 0)
        return errno;
    if (size) *size = s.st_size;
    if (mode) *mode = s.st_mode;
    return 0;
}

static void want_file(const char *name, const char *path) {
    off_t size = -1;
    int err = st(path, &size, NULL);
    char detail[200];
    snprintf(detail, sizeof(detail), "stat(%s): %s, size %lld (want the 5-byte file)", path, err ? strerror(err) : "0",
             (long long) size);
    check(name, err == 0 && size == 5, detail);
}

static void want_err(const char *name, const char *path, int want) {
    int err = st(path, NULL, NULL);
    char detail[200];
    snprintf(detail, sizeof(detail), "stat(%s): %s, want %s", path, err ? strerror(err) : "0", strerror(want));
    check(name, err == want, detail);
}

static void mk(const char *path) {
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        perror(path);
        exit(1);
    }
}

static void ln(const char *target, const char *path) {
    if (symlink(target, path) < 0) {
        perror(path);
        exit(1);
    }
}

static void put(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "hello", 5) != 5) {
        perror(path);
        exit(1);
    }
    close(fd);
}

int main(void) {
    mk("/tmp/w");
    mk("/tmp/w/a");
    mk("/tmp/w/a/b");
    mk("/tmp/w/a/b/c");
    put("/tmp/w/a/b/c/file");
    for (int i = 1; i <= 4; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/tmp/w/a/b/c/file%d", i);
        put(p);
    }
    ln("/tmp/w/a/b", "/tmp/w/abs");
    ln("a/b", "/tmp/w/rel");
    ln("../a/b", "/tmp/w/a/up");
    ln("/tmp/w/nothere", "/tmp/w/dangle");
    ln("loop2", "/tmp/w/loop1");
    ln("loop1", "/tmp/w/loop2");
    put("/tmp/w/f");
    mk("/tmp/w/locked");
    put("/tmp/w/locked/x");
    if (chmod("/tmp/w/locked", 0700) < 0) {
        perror("chmod");
        return 1;
    }

    want_file("an absolute symlink to a directory, in the middle", "/tmp/w/abs/c/file");
    want_file("a relative symlink to a directory, in the middle", "/tmp/w/rel/c/file1");
    want_file("a symlink with .. in its target, in the middle", "/tmp/w/a/up/c/file2");
    want_file("plain directories", "/tmp/w/a/b/c/file3");
    mode_t mode = 0;
    int err = st("/tmp/w/abs/", NULL, &mode);
    char detail[200];
    snprintf(detail, sizeof(detail), "stat(/tmp/w/abs/): %s, mode %o", err ? strerror(err) : "0", (unsigned) mode);
    check("a symlink to a directory with a trailing slash: the directory", err == 0 && S_ISDIR(mode), detail);

    want_err("a dangling symlink in the middle: ENOENT", "/tmp/w/dangle/x", ENOENT);
    struct stat ls;
    err = lstat("/tmp/w/dangle", &ls) < 0 ? errno : 0;
    snprintf(detail, sizeof(detail), "lstat: %s", err ? strerror(err) : "0");
    check("the dangling symlink itself: there (lstat)", err == 0 && S_ISLNK(ls.st_mode), detail);
    want_err("a regular file in the middle: ENOTDIR", "/tmp/w/f/x", ENOTDIR);
    want_err("a loop of symlinks: ELOOP", "/tmp/w/loop1/x", ELOOP);
    want_err("a missing directory in the middle: ENOENT", "/tmp/w/a/none/c/file", ENOENT);

    // a user who is not root: no search permission on /tmp/w/locked
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(1000) < 0 || setuid(1000) < 0)
            _exit(2);
        int in = st("/tmp/w/locked/x", NULL, NULL), dir = st("/tmp/w/locked", NULL, NULL);
        _exit(in == EACCES && dir == 0 ? 0 : in == 0 ? 3 : 4);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    snprintf(detail, sizeof(detail), "the child's status %#x (0: EACCES in, the directory's stat 0; 2: setuid failed; "
             "3: found; 4: another error)", status);
    check("a directory a user may not search, in the middle: EACCES for it", WIFEXITED(status) && WEXITSTATUS(status) == 0,
          detail);

    if (rename("/tmp/w/a/b", "/tmp/w/a/b2") < 0) {
        perror("rename");
        return 1;
    }
    want_err("a directory renamed: through the symlink to its old name, ENOENT", "/tmp/w/abs/c/file4", ENOENT);
    want_file("a directory renamed: its new name", "/tmp/w/a/b2/c/file4");

    err = mkdir("/tmp/w/a/made/", 0755) < 0 ? errno : 0;
    int there = st("/tmp/w/a/made", NULL, &mode);
    snprintf(detail, sizeof(detail), "mkdir: %s, then stat: %s", err ? strerror(err) : "0", there ? strerror(there) : "0");
    check("mkdir with a trailing slash: the directory made", err == 0 && there == 0 && S_ISDIR(mode), detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
