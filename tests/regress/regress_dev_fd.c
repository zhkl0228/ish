// Regression test: /dev/fd/N (/proc/self/fd/N) and /dev/stdin of a pipe, as
// bash's process substitution, <(...) and >(...), and diff use them.
//
// /dev/fd was not there, and /proc/<pid>/fd/N of a pipe linked to
// "anon_inode:[pipe]", which no open or stat could follow: `cat <(echo hi)`
// said "/dev/fd/63: No such file or directory". Every pipe had inode 0, so
// `diff <(...) <(...)` took the two for one file and said they were the same.
// Now the link is to the pipe itself, as Linux's: it opens to the end asked
// for, stats as the pipe (a FIFO, both ends of one inode, each pipe of its
// own), and reads "pipe:[inode]". Another user's is root's alone (EACCES).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_dev_fd regress_dev_fd.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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

int main(void) {
    char detail[512], path[64], buf[64];

    // 1. a pipe's read end, opened as /dev/fd/N: a new descriptor of it
    int p[2];
    pipe(p);
    write(p[1], "hello", 5);
    snprintf(path, sizeof(path), "/dev/fd/%d", p[0]);
    int r = open(path, O_RDONLY);
    int open_errno = errno;
    ssize_t n = r >= 0 ? read(r, buf, sizeof(buf)) : -1;
    snprintf(detail, sizeof(detail), "open %s: %d (%s), read %zd", path, r, r < 0 ? strerror(open_errno) : "ok", n);
    check("/dev/fd/N of a pipe's read end", r >= 0 && r != p[0] && n == 5 && memcmp(buf, "hello", 5) == 0, detail);
    if (r >= 0)
        close(r);

    // 2. its write end, as >(...) is
    snprintf(path, sizeof(path), "/dev/fd/%d", p[1]);
    int w = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    open_errno = errno;
    ssize_t wrote = w >= 0 ? write(w, "world", 5) : -1;
    n = read(p[0], buf, sizeof(buf));
    snprintf(detail, sizeof(detail), "open %s: %d (%s), wrote %zd, read back %zd", path, w,
             w < 0 ? strerror(open_errno) : "ok", wrote, n);
    check("/dev/fd/N of a pipe's write end", w >= 0 && wrote == 5 && n == 5 && memcmp(buf, "world", 5) == 0, detail);
    if (w >= 0)
        close(w);

    // 3. stat: the pipe, a FIFO, one inode for its ends, another pipe's its own;
    // the link reads pipe:[inode]
    int q[2];
    pipe(q);
    struct stat via_link, end0, end1, other;
    snprintf(path, sizeof(path), "/dev/fd/%d", p[0]);
    int r_stat = stat(path, &via_link);
    fstat(p[0], &end0);
    fstat(p[1], &end1);
    fstat(q[0], &other);
    char link[64] = "", want[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", p[0]);
    ssize_t link_len = readlink(path, link, sizeof(link) - 1);
    if (link_len >= 0)
        link[link_len] = '\0';
    snprintf(want, sizeof(want), "pipe:[%lu]", (unsigned long) end0.st_ino);
    snprintf(detail, sizeof(detail), "stat %s (mode %o ino %lu), ends' inodes %lu %lu, another pipe's %lu, link \"%s\"",
             r_stat < 0 ? strerror(errno) : "ok", via_link.st_mode, (unsigned long) via_link.st_ino,
             (unsigned long) end0.st_ino, (unsigned long) end1.st_ino, (unsigned long) other.st_ino, link);
    check("stat: the pipe, its own inode, pipe:[inode]", r_stat == 0 && S_ISFIFO(via_link.st_mode) &&
          via_link.st_ino == end0.st_ino && end0.st_ino == end1.st_ino && other.st_ino != end0.st_ino &&
          strcmp(link, want) == 0, detail);

    // 4. /dev/stdin of a process whose standard input is a pipe
    int in[2], out[2];
    pipe(in);
    pipe(out);
    pid_t child = fork();
    if (child == 0) {
        dup2(in[0], 0);
        close(in[0]);
        close(in[1]);
        int fd = open("/dev/stdin", O_RDONLY);
        char got[16];
        ssize_t got_n = fd >= 0 ? read(fd, got, sizeof(got)) : -errno;
        write(out[1], &got_n, sizeof(got_n));
        if (got_n > 0)
            write(out[1], got, got_n);
        _exit(0);
    }
    close(in[0]);
    write(in[1], "stdin", 5);
    close(in[1]);
    ssize_t got_n = 0;
    read(out[0], &got_n, sizeof(got_n));
    n = got_n > 0 ? read(out[0], buf, sizeof(buf)) : 0;
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "the child read %zd (%s)", got_n, got_n < 0 ? strerror(-got_n) : "ok");
    check("/dev/stdin of a pipe", got_n == 5 && n == 5 && memcmp(buf, "stdin", 5) == 0, detail);

    // 5. another user's: root's only
    if (getuid() == 0) {
        snprintf(path, sizeof(path), "/proc/%d/fd/%d", getpid(), q[0]);
        child = fork();
        if (child == 0) {
            setgid(1000);
            setuid(1000);
            int fd = open(path, O_RDONLY);
            _exit(fd >= 0 ? 0 : errno);
        }
        int status;
        waitpid(child, &status, 0);
        snprintf(detail, sizeof(detail), "open %s as uid 1000: %s", path,
                 WIFEXITED(status) ? WEXITSTATUS(status) ? strerror(WEXITSTATUS(status)) : "opened" : "killed");
        check("another user's: EACCES", WIFEXITED(status) && WEXITSTATUS(status) == EACCES, detail);
    } else {
        printf("SKIP another user's: not root\n");
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
