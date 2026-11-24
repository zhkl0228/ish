// Regression test for a name ending in a slash, which names a directory.
//
// iSH's open with O_CREAT of "new/" made a file "new", where Linux says
// EISDIR (CPython's shutil.copyfile to a directory that is not, which takes
// EISDIR for it: test_copyfile_nonexistent_dir); and unlink of "file/"
// unlinked the file, where Linux says ENOTDIR.
//
// Checked: open with O_CREAT of "new/" is EISDIR and makes nothing; of
// "missing/new/" ENOENT; unlink of "file/" ENOTDIR, the file kept; of
// "dir/" EISDIR; of "none/" ENOENT; stat of "file/" ENOTDIR. Natively
// passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_trailing_slash regress_trailing_slash.c

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

static void expect(const char *name, int r, int want, const char *extra) {
    int e = errno;
    char detail[160];
    snprintf(detail, sizeof(detail), "%d (%s)%s", r, r < 0 ? strerror(e) : "ok", extra);
    check(name, r == -1 && e == want, detail);
}

int main(void) {
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/regress_trailing_slash.%d", getpid());
    mkdir(dir, 0700);
    chdir(dir);
    close(open("file", O_CREAT | O_WRONLY, 0600));
    mkdir("dir", 0700);
    struct stat st;

    int fd = open("new/", O_CREAT | O_WRONLY, 0600);
    int e = errno;
    int made = lstat("new", &st) == 0;
    if (fd >= 0)
        close(fd);
    errno = e;
    expect("open with O_CREAT of \"new/\"", fd, EISDIR, made ? ", and \"new\" was made" : "");
    unlink("new");

    fd = open("missing/new/", O_CREAT | O_WRONLY, 0600);
    expect("open with O_CREAT of \"missing/new/\"", fd, ENOENT, "");

    int r = unlink("file/");
    e = errno;
    int kept = lstat("file", &st) == 0;
    errno = e;
    expect("unlink of \"file/\"", r, ENOTDIR, kept ? "" : ", and the file is gone");
    expect("unlink of \"dir/\"", unlink("dir/"), EISDIR, "");
    expect("unlink of \"none/\"", unlink("none/"), ENOENT, "");
    expect("stat of \"file/\"", stat("file/", &st), ENOTDIR, "");

    unlink("file");
    rmdir("dir");
    chdir("/");
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
