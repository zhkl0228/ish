// Regression test for System V message queues: msgget, msgsnd, msgrcv,
// msgctl.
//
// They were stubs that failed with ENOSYS. msgrcv takes messages by type as
// msgrcv(2) says; a sender waits while the queue is full, a receiver while it
// has nothing for it, and IPC_RMID ends their wait with EIDRM. A signal a
// handler runs for ends it with EINTR, even with SA_RESTART; one that only
// stops and continues the process does not (Linux's ERESTARTNOHAND).
//
// Prints one line per case and "N passed, M failed". Natively passes, as root
// or not.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sysv_msg regress_sysv_msg.c

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_STAT_ANY
#define MSG_STAT_ANY 13
#endif

struct message {
    long type;
    char text[64];
};

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
        case ENOMSG: return "ENOMSG";
        case E2BIG: return "E2BIG";
        case EIDRM: return "EIDRM";
        case EINTR: return "EINTR";
        case EAGAIN: return "EAGAIN";
        case EINVAL: return "EINVAL";
        case EACCES: return "EACCES";
        case EPERM: return "EPERM";
        case EEXIST: return "EEXIST";
        case ENOENT: return "ENOENT";
        case ENOSYS: return "ENOSYS";
    }
    static char other[32];
    snprintf(other, sizeof(other), "errno %d", err);
    return other;
}

static const char *send_msg(int id, long type, const char *text, int flags) {
    struct message msg = {.type = type};
    strcpy(msg.text, text);
    return msgsnd(id, &msg, strlen(text), flags) == 0 ? "ok" : errname(errno);
}

// "type:text" received, or the error
static const char *receive(int id, long type, size_t size, int flags) {
    static char results[8][80];
    static int next;
    char *result = results[next++ % 8];
    struct message msg;
    ssize_t got = msgrcv(id, &msg, size, type, flags);
    if (got < 0)
        snprintf(result, 80, "%s", errname(errno));
    else
        snprintf(result, 80, "%ld:%.*s", msg.type, (int) got, msg.text);
    return result;
}

static struct msqid_ds stat_of(int id) {
    struct msqid_ds ds = {0};
    msgctl(id, IPC_STAT, &ds);
    return ds;
}

static void pause_ms(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// Whether child is still running (blocked), and not stopped or gone
static int running(pid_t child) {
    int status;
    return waitpid(child, &status, WNOHANG) == 0;
}

static int status_of(pid_t child) {
    int status;
    if (waitpid(child, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void on_usr1(int sig) {
    (void) sig;
}

int main(void) {
    char detail[300];
    key_t key = 0x6d730000 + (getpid() & 0xffff);
    // The times the kernel stamps (msg_stime, msg_rtime) are of a coarse
    // clock, as Linux's are (ktime_get_real_seconds) and ish's on a Linux host
    // (glibc's time()): for the few milliseconds a second has begun, still the
    // second before, which time() here (CLOCK_REALTIME) is past. One second
    // before start is the same moment (the semaphores' otime was, in CI's
    // linux job, 2026-11-27).
    time_t start = time(NULL);

    // 1. a private queue's IPC_STAT
    int id = msgget(IPC_PRIVATE, 0600);
    struct msqid_ds ds = stat_of(id);
    snprintf(detail, sizeof(detail), "msgget %d, qbytes %lu, qnum %lu, cbytes %lu, mode %#o, uid %u", id,
             (unsigned long) ds.msg_qbytes, (unsigned long) ds.msg_qnum, (unsigned long) ds.__msg_cbytes,
             ds.msg_perm.mode, ds.msg_perm.uid);
    check("a private queue: IPC_STAT", id >= 0 && ds.msg_qbytes == 16384 && ds.msg_qnum == 0 &&
          ds.__msg_cbytes == 0 && (ds.msg_perm.mode & 0777) == 0600 && ds.msg_perm.uid == geteuid(), detail);

    // 2. msgrcv by type: the first; of a type; MSG_EXCEPT; the lowest up to
    // -type, the first of those
    send_msg(id, 5, "a5", 0);
    send_msg(id, 2, "b2", 0);
    send_msg(id, 7, "c7", 0);
    send_msg(id, 2, "d2", 0);
    send_msg(id, 1, "e1", 0);
    send_msg(id, 1, "f1", 0);
    char got[200];
    snprintf(got, sizeof(got), "%s %s %s %s %s %s %s", receive(id, -3, 64, 0), receive(id, -3, 64, 0),
             receive(id, 2, 64, 0), receive(id, 2, 64, MSG_EXCEPT), receive(id, -10, 64, 0), receive(id, 0, 64, 0),
             receive(id, 0, 64, IPC_NOWAIT));
    check("msgrcv by type", strcmp(got, "1:e1 1:f1 2:b2 5:a5 2:d2 7:c7 ENOMSG") == 0, got);

    // 3. a message larger than the buffer: E2BIG and left queued, or cut
    // with MSG_NOERROR
    send_msg(id, 1, "0123456789", 0);
    const char *too_big = receive(id, 0, 4, IPC_NOWAIT);
    unsigned long left = stat_of(id).msg_qnum;
    const char *cut = receive(id, 0, 4, MSG_NOERROR);
    unsigned long after = stat_of(id).msg_qnum;
    snprintf(detail, sizeof(detail), "%s, %lu queued; MSG_NOERROR %s, %lu queued", too_big, left, cut, after);
    check("larger than the buffer: E2BIG, or cut with MSG_NOERROR", strcmp(too_big, "E2BIG") == 0 && left == 1 &&
          strcmp(cut, "1:0123") == 0 && after == 0, detail);

    // 4. EINVAL: type 0 or negative, more than msgmax, no such id
    struct message big = {.type = 1};
    int over = msgsnd(id, &big, 8193, IPC_NOWAIT);
    int over_errno = errno;
    snprintf(detail, sizeof(detail), "type 0 %s, type -1 %s, 8193 bytes %s, bad id: send %s receive %s",
             send_msg(id, 0, "x", IPC_NOWAIT), send_msg(id, -1, "x", IPC_NOWAIT),
             over < 0 ? errname(over_errno) : "ok", send_msg(id + 1, 1, "x", IPC_NOWAIT),
             receive(id + 1, 0, 64, IPC_NOWAIT));
    check("EINVAL: type, size, id", strcmp(detail, "type 0 EINVAL, type -1 EINVAL, 8193 bytes EINVAL, "
          "bad id: send EINVAL receive EINVAL") == 0, detail);

    // 5. IPC_STAT's counts, pids and times
    send_msg(id, 1, "abc", 0);
    send_msg(id, 1, "defgh", 0);
    struct msqid_ds sent = stat_of(id);
    receive(id, 0, 64, 0);
    struct msqid_ds received = stat_of(id);
    receive(id, 0, 64, 0);
    snprintf(detail, sizeof(detail), "sent: qnum %lu cbytes %lu lspid %d stime %+ld; received: qnum %lu cbytes %lu "
             "lrpid %d rtime %+ld", (unsigned long) sent.msg_qnum, (unsigned long) sent.__msg_cbytes,
             sent.msg_lspid, (long) (sent.msg_stime - start), (unsigned long) received.msg_qnum,
             (unsigned long) received.__msg_cbytes, received.msg_lrpid, (long) (received.msg_rtime - start));
    check("IPC_STAT: counts, pids and times", sent.msg_qnum == 2 && sent.__msg_cbytes == 8 &&
          sent.msg_lspid == getpid() && sent.msg_stime >= start - 1 && received.msg_qnum == 1 &&
          received.__msg_cbytes == 5 && received.msg_lrpid == getpid() && received.msg_rtime >= start - 1, detail);

    // 6. a full queue: EAGAIN with IPC_NOWAIT; more room than msgmnb is
    // root's to give
    ds = stat_of(id);
    ds.msg_qbytes = 100;
    int set = msgctl(id, IPC_SET, &ds);
    char sixty[61];
    memset(sixty, 'x', 60);
    sixty[60] = 0;
    const char *first = send_msg(id, 1, sixty, IPC_NOWAIT);
    const char *full = send_msg(id, 1, sixty, IPC_NOWAIT);
    unsigned long qbytes = stat_of(id).msg_qbytes;
    ds.msg_qbytes = 16385;
    int more = msgctl(id, IPC_SET, &ds);
    int more_errno = errno;
    int more_ok = geteuid() == 0 ? more == 0 : more < 0 && more_errno == EPERM;
    ds.msg_qbytes = 100;
    msgctl(id, IPC_SET, &ds);
    snprintf(detail, sizeof(detail), "IPC_SET %d, qbytes %lu, send %s, then %s; qbytes 16385: %s", set, qbytes, first,
             full, more == 0 ? "ok" : errname(more_errno));
    check("a full queue: EAGAIN; IPC_SET's qbytes", set == 0 && qbytes == 100 && strcmp(first, "ok") == 0 &&
          strcmp(full, "EAGAIN") == 0 && more_ok, detail);

    // 7. a sender waiting for room: a receive makes it
    pid_t child = fork();
    if (child == 0)
        _exit(strcmp(send_msg(id, 1, sixty, 0), "ok") != 0);
    pause_ms(100);
    int waited = running(child);
    receive(id, 0, 64, 0);
    int status = status_of(child);
    unsigned long queued = stat_of(id).msg_qnum;
    receive(id, 0, 64, 0);
    snprintf(detail, sizeof(detail), "waiting %d, then status %d, %lu queued", waited, status, queued);
    check("a full queue: the sender waits for room", waited && status == 0 && queued == 1, detail);

    // 8. a receiver waiting for its type: a send of another does not wake it
    child = fork();
    if (child == 0)
        _exit(strcmp(receive(id, 9, 64, 0), "9:late") != 0);
    pause_ms(100);
    send_msg(id, 3, "other", 0);
    pause_ms(100);
    waited = running(child);
    send_msg(id, 9, "late", 0);
    status = status_of(child);
    const char *other = receive(id, 0, 64, IPC_NOWAIT);
    snprintf(detail, sizeof(detail), "waiting after another type %d, then status %d; left %s", waited, status, other);
    check("a receiver waits for its type", waited && status == 0 && strcmp(other, "3:other") == 0, detail);

    // 9. a handler's signal ends the wait with EINTR, even with SA_RESTART
    child = fork();
    if (child == 0) {
        struct sigaction sa = {.sa_handler = on_usr1, .sa_flags = SA_RESTART};
        sigaction(SIGUSR1, &sa, NULL);
        _exit(strcmp(receive(id, 0, 64, 0), "EINTR") != 0);
    }
    pause_ms(100);
    kill(child, SIGUSR1);
    status = status_of(child);
    snprintf(detail, sizeof(detail), "status %d", status);
    check("a handler's signal: EINTR, even with SA_RESTART", status == 0, detail);

    // 10. stopped and continued, without a handler: still waiting
    child = fork();
    if (child == 0)
        _exit(strcmp(receive(id, 4, 64, 0), "4:after") != 0);
    pause_ms(100);
    kill(child, SIGSTOP);
    int stopped = 0;
    waitpid(child, &stopped, WUNTRACED);
    kill(child, SIGCONT);
    pause_ms(100);
    waited = running(child);
    send_msg(id, 4, "after", 0);
    status = status_of(child);
    snprintf(detail, sizeof(detail), "stopped %d, waiting after SIGCONT %d, then status %d", WIFSTOPPED(stopped),
             waited, status);
    check("stopped and continued: the wait goes on", WIFSTOPPED(stopped) && waited && status == 0, detail);

    // 11. IPC_RMID: the messages go, and the waiting get EIDRM
    send_msg(id, 1, "gone", 0);
    child = fork();
    if (child == 0)
        _exit(strcmp(receive(id, 8, 64, 0), "EIDRM") != 0);
    pause_ms(100);
    int removed = msgctl(id, IPC_RMID, NULL);
    status = status_of(child);
    snprintf(detail, sizeof(detail), "IPC_RMID %d, the waiting child %d, the id after: %s", removed, status,
             send_msg(id, 1, "x", IPC_NOWAIT));
    check("IPC_RMID: EIDRM for the waiting", removed == 0 && status == 0 && strstr(detail, "after: EINVAL"), detail);

    // 12. a key: found again, IPC_EXCL, ENOENT
    int by_key = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    int again = msgget(key, 0);
    int excl = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    int excl_errno = errno;
    int missing = msgget(key + 1, 0);
    int missing_errno = errno;
    snprintf(detail, sizeof(detail), "created %d, again %d, IPC_EXCL %d (%s), missing %d (%s)", by_key, again, excl,
             errname(excl_errno), missing, errname(missing_errno));
    check("msgget by key", by_key >= 0 && again == by_key && excl < 0 && excl_errno == EEXIST && missing < 0 &&
          missing_errno == ENOENT, detail);

    // 13. as ipcs lists them: MSG_INFO's highest index and use, MSG_STAT and
    // MSG_STAT_ANY by index; IPC_INFO's limits
    send_msg(by_key, 1, "abcd", 0);
    struct msginfo info;
    int max = msgctl(0, MSG_INFO, (struct msqid_ds *) &info);
    int found = 0, found_any = 0;
    for (int index = 0; index <= max; index++) {
        if (msgctl(index, MSG_STAT, &ds) == by_key && ds.msg_perm.__key == key)
            found = 1;
        if (msgctl(index, MSG_STAT_ANY, &ds) == by_key)
            found_any = 1;
    }
    struct msginfo limits;
    int ipc_info = msgctl(0, IPC_INFO, (struct msqid_ds *) &limits);
    snprintf(detail, sizeof(detail), "MSG_INFO %d: queues %d, messages %d, bytes %d; MSG_STAT found it %d, "
             "MSG_STAT_ANY %d; IPC_INFO %d: msgmax %d msgmnb %d msgmni %d msgssz %d msgseg %d", max, info.msgpool,
             info.msgmap, info.msgtql, found, found_any, ipc_info, limits.msgmax, limits.msgmnb, limits.msgmni,
             limits.msgssz, limits.msgseg);
    check("MSG_INFO, MSG_STAT, MSG_STAT_ANY and IPC_INFO, as ipcs", max >= 0 && info.msgpool >= 1 &&
          info.msgmap >= 1 && info.msgtql >= 4 && found && found_any && ipc_info >= 0 && limits.msgmax == 8192 &&
          limits.msgmnb == 16384 && limits.msgssz == 16, detail);
    msgctl(by_key, IPC_RMID, NULL);

    // 14. permissions: no one but root may write a 0400 queue, nor read a
    // 0200 one, its owner neither; IPC_RMID is its owner's
    id = msgget(IPC_PRIVATE, 0400);
    int id2 = msgget(IPC_PRIVATE, 0200);
    int fds[2];
    pipe(fds);
    child = fork();
    if (child == 0) {
        // as another user if root, else as the owner
        int other = getuid() == 0;
        if (other) {
            setgid(1000);
            setuid(1000);
        }
        const char *sent = send_msg(id, 1, "x", IPC_NOWAIT);
        const char *got = receive(id2, 0, 64, IPC_NOWAIT);
        int not_owner = other ? (msgctl(id, IPC_RMID, NULL) < 0 ? errno : 0) : EPERM;
        dprintf(fds[1], "send %s, receive %s, IPC_RMID by another user %s", sent, got, errname(not_owner));
        _exit(0);
    }
    close(fds[1]);
    status = status_of(child);
    ssize_t n = read(fds[0], detail, sizeof(detail) - 1);
    detail[n > 0 ? n : 0] = 0;
    close(fds[0]);
    msgctl(id, IPC_RMID, NULL);
    msgctl(id2, IPC_RMID, NULL);
    check("permissions: EACCES, EPERM", status == 0 &&
          strcmp(detail, "send EACCES, receive EACCES, IPC_RMID by another user EPERM") == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
