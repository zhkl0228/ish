// Regression test for blocking calls interrupted by a signal: which restart,
// which fail with EINTR.
//
// As Linux's ERESTARTSYS, a blocking read or waitpid that a signal interrupts
// starts again when no handler runs for the signal (the process was stopped
// and continued: Ctrl-Z, fg), or when the handler has SA_RESTART, and fails
// with EINTR when a handler without SA_RESTART runs. iSH failed them with
// EINTR after a stop.
//
// Each case is a child; the parent signals it and reports what its call
// returned. Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_eintr regress_eintr.c

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0)
        continue;
}

static void on_usr1(int sig) {
    (void) sig;
}

// The child's call and what it returned: >= 0, or -errno.
static int data[2], result[2];
static long report(long r) {
    long out = r < 0 ? -errno : r;
    write(result[1], &out, sizeof(out));
    _exit(0);
}

enum disturb { STOP_CONT, HANDLER, HANDLER_RESTART };

// The child reads data (or waits for a child of its own); the parent
// disturbs it, then gives it something to read (or lets its child exit).
static long run(int wait_child, enum disturb how) {
    pipe(data);
    pipe(result);
    pid_t child = fork();
    if (child == 0) {
        if (how != STOP_CONT) {
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = on_usr1;
            sa.sa_flags = how == HANDLER_RESTART ? SA_RESTART : 0;
            sigaction(SIGUSR1, &sa, NULL);
        }
        alarm(10);
        if (wait_child) {
            pid_t grandchild = fork();
            if (grandchild == 0) {
                sleep_ms(800);
                _exit(0);
            }
            report(waitpid(grandchild, NULL, 0) == grandchild ? 1 : -1);
        }
        char c;
        report(read(data[0], &c, 1));
    }
    sleep_ms(300);
    if (how == STOP_CONT) {
        kill(child, SIGSTOP);
        int status;
        waitpid(child, &status, WUNTRACED);
        kill(child, SIGCONT);
    } else {
        kill(child, SIGUSR1);
    }
    sleep_ms(100);
    write(data[1], "x", 1);
    long out = 0;
    read(result[0], &out, sizeof(out));
    waitpid(child, NULL, 0);
    close(data[0]);
    close(data[1]);
    close(result[0]);
    close(result[1]);
    return out;
}

static void describe(char *detail, size_t size, long r) {
    if (r < 0)
        snprintf(detail, size, "returned %s", strerror((int) -r));
    else
        snprintf(detail, size, "returned %ld", r);
}

int main(void) {
    char detail[100];
    long r = run(0, STOP_CONT);
    describe(detail, sizeof(detail), r);
    check("read, the process stopped and continued: restarts", r == 1, detail);

    r = run(1, STOP_CONT);
    describe(detail, sizeof(detail), r);
    check("waitpid, the process stopped and continued: restarts", r == 1, detail);

    r = run(0, HANDLER);
    describe(detail, sizeof(detail), r);
    check("read, a handler without SA_RESTART: EINTR", r == -EINTR, detail);

    r = run(0, HANDLER_RESTART);
    describe(detail, sizeof(detail), r);
    check("read, a handler with SA_RESTART: restarts", r == 1, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
