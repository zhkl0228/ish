// Regression test for giving back the translated blocks of idle processes
// (/proc/ish/translations): what memory pressure frees, freed at once by
// "reclaim", and the processes run on right after.
//
// Every address space has blocks of its own, and a process waiting for work
// kept them all: a multiprocessing.Pool(200) of python took 24.8 MB a worker,
// 13.5 MB of it translated blocks, and iSH refused memory (the brake) where
// Linux shares the code.
//
// Checked: four forked children that have run some code and wait, idle for
// 3 seconds, give their blocks back (four address spaces at least, blocks
// fewer); each then runs the same code again and gets the same answer; and
// "reclaim" is the only word the file takes (EINVAL). Not a Linux case: an
// iSH file.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_reclaim_translations regress_reclaim_translations.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILDREN 4

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

// Some code of several blocks, its answer the same every time
static unsigned long work(unsigned long seed) {
    unsigned long a = seed, b = 1;
    for (int i = 0; i < 20000; i++) {
        switch (a % 5) {
        case 0: a = a * 31 + i; break;
        case 1: a ^= a >> 7; break;
        case 2: b += a & 0xff; break;
        case 3: a = a / 3 + b; break;
        default: b = b * 7 + 1; break;
        }
    }
    return a ^ b;
}

// The file's numbers: blocks now, and blocks and address spaces reclaimed
static void read_translations(unsigned long long *now, unsigned long long *reclaimed, unsigned long long *spaces) {
    char buf[512] = "";
    int fd = open("/proc/ish/translations", O_RDONLY);
    if (fd >= 0) {
        read(fd, buf, sizeof(buf) - 1);
        close(fd);
    }
    *now = *reclaimed = *spaces = 0;
    char *line = strstr(buf, "blocks now: ");
    if (line)
        *now = strtoull(line + strlen("blocks now: "), NULL, 10);
    line = strstr(buf, "reclaimed: ");
    if (line) {
        *reclaimed = strtoull(line + strlen("reclaimed: "), NULL, 10);
        char *of = strstr(line, " of ");
        if (of)
            *spaces = strtoull(of + strlen(" of "), NULL, 10);
    }
}

static int write_translations(const char *what) {
    int fd = open("/proc/ish/translations", O_WRONLY);
    if (fd < 0)
        return -1;
    int r = write(fd, what, strlen(what)) == (ssize_t) strlen(what) ? 0 : -1;
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

int main(void) {
    char detail[300];
    int ready[2], go[CHILDREN][2], answer[2];
    pipe(ready);
    pipe(answer);
    pid_t kids[CHILDREN];
    for (int i = 0; i < CHILDREN; i++) {
        pipe(go[i]);
        kids[i] = fork();
        if (kids[i] == 0) {
            unsigned long first = work(i + 1);
            write(ready[1], "r", 1);
            char c;
            read(go[i][0], &c, 1);
            unsigned long second = work(i + 1);
            char line[64];
            int n = snprintf(line, sizeof(line), "%d %d\n", i, first == second);
            write(answer[1], line, n);
            _exit(0);
        }
    }
    for (int i = 0; i < CHILDREN; i++) {
        char c;
        read(ready[0], &c, 1);
    }
    sleep(3);

    unsigned long long before, reclaimed0, spaces0, after, reclaimed1, spaces1;
    read_translations(&before, &reclaimed0, &spaces0);
    int r = write_translations("reclaim\n");
    int e = errno;
    read_translations(&after, &reclaimed1, &spaces1);
    snprintf(detail, sizeof(detail), "write %d (%s); blocks %llu -> %llu, reclaimed +%llu blocks of +%llu spaces", r,
             r < 0 ? strerror(e) : "ok", before, after, reclaimed1 - reclaimed0, spaces1 - spaces0);
    check("idle children give their blocks back", r == 0 && spaces1 - spaces0 >= CHILDREN &&
          reclaimed1 > reclaimed0 && after < before, detail);

    for (int i = 0; i < CHILDREN; i++)
        write(go[i][1], "g", 1);
    int same = 0;
    for (int i = 0; i < CHILDREN; i++) {
        int status;
        waitpid(kids[i], &status, 0);
    }
    char buf[256] = "";
    ssize_t n = read(answer[0], buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    for (char *p = buf; (p = strstr(p, " 1\n")) != NULL; p += 3)
        same++;
    snprintf(detail, sizeof(detail), "%d of %d the same: '%s'", same, CHILDREN, buf);
    check("each runs the same code again, to the same answer", same == CHILDREN, detail);

    r = write_translations("something else");
    e = errno;
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check("another word is EINVAL", r == -1 && e == EINVAL, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
