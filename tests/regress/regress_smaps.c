// Regression test for /proc/self/smaps.
//
// iSH had /proc/pid/maps but no smaps. Redis on arm64 checks the kernel for a
// copy-on-write bug before it starts: it writes a page, MADV_FREEs it, writes
// it again, forks, and has the child read Shared_Dirty for that page from
// /proc/self/smaps. Without the file the check fails and redis-server exits
// with "Failed to test the kernel for a bug that could lead to data corruption
// during background save". iSH's MADV_FREE keeps the pages, so it does not
// have that bug.
//
// Follows redis' check (src/syscheck.c, checkLinuxMadvFreeForkBug), and also
// checks that the written page counts as Private_Dirty before the fork, and
// that a read-only anonymous neighbour has its own entry.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_smaps regress_smaps.c
// Run:   ish -r <rootfs> /tmp/regress_smaps

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed;

static void check(int ok, const char *name) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", name);
    }
}

// The value in kB of field (e.g. "Shared_Dirty:") for the smaps entry that
// contains addr, or -1 if there is no such entry or field.
static long smaps_field(unsigned long addr, const char *field) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (f == NULL)
        return -1;
    char line[256];
    int in_mapping = 0;
    long value = -1;
    size_t len = strlen(field);
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long from, to;
        if (sscanf(line, "%lx-%lx", &from, &to) == 2) {
            in_mapping = from <= addr && addr < to;
            continue;
        }
        if (in_mapping && strncmp(line, field, len) == 0) {
            value = strtol(line + len, NULL, 10);
            break;
        }
    }
    fclose(f);
    return value;
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    char *p = mmap(NULL, 3 * page, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    char *q = p + page;
    // RO|RW|RO keeps the middle page its own entry, as redis does.
    if (mprotect(q, page, PROT_READ | PROT_WRITE) < 0) {
        perror("mprotect");
        return 1;
    }
    *(volatile char *) q = 0;

    long kb = page / 1024;
    long size = smaps_field((unsigned long) q, "Size:");
    if (size != kb)
        printf("  Size of the RW page entry is %ld, expected %ld\n", size, kb);
    check(size == kb, "RW page has its own entry");
    long private_dirty = smaps_field((unsigned long) q, "Private_Dirty:");
    if (private_dirty != kb)
        printf("  Private_Dirty before fork is %ld, expected %ld\n", private_dirty, kb);
    check(private_dirty == kb, "written page is private dirty before fork");
    check(smaps_field((unsigned long) p, "Size:") == kb, "RO neighbour has its own entry");

    if (madvise(q, page, MADV_FREE) < 0) {
        perror("madvise");
        return 1;
    }
    *(volatile char *) q = 0;

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        long shared_dirty = smaps_field((unsigned long) q, "Shared_Dirty:");
        write(pipefd[1], &shared_dirty, sizeof(shared_dirty));
        _exit(0);
    }
    long shared_dirty = -2;
    read(pipefd[0], &shared_dirty, sizeof(shared_dirty));
    waitpid(pid, NULL, 0);
    if (shared_dirty != kb)
        printf("  Shared_Dirty in the child is %ld, expected %ld\n", shared_dirty, kb);
    check(shared_dirty == kb, "child sees the page shared dirty (redis check)");

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
