// Regression test for dup2, dup3 and F_DUPFD given an fd they may not take.
//
// iSH's dup2 to -1 took it for an fd that needs no larger table and wrote
// the file at files[-1], over malloc's header of the table: iSH's heap was
// broken and it crashed freeing it at exit (CPython's test_os,
// test_dup2_negative_fd; it said EPERM, the -1 returned). F_DUPFD from a
// negative fd failed an assert and iSH aborted, and F_DUPFD_CLOEXEC set its
// error as a bit before the close-on-exec bitmap.
//
// Checked: dup2 to -1, INT_MIN and RLIMIT_NOFILE is EBADF; dup3 to itself
// and with a flag but O_CLOEXEC EINVAL; dup2 to itself is the fd; dup2 onto
// a close-on-exec fd clears it, dup3 with O_CLOEXEC sets it; F_DUPFD from
// -1 and from RLIMIT_NOFILE is EINVAL; F_DUPFD_CLOEXEC sets it. Then it
// exits, the table freed. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_dup_bad_fd regress_dup_bad_fd.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
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

// A file's fd. Not /dev/null's: a realfs root has no device files, and the
// CI's, an Alpine minirootfs fresh each run, no /dev/null at all, where open
// said -1 and every check of that fd EBADF
static int a_file(void) {
    char path[] = "/tmp/regress_dup_bad_fd.XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    return fd;
}

static void expect(const char *name, int r, int want) {
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, r == -1 && e == want, detail);
}

int main(void) {
    char detail[120];
    struct rlimit nofile;
    getrlimit(RLIMIT_NOFILE, &nofile);
    int fd = a_file();

    expect("dup2 to -1", dup2(fd, -1), EBADF);
    expect("dup2 to INT_MIN", dup2(fd, INT_MIN), EBADF);
    expect("dup2 to RLIMIT_NOFILE", dup2(fd, (int) nofile.rlim_cur), EBADF);
    expect("dup3 to itself", dup3(fd, fd, 0), EINVAL);
    expect("dup3 with a flag but O_CLOEXEC", dup3(fd, fd + 10, O_NONBLOCK), EINVAL);

    int r = dup2(fd, fd);
    snprintf(detail, sizeof(detail), "%d, the fd %d", r, fd);
    check("dup2 to itself is the fd", r == fd, detail);

    int target = a_file();
    fcntl(target, F_SETFD, FD_CLOEXEC);
    r = dup2(fd, target);
    int flags = fcntl(target, F_GETFD);
    snprintf(detail, sizeof(detail), "%d, then FD_CLOEXEC %d", r, flags & FD_CLOEXEC);
    check("dup2 onto a close-on-exec fd clears it", r == target && !(flags & FD_CLOEXEC), detail);
    r = dup3(fd, target, O_CLOEXEC);
    flags = fcntl(target, F_GETFD);
    snprintf(detail, sizeof(detail), "%d, then FD_CLOEXEC %d", r, flags & FD_CLOEXEC);
    check("dup3 with O_CLOEXEC sets it", r == target && (flags & FD_CLOEXEC), detail);

    expect("F_DUPFD from -1", fcntl(fd, F_DUPFD, -1), EINVAL);
    expect("F_DUPFD from RLIMIT_NOFILE", fcntl(fd, F_DUPFD, (int) nofile.rlim_cur), EINVAL);
    r = fcntl(fd, F_DUPFD_CLOEXEC, 20);
    flags = r >= 0 ? fcntl(r, F_GETFD) : -1;
    snprintf(detail, sizeof(detail), "%d, then FD_CLOEXEC %d", r, flags >= 0 ? flags & FD_CLOEXEC : -1);
    check("F_DUPFD_CLOEXEC from 20", r >= 20 && (flags & FD_CLOEXEC), detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
