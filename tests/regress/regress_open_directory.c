// Regression test for open with O_DIRECTORY of what is no directory: ENOTDIR,
// at once, before the open does anything else.
//
// iSH said ENOTDIR after the filesystem opened the file, and a FIFO's open
// waits for its other end: os.walk of a FIFO (scandir, opendir) waited for
// good, where Linux says ENOTDIR at once (CPython's test_walk_named_pipe hung
// test_os). A device was opened first too.
//
// Checked, each with a 2 s alarm (EINTR: it waited): a FIFO opened for
// reading and for writing, and by opendir; a regular file; /dev/null; and a
// directory opens. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_open_directory regress_open_directory.c

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

static void on_alarm(int sig) {
    (void) sig;
}

static void expect_enotdir(const char *name, const char *path, int flags) {
    alarm(2);
    int fd = open(path, flags | O_DIRECTORY | O_CLOEXEC);
    int e = errno;
    alarm(0);
    if (fd >= 0)
        close(fd);
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", fd, fd < 0 ? strerror(e) : "ok");
    check(name, fd == -1 && e == ENOTDIR, detail);
}

int main(void) {
    struct sigaction sa = {.sa_handler = on_alarm};
    sigaction(SIGALRM, &sa, NULL);

    char dir[64], fifo[80], file[80];
    snprintf(dir, sizeof(dir), "/tmp/regress_open_directory.%d", getpid());
    snprintf(fifo, sizeof(fifo), "%s/fifo", dir);
    snprintf(file, sizeof(file), "%s/file", dir);
    mkdir(dir, 0700);
    mkfifo(fifo, 0600);
    close(open(file, O_CREAT | O_WRONLY, 0600));

    expect_enotdir("a FIFO opened for reading", fifo, O_RDONLY);
    expect_enotdir("a FIFO opened for writing", fifo, O_WRONLY);

    alarm(2);
    DIR *d = opendir(fifo);
    int e = errno;
    alarm(0);
    char detail[100];
    snprintf(detail, sizeof(detail), "%s (%s)", d ? "a DIR" : "NULL", d ? "ok" : strerror(e));
    check("opendir of a FIFO", d == NULL && e == ENOTDIR, detail);
    if (d)
        closedir(d);

    expect_enotdir("a regular file", file, O_RDONLY);
    expect_enotdir("/dev/null", "/dev/null", O_RDONLY);

    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", fd, fd < 0 ? strerror(e) : "ok");
    check("a directory opens", fd >= 0, detail);
    if (fd >= 0)
        close(fd);

    unlink(fifo);
    unlink(file);
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
