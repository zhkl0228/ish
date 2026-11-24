// Regression test for mappings larger than the host's largest free range.
//
// pt_map_nothing gave a mapping one host mmap of its whole size. The app's
// address space is the host's, shared by every guest process and capped by
// iOS, and once it is fragmented its largest free range can be smaller than
// one mapping: with Claude Code running it was 212MB. V8 reserves its code
// range, 256MB, with PROT_NONE and MAP_NORESERVE and then makes all of it RWX
// with one mprotect, which iSH commits; that commit failed with ENOMEM and
// node died at its start ("Fatal process out of memory: Failed to reserve
// virtual memory for CodeRange"), and with it Kimi Code. A plain anonymous
// mapping of that size failed the same way, and so did a thread's stack:
// libuv sizes it by RLIMIT_STACK, 128MB in iSH, and musl reserves it PROT_NONE
// and makes it read-write with one mprotect. When a V8 platform worker's
// pthread_create failed so, node's main thread waited forever for that worker
// to start (WorkerThreadsTaskRunner counts the workers it asked for). A file's
// mapping, one host mmap too, failed the same way: claude.exe has a 230MB
// writable segment, and every exec of it (Claude Code, and the ugrep it runs
// as grep) died of SIGSEGV.
//
// Covers V8's code range (reserve, mprotect RWX, write and read its first and
// last pages), a 512MB read-write mapping, a thread with a 256MB stack
// (created, run, joined) and a 256MB private mapping of a file from 4KB into
// it: bytes on either side of the 16MB chunks' edges are the file's, and stay
// so after MADV_DONTNEED. It catches the bug only where the host has no free
// range of that size, as on an iPhone with Claude Code running (checked there:
// all four fail without the fix); elsewhere, the Linux CLI say, it passes
// either way.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_big_commit regress_big_commit.c
// Run:   ish -r <rootfs> /tmp/regress_big_commit

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MB (1024UL * 1024)

static int passed, failed;
static int token;   // what the thread gives back

static void check(const char *name, int ok, const char *detail) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s: %s\n", name, detail);
    }
}

// Write a pattern to the first and last pages, and read it back
static int touch_ends(uint8_t *p, size_t len) {
    p[0] = 0x5a;
    p[len - 1] = 0xa5;
    return p[0] == 0x5a && p[len - 1] == 0xa5 && p[4096] == 0;
}

// The file's marked bytes, the rest zeros: its first and last ones mapped,
// and either side of where the mapping's host chunks meet (at 16MB in the file)
static const size_t marks[] = {4096, 16 * MB - 1, 16 * MB, 100 * MB + 12345, 256 * MB + 4095};

static uint8_t mark_at(size_t offset) {
    return (uint8_t) ((offset * 2654435761u) >> 24) | 1;
}

// Whether every mark is the file's in a mapping of it from 4KB
static int marks_read(const uint8_t *map) {
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (map[marks[i] - 4096] != mark_at(marks[i]))
            return 0;
    return 1;
}

// Uses a page of its stack, and gives back its argument
static void *thread_main(void *arg) {
    volatile uint8_t page[4096];
    page[0] = page[sizeof(page) - 1] = 0x5a;
    return page[0] == 0x5a && page[sizeof(page) - 1] == 0x5a ? arg : NULL;
}

int main(void) {
    size_t range = 256 * MB;
    uint8_t *code = mmap(NULL, range, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    check("code range reserved", code != MAP_FAILED, strerror(errno));
    if (code != MAP_FAILED) {
        int r = mprotect(code, range, PROT_READ | PROT_WRITE | PROT_EXEC);
        check("code range made RWX at once", r == 0, strerror(errno));
        if (r == 0)
            check("code range written and read at both ends", touch_ends(code, range), "pattern differs");
        munmap(code, range);
    }

    size_t big = 512 * MB;
    uint8_t *rw = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("512MB read-write mapping", rw != MAP_FAILED, strerror(errno));
    if (rw != MAP_FAILED) {
        check("512MB mapping written and read at both ends", touch_ends(rw, big), "pattern differs");
        munmap(rw, big);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * MB);
    pthread_t thread;
    int err = pthread_create(&thread, &attr, thread_main, &token);
    check("thread with a 256MB stack created", err == 0, strerror(err));
    if (err == 0) {
        void *ret;
        pthread_join(thread, &ret);
        check("thread with a 256MB stack ran", ret == &token, "its stack page differs");
    }
    pthread_attr_destroy(&attr);

    const char *path = "/tmp/regress_big_commit.file";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check("file created", fd >= 0, strerror(errno));
    if (fd >= 0) {
        unlink(path);
        int written = ftruncate(fd, 256 * MB + 4096) == 0;
        for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
            uint8_t mark = mark_at(marks[i]);
            written = written && pwrite(fd, &mark, 1, marks[i]) == 1;
        }
        check("file written", written, strerror(errno));
        uint8_t *map = mmap(NULL, 256 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 4096);
        check("256MB private mapping of a file", map != MAP_FAILED, strerror(errno));
        if (map != MAP_FAILED) {
            check("file's bytes read through its mapping", marks_read(map), "a mark differs");
            uint8_t *page = map + ((100 * MB + 12345 - 4096) & ~(size_t) 4095);
            madvise(page, 4096, MADV_DONTNEED);
            check("file's bytes read after MADV_DONTNEED", marks_read(map), "a mark differs");
            munmap(map, 256 * MB);
        }
        close(fd);
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
