// Regression test for prlimit of a process by its pid.
//
// iSH said EINVAL of any pid but 0, the caller's own too, and so of a
// process that is not, where Linux says ESRCH (CPython's test_prlimit, and
// util-linux's prlimit --pid). A soft limit above the hard one was taken.
//
// Checked: prlimit of pid -1 is ESRCH; of the caller's pid it is its limit;
// of a child's it reads and sets the child's, not the caller's; a soft limit
// above the hard one is EINVAL. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_prlimit regress_prlimit.c

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
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
    char detail[160];
    struct rlimit got, mine;
    getrlimit(RLIMIT_NOFILE, &mine);

    int r = prlimit(-1, RLIMIT_NOFILE, NULL, &got);
    int e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("prlimit of pid -1", r == -1 && e == ESRCH, detail);

    r = prlimit(getpid(), RLIMIT_NOFILE, NULL, &got);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s), %llu of %llu", r, r < 0 ? strerror(e) : "ok",
             (unsigned long long) got.rlim_cur, (unsigned long long) mine.rlim_cur);
    check("prlimit of the caller's pid", r == 0 && got.rlim_cur == mine.rlim_cur && got.rlim_max == mine.rlim_max,
          detail);

    int ready[2];
    pipe(ready);
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]);
        write(ready[1], "x", 1);
        pause();
        _exit(0);
    }
    char c;
    read(ready[0], &c, 1);
    struct rlimit lower = {.rlim_cur = 64, .rlim_max = mine.rlim_max};
    r = prlimit(child, RLIMIT_NOFILE, &lower, NULL);
    e = errno;
    prlimit(child, RLIMIT_NOFILE, NULL, &got);
    struct rlimit after;
    getrlimit(RLIMIT_NOFILE, &after);
    snprintf(detail, sizeof(detail), "%d (%s), the child's soft limit %llu, the caller's %llu", r,
             r < 0 ? strerror(e) : "ok", (unsigned long long) got.rlim_cur, (unsigned long long) after.rlim_cur);
    check("prlimit sets a child's limit, not the caller's", r == 0 && got.rlim_cur == 64 &&
          after.rlim_cur == mine.rlim_cur, detail);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    struct rlimit inverted = {.rlim_cur = 100, .rlim_max = 50};
    r = prlimit(0, RLIMIT_NOFILE, &inverted, NULL);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("a soft limit above the hard one", r == -1 && e == EINVAL, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
