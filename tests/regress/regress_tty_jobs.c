// Regression test for job control on a controlling terminal.
//
// A process outside the terminal's foreground process group is stopped when
// it reads the terminal (SIGTTIN), writes it with TOSTOP set or changes its
// settings (SIGTTOU), and the signal goes to its whole process group, as
// Linux's tty_check_change does. iSH sent SIGTTIN to the reading thread
// only, never sent SIGTTOU, and refused a write whose SIGTTOU was ignored
// (EIO) where Linux lets it through. A group no parent in its session could
// resume (orphaned) gets EIO instead of stopping for good.
//
// Prints one line per case and "N passed, M failed". Natively passes. In iSH
// it needs /dev/ptmx, which a realfs root cannot have: run.sh runs it as the
// init of a fakefs of its own.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_tty_jobs regress_tty_jobs.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
static int tty; // the slave: this session's controlling terminal

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

static char proc_state(pid_t pid) {
    char path[64], buf[256] = "";
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return '?';
    fgets(buf, sizeof(buf), f);
    fclose(f);
    char *paren = strrchr(buf, ')');
    return paren != NULL ? paren[2] : '?';
}

// Runs fn in a background process group of its own; the status it stopped
// or exited with.
static int background(void (*fn)(void)) {
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        // the defaults (the foreground process ignores SIGTTOU)
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        alarm(5);
        fn();
        _exit(0);
    }
    setpgid(child, child);
    int status;
    waitpid(child, &status, WUNTRACED);
    if (WIFSTOPPED(status)) {
        kill(-child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    return status;
}

static void describe(char *out, size_t size, int status) {
    if (WIFSTOPPED(status))
        snprintf(out, size, "stopped by %d", WSTOPSIG(status));
    else if (WIFSIGNALED(status))
        snprintf(out, size, "killed by %d", WTERMSIG(status));
    else
        snprintf(out, size, "exited %d", WEXITSTATUS(status));
}

static int sibling_pipe[2];
static void read_with_a_sibling(void) {
    pid_t sibling = fork();
    if (sibling == 0) {
        sleep_ms(5000);
        _exit(0);
    }
    write(sibling_pipe[1], &sibling, sizeof(sibling));
    char c;
    read(tty, &c, 1);
}

static void read_ignoring(void) {
    signal(SIGTTIN, SIG_IGN);
    char c;
    _exit(read(tty, &c, 1) < 0 && errno == EIO ? 0 : 1);
}

static void write_once(void) {
    write(tty, "x", 1);
}

static void write_ignoring(void) {
    signal(SIGTTOU, SIG_IGN);
    _exit(write(tty, "y", 1) == 1 ? 0 : 1);
}

static void set_attrs(void) {
    struct termios t;
    tcgetattr(tty, &t);
    tcsetattr(tty, TCSANOW, &t);
}

static int orphan_pipe[2];
static void orphan_read(void) {
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        sleep_ms(300); // its parent gone: the group has nobody to resume it
        char c;
        int result = read(tty, &c, 1) < 0 ? errno : 0;
        write(orphan_pipe[1], &result, sizeof(result));
        _exit(0);
    }
    _exit(0);
}

int main(void) {
    char detail[160];
    // a session of its own: init (run.sh) leads one already
    if (getpid() == 1 || getpgrp() == getpid()) {
        pid_t child = fork();
        if (child != 0) {
            int status;
            waitpid(child, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
    setsid();
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(master);
    unlockpt(master);
    tty = open(ptsname(master), O_RDWR);
    ioctl(tty, TIOCSCTTY, 0);
    tcsetpgrp(tty, getpgrp());
    signal(SIGTTOU, SIG_IGN); // this foreground process sets TOSTOP below

    // 1. A background read stops the reader's whole group.
    pipe(sibling_pipe);
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        signal(SIGTTOU, SIG_DFL);
        alarm(5);
        read_with_a_sibling();
        _exit(0);
    }
    setpgid(child, child);
    pid_t sibling = 0;
    read(sibling_pipe[0], &sibling, sizeof(sibling));
    int status;
    waitpid(child, &status, WUNTRACED);
    sleep_ms(100);
    char sibling_state = proc_state(sibling);
    kill(-child, SIGKILL);
    waitpid(child, NULL, 0);
    describe(detail, sizeof(detail), status);
    snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail), "; its group's other process in state %c",
             sibling_state);
    check("read in the background: SIGTTIN stops the group",
          WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN && sibling_state == 'T', detail);

    status = background(read_ignoring);
    describe(detail, sizeof(detail), status);
    check("read in the background, SIGTTIN ignored: EIO", WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);

    struct termios t;
    tcgetattr(tty, &t);
    t.c_lflag |= TOSTOP;
    tcsetattr(tty, TCSANOW, &t);
    status = background(write_once);
    describe(detail, sizeof(detail), status);
    check("write in the background with TOSTOP: SIGTTOU stops it",
          WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTOU, detail);

    status = background(write_ignoring);
    describe(detail, sizeof(detail), status);
    check("write in the background with TOSTOP, SIGTTOU ignored: written",
          WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);

    status = background(set_attrs);
    describe(detail, sizeof(detail), status);
    check("tcsetattr in the background: SIGTTOU stops it", WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTOU,
          detail);

    pipe(orphan_pipe);
    background(orphan_read);
    int result = -1;
    alarm(5);
    read(orphan_pipe[0], &result, sizeof(result));
    alarm(0);
    snprintf(detail, sizeof(detail), "read: %s", result == 0 ? "ok" : strerror(result));
    check("read from an orphaned group: EIO", result == EIO, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
