// Regression test for access(X_OK) as root: a regular file no one may
// execute is EACCES, as Linux's CAP_DAC_OVERRIDE (generic_permission) says,
// and as exec itself says of it.
//
// iSH let root pass every access check, so access(X_OK) of a 0644 file was 0
// while exec of it was EACCES. The protobuf Gradle plugin makes the protoc it
// downloads executable only if File.canExecute() is false, so it ran a 0644
// protoc and failed: "Exec failed, error: 13 (Permission denied)".
//
// Checked, as root: a 0644 file is EACCES to access(X_OK), to faccessat with
// AT_EACCESS and to exec; a 0744 file and a 0001 one are executable; a 0000
// file may be read and written; a 0000 directory may be searched. Natively
// passes as root.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_access_exec regress_access_exec.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

// r is the call's result, errno its error; want 0 for success, else the errno
static void expect(const char *name, int r, int want) {
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, want == 0 ? r == 0 : r == -1 && e == want, detail);
}

static void make_file(const char *path, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("FAIL create %s: %s\n", path, strerror(errno));
        exit(1);
    }
    const char script[] = "#!/bin/sh\nexit 99\n";
    if (write(fd, script, sizeof(script) - 1) != sizeof(script) - 1) {
        printf("FAIL write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    close(fd);
    if (chmod(path, mode) < 0) {
        printf("FAIL chmod %s: %s\n", path, strerror(errno));
        exit(1);
    }
}

int main(void) {
    char detail[100];
    snprintf(detail, sizeof(detail), "euid %d", geteuid());
    check("run as root", geteuid() == 0, detail);

    char dir[] = "/tmp/regress_access_exec.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        printf("FAIL mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    char noexec[64], ownerexec[64], otherexec[64], none[64], nonedir[64];
    snprintf(noexec, sizeof(noexec), "%s/noexec", dir);
    snprintf(ownerexec, sizeof(ownerexec), "%s/ownerexec", dir);
    snprintf(otherexec, sizeof(otherexec), "%s/otherexec", dir);
    snprintf(none, sizeof(none), "%s/none", dir);
    snprintf(nonedir, sizeof(nonedir), "%s/nonedir", dir);
    make_file(noexec, 0644);
    make_file(ownerexec, 0744);
    make_file(otherexec, 0001);
    make_file(none, 0000);
    if (mkdir(nonedir, 0000) < 0) {
        printf("FAIL mkdir %s: %s\n", nonedir, strerror(errno));
        return 1;
    }

    expect("access(X_OK) of a 0644 file", access(noexec, X_OK), EACCES);
    expect("faccessat(X_OK, AT_EACCESS) of a 0644 file",
           faccessat(AT_FDCWD, noexec, X_OK, AT_EACCESS), EACCES);

    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {noexec, NULL};
        execv(noexec, argv);
        _exit(errno);
    }
    int status;
    waitpid(pid, &status, 0);
    snprintf(detail, sizeof(detail), "status %#x", status);
    check("exec of a 0644 file is EACCES", WIFEXITED(status) && WEXITSTATUS(status) == EACCES, detail);

    expect("access(X_OK) of a 0744 file", access(ownerexec, X_OK), 0);
    expect("access(X_OK) of a 0001 file", access(otherexec, X_OK), 0);
    expect("access(R_OK | W_OK) of a 0000 file", access(none, R_OK | W_OK), 0);
    expect("access(X_OK) of a 0000 directory", access(nonedir, X_OK), 0);

    unlink(noexec);
    unlink(ownerexec);
    unlink(otherexec);
    unlink(none);
    rmdir(nonedir);
    rmdir(dir);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
