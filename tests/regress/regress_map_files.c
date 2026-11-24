// Regression test for /proc/<pid>/map_files: a symlink for each mapping of a
// file, named by its range, to the file.
//
// iSH had no map_files. Gradle's file-events library lists
// /proc/self/map_files to tell musl from glibc (a link to a file named
// "-musl-", ld-musl-aarch64.so.1, is musl), and without it took Alpine for
// glibc, loaded its glibc build, and failed: Gradle 9 ran only with
// org.gradle.native=false.
//
// Checked: every entry is named "%lx-%lx" and is a range /proc/self/maps
// lists with the file the entry links to; a file mapped here is listed with
// its range and links to it, and its entry is gone once it is unmapped; a
// range no mapping has, and the mapped file's range with a leading zero, are
// ENOENT. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_map_files regress_map_files.c

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

// Whether /proc/self/maps has a line of start-end with path
static int maps_has(unsigned long start, unsigned long end, const char *path) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        unsigned long s, e;
        int at = 0;
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &s, &e, &at) < 2 || at == 0)
            continue;
        line[strcspn(line, "\n")] = '\0';
        found = s == start && e == end && strcmp(line + at, path) == 0;
    }
    fclose(f);
    return found;
}

int main(void) {
    char detail[700];

    // every entry: a range maps has, with the file it links to
    DIR *d = opendir("/proc/self/map_files");
    int entries = 0, bad = 0;
    char first_bad[600] = "";
    struct dirent *de;
    while (d != NULL && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        entries++;
        unsigned long start, end;
        char spelled[64], link[300] = "", target[256];
        snprintf(link, sizeof(link), "/proc/self/map_files/%s", de->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        target[n > 0 ? n : 0] = '\0';
        int ok = sscanf(de->d_name, "%lx-%lx", &start, &end) == 2;
        snprintf(spelled, sizeof(spelled), "%lx-%lx", start, end);
        ok = ok && strcmp(spelled, de->d_name) == 0 && n > 0 && maps_has(start, end, target);
        if (!ok && bad++ == 0)
            snprintf(first_bad, sizeof(first_bad), "%s -> '%s'", de->d_name, target);
    }
    if (d != NULL)
        closedir(d);
    snprintf(detail, sizeof(detail), "%s, %d entries, %d not in maps (first: %s)", d ? "listed" : strerror(errno),
             entries, bad, first_bad);
    check("each entry is a range of maps, linked to its file", d != NULL && entries > 0 && bad == 0, detail);

    // a file mapped here
    char path[64];
    snprintf(path, sizeof(path), "/tmp/regress_map_files.%d", getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ftruncate(fd, 3 * 4096);
    char *map = mmap(NULL, 3 * 4096, PROT_READ, MAP_SHARED, fd, 0);
    char name[64], link[128], target[256] = "";
    snprintf(name, sizeof(name), "%lx-%lx", (unsigned long) map, (unsigned long) map + 3 * 4096);
    snprintf(link, sizeof(link), "/proc/self/map_files/%s", name);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    target[n > 0 ? n : 0] = '\0';
    int listed = 0;
    d = opendir("/proc/self/map_files");
    while (d != NULL && (de = readdir(d)) != NULL)
        listed |= strcmp(de->d_name, name) == 0;
    if (d != NULL)
        closedir(d);
    snprintf(detail, sizeof(detail), "%s %s, links to '%s'", name, listed ? "listed" : "not listed", target);
    check("a file mapped here: its range, linked to it", listed && strcmp(target, path) == 0, detail);

    char zero[100];
    snprintf(zero, sizeof(zero), "/proc/self/map_files/0%s", name);
    struct stat st;
    int r = lstat(zero, &st);
    int e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "found");
    check("its range with a leading zero is ENOENT", r == -1 && e == ENOENT, detail);

    r = lstat("/proc/self/map_files/1000-2000", &st);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "found");
    check("a range no mapping has is ENOENT", r == -1 && e == ENOENT, detail);

    munmap(map, 3 * 4096);
    r = lstat(link, &st);
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "found");
    check("unmapped, its entry is gone", r == -1 && e == ENOENT, detail);

    close(fd);
    unlink(path);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
