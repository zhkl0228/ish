// Regression test: getdents never moves the directory position backwards.
//
// sys_getdents_common used to read one entry too many (the one that did not
// fit in the caller's buffer) and then seekdir() the host DIR* back to the
// telldir() it took before reading it. POSIX leaves seekdir() to a position
// taken before entries were deleted unspecified, and on some host filesystems
// (a Docker Desktop host share, for one) the position is gone once the
// entries before it are unlinked. busybox `rm -rf` unlinks entries while it
// reads the directory, so it left about half of a large directory behind and
// failed with "Directory not empty". The entry that does not fit is now kept
// on the fd and returned first by the next call.
//
// Checks, each on a fresh directory of NFILES entries:
//   1. Reading with 2 KB getdents64 calls (musl's readdir buffer) and
//      lstat + unlink of each entry before the next call, as busybox rm does,
//      empties it. A raw syscall keeps the batch size independent of the libc
//      the test is linked with: glibc reads 32 KB at a time and would get most
//      of the directory in one call.
//   2. A buffer too small for one entry gets EINVAL, and the next call with a
//      big buffer still starts with the first entry.
//   3. lseek(fd, 0, SEEK_SET) after a partial read lists every entry once.
//   4. lseek(fd, 0, SEEK_CUR) between calls does not lose an entry.
// Running this natively on Linux passes, which pins the expectations. Run it
// on a rootfs whose host filesystem does not keep seekdir positions across
// deletions to see check 1 fail without the fix.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_getdents regress_getdents.c
// Run:   ish -r <rootfs> /tmp/regress_getdents

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NFILES 1200
#define DIR_PATH "/tmp/regress_getdents.d"

static int passed, failed;

static void check(int ok, const char *what) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void make_dir(void) {
    char path[256];
    mkdir(DIR_PATH, 0755);
    for (int i = 0; i < NFILES; i++) {
        snprintf(path, sizeof(path), DIR_PATH "/file_with_a_longish_name_%d.js", i);
        close(open(path, O_CREAT | O_WRONLY, 0644));
    }
}

static void remove_dir(void) {
    char path[512];
    DIR *d = opendir(DIR_PATH);
    if (d != NULL) {
        // Collect first, then delete, so cleanup does not depend on the code
        // under test.
        static char names[NFILES + 2][256];
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < NFILES + 2)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
        closedir(d);
        for (int i = 0; i < n; i++) {
            snprintf(path, sizeof(path), DIR_PATH "/%s", names[i]);
            unlink(path);
        }
    }
    rmdir(DIR_PATH);
}

struct linux_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

// Count real entries (not . or ..) in one getdents64 result.
static int count_entries(const char *buf, long n, char seen[NFILES]) {
    int count = 0;
    for (long pos = 0; pos < n;) {
        const struct linux_dirent64 *e = (const void *) (buf + pos);
        int idx;
        if (sscanf(e->d_name, "file_with_a_longish_name_%d.js", &idx) == 1 && idx >= 0 && idx < NFILES) {
            if (seen != NULL)
                seen[idx]++;
            count++;
        }
        pos += e->d_reclen;
    }
    return count;
}

static void check_rm_pattern(void) {
    make_dir();
    char path[512];
    int fd = open(DIR_PATH, O_RDONLY | O_DIRECTORY);
    char buf[2048];
    long r;
    while ((r = syscall(SYS_getdents64, fd, buf, sizeof(buf))) > 0) {
        for (long pos = 0; pos < r;) {
            const struct linux_dirent64 *e = (const void *) (buf + pos);
            pos += e->d_reclen;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), DIR_PATH "/%s", e->d_name);
            struct stat st;
            lstat(path, &st);
            unlink(path);
        }
    }
    close(fd);
    int ok = rmdir(DIR_PATH) == 0;
    char what[128];
    snprintf(what, sizeof(what), "getdents+lstat+unlink left entries behind (rmdir: %s)", ok ? "ok" : strerror(errno));
    check(ok, what);
    remove_dir();
}

static void check_einval_keeps_entry(void) {
    make_dir();
    int fd = open(DIR_PATH, O_RDONLY | O_DIRECTORY);
    char tiny[8];
    long r = syscall(SYS_getdents64, fd, tiny, sizeof(tiny));
    check(r == -1 && errno == EINVAL, "tiny buffer: want EINVAL");
    static char seen[NFILES];
    memset(seen, 0, sizeof(seen));
    static char buf[1 << 16];
    int total = 0;
    while ((r = syscall(SYS_getdents64, fd, buf, sizeof(buf))) > 0)
        total += count_entries(buf, r, seen);
    int dup = 0, missing = 0;
    for (int i = 0; i < NFILES; i++) {
        if (seen[i] == 0)
            missing++;
        if (seen[i] > 1)
            dup++;
    }
    char what[128];
    snprintf(what, sizeof(what), "after EINVAL: %d listed, %d missing, %d duplicated", total, missing, dup);
    check(total == NFILES && missing == 0 && dup == 0, what);
    close(fd);
    remove_dir();
}

// Read the whole directory in small chunks; optionally lseek(SEEK_CUR, 0)
// between calls, or rewind once after the first chunk.
static void check_listing(int rewind_after_first, int tell_between, const char *name) {
    make_dir();
    int fd = open(DIR_PATH, O_RDONLY | O_DIRECTORY);
    static char seen[NFILES];
    memset(seen, 0, sizeof(seen));
    char buf[700];  // a handful of entries per call
    long r = syscall(SYS_getdents64, fd, buf, sizeof(buf));
    if (rewind_after_first) {
        lseek(fd, 0, SEEK_SET);
    } else {
        count_entries(buf, r, seen);
    }
    while ((r = syscall(SYS_getdents64, fd, buf, sizeof(buf))) > 0) {
        count_entries(buf, r, seen);
        if (tell_between)
            lseek(fd, 0, SEEK_CUR);
    }
    int dup = 0, missing = 0;
    for (int i = 0; i < NFILES; i++) {
        if (seen[i] == 0)
            missing++;
        if (seen[i] > 1)
            dup++;
    }
    char what[160];
    snprintf(what, sizeof(what), "%s: %d missing, %d duplicated", name, missing, dup);
    check(missing == 0 && dup == 0, what);
    close(fd);
    remove_dir();
}

int main(void) {
    remove_dir();
    check_rm_pattern();
    check_einval_keeps_entry();
    check_listing(1, 0, "rewind after a partial read");
    check_listing(0, 1, "lseek(SEEK_CUR, 0) between calls");
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
