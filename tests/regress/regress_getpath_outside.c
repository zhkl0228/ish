// Regression test for a directory whose host path is out of its fakefs, as
// a bind mount's link is (iOS) once the table of bind mounts is cleared: the
// app restarted, the mounts yet to be registered again.
//
// realfs_getpath said "/" of an fd whose host path was not under its mount's
// source, and every call by the fd's path acted on the root instead, saying
// nothing: getcwd in it said "/", a file made by openat from it as a dirfd
// was made in /, futimens of it set the root's times. The file has no path
// in the guest: ENOENT, as Linux's getcwd of a directory out of the root's
// reach.
//
// run.sh boots it twice on a fakefs of its own: "setup" makes /mnt, which
// the host then makes a link to a directory out of the fakefs; "check":
// /mnt is opened and gone into; getcwd in it, openat with O_CREAT from it
// as a dirfd (nothing made in /), getdents64 of it (musl's readdir takes
// ENOENT for the end, as of a directory removed), and futimens of it (the
// root's times kept) are ENOENT. Not a Linux case: the host's link is one
// only iSH's fakefs follows.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_getpath_outside regress_getpath_outside.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

static const char *result(int r, int e) {
    static char buf[80];
    snprintf(buf, sizeof(buf), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    return buf;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "setup") == 0)
        return mkdir("/mnt", 0755) == 0 ? 0 : 1;

    char detail[300];
    struct stat root_before, root_after;
    stat("/", &root_before);

    int dir = open("/mnt", O_RDONLY | O_DIRECTORY);
    int e = errno;
    check("open /mnt", dir >= 0, result(dir, e));
    int r = chdir("/mnt");
    e = errno;
    check("chdir /mnt", r == 0, result(r, e));

    char cwd[256];
    char *got = getcwd(cwd, sizeof(cwd));
    e = errno;
    snprintf(detail, sizeof(detail), "%s", got != NULL ? got : strerror(e));
    check("getcwd in it is ENOENT", got == NULL && e == ENOENT, detail);

    int fd = openat(dir, "regress_getpath_outside", O_CREAT | O_WRONLY, 0644);
    e = errno;
    int in_root = access("/regress_getpath_outside", F_OK) == 0;
    snprintf(detail, sizeof(detail), "%s, made in /: %s", result(fd, e), in_root ? "yes" : "no");
    check("openat O_CREAT from it as a dirfd is ENOENT, nothing made in /", fd < 0 && e == ENOENT && !in_root, detail);

    char dirents[1024];
    long n = syscall(SYS_getdents64, dir, dirents, sizeof(dirents));
    e = errno;
    check("getdents64 of it is ENOENT", n == -1 && e == ENOENT, result((int) n, e));

    struct timespec times[2] = {{1000000, 0}, {2000000, 0}};
    r = futimens(dir, times);
    e = errno;
    stat("/", &root_after);
    snprintf(detail, sizeof(detail), "%s, the root's mtime %lld, was %lld", result(r, e),
            (long long) root_after.st_mtime, (long long) root_before.st_mtime);
    check("futimens of it is ENOENT, the root's times kept",
            r == -1 && e == ENOENT && root_after.st_mtime == root_before.st_mtime, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
