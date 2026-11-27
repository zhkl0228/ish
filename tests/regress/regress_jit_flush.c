// Regression test for the flush of the JIT's code region (ish -Djit=true, run
// with ISH_JIT_REGION_MB=1; skips without a code region).
//
// A full code region is flushed: every block of every address space is
// invalidated, and once no thread can be running what was in it, the region
// is taken from its start for new translations. Three ways it gave code that
// was still in use to other code:
//   - two flushes at once: a translation begun before the claim finds the
//     region full till it ends, and asked for a flush again, which another
//     thread claimed while the first ran. Each took the region from its start
//     at its own end, the second as translations had gone on after the first.
//   - an address space going away (asbestos_free, at a process's exit or
//     exec) left the flush's list first, then undid its blocks' links one by
//     one, writing each link site's nop back into native code that a flush
//     ending meanwhile had given to other code.
//   - the loop blocks' ranges (sync_pinned's) were kept across a flush.
// Native code then ran other code's instructions: nops in place of the add
// of a TLB lookup, branches into the middle of other blocks. Processes died
// of SIGSEGV with "host fault in native code, not at a guest access" (the AOT
// images' npm install for 2.2.1 (817)), computed wrong results or hung, and
// iSH itself crashed in the run loop with a block pointer read from code.
//
// A region of more than one arena (ISH_JIT_REGION_MB=4 ISH_JIT_ARENA_MB=1)
// has its oldest arena evicted instead, as the one in use nears its end: the
// blocks with native code there invalidated, its translations taken from the
// registry, the direct links into it of the others undone.
//
// Checked: 8 workers each start an awk that sums, and a shell pipeline (ls
// -R, sort, wc), 40 times over; every sum and every count is right, no
// process is killed by a signal, and the region was flushed, or an arena
// evicted, meanwhile. On a Mac, an iSH without the fixes failed every run.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_flush regress_jit_flush.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORKERS 8
#define ROUNDS 40

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

// The times the region's code was given to other code so far: its flushes
// and its arenas evicted (/proc/ish/jit's "code region: ..., arenas evicted
// N (...), flushed N times"); -1 without a code region
static long flushes(void) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL)
        return -1;
    char line[1024];
    long n = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *flushed = strstr(line, ", flushed ");
        char *evicted = strstr(line, ", arenas evicted ");
        if (strncmp(line, "code region: ", 13) == 0 && flushed != NULL && evicted != NULL)
            n = strtol(flushed + strlen(", flushed "), NULL, 10) +
                strtol(evicted + strlen(", arenas evicted "), NULL, 10);
    }
    fclose(f);
    return n;
}

// Runs argv, its output into out; its wait status
static int run(char *const argv[], char *out, size_t size) {
    int fds[2];
    pipe(fds);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    size_t n = 0;
    ssize_t got;
    while ((got = read(fds[0], out + n, size - 1 - n)) > 0)
        n += (size_t) got;
    out[n] = '\0';
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    return status;
}

struct tally {
    int sums_wrong, counts_wrong, killed;
};

static void worker(int id, int report) {
    char *awk[] = {"/bin/busybox", "awk", "BEGIN { s = 0; for (i = 0; i < 20000; i++) s += i % 7; print s }", NULL};
    char *pipeline[] = {"/bin/sh", "-c", "ls -R /lib /etc | sort | wc -l", NULL};
    struct tally t = {0};
    char out[256];
    for (int round = 0; round < ROUNDS; round++) {
        int status = run(awk, out, sizeof(out));
        if (WIFSIGNALED(status))
            t.killed++;
        if (strcmp(out, "59997\n") != 0) {
            if (t.sums_wrong++ == 0)
                printf("worker %d round %d: awk said \"%.40s\" (status %#x)\n", id, round, out, status);
        }
        status = run(pipeline, out, sizeof(out));
        if (WIFSIGNALED(status))
            t.killed++;
        if (atoi(out) <= 10) {
            if (t.counts_wrong++ == 0)
                printf("worker %d round %d: the pipeline said \"%.40s\" (status %#x)\n", id, round, out, status);
        }
        fflush(stdout);
    }
    write(report, &t, sizeof(t));
}

int main(void) {
    char detail[256];
    long before = flushes();
    if (before < 0) {
        printf("SKIP no code region (an ish without -Djit=true, or the JIT off)\n");
        return 0;
    }

    int fds[2];
    pipe(fds);
    pid_t pids[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            close(fds[0]);
            worker(i, fds[1]);
            _exit(0);
        }
    }
    close(fds[1]);
    struct tally sum = {0}, t;
    int reports = 0;
    while (read(fds[0], &t, sizeof(t)) == (ssize_t) sizeof(t)) {
        sum.sums_wrong += t.sums_wrong;
        sum.counts_wrong += t.counts_wrong;
        sum.killed += t.killed;
        reports++;
    }
    close(fds[0]);
    int workers_killed = 0;
    for (int i = 0; i < WORKERS; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFSIGNALED(status))
            workers_killed++;
    }
    long after = flushes();

    int runs = WORKERS * ROUNDS;
    snprintf(detail, sizeof(detail), "%d of %d wrong (of %d workers reporting)", sum.sums_wrong, runs, reports);
    check("every awk's sum right", reports == WORKERS && sum.sums_wrong == 0, detail);
    snprintf(detail, sizeof(detail), "%d of %d wrong (of %d workers reporting)", sum.counts_wrong, runs, reports);
    check("every pipeline's count right", reports == WORKERS && sum.counts_wrong == 0, detail);
    snprintf(detail, sizeof(detail), "%d commands and %d workers", sum.killed, workers_killed);
    check("no process killed by a signal", sum.killed == 0 && workers_killed == 0, detail);
    snprintf(detail, sizeof(detail), "flushed or an arena evicted %ld times before, %ld after (ISH_JIT_REGION_MB=1?)",
             before, after);
    check("the code region flushed, or an arena evicted, meanwhile", after > before, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
