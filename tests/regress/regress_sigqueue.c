// Regression test for sigqueue: rt_sigqueueinfo and rt_tgsigqueueinfo.
//
// rt_sigqueueinfo was a stub that failed with ENOSYS, and rt_tgsigqueueinfo
// (pthread_sigqueue) was sys_rt_sigreturn in the arm64 table: a call returned
// from a signal handler that was never called. A queued signal carries the
// caller's siginfo, its value with it, to a handler, sigwaitinfo or a
// signalfd; real-time ones queue each. A code the kernel's or kill's (SI_USER
// and up, SI_TKILL) is the caller's to give only to itself.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_sigqueue regress_sigqueue.c

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
        case EPERM: return "EPERM";
        case EINVAL: return "EINVAL";
        case ESRCH: return "ESRCH";
        case EFAULT: return "EFAULT";
        case EAGAIN: return "EAGAIN";
        case ENOSYS: return "ENOSYS";
    }
    static char other[32];
    snprintf(other, sizeof(other), "errno %d", err);
    return other;
}

static siginfo_t queued(int sig, int code, int value) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = code;
    info.si_pid = getpid();
    info.si_uid = getuid();
    info.si_value.sival_int = value;
    return info;
}

static const char *sigqueueinfo(pid_t pid, int sig, int code, int value) {
    siginfo_t info = queued(sig, code, value);
    return errname(syscall(SYS_rt_sigqueueinfo, pid, sig, &info) == 0 ? 0 : errno);
}

static const char *tgsigqueueinfo(pid_t tgid, pid_t tid, int sig, int code, int value) {
    siginfo_t info = queued(sig, code, value);
    return errname(syscall(SYS_rt_tgsigqueueinfo, tgid, tid, sig, &info) == 0 ? 0 : errno);
}

static volatile int handled_code, handled_value, handled_pid;

static void on_usr1(int sig, siginfo_t *info, void *ctx) {
    (void) sig;
    (void) ctx;
    handled_code = info->si_code;
    handled_value = info->si_value.sival_int;
    handled_pid = info->si_pid;
}

// "code value pid" of a signal in set taken now, or EAGAIN
static const char *take(sigset_t *set) {
    static char results[8][64];
    static int next;
    char *result = results[next++ % 8];
    struct timespec now = {0, 0};
    siginfo_t info;
    if (sigtimedwait(set, &info, &now) < 0)
        snprintf(result, 64, "%s", errname(errno));
    else
        snprintf(result, 64, "%d %d %d", info.si_code, info.si_value.sival_int, info.si_pid);
    return result;
}

static pid_t thread_tid;
static volatile int thread_value = -1;
static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;

static void *thread_waits(void *arg) {
    (void) arg;
    pthread_mutex_lock(&ready_lock);
    thread_tid = gettid();
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&ready_lock);
    sigset_t usr2;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    siginfo_t info;
    if (sigwaitinfo(&usr2, &info) == SIGUSR2 && info.si_code == SI_QUEUE)
        thread_value = info.si_value.sival_int;
    return NULL;
}

int main(void) {
    char detail[300], expected[100];
    pid_t me = getpid();

    // 1. a handler gets the value, the code and the sender
    struct sigaction sa = {.sa_sigaction = on_usr1, .sa_flags = SA_SIGINFO};
    sigaction(SIGUSR1, &sa, NULL);
    int queued_ok = sigqueue(me, SIGUSR1, (union sigval) {.sival_int = 42});
    snprintf(detail, sizeof(detail), "sigqueue %d (%s), handler: code %d value %d pid %d", queued_ok,
             errname(queued_ok < 0 ? errno : 0), handled_code, handled_value, handled_pid);
    check("sigqueue: the handler's siginfo", queued_ok == 0 && handled_code == SI_QUEUE && handled_value == 42 &&
          handled_pid == me, detail);

    // 2. real-time signals queue each, in order, with their values
    sigset_t rt;
    sigemptyset(&rt);
    sigaddset(&rt, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &rt, NULL);
    for (int value = 1; value <= 3; value++)
        sigqueue(me, SIGRTMIN, (union sigval) {.sival_int = value});
    snprintf(detail, sizeof(detail), "%s, %s, %s, then %s", take(&rt), take(&rt), take(&rt), take(&rt));
    snprintf(expected, sizeof(expected), "%d 1 %d, %d 2 %d, %d 3 %d, then EAGAIN", SI_QUEUE, me, SI_QUEUE, me,
             SI_QUEUE, me);
    check("real-time signals queue, in order", strcmp(detail, expected) == 0, detail);

    // 3. a signalfd reads the value: ssi_int, ssi_ptr
    sigset_t rt1;
    sigemptyset(&rt1);
    sigaddset(&rt1, SIGRTMIN + 1);
    sigprocmask(SIG_BLOCK, &rt1, NULL);
    int fd = signalfd(-1, &rt1, SFD_NONBLOCK);
    sigqueue(me, SIGRTMIN + 1, (union sigval) {.sival_ptr = (void *) 0x12345678});
    struct signalfd_siginfo ssi = {0};
    ssize_t n = read(fd, &ssi, sizeof(ssi));
    close(fd);
    snprintf(detail, sizeof(detail), "read %zd: code %d int %#x ptr %#llx pid %u", n, ssi.ssi_code, ssi.ssi_int,
             (unsigned long long) ssi.ssi_ptr, ssi.ssi_pid);
    check("signalfd: ssi_int and ssi_ptr", n == sizeof(ssi) && ssi.ssi_code == SI_QUEUE && ssi.ssi_int == 0x12345678 &&
          ssi.ssi_ptr == 0x12345678 && ssi.ssi_pid == (uint32_t) me, detail);

    // 4. to another process
    sigset_t usr2;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    sigprocmask(SIG_BLOCK, &usr2, NULL);
    pid_t child = fork();
    if (child == 0) {
        siginfo_t info;
        if (sigwaitinfo(&usr2, &info) != SIGUSR2 || info.si_code != SI_QUEUE || info.si_pid != getppid())
            _exit(100);
        _exit(info.si_value.sival_int);
    }
    int sent = sigqueue(child, SIGUSR2, (union sigval) {.sival_int = 7});
    int status;
    waitpid(child, &status, 0);
    snprintf(detail, sizeof(detail), "sigqueue %d, child exited %d", sent, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    check("sigqueue to another process", sent == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7, detail);

    // 5. rt_tgsigqueueinfo: to one thread
    pthread_t thread;
    pthread_mutex_lock(&ready_lock);
    pthread_create(&thread, NULL, thread_waits, NULL);
    while (thread_tid == 0)
        pthread_cond_wait(&ready, &ready_lock);
    pthread_mutex_unlock(&ready_lock);
    const char *to_thread = tgsigqueueinfo(me, thread_tid, SIGUSR2, SI_QUEUE, 9);
    pthread_join(thread, NULL);
    snprintf(detail, sizeof(detail), "rt_tgsigqueueinfo %s, the thread got %d", to_thread, thread_value);
    check("rt_tgsigqueueinfo: to a thread", strcmp(to_thread, "ok") == 0 && thread_value == 9, detail);

    // 6. a code the kernel's or kill's: only to oneself
    child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    const char *user_other = sigqueueinfo(child, SIGUSR2, SI_USER, 1);
    const char *tkill_other = sigqueueinfo(child, SIGUSR2, SI_TKILL, 1);
    const char *kernel_thread = tgsigqueueinfo(child, child, SIGUSR2, SI_KERNEL, 1);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    const char *user_self = sigqueueinfo(me, SIGUSR2, SI_USER, 5);
    const char *took_self = take(&usr2);
    snprintf(detail, sizeof(detail), "another: SI_USER %s, SI_TKILL %s, SI_KERNEL to a thread %s; itself: SI_USER %s, "
             "taken %s", user_other, tkill_other, kernel_thread, user_self, took_self);
    snprintf(expected, sizeof(expected), "%d 5 %d", SI_USER, me);
    check("a kernel's or kill's code: only to oneself", strcmp(user_other, "EPERM") == 0 &&
          strcmp(tkill_other, "EPERM") == 0 && strcmp(kernel_thread, "EPERM") == 0 && strcmp(user_self, "ok") == 0 &&
          strcmp(took_self, expected) == 0, detail);

    // 7. ESRCH for no such process or thread, EINVAL for a tid of 0, EFAULT
    child = fork();
    if (child == 0)
        _exit(0);
    waitpid(child, NULL, 0);
    const char *gone = sigqueueinfo(child, SIGUSR2, SI_QUEUE, 1);
    const char *zero = sigqueueinfo(0, SIGUSR2, SI_QUEUE, 1);
    const char *no_tid = tgsigqueueinfo(me, 0, SIGUSR2, SI_QUEUE, 1);
    const char *wrong_group = tgsigqueueinfo(child, gettid(), SIGUSR2, SI_QUEUE, 1);
    const char *fault = errname(syscall(SYS_rt_sigqueueinfo, me, SIGUSR2, (void *) 8) == 0 ? 0 : errno);
    snprintf(detail, sizeof(detail), "gone %s, pid 0 %s, tid 0 %s, another's tgid %s, bad siginfo %s", gone, zero,
             no_tid, wrong_group, fault);
    check("ESRCH, EINVAL, EFAULT", strcmp(detail, "gone ESRCH, pid 0 ESRCH, tid 0 EINVAL, another's tgid ESRCH, "
          "bad siginfo EFAULT") == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
