// Regression test: fcntl's record locks, as a 64-bit guest takes them.
//
// F_SETLKW did not wait for a lock another process held: it said EAGAIN,
// each of F_SETLK(W) and F_SETLK(W)64 having asked the other's command.
// utmps's utmpd, finding the lock taken (a boot's logins and gettys at
// once), took that for a failure, removed utmp and died: the first login
// after the app started went unrecorded, and w showed no one. And an arm64
// guest's struct flock, its offsets 64-bit and aligned, was read as i386's:
// a range locked another range, and F_GETLK's answer came back in the wrong
// places. F_GETLK64 and the others are a 32-bit kernel's: EINVAL.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_fcntl_lock regress_fcntl_lock.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int lock(int fd, int cmd, short type, off_t start, off_t len) {
    struct flock fl = {.l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len};
    return fcntl(fd, cmd, &fl);
}

// A child holding a write lock of [start, start + len) of path, for hold_ms
// after it says so on the pipe it returns (then it unlocks and exits)
static pid_t holder(const char *path, off_t start, off_t len, int hold_ms, int *ready) {
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        int fd = open(path, O_RDWR);
        if (lock(fd, F_SETLK, F_WRLCK, start, len) < 0)
            _exit(1);
        write(p[1], "x", 1);
        struct timespec ts = {hold_ms / 1000, hold_ms % 1000 * 1000000};
        nanosleep(&ts, NULL);
        lock(fd, F_SETLK, F_UNLCK, start, len);
        _exit(0);
    }
    close(p[1]);
    char c;
    *ready = read(p[0], &c, 1) == 1;
    close(p[0]);
    return pid;
}

int main(void) {
    char path[] = "/tmp/regress_fcntl_lock.XXXXXX";
    int fd = mkstemp(path);
    char detail[256];
    int ready;

    // 1. F_SETLKW waits for the lock another holds, and then has it
    pid_t pid = holder(path, 0, 0, 400, &ready);
    long t0 = now_ms();
    int r = lock(fd, F_SETLKW, F_WRLCK, 0, 0);
    int e = errno;
    long waited = now_ms() - t0;
    waitpid(pid, NULL, 0);
    snprintf(detail, sizeof(detail), "holder ready %d, F_SETLKW %d (%s) after %ld ms", ready, r, r < 0 ? strerror(e) : "ok", waited);
    check("F_SETLKW waits for another's lock", ready && r == 0 && waited >= 300, detail);
    lock(fd, F_SETLK, F_UNLCK, 0, 0);

    // 2.-4. a range: another range is free, the same is not, and F_GETLK
    // says whose it is
    pid = holder(path, 100, 50, 2000, &ready);
    int before = lock(fd, F_SETLK, F_WRLCK, 0, 100);
    int before_errno = errno;
    lock(fd, F_SETLK, F_UNLCK, 0, 100);
    int inside = lock(fd, F_SETLK, F_WRLCK, 120, 10);
    int inside_errno = errno;
    snprintf(detail, sizeof(detail), "holder ready %d; [0, 100): %d (%s); [120, 130): %d (%s)", ready, before,
             before < 0 ? strerror(before_errno) : "ok", inside, inside < 0 ? strerror(inside_errno) : "ok");
    check("a range is locked, the rest is not", ready && before == 0 && inside < 0 &&
          (inside_errno == EAGAIN || inside_errno == EACCES), detail);
    struct flock query = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
    r = fcntl(fd, F_GETLK, &query);
    snprintf(detail, sizeof(detail), "F_GETLK %d: type %d whence %d start %lld len %lld pid %d (holder %d)", r, query.l_type,
             query.l_whence, (long long) query.l_start, (long long) query.l_len, query.l_pid, pid);
    check("F_GETLK says whose lock it is", r == 0 && query.l_type == F_WRLCK && query.l_whence == SEEK_SET &&
          query.l_start == 100 && query.l_len == 50 && query.l_pid == pid, detail);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    // 5. a 32-bit kernel's commands
    errno = 0;
    r = fcntl(fd, 12, &query);
    e = errno;
    snprintf(detail, sizeof(detail), "fcntl(12): %d (%s)", r, strerror(e));
    check("F_GETLK64 is a 32-bit kernel's: EINVAL", r < 0 && e == EINVAL, detail);

    unlink(path);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
