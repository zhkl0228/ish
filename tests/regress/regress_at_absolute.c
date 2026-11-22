// Regression test for *at() calls with an absolute path and a bad dirfd.
//
// On Linux an absolute path ignores the directory argument of openat() and its
// relatives, so it can be any number. iSH checked the descriptor first and
// returned EBADF. MariaDB opens a file's parent directories with
// openat(-1, "/", O_PATH | O_NOFOLLOW) and walks down from there; with EBADF it
// could not open its Aria tables, and mariadb-install-db crashed in the error
// path that followed.
//
// With dirfd -1, calls on absolute paths must succeed, and a relative path
// must still fail with EBADF. Running natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_at_absolute regress_at_absolute.c
// Run:   ish -r <rootfs> /tmp/regress_at_absolute

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int passed, failed;

static void check(const char *name, int ok) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s (errno %d %s)\n", name, errno, strerror(errno));
    }
}

int main(void) {
    const char *dir = "/tmp/regress_at_absolute.d";
    const char *link = "/tmp/regress_at_absolute.l";
    rmdir(dir);
    unlink(link);

    int fd = openat(-1, "/", O_PATH | O_NOFOLLOW | O_CLOEXEC);
    check("openat O_PATH /", fd >= 0);
    if (fd >= 0)
        close(fd);
    fd = openat(-1, "/tmp", O_RDONLY | O_DIRECTORY);
    check("openat /tmp", fd >= 0);
    if (fd >= 0)
        close(fd);

    struct stat st;
    check("fstatat /tmp", fstatat(-1, "/tmp", &st, 0) == 0 && S_ISDIR(st.st_mode));
    check("mkdirat", mkdirat(-1, dir, 0755) == 0);
    check("unlinkat dir", unlinkat(-1, dir, AT_REMOVEDIR) == 0);
    check("symlinkat", symlinkat("/tmp", -1, link) == 0);
    char buf[64] = {0};
    check("readlinkat", readlinkat(-1, link, buf, sizeof(buf) - 1) == 4 && strcmp(buf, "/tmp") == 0);
    check("unlinkat link", unlinkat(-1, link, 0) == 0);

    errno = 0;
    check("relative path still EBADF", openat(-1, "tmp", O_RDONLY) < 0 && errno == EBADF);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
