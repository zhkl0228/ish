// Regression test: reading memory the guest made unreachable (PROT_NONE).
//
// iSH gave a read of a PROT_NONE page its host memory. Mapped PROT_NONE, that
// memory faulted on the host, and the run loop took the fault through its
// crash path: the TLB flushed and the access tried again 16 times before the
// SIGSEGV came, with the wrong si_addr for a page first reached through a TLB
// miss. HotSpot reads such pages by design (implicit null checks, safepoint
// polls): javac spent nine tenths of the host's time in that path, and took a
// null check, its address wrong, for a crash. Made PROT_NONE by mprotect, the
// page kept readable host memory, and the read went through. A system call
// given such a pointer faulted in iSH's own code.
//
// Each case runs in a child, so one that kills the process fails alone.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_prot_none regress_prot_none.c

#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
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
static void *volatile fault_addr;

static void on_segv(int sig, siginfo_t *info, void *context) {
    (void) sig;
    (void) context;
    fault_addr = info->si_addr;
    siglongjmp(back, 1);
}

static void catch_segv(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
}

// Reads p; 1 and the fault's address if that faulted
static int faults(volatile char *p) {
    if (sigsetjmp(back, 1) == 0) {
        (void) *p;
        return 0;
    }
    return 1;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// The case's verdict as its exit status: 0 passed, else what failed
static int mapped_none(void) {
    catch_segv();
    char *page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    double t0 = now();
    for (int i = 0; i < 2000; i++) {
        fault_addr = NULL;
        if (!faults(page + 12))
            return 1;
        if (fault_addr != page + 12)
            return 2;
    }
    // 2000 faults: well under a second natively, minutes through the crash path
    return now() - t0 < 20 ? 0 : 3;
}

static int protected_after_read(void) {
    catch_segv();
    char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    page[100] = 7;
    if (faults(page + 100))
        return 1;
    mprotect(page, 4096, PROT_NONE);
    fault_addr = NULL;
    if (!faults(page + 100))
        return 2;
    if (fault_addr != page + 100)
        return 3;
    mprotect(page, 4096, PROT_READ);
    return page[100] == 7 ? 0 : 4;
}

static int syscall_pointer(void) {
    char *page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int p[2];
    pipe(p);
    errno = 0;
    if (write(p[1], page, 1) != -1 || errno != EFAULT)
        return 1;
    write(p[1], "x", 1);
    errno = 0;
    if (read(p[0], page, 1) != -1 || errno != EFAULT)
        return 2;
    return 0;
}

static void run(const char *name, int (*body)(void), const char *const *reasons) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(body());
    int status;
    waitpid(pid, &status, 0);
    char detail[128];
    if (WIFSIGNALED(status))
        snprintf(detail, sizeof(detail), "killed by signal %d", WTERMSIG(status));
    else
        snprintf(detail, sizeof(detail), "%s", WEXITSTATUS(status) < 5 ? reasons[WEXITSTATUS(status)] : "?");
    check(name, WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);
}

int main(void) {
    static const char *const mapped_reasons[] = {"", "the read went through", "si_addr is not the address read",
                                                 "2000 faults took 20 s or more"};
    static const char *const protected_reasons[] = {"", "the page did not read before", "the read went through",
                                                    "si_addr is not the address read", "the data changed"};
    static const char *const syscall_reasons[] = {"", "write from it did not fail with EFAULT",
                                                  "read into it did not fail with EFAULT"};
    run("a read of a page mapped PROT_NONE faults at its address, quickly", mapped_none, mapped_reasons);
    run("a read of a page made PROT_NONE after it was read faults", protected_after_read, protected_reasons);
    run("a system call given a PROT_NONE pointer fails with EFAULT", syscall_pointer, syscall_reasons);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
