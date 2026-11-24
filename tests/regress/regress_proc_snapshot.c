// Regression test for reading a /proc file in parts while it changes.
//
// iSH made a proc file's content again for every read(), and a reader that
// took it in several, as musl's fread does, got the parts of different
// contents whenever it changed between them: regress_host_stats, reading
// /proc/ish/host while another process mapped and unmapped, saw a line twice
// and failed. Now, as with Linux's seq_file, the content is made when it is
// read from the start, and further reads go on in it.
//
// /proc/self/maps is read in two parts, and between them a mapping is made
// below every other one, so its line comes first, and the offset is asked for
// (lseek SEEK_CUR, as ftell does): the parts are the content of before, whole.
// Read from the start again, it has the new mapping first. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_proc_snapshot regress_proc_snapshot.c

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

#define BUF (1 << 16)

static char before[BUF], parts[BUF], again[BUF];

// The rest of fd from where it is, into buf at len; the length in all
static size_t read_rest(int fd, char *buf, size_t len) {
    ssize_t n;
    while ((n = read(fd, buf + len, BUF - 1 - len)) > 0)
        len += (size_t) n;
    buf[len] = '\0';
    return len;
}

int main(void) {
    int fd = open("/proc/self/maps", O_RDONLY);
    size_t before_len = read_rest(fd, before, 0);
    close(fd);
    unsigned long lowest = strtoul(before, NULL, 16);

    fd = open("/proc/self/maps", O_RDONLY);
    size_t len = (size_t) read(fd, parts, 64);
    unsigned long below = lowest - 0x100000;
    // Nothing is below the lowest mapping, and MAP_FIXED_NOREPLACE says it
    // need not replace anything there (iSH takes the flag for a hint, as
    // Linux did before 4.17, and the hint is had where nothing is)
    void *map = mmap((void *) below, 0x1000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (map != (void *) below) {
        printf("FAIL mmap at %#lx, below the lowest mapping %#lx: %p\n", below, lowest, map);
        return 1;
    }
    off_t at = lseek(fd, 0, SEEK_CUR);
    len = read_rest(fd, parts, len);

    char detail[256];
    snprintf(detail, sizeof(detail), "at %lld after the first part", (long long) at);
    check("the offset is where the first part ended", at == 64, detail);
    int same = len == before_len && strcmp(parts, before) == 0;
    snprintf(detail, sizeof(detail), "%zu bytes in two parts, %zu before", len, before_len);
    check("read in two parts, it is the content of before", same, detail);
    if (!same)
        printf("before:\n%s\nthe parts:\n%s\n", before, parts);

    lseek(fd, 0, SEEK_SET);
    read_rest(fd, again, 0);
    close(fd);
    snprintf(detail, sizeof(detail), "first line %.40s, the new mapping at %#lx", again, below);
    check("read from the start again, the new mapping comes first", strtoul(again, NULL, 16) == below, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
