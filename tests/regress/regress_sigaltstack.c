// Regression test: the alternate signal stack is per thread.
//
// iSH kept sigaltstack() state in struct sighand, which every thread of a
// process shares. Each thread that set an alternate stack overwrote the one
// slot, so a signal for thread A ran its handler on whichever stack thread B
// registered last. The Go runtime gives every M its own gsignal stack via
// sigaltstack(); with two threads taking signals at once both handlers ran on
// the same stack, and `go build` died in runtime.adjustSignalStack with
// SIGSEGV -- only on multi-core hosts, since one core never runs both at once.
//
// Linux semantics checked here:
//   1. A handler registered with SA_ONSTACK runs on the receiving thread's own
//      alternate stack, and sigaltstack(NULL, &old) in it reports that stack
//      with SS_ONSTACK.
//   2. A new thread (clone with CLONE_VM, without CLONE_VFORK) starts with no
//      alternate stack.
//   3. A fork child inherits the calling thread's alternate stack.
// Running this natively on Linux passes, which pins the expectations.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_sigaltstack
//            regress_sigaltstack.c
// Run:   ish -r <rootfs> /tmp/regress_sigaltstack

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ALT_SIZE (64 * 1024)

static int failed;
static int passed;

static void check(int ok, const char *what) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

// What the handler saw, filled in on the receiving thread.
struct handler_seen {
    uintptr_t sp;
    stack_t reported;
};
static __thread struct handler_seen *seen;

static void on_usr1(int sig) {
    (void) sig;
    volatile char here;
    seen->sp = (uintptr_t) &here;
    sigaltstack(NULL, &seen->reported);
}

static int on_stack(uintptr_t sp, const char *base) {
    return sp > (uintptr_t) base && sp <= (uintptr_t) base + ALT_SIZE;
}

static char main_alt[ALT_SIZE];
static char second_alt[ALT_SIZE];
static pthread_barrier_t registered;
static pthread_barrier_t main_done;

// Raise SIGUSR1 on this thread and check it ran on `alt`.
static void signal_self_and_check(const char *alt, const char *who) {
    struct handler_seen local = {0};
    seen = &local;
    pthread_kill(pthread_self(), SIGUSR1);
    char what[128];
    snprintf(what, sizeof(what), "%s: handler sp %#lx not on own altstack %p",
             who, (unsigned long) local.sp, (void *) alt);
    check(on_stack(local.sp, alt), what);
    snprintf(what, sizeof(what), "%s: sigaltstack in handler reported %p, want %p",
             who, local.reported.ss_sp, (void *) alt);
    check(local.reported.ss_sp == alt, what);
    snprintf(what, sizeof(what), "%s: SS_ONSTACK not reported in handler (flags %#x)",
             who, local.reported.ss_flags);
    check((local.reported.ss_flags & SS_ONSTACK) != 0, what);
}

static void *second_thread(void *arg) {
    (void) arg;
    stack_t initial;
    sigaltstack(NULL, &initial);
    char what[128];
    snprintf(what, sizeof(what), "new thread: inherited altstack %p (flags %#x), want SS_DISABLE",
             initial.ss_sp, initial.ss_flags);
    check((initial.ss_flags & SS_DISABLE) != 0, what);

    // Register after the main thread, so a shared slot would now hold ours.
    pthread_barrier_wait(&registered);
    stack_t ss = {.ss_sp = second_alt, .ss_size = ALT_SIZE};
    sigaltstack(&ss, NULL);
    pthread_barrier_wait(&registered);

    pthread_barrier_wait(&main_done);
    signal_self_and_check(second_alt, "second thread");
    return NULL;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    stack_t ss = {.ss_sp = main_alt, .ss_size = ALT_SIZE};
    sigaltstack(&ss, NULL);

    pthread_barrier_init(&registered, NULL, 2);
    pthread_barrier_init(&main_done, NULL, 2);
    pthread_t t;
    pthread_create(&t, NULL, second_thread, NULL);
    pthread_barrier_wait(&registered);
    pthread_barrier_wait(&registered);

    signal_self_and_check(main_alt, "main thread");
    pthread_barrier_wait(&main_done);
    pthread_join(t, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        stack_t inherited;
        sigaltstack(NULL, &inherited);
        _exit(inherited.ss_sp == main_alt && !(inherited.ss_flags & SS_DISABLE) ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork child: did not inherit the calling thread's altstack");

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
