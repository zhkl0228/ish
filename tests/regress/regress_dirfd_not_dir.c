// Regression test for a *at call whose directory fd is no directory.
//
// Linux says ENOTDIR of a relative path from it. iSH asked the fd for its
// path, which for a terminal, a pipe or a file is no directory's (or the fd
// has no mount): path_normalize's assert failed and iSH aborted (CPython's
// test_posix test_stat_dir_fd: stat('nonexisting', dir_fd=False), fd 0).
//
// Checked: fstatat, openat and mkdirat of a relative path from a regular
// file's fd, a pipe's, /dev/null's and stdin say ENOTDIR; an absolute path
// from a file's fd is looked up as ever (the fd is not used). Natively
// passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_dirfd_not_dir regress_dirfd_not_dir.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

static void expect_enotdir(const char *what, int dirfd) {
    char name[100], detail[100];
    struct stat st;
    int r = fstatat(dirfd, "nonexisting", &st, 0);
    int e = errno;
    snprintf(name, sizeof(name), "fstatat from %s", what);
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, r == -1 && e == ENOTDIR, detail);

    r = openat(dirfd, "nonexisting", O_RDONLY);
    e = errno;
    snprintf(name, sizeof(name), "openat from %s", what);
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, r == -1 && e == ENOTDIR, detail);

    r = mkdirat(dirfd, "nonexisting", 0700);
    e = errno;
    snprintf(name, sizeof(name), "mkdirat from %s", what);
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, r == -1 && e == ENOTDIR, detail);
}

int main(void) {
    char file[64];
    snprintf(file, sizeof(file), "/tmp/regress_dirfd_not_dir.%d", getpid());
    int file_fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0600);
    expect_enotdir("a regular file's fd", file_fd);

    int pipe_fds[2];
    pipe(pipe_fds);
    expect_enotdir("a pipe's fd", pipe_fds[0]);

    int null_fd = open("/dev/null", O_RDONLY);
    expect_enotdir("/dev/null's fd", null_fd);

    // stdin, as the test that found it: whatever it is, no directory here
    int in = open("/dev/null", O_RDONLY);
    dup2(in, 0);
    close(in);
    expect_enotdir("stdin", 0);

    struct stat st;
    int r = fstatat(file_fd, file, &st, 0);
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("fstatat of an absolute path from a file's fd", r == 0, detail);

    unlink(file);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
