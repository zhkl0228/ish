// Regression test for paths longer than the host's: Linux takes 4096 bytes in
// a path, Darwin (iOS, macOS) 1024, and iSH gave a filesystem path to the
// host whole, from the mount's root. A directory deeper than 1024 bytes could
// not be made, entered or had a file in it: ENAMETOOLONG, however short the
// path the guest gave (a relative one, from a cwd down there). CPython's
// test_os and test_tarfile went that deep; so can node_modules, a Maven
// repository or a build's tree.
//
// Twelve directories of 202 bytes each under /tmp (2.4 KB): each made, the
// deepest entered (getcwd), a file in it made by a relative path and read by
// the absolute one; stat, rename, link, symlink and readlink, truncate,
// chmod, utimensat, a FIFO, openat from the deepest directory's fd, readdir
// and realpath there; then all removed. Passes on a Linux host; on Darwin
// the directories past 1024 bytes failed.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_long_path regress_long_path.c

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LEVELS 12
#define NAME_BYTES 202

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

static char detail[256];

// detail says what a call returned: its result, and errno's name if it failed
static const char *said(long r, int e) {
    snprintf(detail, sizeof(detail), "%ld (%s)", r, r < 0 ? strerror(e) : "ok");
    return detail;
}

static char level_path[LEVELS + 1][PATH_MAX];

int main(void) {
    // /tmp/regress_long_path.d/<202 bytes>/... : level_path[i] is i levels deep
    snprintf(level_path[0], PATH_MAX, "/tmp/regress_long_path.d");
    for (int i = 1; i <= LEVELS; i++) {
        char name[NAME_BYTES + 1];
        memset(name, 'd', NAME_BYTES - 2);
        snprintf(name + NAME_BYTES - 2, 3, "%02d", i);
        size_t len = strlen(level_path[i - 1]);
        memcpy(level_path[i], level_path[i - 1], len);
        level_path[i][len] = '/';
        memcpy(level_path[i] + len + 1, name, NAME_BYTES + 1);
    }
    const char *deep = level_path[LEVELS];
    printf("    NOTE deepest directory: %zu bytes\n", strlen(deep));

    int made = mkdir(level_path[0], 0755) == 0 || errno == EEXIST;
    int deepest_made = 0;
    for (int i = 1; i <= LEVELS && made; i++) {
        int r = mkdir(level_path[i], 0755);
        made = r == 0;
        if (!made) {
            snprintf(detail, sizeof(detail), "level %d (%zu bytes): %s", i, strlen(level_path[i]), strerror(errno));
            break;
        }
        deepest_made = i;
    }
    check("mkdir down to 2.4 KB", made, detail);
    if (!made) {
        for (int i = deepest_made; i >= 0; i--)
            rmdir(level_path[i]);
        printf("%d passed, %d failed\n", passed, failed);
        return 1;
    }

    int r = chdir(deep);
    check("chdir to the deepest", r == 0, said(r, errno));
    char cwd[PATH_MAX];
    char *got = getcwd(cwd, sizeof(cwd));
    snprintf(detail, sizeof(detail), "%s", got == NULL ? strerror(errno) : "another path");
    check("getcwd there", got != NULL && strcmp(cwd, deep) == 0, detail);

    int fd = open("f", O_CREAT | O_EXCL | O_WRONLY, 0644);
    check("open(O_CREAT) by a relative path there", fd >= 0, said(fd, errno));
    if (fd >= 0) {
        r = (int) write(fd, "hello", 5);
        close(fd);
        check("write it", r == 5, said(r, errno));
    }
    // a path made of deep and a name: as long as deep can be, and the name
    char path[2 * PATH_MAX], buf[16] = {0};
    snprintf(path, sizeof(path), "%s/f", deep);
    fd = open(path, O_RDONLY);
    r = fd < 0 ? fd : (int) read(fd, buf, sizeof(buf) - 1);
    if (fd >= 0)
        close(fd);
    check("read it by the absolute path", r == 5 && strcmp(buf, "hello") == 0, said(r, errno));

    struct stat st;
    r = stat(path, &st);
    check("stat by the absolute path", r == 0 && st.st_size == 5, said(r, errno));

    r = rename("f", "g");
    check("rename there", r == 0 && access("g", F_OK) == 0 && access("f", F_OK) < 0, said(r, errno));
    r = link("g", "h");
    check("link there", r == 0 && stat("g", &st) == 0 && st.st_nlink == 2, said(r, errno));
    r = symlink("g", "s");
    char target[16] = {0};
    ssize_t n = r < 0 ? r : readlink("s", target, sizeof(target) - 1);
    check("symlink and readlink there", n == 1 && strcmp(target, "g") == 0, said(n, errno));
    r = stat("s", &st);
    check("stat through the symlink", r == 0 && st.st_size == 5, said(r, errno));

    snprintf(path, sizeof(path), "%s/g", deep);
    r = truncate(path, 2);
    check("truncate by the absolute path", r == 0 && stat(path, &st) == 0 && st.st_size == 2, said(r, errno));
    r = chmod(path, 0600);
    check("chmod by the absolute path", r == 0 && stat(path, &st) == 0 && (st.st_mode & 07777) == 0600, said(r, errno));
    struct timespec times[2] = {{.tv_sec = 1000000000}, {.tv_sec = 1000000000}};
    r = utimensat(AT_FDCWD, path, times, 0);
    check("utimensat by the absolute path", r == 0 && stat(path, &st) == 0 && st.st_mtime == 1000000000,
          said(r, errno));
    r = mkfifo("p", 0644);
    check("mkfifo there", r == 0 && lstat("p", &st) == 0 && S_ISFIFO(st.st_mode), said(r, errno));

    int dirfd = open(deep, O_RDONLY | O_DIRECTORY);
    fd = dirfd < 0 ? dirfd : openat(dirfd, "g", O_RDONLY);
    check("openat from the deepest directory's fd", fd >= 0, said(fd, errno));
    if (fd >= 0)
        close(fd);
    if (dirfd >= 0)
        close(dirfd);

    DIR *dir = opendir(".");
    int seen = 0;
    if (dir != NULL) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL)
            seen += strcmp(e->d_name, "g") == 0 || strcmp(e->d_name, "h") == 0 || strcmp(e->d_name, "s") == 0 ||
                    strcmp(e->d_name, "p") == 0;
        closedir(dir);
    }
    snprintf(detail, sizeof(detail), "%d of g, h, s, p (%s)", seen, dir == NULL ? strerror(errno) : "read");
    check("readdir there", seen == 4, detail);

    char resolved[PATH_MAX];
    snprintf(path, sizeof(path), "%s/../%s", deep, strrchr(deep, '/') + 1);
    got = realpath(path, resolved);
    snprintf(detail, sizeof(detail), "%s", got == NULL ? strerror(errno) : "another path");
    check("realpath there", got != NULL && strcmp(resolved, deep) == 0, detail);

    int removed = unlink("g") == 0 && unlink("h") == 0 && unlink("s") == 0 && unlink("p") == 0;
    snprintf(detail, sizeof(detail), "%s", strerror(errno));
    check("unlink there", removed, detail);
    chdir("/");
    removed = 1;
    for (int i = LEVELS; i >= 1 && removed; i--) {
        removed = rmdir(level_path[i]) == 0;
        if (!removed)
            snprintf(detail, sizeof(detail), "level %d: %s", i, strerror(errno));
    }
    check("rmdir back up", removed, detail);
    rmdir(level_path[0]);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
