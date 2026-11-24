// Regression test for truncate(2) of a path.
//
// iSH's truncate looked its path up from a NULL directory, which
// path_normalize asserts is none: iSH aborted on every truncate of a path,
// relative or not (CPython's test_posix test_truncate took the app down).
//
// Checked: truncate of a relative path to 0, of an absolute one longer
// (zeros after), and of one that is not (ENOENT). Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_truncate_path regress_truncate_path.c

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

int main(void) {
    char dir[64], path[96], detail[120];
    snprintf(dir, sizeof(dir), "/tmp/regress_truncate_path.%d", getpid());
    mkdir(dir, 0700);
    chdir(dir);
    int fd = open("file", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    write(fd, "test", 4);
    close(fd);

    int r = truncate("file", 0);
    int e = errno;
    struct stat st;
    stat("file", &st);
    snprintf(detail, sizeof(detail), "%d (%s), size %lld", r, r < 0 ? strerror(e) : "ok", (long long) st.st_size);
    check("truncate of a relative path to 0", r == 0 && st.st_size == 0, detail);

    snprintf(path, sizeof(path), "%s/file", dir);
    r = truncate(path, 8);
    e = errno;
    stat(path, &st);
    char buf[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    fd = open(path, O_RDONLY);
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    static const char zeros[8];
    snprintf(detail, sizeof(detail), "%d (%s), size %lld, read %zd", r, r < 0 ? strerror(e) : "ok",
             (long long) st.st_size, n);
    check("truncate of an absolute path longer", r == 0 && st.st_size == 8 && n == 8 && memcmp(buf, zeros, 8) == 0,
          detail);

    r = truncate("none", 0);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("truncate of a path that is not", r == -1 && e == ENOENT, detail);

    unlink("file");
    chdir("/");
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
