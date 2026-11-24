// Regression test for the permissions /proc/<pid>/maps says of what exec and
// brk map: as Linux, a program's code r-x, its data and bss rw-, its stack and
// its heap rw-.
//
// exec mapped every segment of an ELF readable, and writable if it was, but
// never executable: maps said r--p of the code of the program and of its
// interpreter, where Linux says r-xp (a JVM, a debugger and an unwinder look
// for code there). The stack and brk's heap were mapped writable only, and
// maps said -w-p of them.
//
// Checked: the line of a function's address is r-x; of an initialized
// global's and of a zeroed one's, rw-; of a local's, rw- and [stack]; of
// what brk gave, rw- (by the system call: musl's sbrk takes 0 only). Natively
// passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_maps_perms regress_maps_perms.c

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
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

// The permissions and name of the line of maps that has addr, "?" if none
static void line_of(const void *addr, char perms[5], char *name, size_t name_size) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    strcpy(perms, "?");
    name[0] = '\0';
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long start, end;
        char p[5];
        int at = 0;
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &start, &end, p, &at) < 3)
            continue;
        if ((uintptr_t) addr < start || (uintptr_t) addr >= end)
            continue;
        memcpy(perms, p, 5);
        line[strcspn(line, "\n")] = '\0';
        snprintf(name, name_size, "%s", at ? line + at : "");
        break;
    }
    fclose(f);
}

static int initialized = 42;
static char zeroed[4096 * 4];

static int function(int x) {
    return x * 2;
}

int main(void) {
    char perms[5], name[256], detail[300];
    int local = function(initialized) + zeroed[0];

    line_of((const void *) (uintptr_t) function, perms, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%s %s", perms, name);
    check("a function's line is r-x", strncmp(perms, "r-x", 3) == 0, detail);

    line_of(&initialized, perms, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%s %s", perms, name);
    check("an initialized global's line is rw-", strncmp(perms, "rw-", 3) == 0, detail);

    line_of(&zeroed[sizeof(zeroed) - 1], perms, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%s %s", perms, name);
    check("a zeroed global's line (bss) is rw-", strncmp(perms, "rw-", 3) == 0, detail);

    line_of(&local, perms, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%s %s", perms, name);
    check("a local's line is rw- and [stack]", strncmp(perms, "rw-", 3) == 0 && strcmp(name, "[stack]") == 0,
          detail);

    char *heap = (char *) syscall(SYS_brk, 0);
    char *grown = (char *) syscall(SYS_brk, heap + 4096);
    if (grown == heap + 4096)
        heap[0] = 1;
    line_of(heap, perms, name, sizeof(name));
    snprintf(detail, sizeof(detail), "brk %p -> %p: %s %s", (void *) heap, (void *) grown, perms, name);
    check("what brk gave is rw-", grown == heap + 4096 && strncmp(perms, "rw-", 3) == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0 || local == 0;
}
