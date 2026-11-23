// Regression test for SIGCHLD when a child stops or continues, and wait's
// WCONTINUED.
//
// A stop sent the parent SIGCHLD with no siginfo, even with SA_NOCLDSTOP,
// and a continue nothing: wait could not report it either. As Linux's
// do_notify_parent_cldstop, the parent gets SIGCHLD with CLD_STOPPED or
// CLD_CONTINUED, the child's pid and the signal, unless SA_NOCLDSTOP; wait
// reports a stop with WUNTRACED and a continue, once, with WCONTINUED.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sigchld_stop regress_sigchld_stop.c

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

static sigset_t chld;

// "code pid status" of the SIGCHLD that comes within ms, or "none"
static const char *sigchld(int ms) {
    static char results[8][64];
    static int next;
    char *result = results[next++ % 8];
    struct timespec timeout = {ms / 1000, (ms % 1000) * 1000000L};
    siginfo_t info;
    if (sigtimedwait(&chld, &info, &timeout) < 0)
        snprintf(result, 64, "%s", errno == EAGAIN ? "none" : strerror(errno));
    else
        snprintf(result, 64, "%d %d %d", info.si_code, info.si_pid, info.si_status);
    return result;
}

static const char *expect(int code, pid_t pid, int status) {
    static char results[8][64];
    static int next;
    char *result = results[next++ % 8];
    snprintf(result, 64, "%d %d %d", code, pid, status);
    return result;
}

// what waitpid says now (WNOHANG)
static const char *waited(pid_t child, int options) {
    static char results[8][64];
    static int next;
    char *result = results[next++ % 8];
    int status;
    pid_t got = waitpid(child, &status, options | WNOHANG);
    if (got == 0)
        snprintf(result, 64, "nothing");
    else if (got < 0)
        snprintf(result, 64, "%s", strerror(errno));
    else if (WIFSTOPPED(status))
        snprintf(result, 64, "stopped %d", WSTOPSIG(status));
    else if (WIFCONTINUED(status))
        snprintf(result, 64, "continued");
    else if (WIFSIGNALED(status))
        snprintf(result, 64, "killed %d", WTERMSIG(status));
    else
        snprintf(result, 64, "exited %d", WEXITSTATUS(status));
    return result;
}

// A child that pauses, set up before it is signalled: SIGTSTP its default
// action (bash's command substitution, $(...), leaves it ignored, and an
// ignored signal is dropped when sent), in a process group of its own whose
// parent is in another of the session (not an orphaned one, whose SIGTSTP
// Linux discards).
static pid_t sleeper(void) {
    int ready[2];
    pipe(ready);
    pid_t child = fork();
    if (child == 0) {
        signal(SIGTSTP, SIG_DFL);
        setpgid(0, 0);
        write(ready[1], "", 1);
        for (;;)
            pause();
    }
    char c;
    read(ready[0], &c, 1);
    close(ready[0]);
    close(ready[1]);
    return child;
}

static void on_chld(int sig) {
    (void) sig;
}

int main(void) {
    char detail[300];
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, NULL);

    // 1. SIGSTOP, SIGTSTP: CLD_STOPPED with the signal; waitpid's WUNTRACED
    pid_t child = sleeper();
    kill(child, SIGSTOP);
    const char *stopped = sigchld(2000);
    const char *stop_wait = waited(child, WUNTRACED);
    kill(child, SIGCONT);
    sigchld(2000);
    waited(child, WCONTINUED);
    kill(child, SIGTSTP);
    const char *tstp = sigchld(2000);
    const char *tstp_wait = waited(child, WUNTRACED);
    snprintf(detail, sizeof(detail), "SIGSTOP: %s, waitpid %s; SIGTSTP: %s, waitpid %s", stopped, stop_wait, tstp,
             tstp_wait);
    check("a stop: CLD_STOPPED, and WUNTRACED", strcmp(stopped, expect(CLD_STOPPED, child, SIGSTOP)) == 0 &&
          strcmp(stop_wait, "stopped 19") == 0 && strcmp(tstp, expect(CLD_STOPPED, child, SIGTSTP)) == 0 &&
          strcmp(tstp_wait, "stopped 20") == 0, detail);

    // 2. SIGCONT: CLD_CONTINUED; waitpid's WCONTINUED, once
    kill(child, SIGCONT);
    const char *continued = sigchld(2000);
    const char *cont_wait = waited(child, WCONTINUED);
    const char *cont_again = waited(child, WCONTINUED);
    snprintf(detail, sizeof(detail), "%s, waitpid %s, then %s", continued, cont_wait, cont_again);
    check("a continue: CLD_CONTINUED, and WCONTINUED once", strcmp(continued, expect(CLD_CONTINUED, child, SIGCONT)) == 0 &&
          strcmp(cont_wait, "continued") == 0 && strcmp(cont_again, "nothing") == 0, detail);

    // 3. waitid: CLD_STOPPED and CLD_CONTINUED, with the signal
    kill(child, SIGSTOP);
    sigchld(2000);
    siginfo_t info = {0};
    int stopped_id = waitid(P_PID, child, &info, WSTOPPED | WNOHANG);
    int stop_code = info.si_code, stop_status = info.si_status;
    kill(child, SIGCONT);
    sigchld(2000);
    memset(&info, 0, sizeof(info));
    int continued_id = waitid(P_PID, child, &info, WCONTINUED | WNOHANG);
    snprintf(detail, sizeof(detail), "stopped %d: code %d status %d; continued %d: code %d status %d", stopped_id,
             stop_code, stop_status, continued_id, info.si_code, info.si_status);
    check("waitid: CLD_STOPPED and CLD_CONTINUED", stopped_id == 0 && stop_code == CLD_STOPPED &&
          stop_status == SIGSTOP && continued_id == 0 && info.si_code == CLD_CONTINUED && info.si_status == SIGCONT,
          detail);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    sigchld(2000);

    // 4. SA_NOCLDSTOP: no SIGCHLD for a stop or a continue, which wait still
    // reports; one for the exit
    struct sigaction sa = {.sa_handler = on_chld, .sa_flags = SA_NOCLDSTOP};
    sigaction(SIGCHLD, &sa, NULL);
    child = sleeper();
    kill(child, SIGSTOP);
    const char *quiet_stop = sigchld(300);
    const char *quiet_stop_wait = waited(child, WUNTRACED);
    kill(child, SIGCONT);
    const char *quiet_cont = sigchld(300);
    const char *quiet_cont_wait = waited(child, WCONTINUED);
    kill(child, SIGKILL);
    const char *killed = sigchld(2000);
    const char *killed_wait = waited(child, 0);
    snprintf(detail, sizeof(detail), "stop: %s, waitpid %s; continue: %s, waitpid %s; kill: %s, waitpid %s",
             quiet_stop, quiet_stop_wait, quiet_cont, quiet_cont_wait, killed, killed_wait);
    check("SA_NOCLDSTOP: none for a stop or continue", strcmp(quiet_stop, "none") == 0 &&
          strcmp(quiet_stop_wait, "stopped 19") == 0 && strcmp(quiet_cont, "none") == 0 &&
          strcmp(quiet_cont_wait, "continued") == 0 && strcmp(killed, expect(CLD_KILLED, child, SIGKILL)) == 0 &&
          strcmp(killed_wait, "killed 9") == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
