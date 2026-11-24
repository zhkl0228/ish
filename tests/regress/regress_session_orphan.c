// Regression test: the last process of a session whose leader is gone.
//
// A process keeps its session's terminal after the session's leader (the
// shell that started it, as a job in a process group of its own) has exited
// and been reaped, and exits in turn: init reaps it. iSH looked the session up, to see whether any of it was left,
// after taking the process out of it, when the session's pid was no more:
// init read the session of NULL and the app died of SIGSEGV at 0x10.
//
// Run as init of a fakefs of its own, for /dev/ptmx: the orphan comes to it.
// Natively it is the orphan's subreaper (PR_SET_CHILD_SUBREAPER).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_session_orphan regress_session_orphan.c

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
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

int main(void) {
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    int go[2], told[2];
    pipe(go);
    pipe(told);
    pid_t leader = fork();
    if (leader == 0) {
        // a session of its own, the pty its terminal, and a process in it
        // that outlives it: in a process group of its own, as a shell's job,
        // so that the session's pid has nothing left of it but that process
        setsid();
        signal(SIGHUP, SIG_IGN);
        int master = posix_openpt(O_RDWR | O_NOCTTY);
        grantpt(master);
        unlockpt(master);
        int slave = open(ptsname(master), O_RDWR);
        ioctl(slave, TIOCSCTTY, 0);
        pid_t member = fork();
        if (member == 0) {
            setpgid(0, 0);
            pid_t self = getpid();
            write(told[1], &self, sizeof(self));
            char c;
            read(go[0], &c, 1);
            _exit(7);
        }
        _exit(0);
    }
    pid_t member = 0;
    read(told[0], &member, sizeof(member));
    int status;
    pid_t reaped = waitpid(leader, &status, 0);
    char detail[128];
    snprintf(detail, sizeof(detail), "waitpid %d, status %#x", reaped, status);
    check("the session's leader exits and is reaped", reaped == leader && WIFEXITED(status), detail);

    // the session's last process, an orphan now, exits: this process reaps it
    write(go[1], "x", 1);
    reaped = waitpid(member, &status, 0);
    snprintf(detail, sizeof(detail), "waitpid %d, status %#x", reaped, status);
    check("its last process, orphaned, exits and is reaped", reaped == member && WIFEXITED(status) &&
          WEXITSTATUS(status) == 7, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
