// Regression test for struct rusage as getrusage and wait4 write it: arm64's,
// whose counts are longs, written whole.
//
// iSH's had 4-byte counts (32-bit x86's): it wrote 88 bytes of the 144, the
// guest read each count of two of them, and the last ones of what its buffer
// had before (Python's resource.getrusage: ru_nvcsw 4020917316). And the
// calling thread's (RUSAGE_THREAD) had in its counts what iSH's stack had.
//
// Checked, of getrusage's RUSAGE_SELF, RUSAGE_THREAD and RUSAGE_CHILDREN and of
// wait4's, into a buffer filled with 0xaa first: its last count (ru_nivcsw)
// is written; the counts Linux does not keep (ru_ixrss, ru_idrss, ru_isrss,
// ru_nswap, ru_msgsnd, ru_msgrcv, ru_nsignals) are 0; none is below 0.
// Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_rusage regress_rusage.c

#define _GNU_SOURCE
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

static const long filled = (long) 0xaaaaaaaaaaaaaaaaUL;

static void check_rusage(const char *what, int r, const struct rusage *u) {
    char name[100], detail[300];
    long unkept[] = {u->ru_ixrss, u->ru_idrss, u->ru_isrss, u->ru_nswap, u->ru_msgsnd, u->ru_msgrcv,
                     u->ru_nsignals};
    long counts[] = {u->ru_maxrss, u->ru_ixrss, u->ru_idrss, u->ru_isrss, u->ru_minflt, u->ru_majflt,
                     u->ru_nswap, u->ru_inblock, u->ru_oublock, u->ru_msgsnd, u->ru_msgrcv,
                     u->ru_nsignals, u->ru_nvcsw, u->ru_nivcsw};
    int zero = 1, nonnegative = 1;
    for (size_t i = 0; i < sizeof(unkept) / sizeof(unkept[0]); i++)
        zero &= unkept[i] == 0;
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
        nonnegative &= counts[i] >= 0;
    snprintf(detail, sizeof(detail),
             "%d; ixrss %ld idrss %ld isrss %ld nswap %ld msgsnd %ld msgrcv %ld nsignals %ld nvcsw %ld nivcsw %ld",
             r, u->ru_ixrss, u->ru_idrss, u->ru_isrss, u->ru_nswap, u->ru_msgsnd, u->ru_msgrcv, u->ru_nsignals,
             u->ru_nvcsw, u->ru_nivcsw);
    snprintf(name, sizeof(name), "%s: written whole", what);
    check(name, r >= 0 && u->ru_nivcsw != filled, detail);
    snprintf(name, sizeof(name), "%s: what Linux does not keep is 0, nothing below 0", what);
    check(name, zero && nonnegative, detail);
}

int main(void) {
    struct rusage u;
    int whos[] = {RUSAGE_SELF, RUSAGE_THREAD, RUSAGE_CHILDREN};
    const char *names[] = {"RUSAGE_SELF", "RUSAGE_THREAD", "RUSAGE_CHILDREN"};
    for (int i = 0; i < 3; i++) {
        memset(&u, 0xaa, sizeof(u));
        int r = getrusage(whos[i], &u);
        check_rusage(names[i], r, &u);
    }

    pid_t child = fork();
    if (child == 0)
        _exit(0);
    int status;
    memset(&u, 0xaa, sizeof(u));
    pid_t r = wait4(child, &status, 0, &u);
    check_rusage("wait4", r == child ? 0 : -1, &u);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
