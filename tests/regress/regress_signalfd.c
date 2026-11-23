// Regression test for signalfd (signalfd4), and the siginfo of SIGCHLD.
//
// signalfd4 was a stub that failed with ENOSYS: s6's servers (s6-ipcserver,
// and with it the utmps daemons behind w and who) could not start ("unable to
// create selfpipe"). A read takes the blocked signals of its mask pending for
// the reader, several at once, with their siginfo; poll and epoll say there is
// one, and wake for one another process sends. SIGCHLD has Linux's code
// (CLD_EXITED, CLD_KILLED), the child's pid and its status; it had SI_KERNEL,
// and the pid of the process's last thread.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_signalfd regress_signalfd.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
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

static const char *errname(int err) {
    switch (err) {
        case 0: return "ok";
        case EAGAIN: return "EAGAIN";
        case EINVAL: return "EINVAL";
        case EBADF: return "EBADF";
        case EINTR: return "EINTR";
        case ENOSYS: return "ENOSYS";
    }
    static char other[32];
    snprintf(other, sizeof(other), "errno %d", err);
    return other;
}

static sigset_t set_of(int a, int b) {
    sigset_t set;
    sigemptyset(&set);
    if (a)
        sigaddset(&set, a);
    if (b)
        sigaddset(&set, b);
    return set;
}

// "signo code pid", or the error
static const char *take(int fd, struct signalfd_siginfo *out) {
    static char results[4][80];
    static int next;
    char *result = results[next++ % 4];
    struct signalfd_siginfo info;
    ssize_t n = read(fd, &info, sizeof(info));
    if (n < 0) {
        snprintf(result, 80, "%s", errname(errno));
        return result;
    }
    if (out != NULL)
        *out = info;
    snprintf(result, 80, "%zd: %u %d %u", n, info.ssi_signo, info.ssi_code, info.ssi_pid);
    return result;
}

static long ms_since(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

// a child that sends sig to its parent after 100 ms
static pid_t send_later(int sig) {
    pid_t child = fork();
    if (child == 0) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        kill(getppid(), sig);
        _exit(0);
    }
    return child;
}

int main(void) {
    char detail[300], expected[200];
    sigset_t blocked = set_of(SIGUSR1, SIGUSR2);
    sigaddset(&blocked, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blocked, NULL);
    pid_t me = getpid();

    // 1. a blocked signal, read with its siginfo
    sigset_t usr1 = set_of(SIGUSR1, 0);
    int fd = signalfd(-1, &usr1, SFD_NONBLOCK | SFD_CLOEXEC);
    const char *empty = take(fd, NULL);
    struct pollfd pfd = {fd, POLLIN, 0};
    int before = poll(&pfd, 1, 0);
    kill(me, SIGUSR1);
    int after = poll(&pfd, 1, 0);
    struct signalfd_siginfo info;
    const char *got = take(fd, &info);
    const char *again = take(fd, NULL);
    int cloexec = fcntl(fd, F_GETFD) & FD_CLOEXEC;
    snprintf(detail, sizeof(detail), "fd %d, empty %s, poll %d then %d (%#x), read %s uid %u, again %s, cloexec %d",
             fd, empty, before, after, pfd.revents, got, info.ssi_uid, again, cloexec);
    snprintf(expected, sizeof(expected), "128: %d 0 %d", SIGUSR1, me);
    check("a blocked signal: read, with its siginfo", fd >= 0 && strcmp(empty, "EAGAIN") == 0 && before == 0 &&
          after == 1 && pfd.revents == POLLIN && strcmp(got, expected) == 0 && info.ssi_uid == getuid() &&
          strcmp(again, "EAGAIN") == 0 && cloexec, detail);

    // 2. a new mask for the same fd; several signals in one read
    sigset_t both = set_of(SIGUSR1, SIGUSR2);
    int same = signalfd(fd, &both, 0);
    kill(me, SIGUSR1);
    kill(me, SIGUSR2);
    struct signalfd_siginfo several[4];
    ssize_t n = read(fd, several, sizeof(several));
    snprintf(detail, sizeof(detail), "signalfd(fd) %d, read %zd: %u %u", same, n, several[0].ssi_signo,
             several[1].ssi_signo);
    check("several signals in one read", same == fd && n == 256 && several[0].ssi_signo == SIGUSR1 &&
          several[1].ssi_signo == SIGUSR2, detail);

    // 3. tgkill: SI_TKILL, the sender's pid
    syscall(SYS_tgkill, me, gettid(), SIGUSR1);
    got = take(fd, NULL);
    snprintf(expected, sizeof(expected), "128: %d %d %d", SIGUSR1, SI_TKILL, me);
    check("tgkill: SI_TKILL", strcmp(got, expected) == 0, got);

    // 4. a signal not in the mask is not read, and stays pending
    sigset_t usr2 = set_of(SIGUSR2, 0);
    signalfd(fd, &usr2, 0);
    kill(me, SIGUSR1);
    const char *other = take(fd, NULL);
    kill(me, SIGUSR2);
    const char *in_mask = take(fd, NULL);
    signalfd(fd, &usr1, 0);
    const char *left = take(fd, NULL);
    snprintf(detail, sizeof(detail), "SIGUSR1 %s, SIGUSR2 %s, SIGUSR1 then %s", other, in_mask, left);
    check("only the mask's signals", strcmp(other, "EAGAIN") == 0 && strncmp(in_mask, "128: 12 ", 8) == 0 &&
          strncmp(left, "128: 10 ", 8) == 0, detail);

    // 5. a blocking read, woken by another process's signal
    int blocking = signalfd(-1, &usr1, 0);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pid_t child = send_later(SIGUSR1);
    got = take(blocking, NULL);
    long waited = ms_since(&start);
    waitpid(child, NULL, 0);
    snprintf(expected, sizeof(expected), "128: %d 0 %d", SIGUSR1, child);
    snprintf(detail, sizeof(detail), "%s after %ld ms", got, waited);
    check("a blocking read, woken by another process", strcmp(got, expected) == 0 && waited >= 80, detail);
    close(blocking);

    // 6. poll, woken by another process's signal
    clock_gettime(CLOCK_MONOTONIC, &start);
    child = send_later(SIGUSR1);
    pfd.revents = 0;
    int ready = poll(&pfd, 1, 3000);
    waited = ms_since(&start);
    got = take(fd, NULL);
    waitpid(child, NULL, 0);
    snprintf(detail, sizeof(detail), "poll %d (%#x) after %ld ms, read %s", ready, pfd.revents, waited, got);
    check("poll, woken by another process", ready == 1 && pfd.revents == POLLIN && waited >= 80 && waited < 2000 &&
          strncmp(got, "128: 10 ", 8) == 0, detail);

    // 7. epoll, woken the same way
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    clock_gettime(CLOCK_MONOTONIC, &start);
    child = send_later(SIGUSR1);
    struct epoll_event out;
    ready = epoll_wait(ep, &out, 1, 3000);
    waited = ms_since(&start);
    got = take(fd, NULL);
    waitpid(child, NULL, 0);
    close(ep);
    snprintf(detail, sizeof(detail), "epoll_wait %d after %ld ms, read %s", ready, waited, got);
    check("epoll, woken by another process", ready == 1 && out.data.fd == fd && waited < 2000 &&
          strncmp(got, "128: 10 ", 8) == 0, detail);

    // 8. SIGCHLD: CLD_EXITED and the exit status, CLD_KILLED and the signal;
    // the child's pid
    // the earlier children's, one pending
    sigset_t chld = set_of(SIGCHLD, 0);
    struct timespec no_wait = {0, 0};
    sigtimedwait(&chld, NULL, &no_wait);
    int chld_fd = signalfd(-1, &chld, 0);
    child = fork();
    if (child == 0)
        _exit(3);
    struct signalfd_siginfo exited;
    const char *exit_got = take(chld_fd, &exited);
    waitpid(child, NULL, 0);
    pid_t killed_child = fork();
    if (killed_child == 0) {
        pause();
        _exit(0);
    }
    kill(killed_child, SIGKILL);
    struct signalfd_siginfo killed;
    const char *kill_got = take(chld_fd, &killed);
    waitpid(killed_child, NULL, 0);
    close(chld_fd);
    snprintf(detail, sizeof(detail), "exit: %s status %d; killed: %s status %d", exit_got, exited.ssi_status,
             kill_got, killed.ssi_status);
    snprintf(expected, sizeof(expected), "128: %d %d %d", SIGCHLD, CLD_EXITED, child);
    char expected_killed[80];
    snprintf(expected_killed, sizeof(expected_killed), "128: %d %d %d", SIGCHLD, CLD_KILLED, killed_child);
    check("SIGCHLD: Linux's code, pid and status", strcmp(exit_got, expected) == 0 && exited.ssi_status == 3 &&
          strcmp(kill_got, expected_killed) == 0 && killed.ssi_status == SIGKILL, detail);

    // 9. a child of fork reads its own signals with the fd, not its parent's
    child = fork();
    if (child == 0) {
        kill(getpid(), SIGUSR1);
        struct signalfd_siginfo own;
        const char *child_got = take(fd, &own);
        _exit(strncmp(child_got, "128: 10 ", 8) != 0 || own.ssi_pid != (unsigned) getpid());
    }
    int status;
    waitpid(child, &status, 0);
    const char *parent_got = take(fd, NULL);
    snprintf(detail, sizeof(detail), "child %d, parent %s", WEXITSTATUS(status), parent_got);
    check("fork: the child reads its own", WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
          strcmp(parent_got, "EAGAIN") == 0, detail);

    // 10. EINVAL: flags, a mask's size, an fd not a signalfd, a short read;
    // EBADF
    int pipes[2];
    pipe(pipes);
    unsigned long mask = 1UL << (SIGUSR1 - 1);
    int bad_flags = signalfd(-1, &usr1, 1) < 0 ? errno : 0;
    int bad_size = syscall(SYS_signalfd4, -1, &mask, 16, 0) < 0 ? errno : 0;
    int not_signalfd = signalfd(pipes[0], &usr1, 0) < 0 ? errno : 0;
    int bad_fd = signalfd(9999, &usr1, 0) < 0 ? errno : 0;
    char small[64];
    kill(me, SIGUSR1);
    int short_read = read(fd, small, sizeof(small)) < 0 ? errno : 0;
    take(fd, NULL);
    snprintf(detail, sizeof(detail), "flags %s, size 16 %s, a pipe %s, fd 9999 %s, 64 bytes %s", errname(bad_flags),
             errname(bad_size), errname(not_signalfd), errname(bad_fd), errname(short_read));
    check("EINVAL and EBADF", strcmp(detail, "flags EINVAL, size 16 EINVAL, a pipe EINVAL, fd 9999 EBADF, "
          "64 bytes EINVAL") == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
