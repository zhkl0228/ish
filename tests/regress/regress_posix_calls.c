// Regression test for calls CPython's test_posix found iSH wrong in, each as
// Linux has it:
//
// - fexecve, which musl makes execveat(fd, "", AT_EMPTY_PATH): execveat was
//   execve, its fd taken for the path's address (EFAULT);
// - sched_get_priority_min/max of SCHED_FIFO and SCHED_RR, 1 and 99: EINVAL
//   (and min was max);
// - pidfd_open of a pid <= 0, EINVAL: ESRCH;
// - futimens, utimensat with no path: it was "." from the fd, a regular
//   file's only by its path;
// - chown of a file that is not, with -1 -1: success (nothing looked up);
// - lchown of a symlink: on realfs the host followed it (a dangling one's
//   ENOENT), and a group set there set the owner;
// - linkat with AT_SYMLINK_FOLLOW (os.link's default): the flags were never
//   read, and it linked the symlink;
// - sched_setaffinity: said success and forgot the mask, which
//   sched_getaffinity, nproc and the like read.
//
// Natively passes (as root, for the chown checks).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_posix_calls regress_posix_calls.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
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

static const char *result(int r, int e) {
    static char buf[80];
    snprintf(buf, sizeof(buf), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    return buf;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0)
        return 42;

    char dir[64], file[96], symlink_path[96], dangling[96], link_path[96];
    snprintf(dir, sizeof(dir), "/tmp/regress_posix_calls.%d", getpid());
    snprintf(file, sizeof(file), "%s/file", dir);
    snprintf(symlink_path, sizeof(symlink_path), "%s/symlink", dir);
    snprintf(dangling, sizeof(dangling), "%s/dangling", dir);
    snprintf(link_path, sizeof(link_path), "%s/link", dir);
    mkdir(dir, 0700);
    close(open(file, O_CREAT | O_WRONLY, 0600));
    symlink(file, symlink_path);
    symlink("/nonexistent/target", dangling);
    char detail[200];

    // fexecve: this program again, by an fd to it
    int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    pid_t child = fork();
    if (child == 0) {
        char *args[] = {argv[0], "exec-child", NULL};
        char *env[] = {NULL};
        fexecve(self, args, env);
        _exit(errno);
    }
    int status;
    waitpid(child, &status, 0);
    close(self);
    snprintf(detail, sizeof(detail), "exit %d (42 is the exec'd program's; else the errno)", WEXITSTATUS(status));
    check("fexecve of an fd to a program", WIFEXITED(status) && WEXITSTATUS(status) == 42, detail);

    int lo = sched_get_priority_min(SCHED_RR), hi = sched_get_priority_max(SCHED_RR);
    int other = sched_get_priority_max(SCHED_OTHER);
    snprintf(detail, sizeof(detail), "SCHED_RR %d to %d, SCHED_OTHER max %d", lo, hi, other);
    check("sched_get_priority_min/max", lo == 1 && hi == 99 && other == 0, detail);

    int r = syscall(SYS_pidfd_open, -1, 0);
    int e = errno;
    check("pidfd_open of pid -1", r == -1 && e == EINVAL, result(r, e));

    int fd = open(file, O_RDWR);
    struct timespec times[2] = {{1000000, 0}, {2000000, 0}};
    r = futimens(fd, times);
    e = errno;
    struct stat st;
    fstat(fd, &st);
    close(fd);
    snprintf(detail, sizeof(detail), "%s, mtime %lld", result(r, e), (long long) st.st_mtime);
    check("futimens", r == 0 && st.st_mtime == 2000000, detail);

    r = chown("/nonexistent/file", -1, -1);
    e = errno;
    check("chown of a file that is not, with -1 -1", r == -1 && e == ENOENT, result(r, e));

    r = lchown(dangling, 1234, 5678);
    e = errno;
    struct stat lst;
    lstat(dangling, &lst);
    snprintf(detail, sizeof(detail), "%s, owner %u group %u", result(r, e), lst.st_uid, lst.st_gid);
    check("lchown of a dangling symlink", r == 0 && lst.st_uid == 1234 && lst.st_gid == 5678, detail);

    chown(file, 0, 0);
    r = chown(file, -1, 4321);
    e = errno;
    stat(file, &st);
    snprintf(detail, sizeof(detail), "%s, owner %u group %u", result(r, e), st.st_uid, st.st_gid);
    check("chown of the group only leaves the owner", r == 0 && st.st_uid == 0 && st.st_gid == 4321, detail);

    fd = open(file, O_RDWR);
    r = fchown(fd, 2147483648u, 2147483648u);
    e = errno;
    fstat(fd, &st);
    close(fd);
    snprintf(detail, sizeof(detail), "%s, owner %u group %u", result(r, e), st.st_uid, st.st_gid);
    check("fchown to uid 2^31", r == 0 && st.st_uid == 2147483648u && st.st_gid == 2147483648u, detail);

    r = linkat(AT_FDCWD, symlink_path, AT_FDCWD, link_path, AT_SYMLINK_FOLLOW);
    e = errno;
    lstat(link_path, &lst);
    stat(file, &st);
    snprintf(detail, sizeof(detail), "%s, the link's mode %o inode %llu, the file's inode %llu", result(r, e),
             lst.st_mode, (unsigned long long) lst.st_ino, (unsigned long long) st.st_ino);
    check("linkat with AT_SYMLINK_FOLLOW links the target", r == 0 && S_ISREG(lst.st_mode) && lst.st_ino == st.st_ino,
          detail);
    unlink(link_path);
    r = link(symlink_path, link_path);
    e = errno;
    lstat(link_path, &lst);
    snprintf(detail, sizeof(detail), "%s, the link's mode %o", result(r, e), lst.st_mode);
    check("link links the symlink itself", r == 0 && S_ISLNK(lst.st_mode), detail);
    unlink(link_path);

    cpu_set_t all, one, got;
    sched_getaffinity(0, sizeof(all), &all);
    CPU_ZERO(&one);
    CPU_SET(0, &one);
    r = sched_setaffinity(0, sizeof(one), &one);
    e = errno;
    sched_getaffinity(0, sizeof(got), &got);
    snprintf(detail, sizeof(detail), "%s, then %d CPUs (of %d)", result(r, e), CPU_COUNT(&got), CPU_COUNT(&all));
    check("sched_setaffinity to CPU 0, as getaffinity says", r == 0 && CPU_COUNT(&got) == 1 && CPU_ISSET(0, &got),
          detail);
    child = fork();
    if (child == 0) {
        cpu_set_t its;
        sched_getaffinity(0, sizeof(its), &its);
        _exit(CPU_COUNT(&its));
    }
    waitpid(child, &status, 0);
    snprintf(detail, sizeof(detail), "the child's %d CPUs", WEXITSTATUS(status));
    check("a child has its parent's CPUs", WEXITSTATUS(status) == 1, detail);
    CPU_ZERO(&one);
    r = sched_setaffinity(0, sizeof(one), &one);
    e = errno;
    check("sched_setaffinity to no CPU", r == -1 && e == EINVAL, result(r, e));
    sched_setaffinity(0, sizeof(all), &all);

    unlink(file);
    unlink(symlink_path);
    unlink(dangling);
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
