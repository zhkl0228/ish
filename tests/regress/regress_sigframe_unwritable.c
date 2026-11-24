// Regression test: a signal whose frame cannot be written.
//
// A handler runs on its stack, the alternate one with SA_ONSTACK. When the
// frame cannot be written there, Linux kills the process with SIGSEGV
// (force_sigsegv). iSH forced the SIGSEGV holding the lock it then took
// again: the thread hung for good, and so did whatever went to signal its
// process (timeout, killing a CPython test), holding pids_lock, and the
// app's main thread waiting for that, until iOS killed the app.
//
// Each case runs in a child: killed by SIGSEGV passes; one that does not
// return in 5 s fails.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sigframe_unwritable regress_sigframe_unwritable.c

#define _GNU_SOURCE
#include <signal.h>
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

static void handler(int sig) {
    (void) sig;
}

// The alternate signal stack, read-only: no frame can be written there
static void unwritable_altstack(void) {
    size_t size = 64 * 1024;
    void *stack = mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    stack_t ss = {.ss_sp = stack, .ss_size = size};
    sigaltstack(&ss, NULL);
}

static void on_altstack(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = SA_ONSTACK;
    sigaction(sig, &sa, NULL);
}

// SIGUSR1's handler on the stack
static int raise_usr1(void) {
    unwritable_altstack();
    on_altstack(SIGUSR1);
    raise(SIGUSR1);
    return 0;
}

// SIGSEGV's own handler on the stack, and a fault
static int fault(void) {
    unwritable_altstack();
    on_altstack(SIGSEGV);
    *(volatile int *) 8 = 1;
    return 0;
}

// The child's end: its exit status, 128 + its signal, -1 if it hung
static int in_child(int (*body)(void)) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(body());
    for (int waited = 0; waited < 500; waited++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    return -1;
}

static void run(const char *name, int (*body)(void)) {
    int end = in_child(body);
    char detail[64];
    if (end < 0)
        snprintf(detail, sizeof(detail), "hung for 5 s");
    else if (end >= 128)
        snprintf(detail, sizeof(detail), "killed by signal %d", end - 128);
    else
        snprintf(detail, sizeof(detail), "exit %d", end);
    check(name, end == 128 + SIGSEGV, detail);
}

int main(void) {
    run("a handler's frame not writable on its stack: SIGSEGV", raise_usr1);
    run("SIGSEGV's own handler's frame not writable: SIGSEGV", fault);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
