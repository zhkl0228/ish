// Regression test: address space reserved with MAP_NORESERVE, the JVM's way.
//
// HotSpot reserves with mmap(PROT_NONE, MAP_NORESERVE), commits parts with
// mmap(MAP_FIXED, RW), and uncommits them with mmap(MAP_FIXED | MAP_NORESERVE,
// PROT_NONE). iSH gave a PROT_NONE mapping under 256MB host address space of
// its whole size: no memory, but every guest process shares the app's one
// address space, which iOS caps, and with a few JVMs at once (Maven, the
// javadoc it forks, another build) one failed to start: "Failed to reserve
// memory for metaspace". Over 256MB the mapping was lazy, but there MAP_FIXED
// left the old pages in place, readable, their contents back at the next
// commit; and a hint iSH honoured it moved to an aligned address.
//
// Each case runs in a child, so one that kills the process fails alone. On a
// Linux host every case passes. On an iSH without the fix the uncommit and
// hint cases fail anywhere, and "a JVM's reservations, ten times" on a device,
// where the host address space runs out.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_noreserve regress_noreserve.c

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define MB (1024UL * 1024)

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

static sigjmp_buf back;

static void on_segv(int sig) {
    (void) sig;
    siglongjmp(back, 1);
}

static void catch_segv(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_segv;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
}

// Reads p; 1 if that faulted
static int faults(volatile char *p) {
    if (sigsetjmp(back, 1) == 0) {
        (void) *p;
        return 0;
    }
    return 1;
}

static char *reserve(void *hint, size_t size) {
    char *p = mmap(hint, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static int commit(char *p, size_t size) {
    return mmap(p, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == p;
}

static int uncommit(char *p, size_t size) {
    return mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == p;
}

// Reserve, commit, write, uncommit (all of the reservation, or a part), and
// the uncommitted pages fault, and come back zeroed at the next commit.
static int uncommit_case(size_t reserved, size_t uncommitted) {
    catch_segv();
    char *base = reserve(NULL, reserved);
    if (base == NULL)
        return 1;
    char *part = base + reserved - uncommitted;   // its last part, or all of it
    char *page = part + 3 * MB / 2;
    if (!commit(part, 2 * MB))
        return 2;
    memset(part, 0x5a, 2 * MB);
    if (!uncommit(part, uncommitted))
        return 3;
    if (!faults(page))
        return 4;
    if (!commit(part, 2 * MB))
        return 2;
    if (page[0] != 0 || part[0] != 0)
        return 5;
    munmap(base, reserved);
    return 0;
}

static int uncommit_whole_big(void) {
    return uncommit_case(512 * MB, 512 * MB);
}

static int uncommit_part_of_node(void) {
    return uncommit_case(64 * MB, 4 * MB);
}

// mprotect commits too (V8's way, and the JVM's for its stack guards)
static int mprotect_commit(void) {
    catch_segv();
    char *base = reserve(NULL, 64 * MB);
    if (base == NULL)
        return 1;
    char *mid = base + 30 * MB;
    if (mprotect(mid, 4 * MB, PROT_READ | PROT_WRITE) != 0)
        return 2;
    mid[0] = 1;
    mid[4 * MB - 1] = 2;
    if (mid[0] != 1 || mid[4 * MB - 1] != 2)
        return 3;
    if (!faults(mid + 4 * MB) || !faults(mid - 1))
        return 4;
    mprotect(mid, 4 * MB, PROT_NONE);
    if (!faults(mid))
        return 5;
    munmap(base, 64 * MB);
    return 0;
}

// A free address asked for is the address given, at any size
static int hint_case(size_t size) {
    char *area = reserve(NULL, 2 * size + 64 * MB);
    if (area == NULL)
        return 1;
    munmap(area, 2 * size + 64 * MB);
    char *want = area + 16 * MB;
    char *got = reserve(want, size);
    if (got == NULL)
        return 1;
    munmap(got, size);
    return got == want ? 0 : 2;
}

static int hint_big(void) {
    return hint_case(512 * MB);
}

static int hint_node(void) {
    return hint_case(64 * MB);
}

// Reserved one beside the other, and parts unmapped from the middle: each
// part still commits and faults as it should
static int neighbours(void) {
    catch_segv();
    char *base = reserve(NULL, 64 * MB);
    if (base == NULL)
        return 1;
    // three of them, one above the other, at fixed addresses
    if (!uncommit(base, 16 * MB) || !uncommit(base + 16 * MB, 16 * MB) ||
            !uncommit(base + 32 * MB, 32 * MB))
        return 2;
    munmap(base + 20 * MB, 8 * MB);
    if (!faults(base + 24 * MB))
        return 3;
    if (reserve(base + 20 * MB, 8 * MB) != base + 20 * MB)
        return 4;
    char *parts[] = {base, base + 18 * MB, base + 20 * MB, base + 40 * MB, base + 62 * MB};
    for (int i = 0; i < 5; i++) {
        if (!commit(parts[i], 2 * MB))
            return 5;
        parts[i][MB] = 7;
    }
    for (int i = 0; i < 5; i++)
        if (parts[i][MB] != 7 || parts[i][0] != 0)
            return 6;
    if (!faults(base + 30 * MB))
        return 3;
    munmap(base, 64 * MB);
    return 0;
}

// What one JVM reserves (its code cache, four metaspace nodes), ten times
// over: about 5GB, none of it memory
static int jvm_reservations(void) {
    char *got[50];
    int n = 0;
    for (int jvm = 0; jvm < 10; jvm++) {
        if ((got[n++] = reserve(NULL, 240 * MB)) == NULL)
            return 1;
        for (int node = 0; node < 4; node++)
            if ((got[n++] = reserve(NULL, 64 * MB)) == NULL)
                return 1;
    }
    // and each commits
    for (int i = 0; i < n; i++) {
        if (!commit(got[i], 64 * 1024))
            return 2;
        got[i][100] = 1;
    }
    for (int i = 0; i < n; i++)
        munmap(got[i], i % 5 == 0 ? 240 * MB : 64 * MB);
    return 0;
}

static void run(const char *name, int (*body)(void), const char *const *reasons) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(body());
    int status;
    waitpid(pid, &status, 0);
    char detail[128];
    if (WIFSIGNALED(status))
        snprintf(detail, sizeof(detail), "killed by signal %d", WTERMSIG(status));
    else
        snprintf(detail, sizeof(detail), "%s", WEXITSTATUS(status) < 7 ? reasons[WEXITSTATUS(status)] : "?");
    check(name, WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);
}

int main(void) {
    static const char *const uncommit_reasons[] = {"", "the reservation failed", "the commit failed",
                                                   "the uncommit failed or moved",
                                                   "an uncommitted page did not fault",
                                                   "an uncommitted page kept its contents"};
    static const char *const mprotect_reasons[] = {"", "the reservation failed", "mprotect failed",
                                                   "the committed pages did not keep what was written",
                                                   "a page beside them did not fault",
                                                   "PROT_NONE again did not fault"};
    static const char *const hint_reasons[] = {"", "the reservation failed", "it was put elsewhere"};
    static const char *const neighbour_reasons[] = {"", "the reservation failed",
                                                    "a MAP_FIXED reservation failed or moved",
                                                    "an unmapped or uncommitted page did not fault",
                                                    "the hole was not given back where asked",
                                                    "a commit failed", "a part lost what was written, or was not zero"};
    static const char *const jvm_reasons[] = {"", "a reservation failed", "a commit failed"};
    run("uncommitting a whole 512MB reservation drops its pages", uncommit_whole_big, uncommit_reasons);
    run("uncommitting 4MB of a 64MB reservation drops its pages", uncommit_part_of_node, uncommit_reasons);
    run("mprotect commits part of a 64MB reservation, and only that part", mprotect_commit, mprotect_reasons);
    run("a 512MB reservation at a free address is at that address", hint_big, hint_reasons);
    run("a 64MB reservation at a free address is at that address", hint_node, hint_reasons);
    run("reservations side by side, split and unmapped, each part sound", neighbours, neighbour_reasons);
    run("a JVM's reservations, ten times: 5GB of address space", jvm_reservations, jvm_reasons);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
