// Regression test for mremap's lengths and range, as Linux has them.
//
// iSH took the lengths in whole pages rounded down: a mapping shrunk to less
// than a page was taken for none and its page unmapped (CPython's
// mmap.resize(512), test_mmap's test_offset, read after it and crashed). And
// a range to grow with a page not mapped was read through a NULL entry:
// iSH crashed, not the caller.
//
// Checked: a file mapping at an offset shrunk to 512 bytes still reads (its
// page stays); an anonymous one of two pages shrunk to a page and a byte
// keeps both; a new length of 0 is EINVAL; a range with an unmapped page
// is EFAULT; an anonymous one grows in place where there is room, the new
// page zeros and writable; with no room it is ENOMEM, and with
// MREMAP_MAYMOVE it moves (iSH said ENOMEM: CPython's
// test_resize_up_private_anonymous_mapping), its bytes with it, the new
// pages zeros, the old page gone. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_mremap regress_mremap.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
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

static sigjmp_buf back;
static void on_fault(int sig) {
    (void) sig;
    siglongjmp(back, 1);
}

// whether the byte reads, and what it is
static int readable(volatile char *p, char *value) {
    if (sigsetjmp(back, 1) != 0)
        return 0;
    *value = *p;
    return 1;
}

int main(void) {
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    long page = sysconf(_SC_PAGESIZE);
    char detail[160];

    char path[64];
    snprintf(path, sizeof(path), "/tmp/regress_mremap.%d", getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    static char zeros[65536];
    write(fd, zeros, page);
    write(fd, "foo", 3);
    write(fd, zeros, page - 3);
    char *m = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
    ftruncate(fd, page + 512);
    char *small = mremap(m, page, 512, MREMAP_MAYMOVE);
    char c = 0;
    int ok = small != MAP_FAILED && readable(small, &c);
    snprintf(detail, sizeof(detail), "mremap %p -> %p, %s '%c'", (void *) m, (void *) small,
             ok ? "reads" : "does not read", c);
    check("a file mapping at an offset shrunk to 512 bytes", ok && c == 'f', detail);
    munmap(small, 512);
    close(fd);
    unlink(path);

    char *two = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    two[page] = 'x';
    char *kept = mremap(two, 2 * page, page + 1, 0);
    c = 0;
    ok = kept == two && readable(two + page, &c);
    snprintf(detail, sizeof(detail), "mremap %p -> %p, second page %s '%c'", (void *) two, (void *) kept,
             ok ? "reads" : "does not read", c);
    check("two anonymous pages shrunk to a page and a byte keep both", ok && c == 'x', detail);

    char *r = mremap(two, 2 * page, 0, 0);
    int e = errno;
    snprintf(detail, sizeof(detail), "%p (%s)", (void *) r, r == MAP_FAILED ? strerror(e) : "ok");
    check("a new length of 0", r == MAP_FAILED && e == EINVAL, detail);
    munmap(two, 2 * page);

    char *holed = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(holed + page, page);
    r = mremap(holed, 2 * page, 4 * page, 0);
    e = errno;
    snprintf(detail, sizeof(detail), "%p (%s)", (void *) r, r == MAP_FAILED ? strerror(e) : "ok");
    check("a range with a page not mapped", r == MAP_FAILED && e == EFAULT, detail);
    munmap(holed, 3 * page);

    char *room = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(room + page, 2 * page);
    room[0] = 'y';
    char *grown = mremap(room, page, 2 * page, 0);
    c = 1;
    ok = grown == room && readable(room + page, &c);
    if (ok)
        room[page] = 'z';
    snprintf(detail, sizeof(detail), "mremap %p -> %p, new page %s %d", (void *) room, (void *) grown,
             ok ? "reads" : "does not read", c);
    check("an anonymous mapping grows in place where there is room", ok && c == 0 && room[0] == 'y', detail);

    // no room where it is: the next page mapped
    char *blocked = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    blocked[0] = 'w';
    r = mremap(blocked, page, 4 * page, 0);
    e = errno;
    snprintf(detail, sizeof(detail), "%p (%s)", (void *) r, r == MAP_FAILED ? strerror(e) : "ok");
    check("no room and no MREMAP_MAYMOVE", r == MAP_FAILED && e == ENOMEM, detail);
    char *moved = mremap(blocked, page, 4 * page, MREMAP_MAYMOVE);
    e = errno;
    c = 1;
    char first = 0;
    ok = moved != MAP_FAILED && moved != blocked && readable(moved, &first) && readable(moved + 3 * page, &c);
    if (ok)
        moved[3 * page] = 'v';
    char old_c;
    int old_gone = !readable(blocked, &old_c);
    snprintf(detail, sizeof(detail), "%p -> %p (%s), first byte '%c', new page %d, old page %s", (void *) blocked,
             (void *) moved, moved == MAP_FAILED ? strerror(e) : "ok", first, c, old_gone ? "gone" : "still there");
    check("no room: MREMAP_MAYMOVE moves it, its bytes with it", ok && first == 'w' && c == 0 && old_gone, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
