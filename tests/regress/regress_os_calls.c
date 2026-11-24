// Regression test for calls CPython's test_os found iSH wrong in, each as
// Linux has it:
//
// - eventfd with EFD_SEMAPHORE: EINVAL. A read takes 1 and says 1.
// - memfd_create: ENOSYS. A file of memory: written, read back, sized by
//   ftruncate, fstat's size, mapped shared, /proc/self/fd's "/memfd:name
//   (deleted)", F_GET_SEALS F_SEAL_SEAL and F_ADD_SEALS EPERM (no sealing
//   here), MFD_ALLOW_SEALING and a name past 249 bytes EINVAL.
// - getpriority and setpriority: 20 (nice 0) of anything, and success
//   forgotten. A nice set is the nice got, a child's too, and /proc/self/stat
//   says it (its priority 20 more), as ps and top read it; another pid is
//   ESRCH, another which EINVAL.
// - splice: EINVAL of anything. A file to a pipe, at its position or at an
//   offset (which moves, the position not), a pipe to a file at an offset;
//   two files EINVAL, an offset of a pipe ESPIPE.
//
// Natively passes (but MFD_ALLOW_SEALING, which Linux takes: skipped there).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_os_calls regress_os_calls.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
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

static const char *result(long r, int e) {
    static char buf[80];
    snprintf(buf, sizeof(buf), "%ld (%s)", r, r < 0 ? strerror(e) : "ok");
    return buf;
}

int main(void) {
    char detail[200];

    int efd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    int e = errno;
    uint64_t v1 = 0, v2 = 0, v3 = 0;
    ssize_t r1 = -1, r2 = -1, r3 = -1;
    if (efd >= 0) {
        r1 = read(efd, &v1, 8);
        r2 = read(efd, &v2, 8);
        r3 = read(efd, &v3, 8);
    }
    snprintf(detail, sizeof(detail), "eventfd %s; reads %zd %zd %zd, values %llu %llu", result(efd, e), r1, r2, r3,
             (unsigned long long) v1, (unsigned long long) v2);
    check("EFD_SEMAPHORE: two reads of 1 from 2, then none", efd >= 0 && r1 == 8 && v1 == 1 && r2 == 8 && v2 == 1 &&
          r3 == -1, detail);

    int mfd = memfd_create("regress", MFD_CLOEXEC);
    e = errno;
    char back[8] = "";
    ssize_t w = -1, rd = -1;
    struct stat st = {0};
    if (mfd >= 0) {
        w = write(mfd, "memfd", 5);
        lseek(mfd, 0, SEEK_SET);
        rd = read(mfd, back, 5);
        ftruncate(mfd, 8192);
        fstat(mfd, &st);
    }
    snprintf(detail, sizeof(detail), "memfd_create %s; write %zd, read %zd '%.5s', size %lld, mode %o",
             result(mfd, e), w, rd, back, (long long) st.st_size, st.st_mode);
    check("memfd_create: written, read back, sized by ftruncate", mfd >= 0 && w == 5 && rd == 5 &&
          memcmp(back, "memfd", 5) == 0 && st.st_size == 8192 && S_ISREG(st.st_mode), detail);

    char *map = mfd >= 0 ? mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0) : MAP_FAILED;
    char through[8] = "";
    if (map != MAP_FAILED) {
        memcpy(map + 4096, "shared", 6);
        pread(mfd, through, 6, 4096);
        munmap(map, 8192);
    }
    snprintf(detail, sizeof(detail), "mmap %s, read through the fd '%.6s'", map == MAP_FAILED ? "failed" : "ok",
             through);
    check("memfd mapped shared", map != MAP_FAILED && memcmp(through, "shared", 6) == 0, detail);

    char link[64], target[128] = "";
    snprintf(link, sizeof(link), "/proc/self/fd/%d", mfd);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n > 0)
        target[n] = '\0';
    check("memfd in /proc/self/fd", strcmp(target, "/memfd:regress (deleted)") == 0, target);

    int seals = fcntl(mfd, F_GET_SEALS);
    int add = fcntl(mfd, F_ADD_SEALS, F_SEAL_WRITE);
    e = errno;
    snprintf(detail, sizeof(detail), "F_GET_SEALS %#x, F_ADD_SEALS %s", seals, result(add, e));
    check("memfd made without sealing: F_SEAL_SEAL, F_ADD_SEALS EPERM", seals == F_SEAL_SEAL && add == -1 &&
          e == EPERM, detail);
    if (mfd >= 0)
        close(mfd);

    char longname[260];
    memset(longname, 'n', 250);
    longname[250] = '\0';
    int r = memfd_create(longname, 0);
    e = errno;
    check("memfd_create of a name of 250 bytes", r == -1 && e == EINVAL, result(r, e));
    if (r >= 0)
        close(r);

    errno = 0;
    int base = getpriority(PRIO_PROCESS, 0);
    r = setpriority(PRIO_PROCESS, 0, base + 3);
    e = errno;
    errno = 0;
    int now = getpriority(PRIO_PROCESS, 0);
    snprintf(detail, sizeof(detail), "base %d, setpriority %s, now %d", base, result(r, e), now);
    check("setpriority then getpriority", r == 0 && now == base + 3, detail);
    // stat's fields 18 and 19, after the name's parentheses
    long stat_prio = -100, stat_nice = -100;
    FILE *statf = fopen("/proc/self/stat", "r");
    char statbuf[1024] = "";
    if (statf != NULL) {
        fread(statbuf, 1, sizeof(statbuf) - 1, statf);
        fclose(statf);
    }
    char *after = strrchr(statbuf, ')');
    if (after != NULL)
        sscanf(after + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %ld %ld", &stat_prio,
               &stat_nice);
    snprintf(detail, sizeof(detail), "priority %ld, nice %ld (the nice is %d)", stat_prio, stat_nice, now);
    check("/proc/self/stat says the nice, and the priority 20 more", stat_nice == now && stat_prio == 20 + now,
          detail);
    pid_t child = fork();
    if (child == 0)
        _exit(getpriority(PRIO_PROCESS, 0) == base + 3 ? 0 : 1);
    int status;
    waitpid(child, &status, 0);
    check("a child has its parent's nice", WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child's differs");
    errno = 0;
    r = getpriority(PRIO_PROCESS, 1000000);
    e = errno;
    check("getpriority of a pid that is not", r == -1 && e == ESRCH, result(r, e));
    errno = 0;
    r = setpriority(99, 0, 0);
    e = errno;
    check("setpriority of a which that is not", r == -1 && e == EINVAL, result(r, e));

    // splice: a file to a pipe, at its position and at an offset; a pipe to
    // a file at an offset; two files, and an offset of a pipe
    char path[64];
    snprintf(path, sizeof(path), "/tmp/regress_os_calls.%d", getpid());
    int file = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    write(file, "0123456789", 10);
    lseek(file, 2, SEEK_SET);
    int p[2];
    pipe(p);
    ssize_t s1 = splice(file, NULL, p[1], NULL, 3, 0);
    char got[16] = "";
    ssize_t g1 = read(p[0], got, sizeof(got));
    off_t pos = lseek(file, 0, SEEK_CUR);
    snprintf(detail, sizeof(detail), "spliced %zd, read %zd '%.*s', position %lld", s1, g1, (int) (g1 > 0 ? g1 : 0),
             got, (long long) pos);
    check("splice of a file to a pipe, at its position", s1 == 3 && g1 == 3 && memcmp(got, "234", 3) == 0 &&
          pos == 5, detail);
    loff_t in_off = 7;
    ssize_t s2 = splice(file, &in_off, p[1], NULL, 2, 0);
    ssize_t g2 = read(p[0], got, sizeof(got));
    pos = lseek(file, 0, SEEK_CUR);
    snprintf(detail, sizeof(detail), "spliced %zd, read %zd '%.*s', offset %lld, position %lld", s2, g2,
             (int) (g2 > 0 ? g2 : 0), got, (long long) in_off, (long long) pos);
    check("splice of a file at an offset: the offset moves, the position not", s2 == 2 && g2 == 2 &&
          memcmp(got, "78", 2) == 0 && in_off == 9 && pos == 5, detail);
    write(p[1], "ab", 2);
    loff_t out_off = 1;
    ssize_t s3 = splice(p[0], NULL, file, &out_off, 2, 0);
    char head[4] = "";
    pread(file, head, 4, 0);
    snprintf(detail, sizeof(detail), "spliced %zd, offset %lld, file starts '%.4s'", s3, (long long) out_off, head);
    check("splice of a pipe to a file at an offset", s3 == 2 && out_off == 3 && memcmp(head, "0ab3", 4) == 0, detail);
    int other = open(path, O_RDONLY);
    r = splice(other, NULL, file, NULL, 1, 0);
    e = errno;
    check("splice of two files", r == -1 && e == EINVAL, result(r, e));
    loff_t pipe_off = 0;
    r = splice(file, NULL, p[1], &pipe_off, 1, 0);
    e = errno;
    check("splice with an offset of a pipe", r == -1 && e == ESPIPE, result(r, e));
    close(other);
    close(file);
    close(p[0]);
    close(p[1]);
    unlink(path);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
