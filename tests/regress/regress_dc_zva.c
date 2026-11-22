// Regression test for DC ZVA and DCZID_EL0 in the arm64 guest.
//
// The guest reported DC ZVA as prohibited (DCZID_EL0.DZP = 1) and did not
// decode DC ZVA. HotSpot reads DCZID_EL0 itself, takes the block size to be 0
// when DZP is set, and then aborts generating its block zeroing stub
// ("guarantee(val < (1ULL << nbits)) failed: Field too big for insn"), so no
// JVM could start. The guest now reports 64-byte blocks, as real cores do, and
// implements the instruction.
//
// Checks that DCZID_EL0 allows ZVA with 64-byte blocks, and that DC ZVA on an
// unaligned address zeroes exactly the aligned 64-byte block containing it: at
// the start and the end of a page, on a page never touched before (the TLB
// write miss path), and that it faults on a read-only page. Running natively
// on hardware with 64-byte ZVA blocks passes, which pins the expectations to
// the real instruction.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_dc_zva regress_dc_zva.c
// Run:   ish -r <rootfs> /tmp/regress_dc_zva

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096
#define BLOCK 64

static int passed, failed;

static void check(int ok, const char *name) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", name);
    }
}

static void dc_zva(void *addr) {
    __asm__ volatile("dc zva, %0" : : "r"(addr) : "memory");
}

// Zeroes the block containing buf + offset and checks that exactly that block
// changed in the len bytes of buf.
static void check_zva(const char *name, uint8_t *buf, size_t len, size_t offset) {
    memset(buf, 0xaa, len);
    dc_zva(buf + offset);
    size_t start = offset & ~(size_t) (BLOCK - 1);
    int ok = 1;
    for (size_t i = 0; i < len; i++) {
        uint8_t expected = i >= start && i < start + BLOCK ? 0 : 0xaa;
        if (buf[i] != expected) {
            printf("  %s: byte %zu is %#x, expected %#x\n", name, i, buf[i], expected);
            ok = 0;
            break;
        }
    }
    check(ok, name);
}

static sigjmp_buf fault_jmp;

static void on_segv(int sig) {
    (void) sig;
    siglongjmp(fault_jmp, 1);
}

int main(void) {
    uint64_t dczid;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    if (dczid != 4)
        printf("  DCZID_EL0 is %#llx, expected 0x4\n", (unsigned long long) dczid);
    check(dczid == 4, "dczid_el0 allows zva with 64-byte blocks");

    uint8_t *pages = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    check_zva("first block of a page", pages, 2 * PAGE, 13);
    check_zva("block-aligned address", pages, 2 * PAGE, 2 * BLOCK);
    check_zva("last byte of a page", pages, 2 * PAGE, PAGE - 1);
    check_zva("second page", pages, 2 * PAGE, PAGE + 100);

    // A page not touched yet: the zeroing store is its first access.
    uint8_t *fresh = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fresh == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    dc_zva(fresh + 200);
    int fresh_ok = 1;
    for (size_t i = 0; i < PAGE; i++)
        fresh_ok &= fresh[i] == 0;
    check(fresh_ok, "untouched page");

    // Zeroing is a store: it faults on a read-only page.
    uint8_t *ro = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    struct sigaction sa = {.sa_handler = on_segv};
    sigaction(SIGSEGV, &sa, NULL);
    int faulted = 0;
    if (sigsetjmp(fault_jmp, 1) == 0)
        dc_zva(ro + 64);
    else
        faulted = 1;
    check(faulted, "read-only page faults");

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
