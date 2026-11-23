// Regression test for signals a task brings on itself (a fault, BRK, UDF).
//
// As Linux's force_sig_info_to_task: if the task blocks or ignores such a
// signal, it is unblocked and its action reset to the default, so the task
// dies of it; if it has a handler, the handler runs. iSH delivered them like
// any other signal, so a blocked or ignored SIGSEGV or SIGILL left the task
// faulting at the same instruction forever, and every BRK reset SIGTRAP to
// its default even when a handler was installed.
//
// Each case runs in a child with a 5 second alarm, so a task that faults
// forever dies of SIGALRM instead of hanging the test. Natively all pass.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_force_signal regress_force_signal.c

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static int passed, failed;

static void fault(void) {
    *(volatile int *) 8 = 1;
}
static void breakpoint(void) {
    __asm__ volatile("brk #0");
}
static void undefined(void) {
    __asm__ volatile("udf #0");
}

static void block(int sig) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

static void skip_instruction(int sig, siginfo_t *info, void *context) {
    (void) sig;
    (void) info;
    ((ucontext_t *) context)->uc_mcontext.pc += 4;
}

// Runs setup then trigger in a child and checks how the child ended: killed
// by want_signal, or (want_signal 0) exited with status 0.
static void check(const char *name, void (*setup)(int), int setup_sig,
        void (*trigger)(void), int want_signal) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);
        if (setup != NULL)
            setup(setup_sig);
        trigger();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    int ok = want_signal == 0
        ? WIFEXITED(status) && WEXITSTATUS(status) == 0
        : WIFSIGNALED(status) && WTERMSIG(status) == want_signal;
    if (ok) {
        passed++;
    } else {
        failed++;
        if (WIFSIGNALED(status))
            printf("FAIL %s: killed by signal %d\n", name, WTERMSIG(status));
        else
            printf("FAIL %s: exited %d\n", name, WEXITSTATUS(status));
    }
}

static void ignore(int sig) {
    signal(sig, SIG_IGN);
}
static void handle(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = skip_instruction;
    sa.sa_flags = SA_SIGINFO;
    sigaction(sig, &sa, NULL);
}

int main(void) {
    check("blocked SIGSEGV kills", block, SIGSEGV, fault, SIGSEGV);
    check("ignored SIGSEGV kills", ignore, SIGSEGV, fault, SIGSEGV);
    check("BRK kills with SIGTRAP", NULL, 0, breakpoint, SIGTRAP);
    check("blocked SIGTRAP from BRK kills", block, SIGTRAP, breakpoint, SIGTRAP);
    check("ignored SIGTRAP from BRK kills", ignore, SIGTRAP, breakpoint, SIGTRAP);
    check("SIGTRAP handler runs for BRK", handle, SIGTRAP, breakpoint, 0);
    check("blocked SIGILL kills", block, SIGILL, undefined, SIGILL);
    check("SIGILL handler runs for UDF", handle, SIGILL, undefined, 0);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
