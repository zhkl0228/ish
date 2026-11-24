// Regression test for a path's components looked up as Linux's walk looks
// them up, and the reflink ioctls of a filesystem without reflinks.
//
// iSH let a component that is not pass when more came after it, and a ".."
// after it took it away: "xyzzy/.." was "." (CPython's test_pathlib,
// test_glob_dotdot). And FICLONE of a file was ENOTTY, which CPython 3.14's
// Path.copy takes for an error (its copy tests): Linux's VFS says
// EOPNOTSUPP where the filesystem has no reflinks, and Path.copy copies.
//
// Checked: stat of "missing/.." is ENOENT, of "file/.." ENOTDIR, of
// "dir/sub/.." the directory; open of "missing/x" ENOENT; mkdir of "new/"
// makes it; FICLONE and FICLONERANGE of a file are EOPNOTSUPP. Natively
// passes (on a filesystem without reflinks, as ext4).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_path_walk regress_path_walk.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define FICLONE_ 0x40049409
#define FICLONERANGE_ 0x4020940d

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

static void expect(const char *name, int r, int want) {
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, want == 0 ? r >= 0 : r == -1 && e == want, detail);
}

int main(void) {
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/regress_path_walk.%d", getpid());
    mkdir(dir, 0700);
    chdir(dir);
    mkdir("dir", 0700);
    mkdir("dir/sub", 0700);
    int file = open("file", O_CREAT | O_RDWR, 0600);
    write(file, "x", 1);
    struct stat st, dir_st;

    expect("stat of \"missing/..\"", stat("missing/..", &st), ENOENT);
    expect("stat of \"file/..\"", stat("file/..", &st), ENOTDIR);
    stat("dir", &dir_st);
    int r = stat("dir/sub/..", &st);
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s), %s", r, r < 0 ? strerror(e) : "ok",
             r == 0 && st.st_ino == dir_st.st_ino ? "the directory" : "another");
    check("stat of \"dir/sub/..\" is the directory", r == 0 && st.st_ino == dir_st.st_ino, detail);
    expect("open of \"missing/x\"", open("missing/x", O_RDONLY), ENOENT);
    expect("mkdir of \"new/\"", mkdir("new/", 0700), 0);

    int clone = open("clone", O_CREAT | O_RDWR, 0600);
    expect("FICLONE of a file", ioctl(clone, FICLONE_, file), EOPNOTSUPP);
    struct { int64_t src_fd; uint64_t src_offset, src_length, dest_offset; } range = {file, 0, 0, 0};
    expect("FICLONERANGE of a file", ioctl(clone, FICLONERANGE_, &range), EOPNOTSUPP);

    close(clone);
    close(file);
    unlink("clone");
    unlink("file");
    rmdir("new");
    rmdir("dir/sub");
    rmdir("dir");
    chdir("/");
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
