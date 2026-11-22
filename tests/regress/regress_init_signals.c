// Regression test for the signals init acts on. Run it as init (pid 1).
//
// As the init of a Linux PID namespace, init only gets the signals it has a
// handler for, SIGKILL and SIGSTOP included; a signal it blocks still queues
// (sigwait may take it) and is dropped if it is unblocked without a handler.
// iSH let any signal end init, so `kill -9 1` took the whole system down.
//
// Checks that SIGKILL and SIGSTOP sent to itself do nothing, and that a blocked
// SIGTERM is pending and then dropped when unblocked. run.sh checks the exit
// status, and that a fault still ends init. Running natively as the init of a
// PID namespace passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_init_signals regress_init_signals.c
// Run:   ish -r <rootfs> /tmp/regress_init_signals

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static int passed, failed;

static void check(const char *name, int ok) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", name);
    }
}

int main(void) {
    if (getpid() != 1) {
        printf("not init (pid %d); run this as pid 1\n", getpid());
        return 1;
    }
    kill(getpid(), SIGKILL);
    check("SIGKILL to init is ignored", 1);
    kill(getpid(), SIGSTOP);
    check("SIGSTOP to init is ignored", 1);

    sigset_t set, pending;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigprocmask(SIG_BLOCK, &set, NULL);
    kill(getpid(), SIGTERM);
    sigpending(&pending);
    check("blocked SIGTERM is pending", sigismember(&pending, SIGTERM) == 1);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    sigpending(&pending);
    check("unblocked SIGTERM without a handler is dropped", sigismember(&pending, SIGTERM) == 0);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
