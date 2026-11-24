// Regression test for utimensat with AT_SYMLINK_NOFOLLOW: a symlink's own
// times.
//
// iSH's realfs (fakefs's too) set them with the host following the symlink
// it was given, already not followed: a dangling symlink's said ENOENT
// (CPython's shutil.copytree(symlinks=True), whose copystat sets a copied
// link's times), and a symlink to a file set the file's.
//
// Checked: utimensat with AT_SYMLINK_NOFOLLOW of a dangling symlink sets its
// times; of a symlink to a file sets the link's, the file's kept; without
// the flag, the file's. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_symlink_utime regress_symlink_utime.c

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
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/regress_symlink_utime.%d", getpid());
    mkdir(dir, 0700);
    chdir(dir);
    close(open("file", O_CREAT | O_WRONLY, 0600));
    symlink("file", "link");
    symlink("/nonexistent/target", "dangling");
    struct timespec t1[2] = {{1000000, 0}, {1000000, 0}};
    struct timespec t2[2] = {{2000000, 0}, {2000000, 0}};
    struct timespec t3[2] = {{3000000, 0}, {3000000, 0}};
    utimensat(AT_FDCWD, "file", t1, 0);
    char detail[160];
    struct stat link_st, file_st;

    int r = utimensat(AT_FDCWD, "dangling", t2, AT_SYMLINK_NOFOLLOW);
    int e = errno;
    lstat("dangling", &link_st);
    snprintf(detail, sizeof(detail), "%d (%s), its mtime %lld", r, r < 0 ? strerror(e) : "ok",
             (long long) link_st.st_mtime);
    check("a dangling symlink's own times", r == 0 && link_st.st_mtime == 2000000, detail);

    r = utimensat(AT_FDCWD, "link", t2, AT_SYMLINK_NOFOLLOW);
    e = errno;
    lstat("link", &link_st);
    stat("file", &file_st);
    snprintf(detail, sizeof(detail), "%d (%s), the link's mtime %lld, the file's %lld", r, r < 0 ? strerror(e) : "ok",
             (long long) link_st.st_mtime, (long long) file_st.st_mtime);
    check("a symlink's own times, its file's kept", r == 0 && link_st.st_mtime == 2000000 &&
          file_st.st_mtime == 1000000, detail);

    r = utimensat(AT_FDCWD, "link", t3, 0);
    e = errno;
    stat("file", &file_st);
    snprintf(detail, sizeof(detail), "%d (%s), the file's mtime %lld", r, r < 0 ? strerror(e) : "ok",
             (long long) file_st.st_mtime);
    check("without the flag, its file's times", r == 0 && file_st.st_mtime == 3000000, detail);

    unlink("dangling");
    unlink("link");
    unlink("file");
    chdir("/");
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
