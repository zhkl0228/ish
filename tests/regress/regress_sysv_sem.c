// Regression test for System V semaphores: semget, semop, semtimedop,
// semctl.
//
// They were stubs that failed with ENOSYS. A semop applies all its
// operations or none; one that would block waits (GETNCNT and GETZCNT count it
// by the operation it is blocked on) or, with IPC_NOWAIT, fails with EAGAIN;
// semtimedop's timeout ends the wait with EAGAIN, IPC_RMID with EIDRM, a
// signal with EINTR. SEM_UNDO's adjustments are applied when the process
// exits, not one of its threads, before its parent can wait for it; SETVAL
// clears them; a child of fork has none.
//
// Prints one line per case and "N passed, M failed". Natively passes, as root
// or not.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_sysv_sem regress_sysv_sem.c

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SEM_STAT_ANY
#define SEM_STAT_ANY 20
#endif

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *info;
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
        case 0: return "ok";
        case EIDRM: return "EIDRM";
        case EINTR: return "EINTR";
        case EAGAIN: return "EAGAIN";
        case EINVAL: return "EINVAL";
        case EACCES: return "EACCES";
        case EPERM: return "EPERM";
        case EEXIST: return "EEXIST";
        case ENOENT: return "ENOENT";
        case ERANGE: return "ERANGE";
        case EFBIG: return "EFBIG";
        case E2BIG: return "E2BIG";
        case ENOSYS: return "ENOSYS";
    }
    static char other[32];
    snprintf(other, sizeof(other), "errno %d", err);
    return other;
}

// one operation: "ok" or the error
static const char *op(int id, unsigned short num, short val, short flg) {
    struct sembuf sop = {num, val, flg};
    return errname(semop(id, &sop, 1) == 0 ? 0 : errno);
}

static int get(int id, int num, int cmd) {
    return semctl(id, num, cmd);
}

static const char *set_val(int id, int num, int val) {
    return errname(semctl(id, num, SETVAL, (union semun) {.val = val}) == 0 ? 0 : errno);
}

static void values(int id, char *out, size_t size) {
    unsigned short vals[3];
    semctl(id, 0, GETALL, (union semun) {.array = vals});
    snprintf(out, size, "%u %u %u", vals[0], vals[1], vals[2]);
}

static void pause_ms(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

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

static int thread_set;

static void *thread_takes(void *arg) {
    (void) arg;
    op(thread_set, 0, -1, SEM_UNDO);
    return NULL;
}

int main(void) {
    char detail[300], vals[40];
    key_t key = 0x73650000 + (getpid() & 0xffff);
    // The times the kernel stamps (sem_otime) are of a coarse clock, as Linux's
    // are (ktime_get_real_seconds) and ish's on a Linux host (glibc's time()):
    // for the few milliseconds a second has begun, still the second before,
    // which time() here (CLOCK_REALTIME) is past. One second before start is
    // the same moment (CI's linux job, 2026-11-27: otime -1).
    time_t start = time(NULL);

    // 1. a private set: IPC_STAT, all zero
    int id = semget(IPC_PRIVATE, 3, 0600);
    struct semid_ds ds = {0};
    int stat = semctl(id, 0, IPC_STAT, (union semun) {.buf = &ds});
    values(id, vals, sizeof(vals));
    snprintf(detail, sizeof(detail), "semget %d, IPC_STAT %d: nsems %lu mode %#o otime %ld; values %s", id, stat,
             (unsigned long) ds.sem_nsems, ds.sem_perm.mode, (long) ds.sem_otime, vals);
    check("a private set: IPC_STAT, all zero", id >= 0 && stat == 0 && ds.sem_nsems == 3 &&
          (ds.sem_perm.mode & 0777) == 0600 && ds.sem_otime == 0 && strcmp(vals, "0 0 0") == 0, detail);

    // 2. SETVAL, GETVAL, SETALL, GETALL, GETPID; out of range
    const char *setval = set_val(id, 0, 5);
    int got = get(id, 0, GETVAL);
    int pid = get(id, 0, GETPID);
    const char *too_big = set_val(id, 0, 32768);
    const char *negative = set_val(id, 0, -1);
    const char *no_such = set_val(id, 3, 1);
    unsigned short all[3] = {1, 2, 3};
    int setall = semctl(id, 0, SETALL, (union semun) {.array = all});
    values(id, vals, sizeof(vals));
    unsigned short over[3] = {1, 32768, 3};
    int setall_over = semctl(id, 0, SETALL, (union semun) {.array = over});
    int setall_over_errno = errno;
    char after[40];
    values(id, after, sizeof(after));
    snprintf(detail, sizeof(detail), "SETVAL %s, GETVAL %d, GETPID %d; 32768 %s, -1 %s, num 3 %s; SETALL %d: %s; "
             "with 32768 %s: %s", setval, got, pid, too_big, negative, no_such, setall, vals,
             setall_over < 0 ? errname(setall_over_errno) : "ok", after);
    check("SETVAL, GETVAL, SETALL, GETALL, GETPID; ERANGE", strcmp(setval, "ok") == 0 && got == 5 &&
          pid == getpid() && strcmp(too_big, "ERANGE") == 0 && strcmp(negative, "ERANGE") == 0 &&
          strcmp(no_such, "EINVAL") == 0 && setall == 0 && strcmp(vals, "1 2 3") == 0 && setall_over < 0 &&
          setall_over_errno == ERANGE && strcmp(after, "1 2 3") == 0, detail);

    // 3. all or none: one that would block leaves the others undone; the same
    // semaphore twice goes in order
    unsigned short start_vals[3] = {1, 0, 0};
    semctl(id, 0, SETALL, (union semun) {.array = start_vals});
    struct sembuf blocked[] = {{0, -1, IPC_NOWAIT}, {1, -1, IPC_NOWAIT}};
    const char *none = errname(semop(id, blocked, 2) == 0 ? 0 : errno);
    char after_none[40];
    values(id, after_none, sizeof(after_none));
    struct sembuf both[] = {{0, -1, 0}, {1, 2, 0}};
    const char *done = errname(semop(id, both, 2) == 0 ? 0 : errno);
    char after_both[40];
    values(id, after_both, sizeof(after_both));
    const char *zero_nowait = op(id, 1, 0, IPC_NOWAIT);
    const char *zero = op(id, 2, 0, 0);
    struct sembuf twice[] = {{1, -1, 0}, {1, -1, 0}};
    const char *twice_res = errname(semop(id, twice, 2) == 0 ? 0 : errno);
    struct sembuf up_down[] = {{1, 1, 0}, {1, -2, IPC_NOWAIT}};
    const char *up_down_res = errname(semop(id, up_down, 2) == 0 ? 0 : errno);
    values(id, vals, sizeof(vals));
    snprintf(detail, sizeof(detail), "blocked %s: %s; both %s: %s; zero of 2 %s, zero of 0 %s; twice %s, "
             "+1 -2 %s: %s", none, after_none, done, after_both, zero_nowait, zero, twice_res, up_down_res, vals);
    check("semop: all or none, in order", strcmp(none, "EAGAIN") == 0 && strcmp(after_none, "1 0 0") == 0 &&
          strcmp(done, "ok") == 0 && strcmp(after_both, "0 2 0") == 0 && strcmp(zero_nowait, "EAGAIN") == 0 &&
          strcmp(zero, "ok") == 0 && strcmp(twice_res, "ok") == 0 && strcmp(up_down_res, "EAGAIN") == 0 &&
          strcmp(vals, "0 0 0") == 0, detail);

    // 4. ERANGE past 32767, EFBIG, E2BIG, EINVAL
    set_val(id, 0, 32767);
    const char *range = op(id, 0, 1, IPC_NOWAIT);
    const char *fbig = op(id, 3, 1, IPC_NOWAIT);
    struct sembuf many[501] = {{0}};
    const char *e2big = errname(semop(id, many, 501) == 0 ? 0 : errno);
    const char *nothing = errname(semop(id, many, 0) == 0 ? 0 : errno);
    const char *bad_id = op(id + 1, 0, 1, IPC_NOWAIT);
    set_val(id, 0, 0);
    snprintf(detail, sizeof(detail), "past 32767 %s, num 3 %s, 501 ops %s, none %s, bad id %s", range, fbig, e2big,
             nothing, bad_id);
    check("ERANGE, EFBIG, E2BIG, EINVAL", strcmp(detail, "past 32767 ERANGE, num 3 EFBIG, 501 ops E2BIG, "
          "none EINVAL, bad id EINVAL") == 0, detail);

    // 5. GETPID and otime: the last to operate
    pid_t child = fork();
    if (child == 0)
        _exit(strcmp(op(id, 2, 1, 0), "ok") != 0);
    int status = status_of(child);
    pid = get(id, 2, GETPID);
    semctl(id, 0, IPC_STAT, (union semun) {.buf = &ds});
    snprintf(detail, sizeof(detail), "child %d status %d, GETPID %d, otime %+ld", child, status, pid,
             (long) (ds.sem_otime - start));
    check("GETPID and otime: the last to operate", status == 0 && pid == child && ds.sem_otime >= start - 1, detail);
    set_val(id, 2, 0);

    // 6. semtimedop: EAGAIN after its timeout; EINVAL for a bad one
    struct sembuf take = {0, -1, 0};
    struct timespec timeout = {0, 150000000}, before, now;
    clock_gettime(CLOCK_MONOTONIC, &before);
    const char *timed = errname(semtimedop(id, &take, 1, &timeout) == 0 ? 0 : errno);
    clock_gettime(CLOCK_MONOTONIC, &now);
    long waited_ms = (now.tv_sec - before.tv_sec) * 1000 + (now.tv_nsec - before.tv_nsec) / 1000000;
    struct timespec bad = {0, 1000000000};
    const char *bad_timeout = errname(semtimedop(id, &take, 1, &bad) == 0 ? 0 : errno);
    snprintf(detail, sizeof(detail), "%s after %ld ms; nsec 1000000000 %s", timed, waited_ms, bad_timeout);
    check("semtimedop: EAGAIN after its timeout", strcmp(timed, "EAGAIN") == 0 && waited_ms >= 140 &&
          waited_ms < 2000 && strcmp(bad_timeout, "EINVAL") == 0, detail);

    // 7. a waiter to decrease, and one for zero: GETNCNT and GETZCNT, then
    // woken
    set_val(id, 1, 1);
    child = fork();
    if (child == 0)
        _exit(strcmp(op(id, 0, -1, 0), "ok") != 0);
    pid_t child2 = fork();
    if (child2 == 0)
        _exit(strcmp(op(id, 1, 0, 0), "ok") != 0);
    pause_ms(100);
    int ncnt = get(id, 0, GETNCNT), zcnt = get(id, 1, GETZCNT), others = get(id, 0, GETZCNT) + get(id, 1, GETNCNT);
    int waiting = running(child) && running(child2);
    op(id, 0, 1, 0);
    op(id, 1, -1, 0);
    status = status_of(child);
    int status2 = status_of(child2);
    int ncnt_after = get(id, 0, GETNCNT), zcnt_after = get(id, 1, GETZCNT);
    values(id, vals, sizeof(vals));
    snprintf(detail, sizeof(detail), "waiting %d: GETNCNT %d GETZCNT %d others %d; then %d %d, counts %d %d, %s",
             waiting, ncnt, zcnt, others, status, status2, ncnt_after, zcnt_after, vals);
    check("waiters: GETNCNT and GETZCNT, then woken", waiting && ncnt == 1 && zcnt == 1 && others == 0 &&
          status == 0 && status2 == 0 && ncnt_after == 0 && zcnt_after == 0 && strcmp(vals, "0 0 0") == 0, detail);

    // 8. a waiter of several operations is counted by the one it is blocked on
    set_val(id, 0, 1);
    child = fork();
    if (child == 0) {
        struct sembuf ops[] = {{0, -1, 0}, {1, -1, 0}};
        _exit(semop(id, ops, 2) != 0);
    }
    pause_ms(100);
    int on_first = get(id, 0, GETNCNT), on_second = get(id, 1, GETNCNT);
    op(id, 1, 1, 0);
    status = status_of(child);
    values(id, vals, sizeof(vals));
    snprintf(detail, sizeof(detail), "GETNCNT of 0 %d, of 1 %d; then %d: %s", on_first, on_second, status, vals);
    check("counted by the operation it is blocked on", on_first == 0 && on_second == 1 && status == 0 &&
          strcmp(vals, "0 0 0") == 0, detail);

    // 9. SEM_UNDO: undone when the process exits, before its parent's wait
    // returns
    set_val(id, 0, 1);
    child = fork();
    if (child == 0) {
        op(id, 0, -1, SEM_UNDO);
        op(id, 1, 2, SEM_UNDO);
        op(id, 1, -1, SEM_UNDO);
        _exit(get(id, 0, GETVAL) != 0 || get(id, 1, GETVAL) != 1);
    }
    status = status_of(child);
    values(id, vals, sizeof(vals));
    snprintf(detail, sizeof(detail), "child %d, then %s", status, vals);
    check("SEM_UNDO: undone at the process's exit", status == 0 && strcmp(vals, "1 0 0") == 0, detail);

    // 10. SEM_UNDO: a thread's exit is not the process's
    thread_set = id;
    child = fork();
    if (child == 0) {
        pthread_t thread;
        pthread_create(&thread, NULL, thread_takes, NULL);
        pthread_join(thread, NULL);
        _exit(get(id, 0, GETVAL) != 0);
    }
    status = status_of(child);
    got = get(id, 0, GETVAL);
    snprintf(detail, sizeof(detail), "kept after the thread's exit %d, %d after the process's", status == 0, got);
    check("SEM_UNDO: not at a thread's exit", status == 0 && got == 1, detail);

    // 11. SETVAL clears what SEM_UNDO would undo
    int to_child[2], to_parent[2];
    pipe(to_child);
    pipe(to_parent);
    child = fork();
    if (child == 0) {
        char c;
        op(id, 0, -1, SEM_UNDO);
        write(to_parent[1], "x", 1);
        read(to_child[0], &c, 1);
        _exit(0);
    }
    char c;
    read(to_parent[0], &c, 1);
    set_val(id, 0, 5);
    write(to_child[1], "x", 1);
    status = status_of(child);
    got = get(id, 0, GETVAL);
    snprintf(detail, sizeof(detail), "child %d, then %d", status, got);
    check("SETVAL clears SEM_UNDO's adjustment", status == 0 && got == 5, detail);

    // 12. fork: a child has no SEM_UNDO of its parent's
    set_val(id, 0, 1);
    op(id, 0, -1, SEM_UNDO);
    child = fork();
    if (child == 0)
        _exit(0);
    status = status_of(child);
    got = get(id, 0, GETVAL);
    op(id, 0, 1, SEM_UNDO);
    snprintf(detail, sizeof(detail), "child %d, then %d", status, got);
    check("fork: no SEM_UNDO for the child", status == 0 && got == 0, detail);
    set_val(id, 0, 0);

    // 13. a handler's signal: EINTR, even with SA_RESTART
    child = fork();
    if (child == 0) {
        struct sigaction sa = {.sa_handler = on_usr1, .sa_flags = SA_RESTART};
        sigaction(SIGUSR1, &sa, NULL);
        _exit(strcmp(op(id, 0, -1, 0), "EINTR") != 0);
    }
    pause_ms(100);
    kill(child, SIGUSR1);
    status = status_of(child);
    snprintf(detail, sizeof(detail), "status %d", status);
    check("a handler's signal: EINTR, even with SA_RESTART", status == 0, detail);

    // 14. IPC_RMID: EIDRM for the waiting; the id names nothing then
    child = fork();
    if (child == 0)
        _exit(strcmp(op(id, 0, -1, 0), "EIDRM") != 0);
    pause_ms(100);
    int removed = semctl(id, 0, IPC_RMID);
    status = status_of(child);
    got = get(id, 0, GETVAL);
    snprintf(detail, sizeof(detail), "IPC_RMID %d, the waiting child %d, GETVAL after %d (%s)", removed, status, got,
             errname(errno));
    check("IPC_RMID: EIDRM for the waiting", removed == 0 && status == 0 && got < 0 && errno == EINVAL, detail);

    // 15. a key: found again with fewer or as many, not more; IPC_EXCL,
    // ENOENT; none or a negative number
    int by_key = semget(key, 2, IPC_CREAT | IPC_EXCL | 0600);
    int again = semget(key, 0, 0) == by_key && semget(key, 2, 0) == by_key;
    int more = semget(key, 3, 0);
    int more_errno = errno;
    int excl = semget(key, 2, IPC_CREAT | IPC_EXCL | 0600);
    int excl_errno = errno;
    int missing = semget(key + 1, 2, 0);
    int missing_errno = errno;
    int empty = semget(IPC_PRIVATE, 0, 0600);
    int empty_errno = errno;
    int negative_n = semget(IPC_PRIVATE, -1, 0600);
    int negative_errno = errno;
    snprintf(detail, sizeof(detail), "created %d, again %d, 3 %s, IPC_EXCL %s, missing %s, 0 %s, -1 %s", by_key,
             again, more < 0 ? errname(more_errno) : "ok", excl < 0 ? errname(excl_errno) : "ok",
             missing < 0 ? errname(missing_errno) : "ok", empty < 0 ? errname(empty_errno) : "ok",
             negative_n < 0 ? errname(negative_errno) : "ok");
    check("semget by key, and nsems", by_key >= 0 && again && strstr(detail, "3 EINVAL, IPC_EXCL EEXIST, "
          "missing ENOENT, 0 EINVAL, -1 EINVAL"), detail);

    // 16. as ipcs lists them: SEM_INFO's highest index and use, SEM_STAT and
    // SEM_STAT_ANY by index; IPC_INFO's limits
    struct seminfo info;
    int max = semctl(0, 0, SEM_INFO, (union semun) {.info = &info});
    int found = 0, found_any = 0;
    for (int index = 0; index <= max; index++) {
        if (semctl(index, 0, SEM_STAT, (union semun) {.buf = &ds}) == by_key && ds.sem_perm.__key == key)
            found = 1;
        if (semctl(index, 0, SEM_STAT_ANY, (union semun) {.buf = &ds}) == by_key)
            found_any = 1;
    }
    struct seminfo limits;
    int ipc_info = semctl(0, 0, IPC_INFO, (union semun) {.info = &limits});
    snprintf(detail, sizeof(detail), "SEM_INFO %d: sets %d, semaphores %d; SEM_STAT found it %d, SEM_STAT_ANY %d; "
             "IPC_INFO %d: semmsl %d semopm %d semvmx %d semaem %d semume %d semusz %d", max, info.semusz,
             info.semaem, found, found_any, ipc_info, limits.semmsl, limits.semopm, limits.semvmx, limits.semaem,
             limits.semume, limits.semusz);
    check("SEM_INFO, SEM_STAT, SEM_STAT_ANY and IPC_INFO, as ipcs", max >= 0 && info.semusz >= 1 &&
          info.semaem >= 2 && found && found_any && ipc_info >= 0 && limits.semvmx == 32767 &&
          limits.semaem == 32767 && limits.semusz == 20, detail);
    semctl(by_key, 0, IPC_RMID);

    // 17. permissions: no one but root may change a 0400 set, nor read a 0200
    // one, its owner neither; IPC_RMID is its owner's
    id = semget(IPC_PRIVATE, 1, 0400);
    int id2 = semget(IPC_PRIVATE, 1, 0200);
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
        const char *up = op(id, 0, 1, IPC_NOWAIT);
        const char *setval_denied = set_val(id, 0, 1);
        const char *wait_zero = op(id2, 0, 0, IPC_NOWAIT);
        int getval = get(id2, 0, GETVAL);
        int getval_errno = errno;
        int not_owner = other ? (semctl(id, 0, IPC_RMID) < 0 ? errno : 0) : EPERM;
        dprintf(fds[1], "semop +1 %s, SETVAL %s; semop 0 %s, GETVAL %s; IPC_RMID by another user %s", up,
                setval_denied, wait_zero, getval < 0 ? errname(getval_errno) : "ok", errname(not_owner));
        _exit(0);
    }
    close(fds[1]);
    status = status_of(child);
    ssize_t n = read(fds[0], detail, sizeof(detail) - 1);
    detail[n > 0 ? n : 0] = 0;
    close(fds[0]);
    semctl(id, 0, IPC_RMID);
    semctl(id2, 0, IPC_RMID);
    check("permissions: EACCES, EPERM", status == 0 && strcmp(detail, "semop +1 EACCES, SETVAL EACCES; semop 0 "
          "EACCES, GETVAL EACCES; IPC_RMID by another user EPERM") == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
