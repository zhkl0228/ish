// Regression test for LDPSW in the arm64 guest.
//
// gen.c decoded LDPSW (load pair of signed words) as LDP of two W registers,
// so both values were zero-extended instead of sign-extended: a negative int
// read with it came out as a large positive number. GCC uses LDPSW to load two
// int fields at once, and HotSpot's C2 compiler, which runs on it, crashed
// with a null dereference while compiling.
//
// Covers the signed-offset, pre-indexed and post-indexed forms with negative
// and positive words, and LDP of W registers, which zero-extends. Running
// natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_ldpsw regress_ldpsw.c
// Run:   ish -r <rootfs> /tmp/regress_ldpsw

#include <stdint.h>
#include <stdio.h>

static int passed, failed;

static void check(const char *name, uint64_t got, uint64_t expected) {
    if (got == expected) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s: %#llx, expected %#llx\n", name, (unsigned long long) got, (unsigned long long) expected);
    }
}

int main(void) {
    static int32_t words[4] = {-2, 0x7ffffffe, (int32_t) 0x80000000, 5};
    uint64_t a, b;

    __asm__ volatile("ldpsw %0, %1, [%2, #8]" : "=&r"(a), "=&r"(b) : "r"(words) : "memory");
    check("offset first", a, (uint64_t) (int64_t) INT32_MIN);
    check("offset second", b, 5);

    const int32_t *p = words;
    __asm__ volatile("ldpsw %0, %1, [%2], #8" : "=&r"(a), "=&r"(b), "+r"(p) : : "memory");
    check("post-indexed first", a, (uint64_t) -2);
    check("post-indexed second", b, 0x7ffffffe);
    check("post-indexed writeback", (uint64_t) (p - words), 2);

    p = words;
    __asm__ volatile("ldpsw %0, %1, [%2, #8]!" : "=&r"(a), "=&r"(b), "+r"(p) : : "memory");
    check("pre-indexed first", a, (uint64_t) (int64_t) INT32_MIN);
    check("pre-indexed second", b, 5);
    check("pre-indexed writeback", (uint64_t) (p - words), 2);

    // The X view of the W destinations: the upper halves must be zero.
    __asm__ volatile("mov %0, #-1\n" "mov %1, #-1\n" "ldp %w0, %w1, [%2]" : "=&r"(a), "=&r"(b) : "r"(words) : "memory");
    check("ldp w zero-extends first", a, 0xfffffffe);
    check("ldp w zero-extends second", b, 0x7ffffffe);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
