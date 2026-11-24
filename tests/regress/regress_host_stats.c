// Regression test for what /proc/ish/host says of the host mappings and the
// app's malloc (0d586712):
//
//   anonymous host mmaps since the app started: N, for X MB; of them lazy
//       commits C (H host pages, G guest pages), Y MB; copies on write K, a host
//       page each: V MB
//   MADV_DONTNEED zeroed in place since the app started: P pages, Z MB
//   translated blocks now: B, W MB
//
// Checked here:
//  - a 512MB read-write MAP_NORESERVE reservation (a lazy one, as JSC's heap)
//    touched every 16KB over 64MB: 4096 lazy commits, each an anonymous host
//    mmap, a host page or a guest page each, at least 16MB committed;
//  - MADV_DONTNEED over those 64MB zeroes in place what was committed, and the
//    bytes read zero; over a shared mapping it zeroes nothing, which keeps its
//    contents;
//  - an 8MB PROT_NONE mapping, a host one though it costs no memory: an
//    anonymous host mmap of 8MB;
//  - 256 pages a forked child writes: as many copies on write, each an
//    anonymous host mmap;
//  - 20000 functions of their own, called once each: 20000 blocks more, and
//    their bytes at least the 128 of each block's gadgets (16 unsigned longs,
//    FIBER_BLOCK_INITIAL_CAPACITY); unmapped, and in a child that exits or
//    execs, the blocks go again.
// On 0d586712 itself the bytes check fails: it counted a block's used, its
// gadgets' unsigned longs, as bytes (0.3 MB for 20000 blocks); and it counted
// no PROT_NONE mapping and no page copied on write.
// Natively there is no /proc/ish/host and nothing is checked.
//
// The counts are the app's, every process's: exact only with no other process
// in it, as when this is its first, pid 1 (tests/regress/run.sh, iSH's CLI).
// With others (a device with Claude Code running, whose JSC heap commits and
// gives back pages and makes and drops blocks all the time) the counts since
// the app started are checked as at least what this process did (the commits,
// the pages zeroed, the copies on write). What cannot be told then is said
// SKIP: that nothing was committed or zeroed, and anything of the blocks,
// which are counted as they are now and fall when others drop theirs.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_host_stats regress_host_stats.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define MB (1024UL * 1024)
#define TOUCHES 4096
#define FUNCS 20000

static int passed, failed, skipped;
static int alone; // the only process: the counts are this one's

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

// A check that holds only when the counts are this process's alone
static void check_alone(const char *name, int ok, const char *detail) {
    if (alone) {
        check(name, ok, detail);
        return;
    }
    skipped++;
    printf("SKIP %s: other processes run, and the counts are the app's\n", name);
    fflush(stdout);
}

static char detail[256];

struct stats {
    unsigned long long anon_mmaps, lazy, host_pages, guest_pages, cow, zeroed, blocks;
    double anon_mb, committed_mb, cow_mb, zeroed_mb, blocks_mb;
};

static char text[1 << 16];

static struct stats read_stats(void) {
    struct stats s;
    FILE *f = fopen("/proc/ish/host", "r");
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    int lines = 0;
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        lines += sscanf(line, "anonymous host mmaps since the app started: %llu, for %lf MB; of them lazy commits "
                        "%llu (%llu host pages, %llu guest pages), %lf MB; copies on write %llu, a host page each: "
                        "%lf MB",
                        &s.anon_mmaps, &s.anon_mb, &s.lazy, &s.host_pages, &s.guest_pages, &s.committed_mb, &s.cow,
                        &s.cow_mb) == 8;
        lines += sscanf(line, "MADV_DONTNEED zeroed in place since the app started: %llu pages, %lf MB", &s.zeroed,
                        &s.zeroed_mb) == 2;
        lines += sscanf(line, "translated blocks now: %llu, %lf MB", &s.blocks, &s.blocks_mb) == 2;
    }
    if (lines != 3) {
        printf("FAIL /proc/ish/host has %d of the 3 lines\n", lines);
        exit(1);
    }
    return s;
}

static void lazy_commits_and_dontneed(void) {
    struct stats s0 = read_stats();
    volatile char *p = mmap(NULL, 512 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL mmap of 512MB with MAP_NORESERVE\n");
        exit(1);
    }
    struct stats s1 = read_stats();
    snprintf(detail, sizeof(detail), "%llu", s1.lazy - s0.lazy);
    check_alone("a lazy reservation commits nothing when made", s1.lazy == s0.lazy, detail);

    for (int i = 0; i < TOUCHES; i++)
        p[(size_t) i * 16384] = 1;
    struct stats s2 = read_stats();
    unsigned long long lazy = s2.lazy - s1.lazy;
    unsigned long long host = s2.host_pages - s1.host_pages, guest = s2.guest_pages - s1.guest_pages;
    snprintf(detail, sizeof(detail), "%llu, expected %d", lazy, TOUCHES);
    check("a lazy commit for each page touched", alone ? lazy == TOUCHES : lazy >= TOUCHES, detail);
    snprintf(detail, sizeof(detail), "%llu host and %llu guest pages, %llu commits", host, guest, lazy);
    check("each a host page or a guest page, all alike",
          alone ? host + guest == lazy && (host == 0 || guest == 0) : host >= TOUCHES || guest >= TOUCHES, detail);
    snprintf(detail, sizeof(detail), "%llu mmaps, %llu commits", s2.anon_mmaps - s1.anon_mmaps, lazy);
    check("each an anonymous host mmap", s2.anon_mmaps - s1.anon_mmaps >= lazy, detail);
    double committed = s2.committed_mb - s1.committed_mb;
    snprintf(detail, sizeof(detail), "%.1f MB committed, %.1f MB in anonymous mmaps", committed,
             s2.anon_mb - s1.anon_mb);
    check("16MB committed at least (a guest page each), all in anonymous mmaps",
          committed > 15.85 && s2.anon_mb - s1.anon_mb > committed - 0.15, detail);

    madvise((void *) p, 64 * MB, MADV_DONTNEED);
    struct stats s3 = read_stats();
    unsigned long long zeroed = s3.zeroed - s2.zeroed;
    double zeroed_mb = zeroed * 4096.0 / MB;
    snprintf(detail, sizeof(detail), "%llu pages (%.1f MB, said %.1f MB), %.1f MB committed", zeroed, zeroed_mb,
             s3.zeroed_mb - s2.zeroed_mb, committed);
    // With others, what they committed is in committed too: at least the
    // pages this process touched
    check("MADV_DONTNEED zeroes in place what was committed",
          (alone ? zeroed_mb > committed - 0.15 && zeroed_mb < committed + 0.15 : zeroed >= TOUCHES) &&
          s3.zeroed_mb - s2.zeroed_mb > zeroed_mb - 0.15 && s3.zeroed_mb - s2.zeroed_mb < zeroed_mb + 0.15, detail);
    int nonzero = 0;
    for (int i = 0; i < TOUCHES; i++)
        nonzero += p[(size_t) i * 16384] != 0;
    snprintf(detail, sizeof(detail), "%d of %d bytes not zero", nonzero, TOUCHES);
    check("the bytes MADV_DONTNEED zeroed read zero", nonzero == 0, detail);
    munmap((void *) p, 512 * MB);

    char *shared = mmap(NULL, MB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(shared, 1, MB);
    struct stats s4 = read_stats();
    madvise(shared, MB, MADV_DONTNEED);
    struct stats s5 = read_stats();
    snprintf(detail, sizeof(detail), "a byte %d", shared[4096]);
    check("MADV_DONTNEED of a shared mapping keeps its contents", shared[4096] == 1, detail);
    snprintf(detail, sizeof(detail), "%llu pages zeroed", s5.zeroed - s4.zeroed);
    check_alone("MADV_DONTNEED of a shared mapping zeroes nothing", s5.zeroed == s4.zeroed, detail);
    munmap(shared, MB);
}

static void prot_none_and_copies_on_write(void) {
    struct stats s0 = read_stats();
    // Under 128MB and without MAP_NORESERVE: not a lazy reservation, a host
    // mapping of its own
    void *none = mmap(NULL, 8 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (none == MAP_FAILED) {
        printf("FAIL mmap of 8MB PROT_NONE\n");
        exit(1);
    }
    struct stats s1 = read_stats();
    snprintf(detail, sizeof(detail), "%llu mmaps, %.1f MB", s1.anon_mmaps - s0.anon_mmaps, s1.anon_mb - s0.anon_mb);
    check("a PROT_NONE mapping is an anonymous host mmap",
          s1.anon_mmaps - s0.anon_mmaps >= 1 && s1.anon_mb - s0.anon_mb > 7.95, detail);
    munmap(none, 8 * MB);

    char *pages = mmap(NULL, 256 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        printf("FAIL mmap of 1MB\n");
        exit(1);
    }
    memset(pages, 1, 256 * 4096);
    struct stats s2 = read_stats();
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 256; i++)
            pages[i * 4096] = 2;
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    struct stats s3 = read_stats();
    // and a few more: the pages of the child's stack and data it writes, and
    // this process's since the fork
    unsigned long long cow = s3.cow - s2.cow;
    snprintf(detail, sizeof(detail), "%llu copies (%.1f MB), %llu anonymous host mmaps", cow, s3.cow_mb - s2.cow_mb,
             s3.anon_mmaps - s2.anon_mmaps);
    check("each page a child writes a copy on write, an anonymous host mmap",
          cow >= 256 && (!alone || cow < 256 + 200) && s3.cow_mb - s2.cow_mb > 0.95 &&
          s3.anon_mmaps - s2.anon_mmaps >= cow,
          detail);
    munmap(pages, 256 * 4096);
}

// FUNCS functions, add x0, x0, #(i % 4096); ret: a block each
static void *make_and_run_code(void) {
    uint32_t *code = mmap(NULL, FUNCS * 8, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        printf("FAIL mmap of the code\n");
        exit(1);
    }
    for (int i = 0; i < FUNCS; i++) {
        code[2 * i] = 0x91000000 | ((uint32_t) (i % 4096) << 10);
        code[2 * i + 1] = 0xd65f03c0;
    }
    __builtin___clear_cache((char *) code, (char *) (code + 2 * FUNCS));
    unsigned long sum = 0, want = 0;
    for (int i = 0; i < FUNCS; i++) {
        sum = ((unsigned long (*)(unsigned long)) (code + 2 * i))(sum);
        want += i % 4096;
    }
    if (sum != want) {
        printf("FAIL the functions summed %lu, expected %lu\n", sum, want);
        exit(1);
    }
    return code;
}

static void translated_blocks(void) {
    struct stats s0 = read_stats();
    void *code = make_and_run_code();
    struct stats s1 = read_stats();
    unsigned long long blocks = s1.blocks - s0.blocks;
    snprintf(detail, sizeof(detail), "%llu more", blocks);
    check_alone("a block for each function", blocks >= FUNCS && blocks < FUNCS + 1000, detail);
    // the MB are said to one decimal
    double per_block = (s1.blocks_mb - s0.blocks_mb + 0.1) * MB / blocks;
    snprintf(detail, sizeof(detail), "%.1f MB for %llu blocks, %.0f bytes a block", s1.blocks_mb - s0.blocks_mb,
             blocks, per_block);
    check_alone("a block's bytes at least its gadgets' 128", per_block >= 128, detail);

    munmap(code, FUNCS * 8);
    struct stats s2 = read_stats();
    snprintf(detail, sizeof(detail), "%llu blocks, %.1f MB; %llu, %.1f MB before the functions", s2.blocks,
             s2.blocks_mb, s0.blocks, s0.blocks_mb);
    check_alone("unmapped, their blocks go", s2.blocks < s0.blocks + 200 && s2.blocks_mb < s0.blocks_mb + 0.15,
                detail);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        // 1 where it failed itself (said FAIL), 2 where it saw too few blocks
        make_and_run_code();
        _exit(read_stats().blocks >= s2.blocks + FUNCS ? 0 : 2);
    }
    int status;
    waitpid(pid, &status, 0);
    struct stats s3 = read_stats();
    snprintf(detail, sizeof(detail), "child status %#x; %llu blocks, %.1f MB after it, %llu, %.1f MB before",
             status, s3.blocks, s3.blocks_mb, s2.blocks, s2.blocks_mb);
    check("a child runs them and exits",
          WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 2), detail);
    check_alone("a child's blocks are counted while it runs", WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);
    check_alone("a child's blocks go when it exits", s3.blocks < s2.blocks + 200 && s3.blocks_mb < s2.blocks_mb + 0.15,
                detail);

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        make_and_run_code();
        execl("/bin/true", "true", (char *) NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    struct stats s4 = read_stats();
    snprintf(detail, sizeof(detail), "child status %#x; %llu blocks, %.1f MB after it, %llu, %.1f MB before",
             status, s4.blocks, s4.blocks_mb, s3.blocks, s3.blocks_mb);
    check("a child that ran them execs", WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);
    check_alone("a child's blocks go when it execs", s4.blocks < s3.blocks + 200 && s4.blocks_mb < s3.blocks_mb + 0.15,
                detail);
}

int main(void) {
    if (access("/proc/ish/host", R_OK) != 0) {
        printf("no /proc/ish/host (not iSH): nothing to check\n0 passed, 0 failed\n");
        return 0;
    }
    alone = getpid() == 1;
    if (!alone)
        printf("pid %d, not the only process: exact counts not checked\n", getpid());
    lazy_commits_and_dontneed();
    prot_none_and_copies_on_write();
    translated_blocks();
    if (skipped != 0)
        printf("%d skipped: other processes run\n", skipped);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
