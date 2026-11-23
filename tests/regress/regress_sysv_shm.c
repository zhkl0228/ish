// Regression test for System V shared memory: shmget, shmat, shmdt, shmctl.
//
// They were stubs that failed with ENOSYS (PostgreSQL needs a segment to
// start, X11's MIT-SHM too; ipcs said "kernel not configured for shared
// memory"). Segments are now shared across fork and between processes that
// attach the same key or id; a removed one can still be attached by id until
// its last process detaches it, as on Linux. ipcs lists them with SHM_INFO
// and SHM_STAT. SHM_LOCK and SHM_UNLOCK set and clear SHM_LOCKED.
//
// Prints one line per case and "N passed, M failed". Natively passes, as root
// or not.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_sysv_shm regress_sysv_shm.c

#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SHM_STAT_ANY
#define SHM_STAT_ANY 15
#endif
#ifndef SHM_LOCKED
#define SHM_LOCKED 02000
#endif

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

static unsigned long nattch(int id) {
    struct shmid_ds ds;
    if (shmctl(id, IPC_STAT, &ds) < 0)
        return (unsigned long) -1;
    return ds.shm_nattch;
}

// runs fn in a child; its exit status, or 128 + the signal that killed it
static int in_child(int (*fn)(int), int arg) {
    pid_t child = fork();
    if (child == 0)
        _exit(fn(arg));
    int status;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int child_writes(int id) {
    char *p = shmat(id, NULL, 0);
    if (p == (void *) -1)
        return 1;
    strcpy(p + 100, "from the child");
    return 0;
}

static int child_writes_read_only(int id) {
    char *p = shmat(id, NULL, SHM_RDONLY);
    if (p == (void *) -1)
        return 1;
    p[0] = 'x';
    return 0;
}

static int other_user(int id) {
    if (getuid() == 0) {
        setgid(1000);
        setuid(1000);
    }
    // the segment is 0400: nobody else may write it, not even its owner
    if (shmat(id, NULL, 0) != (void *) -1 || errno != EACCES)
        return 1;
    return 0;
}

static int other_user_removes(int id) {
    setgid(1000);
    setuid(1000);
    return shmctl(id, IPC_RMID, NULL) < 0 && errno == EPERM ? 0 : 1;
}

int main(void) {
    char detail[200];
    key_t key = 0x5e5e0000 + (getpid() & 0xffff);

    // 1. a private segment, shared with a child
    int id = shmget(IPC_PRIVATE, 10000, IPC_CREAT | 0600);
    char *p = id >= 0 ? shmat(id, NULL, 0) : (void *) -1;
    unsigned long attached = nattch(id);
    int status = p != (void *) -1 ? in_child(child_writes, id) : -1;
    snprintf(detail, sizeof(detail), "shmget %d, shmat %s, nattch %lu, child %d, read \"%.20s\"", id,
             p != (void *) -1 ? "ok" : strerror(errno), attached, status, p != (void *) -1 ? p + 100 : "");
    check("a private segment: shared with a child", id >= 0 && p != (void *) -1 && attached == 1 && status == 0 &&
          strcmp(p + 100, "from the child") == 0, detail);

    // 2. nattch: a fork attaches it too, until the child exits
    int sync[2];
    pipe(sync);
    pid_t child = fork();
    if (child == 0) {
        char c;
        read(sync[0], &c, 1);
        _exit(0);
    }
    unsigned long with_child = nattch(id);
    write(sync[1], "x", 1);
    waitpid(child, NULL, 0);
    unsigned long after_child = nattch(id);
    int detached = shmdt(p);
    unsigned long after_detach = nattch(id);
    snprintf(detail, sizeof(detail), "with a forked child %lu, after it exits %lu, after shmdt %d: %lu", with_child,
             after_child, detached, after_detach);
    check("nattch counts forks, exits and shmdt", with_child == 2 && after_child == 1 && detached == 0 &&
          after_detach == 0, detail);
    shmctl(id, IPC_RMID, NULL);

    // 3. a key: found again, IPC_EXCL, ENOENT, a larger size
    int by_key = shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0600);
    int again = shmget(key, 4096, 0);
    int excl = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    int excl_errno = errno;
    int larger = shmget(key, 16384, 0);
    int larger_errno = errno;
    int missing = shmget(key + 1, 4096, 0);
    int missing_errno = errno;
    snprintf(detail, sizeof(detail), "created %d, again %d, IPC_EXCL %d (%s), larger %d (%s), missing %d (%s)", by_key,
             again, excl, strerror(excl_errno), larger, strerror(larger_errno), missing, strerror(missing_errno));
    check("shmget by key", by_key >= 0 && again == by_key && excl < 0 && excl_errno == EEXIST && larger < 0 &&
          larger_errno == EINVAL && missing < 0 && missing_errno == ENOENT, detail);

    // 4. IPC_STAT
    struct shmid_ds ds;
    int stat = shmctl(by_key, IPC_STAT, &ds);
    snprintf(detail, sizeof(detail), "rc %d: key %#x, mode %o, size %zu, cpid %d (us %d), uid %d", stat,
             ds.shm_perm.__key, ds.shm_perm.mode, ds.shm_segsz, ds.shm_cpid, getpid(), ds.shm_perm.uid);
    check("IPC_STAT", stat == 0 && ds.shm_perm.__key == key && (ds.shm_perm.mode & 0777) == 0600 &&
          ds.shm_segsz == 8192 && ds.shm_cpid == getpid() && ds.shm_perm.uid == geteuid(), detail);

    // 5. IPC_RMID while attached: the key is free, the id still attaches
    // until the last detach
    p = shmat(by_key, NULL, 0);
    strcpy(p, "kept");
    int removed = shmctl(by_key, IPC_RMID, NULL);
    int key_gone = shmget(key, 4096, 0) < 0 && errno == ENOENT;
    shmctl(by_key, IPC_STAT, &ds);
    char *second = shmat(by_key, NULL, 0);
    int same = second != (void *) -1 && strcmp(second, "kept") == 0;
    shmdt(second);
    shmdt(p);
    int gone = shmctl(by_key, IPC_STAT, &ds) < 0 && errno == EINVAL;
    snprintf(detail, sizeof(detail), "rmid %d, key free %d, mode %o key %#x, attached again %d, gone after %d",
             removed, key_gone, ds.shm_perm.mode, ds.shm_perm.__key, same, gone);
    check("IPC_RMID: attachable by id until the last detach", removed == 0 && key_gone && same && gone, detail);

    // 6. SHM_RDONLY: a write is SIGSEGV
    id = shmget(IPC_PRIVATE, 4096, 0600);
    status = in_child(child_writes_read_only, id);
    snprintf(detail, sizeof(detail), "child status %d", status);
    check("SHM_RDONLY: a write faults", status == 128 + SIGSEGV, detail);

    // 7. an address: given, rounded with SHM_RND, taken without SHM_REMAP
    char *hole = mmap(NULL, 3 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(hole, 3 * 4096);
    char *at = shmat(id, hole + 4096 + 123, SHM_RND);
    char *taken = shmat(id, hole + 4096, 0);
    int taken_errno = errno;
    char *remapped = shmat(id, hole + 4096, SHM_REMAP);
    char *unaligned = shmat(id, hole + 123, 0);
    int unaligned_errno = errno;
    snprintf(detail, sizeof(detail), "SHM_RND %p (want %p), taken %p (%s), SHM_REMAP %p, unaligned %p (%s)", at,
             hole + 4096, taken, strerror(taken_errno), remapped, unaligned, strerror(unaligned_errno));
    check("shmat at an address", at == hole + 4096 && taken == (void *) -1 && taken_errno == EINVAL &&
          remapped == hole + 4096 && unaligned == (void *) -1 && unaligned_errno == EINVAL, detail);

    // 8. shmdt of what is not a segment's start
    char *anon = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int dt_anon = shmdt(anon);
    int dt_anon_errno = errno;
    int dt_inside = shmdt(remapped + 1);
    int dt_inside_errno = errno;
    snprintf(detail, sizeof(detail), "anonymous memory %d (%s), inside one %d (%s)", dt_anon, strerror(dt_anon_errno),
             dt_inside, strerror(dt_inside_errno));
    check("shmdt elsewhere: EINVAL", dt_anon < 0 && dt_anon_errno == EINVAL && dt_inside < 0 &&
          dt_inside_errno == EINVAL, detail);
    shmdt(remapped);

    // 9. MADV_DONTNEED keeps a segment's contents; /proc/self/maps names it
    p = shmat(id, NULL, 0);
    strcpy(p, "still here");
    madvise(p, 4096, MADV_DONTNEED);
    char line[256], name[64] = "";
    FILE *maps = fopen("/proc/self/maps", "r");
    unsigned long start;
    while (fgets(line, sizeof(line), maps) != NULL)
        if (sscanf(line, "%lx-", &start) == 1 && start == (unsigned long) p)
            snprintf(name, sizeof(name), "%s", strchr(line, '/') != NULL ? strchr(line, '/') : "(none)");
    fclose(maps);
    name[strcspn(name, "\n")] = '\0';
    snprintf(detail, sizeof(detail), "after MADV_DONTNEED \"%s\", maps \"%s\"", p, name);
    check("MADV_DONTNEED keeps it; named in /proc/self/maps", strcmp(p, "still here") == 0 &&
          strncmp(name, "/SYSV00000000", 13) == 0, detail);
    shmdt(p);
    shmctl(id, IPC_RMID, NULL);

    // 10. as ipcs lists them: SHM_INFO's highest index, SHM_STAT by index
    id = shmget(key + 2, 4096, IPC_CREAT | 0640);
    struct shm_info info;
    int max = shmctl(0, SHM_INFO, (struct shmid_ds *) &info);
    int found = 0;
    for (int index = 0; index <= max; index++) {
        int listed = shmctl(index, SHM_STAT, &ds);
        if (listed == id && ds.shm_perm.__key == key + 2 && (ds.shm_perm.mode & 0777) == 0640)
            found = 1;
    }
    struct shminfo limits;
    int ipc_info = shmctl(0, IPC_INFO, (struct shmid_ds *) &limits);
    shmctl(id, IPC_RMID, NULL);
    snprintf(detail, sizeof(detail), "SHM_INFO %d (used %d), SHM_STAT found it %d; IPC_INFO %d, shmmni %lu", max,
             info.used_ids, found, ipc_info, (unsigned long) limits.shmmni);
    check("SHM_INFO, SHM_STAT and IPC_INFO, as ipcs", max >= 0 && info.used_ids >= 1 && found && ipc_info >= 0 &&
          limits.shmmni > 0, detail);

    // 11. permissions
    id = shmget(IPC_PRIVATE, 4096, 0400);
    int denied = in_child(other_user, id);
    int not_owner = getuid() == 0 ? in_child(other_user_removes, id) : 0;
    shmctl(id, IPC_RMID, NULL);
    snprintf(detail, sizeof(detail), "write attach %d, IPC_RMID by another user %d", denied, not_owner);
    check("permissions: EACCES, EPERM", denied == 0 && not_owner == 0, detail);

    // 12. SHM_LOCK and SHM_UNLOCK: SHM_LOCKED in its mode; SHM_STAT_ANY;
    // IPC_SET of an owner -1
    id = shmget(IPC_PRIVATE, 4096, 0600);
    int locked = shmctl(id, SHM_LOCK, NULL);
    shmctl(id, IPC_STAT, &ds);
    unsigned mode_locked = ds.shm_perm.mode;
    int unlocked = shmctl(id, SHM_UNLOCK, NULL);
    shmctl(id, IPC_STAT, &ds);
    unsigned mode_unlocked = ds.shm_perm.mode;
    max = shmctl(0, SHM_INFO, (struct shmid_ds *) &info);
    int stat_any = 0;
    struct shmid_ds any;
    for (int index = 0; index <= max; index++)
        if (shmctl(index, SHM_STAT_ANY, &any) == id)
            stat_any = 1;
    ds.shm_perm.uid = (uid_t) -1;
    int set_bad = shmctl(id, IPC_SET, &ds);
    int set_bad_errno = errno;
    shmctl(id, IPC_RMID, NULL);
    snprintf(detail, sizeof(detail), "SHM_LOCK %d: mode %#o, SHM_UNLOCK %d: mode %#o; SHM_STAT_ANY found it %d; "
             "IPC_SET uid -1 %d (%s)", locked, mode_locked, unlocked, mode_unlocked, stat_any, set_bad,
             strerror(set_bad_errno));
    check("SHM_LOCK, SHM_UNLOCK, SHM_STAT_ANY; IPC_SET's owner -1", locked == 0 && (mode_locked & SHM_LOCKED) &&
          unlocked == 0 && !(mode_unlocked & SHM_LOCKED) && (mode_unlocked & 0777) == 0600 && stat_any &&
          set_bad < 0 && set_bad_errno == EINVAL, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
