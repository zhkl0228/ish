// Regression test for /dev/shm, a fakefs made afresh at each boot on a
// fakefs root.
//
//   regress_shm_mount leave   leaves a file in /dev/shm
//   regress_shm_mount check   (the next boot) checks /dev/shm
//
// /dev/shm was a directory of the root filesystem: what a program left there
// outlived a restart, unlike Linux's tmpfs. It is now a fakefs of its own,
// empty at each boot, and as complete a filesystem as the root: shared mmap,
// named semaphores, files used after unlink (Python's multiprocessing).
//
// check prints one line per case and "N passed, M failed". Natively it
// passes when run alone (Linux's /dev/shm is a tmpfs).
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_shm_mount regress_shm_mount.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
}

int main(int argc, char **argv) {
    char detail[200];
    if (argc > 1 && strcmp(argv[1], "leave") == 0) {
        FILE *f = fopen("/dev/shm/regress_left", "w");
        if (f == NULL) {
            perror("/dev/shm/regress_left");
            return 1;
        }
        fputs("left\n", f);
        fclose(f);
        return 0;
    }

    struct stat st;
    int gone = stat("/dev/shm/regress_left", &st) < 0 && errno == ENOENT;
    int dir = stat("/dev/shm", &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 01777;
    snprintf(detail, sizeof(detail), "left file gone %d, mode %o", gone, st.st_mode);
    check("empty at boot, a directory with mode 01777", gone && dir, detail);

    FILE *mounts = fopen("/proc/mounts", "r");
    char line[512];
    int mounted = 0;
    while (mounts != NULL && fgets(line, sizeof(line), mounts) != NULL) {
        char source[256], point[256];
        if (sscanf(line, "%255s %255s", source, point) == 2 && strcmp(point, "/dev/shm") == 0)
            mounted = 1;
    }
    if (mounts != NULL)
        fclose(mounts);
    check("a filesystem of its own", mounted, "no /dev/shm in /proc/mounts");

    // shm_open, ftruncate, a shared mapping across fork
    int fd = shm_open("/regress_shm", O_CREAT | O_RDWR | O_EXCL, 0600);
    int ok = fd >= 0 && ftruncate(fd, 4096) == 0;
    volatile int *shared = ok ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    int seen = -1;
    if (shared != MAP_FAILED) {
        shared[0] = 0;
        pid_t child = fork();
        if (child == 0) {
            shared[0] = 42;
            _exit(0);
        }
        waitpid(child, NULL, 0);
        seen = shared[0];
    }
    snprintf(detail, sizeof(detail), "shm_open %d, mmap %s, the child's write seen as %d", fd,
             shared != MAP_FAILED ? "ok" : strerror(errno), seen);
    check("shm_open and a shared mapping", seen == 42, detail);
    shm_unlink("/regress_shm");

    // a named semaphore: the child posts, the parent waits
    sem_t *sem = sem_open("/regress_sem", O_CREAT | O_EXCL, 0600, 0);
    int waited = -1;
    if (sem != SEM_FAILED) {
        pid_t child = fork();
        if (child == 0) {
            sem_post(sem);
            _exit(0);
        }
        alarm(5);
        waited = sem_wait(sem);
        alarm(0);
        waitpid(child, NULL, 0);
        sem_close(sem);
        sem_unlink("/regress_sem");
    }
    snprintf(detail, sizeof(detail), "sem_open %s, sem_wait %d", sem != SEM_FAILED ? "ok" : strerror(errno), waited);
    check("a named semaphore across processes", waited == 0, detail);

    // as Python's multiprocessing heap: a file unlinked, then sized and mapped
    char name[] = "/dev/shm/pym-XXXXXX";
    int tmp = mkstemp(name);
    unlink(name);
    int sized = tmp >= 0 ? ftruncate(tmp, 8192) : -1;
    int size_errno = errno;
    fstat(tmp, &st);
    snprintf(detail, sizeof(detail), "ftruncate %d (%s), size %lld", sized, sized == 0 ? "ok" : strerror(size_errno),
             (long long) st.st_size);
    check("a file unlinked while open grows", sized == 0 && st.st_size == 8192, detail);

    // another user can make files there (mode 01777)
    pid_t child = fork();
    if (child == 0) {
        setgid(1000);
        setuid(1000);
        int f = open("/dev/shm/regress_user", O_CREAT | O_WRONLY, 0600);
        _exit(f >= 0 ? 0 : 1);
    }
    int status;
    waitpid(child, &status, 0);
    unlink("/dev/shm/regress_user");
    snprintf(detail, sizeof(detail), "status %#x", status);
    check("uid 1000 makes a file there", WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
