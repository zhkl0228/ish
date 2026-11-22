// Regression test: the offset argument of preadv/pwritev(2) on a 64-bit guest.
//
// i386 splits the offset into two 32-bit registers (pos_l, pos_h); arm64
// passes the whole 64-bit offset in pos_l and the kernel ignores pos_h. iSH
// declared both as 32-bit and rebuilt the offset from the two halves, so on the
// arm64 guest an offset of -1 ("use the current position", as readv/writev)
// became 0xffffffff, and pwritev2(fd, ..., -1, ...) on a pipe failed with
// ESPIPE. Bun writes stdout/stderr that way: everything a Bun program (Claude
// Code included) printed into a pipe was lost, e.g. `claude -p ... | cat`
// showed nothing. Offsets of 4 GB and more were truncated too.
//
// Checks:
//   1. pwritev2(pipe, offset -1) writes; preadv2(pipe, offset -1) reads it back.
//   2. pwritev/preadv at an offset above 4 GB use the full offset.
// Running this natively on Linux passes, which pins the expectations.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_preadv2 regress_preadv2.c
// Run:   ish -r <rootfs> /tmp/regress_preadv2

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static int passed, failed;

static void check(int ok, const char *what) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

int main(void) {
    char what[160];

    int p[2];
    pipe(p);
    char msg[] = "Invalid API key\n";
    struct iovec wv = {msg, sizeof(msg) - 1};
    // Raw syscalls: the point is the register layout, not the libc wrapper.
    long w = syscall(SYS_pwritev2, p[1], &wv, 1, -1L, 0L, 0);
    snprintf(what, sizeof(what), "pwritev2(pipe, -1): returned %ld (%s), want %zu",
             w, w < 0 ? strerror(errno) : "ok", sizeof(msg) - 1);
    check(w == (long) sizeof(msg) - 1, what);
    char got[64] = {0};
    struct iovec rv = {got, sizeof(got)};
    long r = syscall(SYS_preadv2, p[0], &rv, 1, -1L, 0L, 0);
    snprintf(what, sizeof(what), "preadv2(pipe, -1): returned %ld (%s), want the message back",
             r, r < 0 ? strerror(errno) : "ok");
    check(r == (long) sizeof(msg) - 1 && memcmp(got, msg, r) == 0, what);

    const char *path = "/tmp/regress_preadv2.dat";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    long long off = (5LL << 30) + 16;  // 5 GB + 16, a sparse file
    char data[] = "beyond 4G";
    struct iovec fv = {data, sizeof(data)};
    w = syscall(SYS_pwritev, fd, &fv, 1, off, 0L);
    snprintf(what, sizeof(what), "pwritev(file, 5G+16): returned %ld (%s)", w, w < 0 ? strerror(errno) : "ok");
    check(w == (long) sizeof(data), what);
    struct stat st;
    fstat(fd, &st);
    snprintf(what, sizeof(what), "file size after pwritev at 5G+16: %lld, want %lld",
             (long long) st.st_size, off + (long long) sizeof(data));
    check(st.st_size == off + (long long) sizeof(data), what);
    char back[sizeof(data)] = {0};
    struct iovec bv = {back, sizeof(back)};
    r = syscall(SYS_preadv, fd, &bv, 1, off, 0L);
    snprintf(what, sizeof(what), "preadv(file, 5G+16): returned %ld, data %s", r,
             memcmp(back, data, sizeof(data)) == 0 ? "matches" : "differs");
    check(r == (long) sizeof(data) && memcmp(back, data, sizeof(data)) == 0, what);
    close(fd);
    unlink(path);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
