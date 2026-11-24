// Regression test for read-only file mappings shared between processes.
//
// Each process that mapped a file took host address space of its own for it,
// and the app's is a few GB in all: each JVM maps the JDK's 140MB lib/modules
// and libjvm's 19MB of code, each claude.exe 128MB, and a Gradle build (its
// daemon, a test JVM, kotlinter's workers) ran it out; every JVM started then
// died of SIGSEGV. A read-only mapping no process can write is shared now: a
// private one copy on write, a shared one only from a file opened read-only.
//
// A file 64MB and 1234 bytes long, its marks either side of where the 16MB
// host chunks would meet and its last byte:
//  - mapped shared, read-only, by this process and then by a child that opens
//    it itself: the child's is mapped again from this one's (iSH: what
//    /proc/ish/host counts grows by its size), and it reads the marks;
//  - mapped private, read-only, by both: the child makes its mapping writable
//    and writes a mark, which this process does not see, nor the file;
//  - mapped private, read-only, 1MB past its end (as musl maps a library:
//    code, data and BSS from the file at once), by this process and a child:
//    the child's is mapped again from this one's, the bytes after the end in
//    its last page are zeros;
//  - written (a mark changed), then mapped again: the new mark is read;
//  - unmapped by every process: iSH's table has none of it left.
// Natively it passes, /proc/ish/host's checks skipped.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_shared_map regress_shared_map.c

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static const size_t len = 64 * MB + 1234;
static const size_t marks[] = {0, 16 * MB - 1, 16 * MB, 48 * MB + 777, 64 * MB + 1233};

static uint8_t mark_at(size_t offset) {
    return (uint8_t) ((offset * 2654435761u) >> 24) | 1;
}

static int marks_read(const uint8_t *map) {
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (map[marks[i]] != mark_at(marks[i]))
            return 0;
    return 1;
}

// /proc/ish/host's "shared file mappings: N, X MB; mapped again from them R
// times, B MB": 1 with the numbers, 0 where there is no /proc/ish/host
// (natively), -1 where it has no such line (an iSH without sharing)
static int shared_stats(long *maps, double *reused_mb) {
    FILE *f = fopen("/proc/ish/host", "r");
    if (f == NULL)
        return 0;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        long reused;
        double mb;
        if (sscanf(line, "shared file mappings: %ld, %lf MB; mapped again from them %ld times, %lf MB", maps, &mb,
                   &reused, reused_mb) == 4)
            found = 1;
    }
    fclose(f);
    return found;
}

// Runs child in a child process, which reports through its exit status
static int in_child(int (*child)(const char *path), const char *path) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(child(path));
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status);
}

// 0: its own shared read-only mapping of the file reads the marks
static int child_maps_shared(const char *path) {
    int fd = open(path, O_RDONLY);
    uint8_t *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return 1;
    return marks_read(map) ? 0 : 2;
}

// 0: its own private read-only mapping past the file's end reads the marks,
// and zeros after the end in the file's last page (natively a page wholly past
// the end is SIGBUS: not read)
static int child_maps_past_end(const char *path) {
    int fd = open(path, O_RDONLY);
    uint8_t *map = mmap(NULL, len + MB, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
        return 1;
    return marks_read(map) && map[len] == 0 ? 0 : 2;
}

// Whether what /proc/ish/host counts as mapped again grew by a mapping of
// size bytes: it says MB to one decimal
static int grew_by(double before, double after, size_t size) {
    double mb = (double) size / MB;
    return after - before > mb - 0.1 && after - before < mb + 1;
}

// 0: its private mapping, made writable, takes a write the file does not
static int child_writes_private(const char *path) {
    int fd = open(path, O_RDONLY);
    uint8_t *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
        return 1;
    if (mprotect(map, len, PROT_READ | PROT_WRITE) != 0)
        return 2;
    map[16 * MB] = 0x42;
    uint8_t in_file;
    pread(fd, &in_file, 1, 16 * MB);
    return map[16 * MB] == 0x42 && in_file == mark_at(16 * MB) ? 0 : 3;
}

int main(void) {
    char path[] = "/tmp/regress_shared_map.XXXXXX";
    int fd = mkstemp(path);
    int written = fd >= 0 && ftruncate(fd, len) == 0;
    for (size_t i = 0; written && i < sizeof(marks) / sizeof(marks[0]); i++) {
        uint8_t mark = mark_at(marks[i]);
        written = pwrite(fd, &mark, 1, marks[i]) == 1;
    }
    check("file written", written, strerror(errno));
    close(fd);
    int ro = open(path, O_RDONLY);
    char detail[200];

    // Shared, read-only, here and in a child that opens the file itself
    long maps_before = 0, maps_after = 0;
    double reused_before = 0, reused_after = 0;
    int ish = shared_stats(&maps_before, &reused_before);
    uint8_t *shared = mmap(NULL, len, PROT_READ, MAP_SHARED, ro, 0);
    check("shared read-only mapping", shared != MAP_FAILED && marks_read(shared), strerror(errno));
    int child = in_child(child_maps_shared, path);
    snprintf(detail, sizeof(detail), "child said %d", child);
    check("a child's own shared read-only mapping reads the marks", child == 0, detail);
    if (ish != 0) {
        int found = shared_stats(&maps_after, &reused_after);
        snprintf(detail, sizeof(detail), "/proc/ish/host %s; mapped again %.1f MB, the mapping %.1f MB",
                 found < 0 ? "has no shared file mappings line" : "has the line", reused_after - reused_before,
                 (double) len / MB);
        check("the child's mapped again from this one (iSH)", found > 0 && grew_by(reused_before, reused_after, len),
              detail);
    }

    // Private, read-only, past the file's end, here and in a child
    shared_stats(&maps_after, &reused_before);
    uint8_t *past = mmap(NULL, len + MB, PROT_READ, MAP_PRIVATE, ro, 0);
    check("private read-only mapping past the file's end", past != MAP_FAILED && marks_read(past) && past[len] == 0,
          strerror(errno));
    child = in_child(child_maps_past_end, path);
    snprintf(detail, sizeof(detail), "child said %d", child);
    check("a child's own mapping past the end reads the marks", child == 0, detail);
    if (ish != 0) {
        int found = shared_stats(&maps_after, &reused_after);
        snprintf(detail, sizeof(detail), "mapped again %.1f MB, the mapping %.1f MB", reused_after - reused_before,
                 (double) (len + MB) / MB);
        check("the child's past the end mapped again from this one (iSH)",
              found > 0 && grew_by(reused_before, reused_after, len + MB), detail);
    }

    // Private, read-only, here and in a child that writes its own
    uint8_t *private = mmap(NULL, len, PROT_READ, MAP_PRIVATE, ro, 0);
    check("private read-only mapping", private != MAP_FAILED && marks_read(private), strerror(errno));
    child = in_child(child_writes_private, path);
    snprintf(detail, sizeof(detail), "child said %d", child);
    check("a child's private mapping made writable takes its write", child == 0, detail);
    check("this process does not see it", private != MAP_FAILED && private[16 * MB] == mark_at(16 * MB), "it does");

    // Written: mapped again, the new mark is read
    int rw = open(path, O_RDWR);
    uint8_t changed = 0x99;
    pwrite(rw, &changed, 1, 48 * MB + 777);
    close(rw);
    int ro2 = open(path, O_RDONLY);
    uint8_t *again = mmap(NULL, len, PROT_READ, MAP_PRIVATE, ro2, 0);
    check("mapped again after a write, the new byte is read",
          again != MAP_FAILED && again[48 * MB + 777] == 0x99, "the old one is");

    // Unmapped by every process: none of it left in the table
    munmap(shared, len);
    munmap(past, len + MB);
    munmap(private, len);
    munmap(again, len);
    close(ro);
    close(ro2);
    if (ish != 0) {
        long maps_end = 0;
        double reused_end = 0;
        int found = shared_stats(&maps_end, &reused_end);
        snprintf(detail, sizeof(detail), "%ld shared before, %ld now", maps_before, maps_end);
        check("unmapped everywhere, the table has none of it (iSH)", found > 0 && maps_end <= maps_before, detail);
    }
    unlink(path);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
