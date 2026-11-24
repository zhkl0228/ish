// Regression test for a forked child using its parent's translated blocks
// (mm_copy, mem_asbestos_share), and leaving them for its own when its code
// comes to differ.
//
// A forked child translated again all its parent had: a Pool worker of python
// took 13.5 MB of blocks. It uses its parent's now, and an address space whose
// code changes takes its own first: a JIT's page translated, its own code
// written (/proc/self/mem), a code page reprotected.
//
// Checked: a child running code its parent ran translates few blocks of its
// own, and the fork is counted as shared (/proc/ish/translations); parent and
// child writing different code to one anonymous executable page each run
// their own; a child that rewrites a function's code through /proc/self/mem,
// and one that makes the page writable and rewrites it, each get the new
// code, the parent the old. Of code mapped from a file after the fork, as
// each worker of a pool dlopens the same modules: a child that maps the same
// file where its parent did translates few blocks, and neither leaves; one
// that maps another file there runs its own; one that has nothing there gets
// SIGSEGV calling it, not its parent's code. The code checks pass natively;
// the counts are iSH's (Not a Linux case).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_fork_translations regress_fork_translations.c

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// A function alone on its page: mov w0, #42; ret
__asm__(".text\n.balign 4096\n.global patch_me\npatch_me:\n mov w0, #42\n ret\n.balign 4096\n");
extern int patch_me(void);

#define MOV_W0(imm) (0x52800000u | ((uint32_t) (imm) << 5))
#define RET 0xd65f03c0u
#define ADD_W0(imm) (0x11000000u | ((uint32_t) (imm) << 10))
#define B_NEXT 0x14000001u

// Code of many blocks, for a file: w0 = 0, then BLOCKS times w0 += step, each
// add its own block (b to the next instruction), and ret
#define BLOCKS 2000
#define CODE_SIZE (((BLOCKS * 2 + 2) * 4 + 4095) & ~4095)

static char *code_file(int step) {
    static char paths[2][64];
    char *path = paths[step - 1];
    snprintf(path, sizeof(paths[0]), "/tmp/regress_fork_translations.%d.XXXXXX", step);
    int fd = mkstemp(path);
    uint32_t *code = calloc(1, CODE_SIZE);
    int n = 0;
    code[n++] = MOV_W0(0);
    for (int i = 0; i < BLOCKS; i++) {
        code[n++] = ADD_W0(step);
        code[n++] = B_NEXT;
    }
    code[n++] = RET;
    write(fd, code, CODE_SIZE);
    close(fd);
    free(code);
    return path;
}

// The file's code mapped at at (reserved before the fork), and its answer
static int map_and_run(void *at, const char *path) {
    int fd = open(path, O_RDONLY);
    void *code = mmap(at, CODE_SIZE, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, 0);
    close(fd);
    if (code != at)
        return -1;
    return ((int (*)(void)) code)();
}

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

// Code of many blocks: 64 cases, each its own
#define CASE(n) case n: a = a * (n + 3) + (b >> (n % 13)); b ^= a + n; break;
#define CASES8(n) CASE(n) CASE(n + 1) CASE(n + 2) CASE(n + 3) CASE(n + 4) CASE(n + 5) CASE(n + 6) CASE(n + 7)
static unsigned long work(unsigned long seed) {
    unsigned long a = seed, b = 1;
    for (int i = 0; i < 20000; i++) {
        switch ((a ^ i) % 64) {
        CASES8(0) CASES8(8) CASES8(16) CASES8(24) CASES8(32) CASES8(40) CASES8(48) CASES8(56)
        }
    }
    return a ^ b;
}

// /proc/ish/translations: blocks now, forks that shared, and address spaces
// that left a shared one for a mapping, a block of their own, code written
struct counts {
    unsigned long long blocks, shared, mapping, own, written;
};

static struct counts translations(void) {
    char buf[1024] = "";
    int fd = open("/proc/ish/translations", O_RDONLY);
    if (fd >= 0) {
        read(fd, buf, sizeof(buf) - 1);
        close(fd);
    }
    struct counts c = {0};
    char *p = strstr(buf, "blocks now: ");
    c.blocks = p ? strtoull(p + 12, NULL, 10) : 0;
    p = strstr(buf, "shared with forks: ");
    if (p)
        sscanf(p, "shared with forks: %llu; left: %llu for a mapping, %llu for a block of their own, %llu",
               &c.shared, &c.mapping, &c.own, &c.written);
    return c;
}

// A child's answer, its exit status
static int child_answer(pid_t pid) {
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void) {
    char detail[300];

    // 1. the child runs what the parent ran, with the parent's blocks
    translations();
    struct counts c0 = translations();
    unsigned long want = work(5);
    struct counts c1 = translations();
    unsigned long long parent_made = c1.blocks - c0.blocks;
    int report[2];
    pipe(report);
    pid_t pid = fork();
    if (pid == 0) {
        struct counts b0 = translations();
        unsigned long got = work(5);
        struct counts b1 = translations();
        unsigned long long made = b1.blocks - b0.blocks;
        write(report[1], &made, sizeof(made));
        _exit(got == want ? 0 : 1);
    }
    unsigned long long child_made = 0;
    read(report[0], &child_made, sizeof(child_made));
    int ok = child_answer(pid) == 0;
    struct counts c2 = translations();
    snprintf(detail, sizeof(detail), "answer %s; the parent made %llu blocks, the child %llu; shared forks +%llu",
             ok ? "right" : "wrong", parent_made, child_made, c2.shared - c1.shared);
    check("a child runs its parent's code with its parent's blocks",
          ok && parent_made > 50 && child_made * 4 < parent_made && c2.shared > c1.shared, detail);

    // 2. the child rewrites a function through /proc/self/mem
    int before = patch_me();
    c1 = translations();
    pid = fork();
    if (pid == 0) {
        int fd = open("/proc/self/mem", O_RDWR);
        uint32_t insn = MOV_W0(7);
        pwrite(fd, &insn, sizeof(insn), (off_t) (uintptr_t) patch_me);
        close(fd);
        __builtin___clear_cache((char *) patch_me, (char *) patch_me + 8);
        _exit(patch_me());
    }
    int child_got = child_answer(pid);
    c2 = translations();
    // left for the write, or for the copy on write it took of the page
    unsigned long long left = c2.written - c1.written + c2.mapping - c1.mapping;
    snprintf(detail, sizeof(detail), "before %d; child %d, parent %d; left +%llu", before, child_got, patch_me(),
             left);
    check("code written through /proc/self/mem: the child's new, the parent's old",
          before == 42 && child_got == 7 && patch_me() == 42 && left > 0, detail);

    // 3. the child makes the page writable and rewrites it
    c1 = translations();
    pid = fork();
    if (pid == 0) {
        void *page = (void *) ((uintptr_t) patch_me & ~(uintptr_t) 4095);
        mprotect(page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
        *(volatile uint32_t *) (void *) patch_me = MOV_W0(9);
        __builtin___clear_cache((char *) patch_me, (char *) patch_me + 8);
        _exit(patch_me());
    }
    child_got = child_answer(pid);
    c2 = translations();
    snprintf(detail, sizeof(detail), "child %d, parent %d; left for a mapping +%llu", child_got, patch_me(),
             c2.mapping - c1.mapping);
    check("code rewritten after mprotect: the child's new, the parent's old",
          child_got == 9 && patch_me() == 42 && c2.mapping > c1.mapping, detail);

    // 4. code of a file mapped after the fork, the same where both map it
    char *same = code_file(1), *other = code_file(2);
    void *at = mmap(NULL, CODE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int go[2];
    pipe(go);
    c0 = translations();
    pid = fork();
    if (pid == 0) {
        char c;
        read(go[0], &c, 1);
        struct counts b0 = translations();
        int answer = map_and_run(at, same);
        struct counts b1 = translations();
        unsigned long long made = b1.blocks - b0.blocks;
        write(report[1], &made, sizeof(made));
        _exit(answer == BLOCKS ? 0 : 1);
    }
    c1 = translations();
    int got = map_and_run(at, same);
    c2 = translations();
    parent_made = c2.blocks - c1.blocks;
    write(go[1], "g", 1);
    read(report[0], &child_made, sizeof(child_made));
    ok = child_answer(pid) == 0;
    struct counts c3 = translations();
    snprintf(detail, sizeof(detail),
             "answers %d, %s; the parent made %llu blocks, the child %llu; shared +%llu, left +%llu +%llu +%llu", got,
             ok ? "right" : "wrong", parent_made, child_made, c3.shared - c0.shared, c3.mapping - c0.mapping,
             c3.own - c0.own, c3.written - c0.written);
    check("a file mapped after the fork where both map it: the child uses the parent's blocks",
          got == BLOCKS && ok && parent_made > 100 && child_made * 4 < parent_made && c3.shared > c0.shared &&
          c3.mapping == c0.mapping && c3.own == c0.own && c3.written == c0.written, detail);

    // 5. another file where the parent maps the first
    munmap(at, CODE_SIZE);
    at = mmap(NULL, CODE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    c0 = translations();
    pid = fork();
    if (pid == 0) {
        char c;
        read(go[0], &c, 1);
        _exit(map_and_run(at, other) == 2 * BLOCKS ? 0 : 1);
    }
    got = map_and_run(at, same);
    write(go[1], "g", 1);
    ok = child_answer(pid) == 0;
    c1 = translations();
    snprintf(detail, sizeof(detail), "parent %d, child's %s; left for a mapping +%llu", got, ok ? "right" : "wrong",
             c1.mapping - c0.mapping);
    check("another file where the parent mapped one: each runs its own, the child leaves",
          got == BLOCKS && ok && c1.mapping > c0.mapping, detail);

    // 6. nothing where the parent maps code: SIGSEGV
    munmap(at, CODE_SIZE);
    at = mmap(NULL, CODE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid = fork();
    if (pid == 0) {
        munmap(at, CODE_SIZE);
        char c;
        read(go[0], &c, 1);
        _exit(((int (*)(void)) at)() == BLOCKS ? 1 : 2);
    }
    got = map_and_run(at, same);
    write(go[1], "g", 1);
    int status;
    waitpid(pid, &status, 0);
    snprintf(detail, sizeof(detail), "parent %d; child %s %d", got, WIFSIGNALED(status) ? "signal" : "exit",
             WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    check("calling where only the parent mapped code: SIGSEGV", got == BLOCKS && WIFSIGNALED(status) &&
          WTERMSIG(status) == SIGSEGV, detail);

    // 7. one anonymous executable page, different code in parent and child
    // (last: the parent's blocks of it are its own, and it shares no more)
    uint32_t *jit = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    c1 = translations();
    pid = fork();
    int mine = pid == 0 ? 2 : 1;
    jit[0] = MOV_W0(mine);
    jit[1] = RET;
    __builtin___clear_cache((char *) jit, (char *) (jit + 2));
    got = ((int (*)(void)) jit)();
    if (pid == 0)
        _exit(got);
    child_got = child_answer(pid);
    c2 = translations();
    snprintf(detail, sizeof(detail), "parent %d, child %d; left for a block of their own +%llu", got, child_got,
             c2.own - c1.own);
    check("parent and child each run the code they wrote", got == 1 && child_got == 2 && c2.own > c1.own, detail);

    unlink(same);
    unlink(other);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
