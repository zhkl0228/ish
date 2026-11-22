// Regression test for the size of the main thread's stack mapping at exec.
//
// Linux maps 128 KB of stack below the arguments when it starts a program
// (setup_arg_pages' stack_expand). musl's pthread_getattr_np measures the main
// thread's stack by probing that mapping with mremap, one page at a time, until
// it stops answering ENOMEM. iSH mapped a single page, so musl reported a 4 KB
// stack to programs that size their stack checks by it.
//
// Probes the way musl does, from the page above the auxiliary vector, and
// requires at least 128 KB. Running natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_stack_extent regress_stack_extent.c
// Run:   ish -r <rootfs> /tmp/regress_stack_extent

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/mman.h>

#define PAGE 4096ul

extern char **environ;

int main(void) {
    // The auxiliary vector follows the environment pointers on the stack.
    char **env = environ;
    while (*env != NULL)
        env++;
    uintptr_t auxv = (uintptr_t) (env + 1);
    char *p = (char *) ((auxv + PAGE - 1) & ~(PAGE - 1));
    size_t size = PAGE;
    while (mremap(p - size - PAGE, PAGE, 2 * PAGE, 0) == MAP_FAILED && errno == ENOMEM)
        size += PAGE;
    int ok = size >= 128 * 1024;
    if (!ok)
        printf("FAIL main thread stack mapping is %zu KB, expected at least 128 KB\n", size / 1024);
    printf("%d passed, %d failed\n", ok, !ok);
    return !ok;
}
