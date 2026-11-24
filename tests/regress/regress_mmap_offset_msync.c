// Regression test for mmap of a file past 4GB, and what msync refuses.
//
// iSH cut mmap's offset to 32 bits: a file mapped at 5GB was mapped at 1GB
// (CPython's test_mmap test_large_offset read 0 where it wrote 32). And
// msync said success of anything, where Linux refuses an address not
// page-aligned (CPython's mmap.flush at an offset, test_flush_return_value).
//
// Checked: a sparse file's byte at 5GB, read through a mapping at that
// offset; msync of an address not page-aligned, of MS_ASYNC with MS_SYNC and
// of a flag there is not is EINVAL, of a page not mapped ENOMEM, and of a
// mapped page with MS_SYNC succeeds. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_mmap_offset_msync regress_mmap_offset_msync.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

static void expect(const char *name, int r, int want) {
    int e = errno;
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    check(name, want == 0 ? r == 0 : r == -1 && e == want, detail);
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    char path[64], detail[160];
    snprintf(path, sizeof(path), "/tmp/regress_mmap_offset_msync.%d", getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    off_t at = 5LL << 30;
    pwrite(fd, " ", 1, at);
    char *m = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, at);
    int e = errno;
    snprintf(detail, sizeof(detail), "%p (%s), byte %d", (void *) m, m == MAP_FAILED ? strerror(e) : "ok",
             m == MAP_FAILED ? -1 : m[0]);
    check("a file's byte at 5GB through a mapping at that offset", m != MAP_FAILED && m[0] == ' ', detail);
    if (m != MAP_FAILED)
        munmap(m, page);
    close(fd);
    unlink(path);

    char *two = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    expect("msync of an address not page-aligned", msync(two + 1, 6, MS_SYNC), EINVAL);
    expect("msync with MS_ASYNC and MS_SYNC", msync(two, page, MS_ASYNC | MS_SYNC), EINVAL);
    expect("msync with a flag there is not", msync(two, page, 0x100), EINVAL);
    expect("msync of a mapped page", msync(two, page, MS_SYNC), 0);
    munmap(two + page, page);
    expect("msync of a page not mapped", msync(two, 2 * page, MS_SYNC), ENOMEM);
    munmap(two, page);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
