// Regression test for a pipe's capacity, F_GETPIPE_SZ and F_SETPIPE_SZ.
//
// fcntl was EINVAL of both: the host's (Darwin's) pipes have no capacity to
// ask or set. CPython's test_fcntl (test_fcntl_f_pipesize) and
// test_subprocess (test_pipesizes, test_pipesize_default: Popen's pipesize)
// failed.
//
// Checked: a pipe's is 64KB, Linux's default, at either end; set at one end
// it is the other's, rounded up as Linux's (a page at least, a power of two
// pages), and a dup's and a forked child's; a FIFO's set through one open is
// another open's; a regular file's is EBADF. On iSH only, more than 64KB is
// EPERM: a Darwin pipe holds no more (on Linux pipe-max-size is 1MB).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_pipe_size regress_pipe_size.c

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

static int get_size(int fd) {
    return fcntl(fd, F_GETPIPE_SZ);
}

int main(void) {
    char detail[200];
    int p[2];
    pipe(p);
    int r = get_size(p[0]), w = get_size(p[1]);
    snprintf(detail, sizeof(detail), "read end %d, write end %d (%s)", r, w, r < 0 ? strerror(errno) : "ok");
    check("a pipe's is 64KB at either end", r == 65536 && w == 65536, detail);

    int set = fcntl(p[1], F_SETPIPE_SZ, 32768);
    r = get_size(p[0]);
    snprintf(detail, sizeof(detail), "set %d, read end %d", set, r);
    check("set at one end, it is the other's", set == 32768 && r == 32768, detail);

    int odd = fcntl(p[0], F_SETPIPE_SZ, 5000), tiny = 0;
    int odd_got = get_size(p[1]);
    tiny = fcntl(p[0], F_SETPIPE_SZ, 1);
    snprintf(detail, sizeof(detail), "5000: %d (then %d); 1: %d", odd, odd_got, tiny);
    check("rounded up to a power of two pages, a page at least", odd == 8192 && odd_got == 8192 && tiny == 4096,
          detail);

    fcntl(p[1], F_SETPIPE_SZ, 16384);
    int copy = dup(p[1]);
    int dup_got = get_size(copy);
    pid_t child = fork();
    if (child == 0)
        _exit(get_size(p[0]) == 16384 ? 0 : 1);
    int status;
    waitpid(child, &status, 0);
    snprintf(detail, sizeof(detail), "dup %d; child %s", dup_got,
             WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "16384" : "another");
    check("a dup's and a forked child's are the pipe's", dup_got == 16384 && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, detail);
    close(copy);
    close(p[0]);
    close(p[1]);

    const char *fifo = "/tmp/regress_pipe_size.fifo";
    unlink(fifo);
    mkfifo(fifo, 0600);
    int fr = open(fifo, O_RDONLY | O_NONBLOCK);
    int fw = open(fifo, O_WRONLY);
    int fifo_set = fcntl(fw, F_SETPIPE_SZ, 8192);
    int fifo_got = get_size(fr);
    snprintf(detail, sizeof(detail), "opens %d %d; set %d, the other open %d (%s)", fr, fw, fifo_set, fifo_got,
             fifo_got < 0 ? strerror(errno) : "ok");
    check("a FIFO's set through one open is another's", fifo_set == 8192 && fifo_got == 8192, detail);
    close(fr);
    close(fw);
    unlink(fifo);

    const char *file = "/tmp/regress_pipe_size.file";
    int ff = open(file, O_RDWR | O_CREAT | O_TRUNC, 0600);
    errno = 0;
    int file_got = get_size(ff);
    int e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", file_got, strerror(e));
    check("a regular file's is EBADF", file_got == -1 && e == EBADF, detail);
    close(ff);
    unlink(file);

    struct stat st;
    if (stat("/proc/ish", &st) == 0) {
        pipe(p);
        errno = 0;
        int big = fcntl(p[1], F_SETPIPE_SZ, 131072);
        e = errno;
        int after = get_size(p[0]);
        snprintf(detail, sizeof(detail), "%d (%s), then %d", big, strerror(e), after);
        check("more than the host's 64KB is EPERM, and leaves it as it was (iSH)",
              big == -1 && e == EPERM && after == 65536, detail);
        close(p[0]);
        close(p[1]);
    } else {
        printf("SKIP more than 64KB: iSH's own (no /proc/ish)\n");
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
