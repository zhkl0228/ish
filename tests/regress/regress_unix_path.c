// Regression test for unix sockets bound to a path on the root filesystem,
// and for fchmodat2.
//
// On a realfs mount (run.sh's rootfs, a folder mounted in the app) bind()
// failed with EPERM: iSH makes the socket's name with mknod, and realfs had
// no socket files. It makes them with bind() now, from the socket's own
// directory, since the host path may be longer than sun_path. The path here
// is long enough for that.
//
// fchmodat2 (Linux 6.6, syscall 452) was missing; musl and apk try it first.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_unix_path regress_unix_path.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452
#endif

static int passed, failed;

static void check(const char *name, int ok, const char *detail) {
    if (ok) {
        passed++;
        printf("PASS %s\n", name);
    } else {
        failed++;
        printf("FAIL %s: %s\n", name, detail);
    }
}

int main(void) {
    char detail[300];
    // room for it and /listening.sock in sun_path's 108 bytes
    char dir[90];
    snprintf(dir, sizeof(dir), "/tmp/regress_unix_path_with_a_directory_name_long_enough.%d", getpid());
    mkdir(dir, 0755);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/listening.sock", dir);

    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    int bound = bind(l, (struct sockaddr *) &addr, sizeof(addr));
    snprintf(detail, sizeof(detail), "bind(%s): %s", addr.sun_path, bound == 0 ? "ok" : strerror(errno));
    check("bind to a path", bound == 0, detail);
    listen(l, 4);

    struct stat st;
    int got = stat(addr.sun_path, &st);
    snprintf(detail, sizeof(detail), "stat: %s, mode %o", got == 0 ? "ok" : strerror(errno), got == 0 ? st.st_mode : 0);
    check("the name stats as a socket", got == 0 && S_ISSOCK(st.st_mode), detail);

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    int connected = connect(c, (struct sockaddr *) &addr, sizeof(addr));
    int a = accept(l, NULL, NULL);
    char buf[8] = "";
    write(c, "ping", 4);
    ssize_t n = a >= 0 ? read(a, buf, sizeof(buf) - 1) : -1;
    struct ucred cred = {0};
    socklen_t len = sizeof(cred);
    getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len);
    snprintf(detail, sizeof(detail), "connect %d accept %d read %zd \"%s\", peer pid %d (want %d)",
             connected, a, n, buf, cred.pid, getpid());
    check("connect, accept, data, the listener's credentials",
          connected == 0 && a >= 0 && n == 4 && strcmp(buf, "ping") == 0 && cred.pid == getpid(), detail);
    close(a);
    close(c);

    int again = socket(AF_UNIX, SOCK_STREAM, 0);
    int rebound = bind(again, (struct sockaddr *) &addr, sizeof(addr));
    int rebind_errno = errno;
    snprintf(detail, sizeof(detail), "bind again: %s", rebound == 0 ? "ok" : strerror(rebind_errno));
    check("a name in use: EADDRINUSE", rebound < 0 && rebind_errno == EADDRINUSE, detail);

    close(l);
    unlink(addr.sun_path);
    rebound = bind(again, (struct sockaddr *) &addr, sizeof(addr));
    snprintf(detail, sizeof(detail), "bind after unlink: %s", rebound == 0 ? "ok" : strerror(errno));
    check("unlinked, the name is free", rebound == 0, detail);
    close(again);
    unlink(addr.sun_path);

    // fchmodat2
    char file[256], link[256];
    snprintf(file, sizeof(file), "%s/file", dir);
    snprintf(link, sizeof(link), "%s/link", dir);
    close(open(file, O_CREAT | O_WRONLY, 0644));
    symlink(file, link);
    long r1 = syscall(SYS_fchmodat2, AT_FDCWD, file, 0600, 0);
    stat(file, &st);
    long r2 = syscall(SYS_fchmodat2, AT_FDCWD, link, 0600, AT_SYMLINK_NOFOLLOW);
    int e2 = errno;
    long r3 = syscall(SYS_fchmodat2, AT_FDCWD, file, 0600, 0x1);
    int e3 = errno;
    snprintf(detail, sizeof(detail), "chmod %ld -> mode %o; on a symlink without following %ld (%s); bad flags %ld (%s)",
             r1, st.st_mode & 07777, r2, strerror(e2), r3, strerror(e3));
    check("fchmodat2", r1 == 0 && (st.st_mode & 07777) == 0600 && r2 < 0 && e2 == EOPNOTSUPP && r3 < 0 && e3 == EINVAL,
          detail);
    unlink(link);
    unlink(file);
    rmdir(dir);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
