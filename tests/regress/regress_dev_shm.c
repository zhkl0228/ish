// Regression test for POSIX shared memory and named semaphores.
//
// They are files in /dev/shm (musl's shm_open and sem_open), a tmpfs Linux
// always has. iSH had no /dev/shm, so both failed with ENOENT, and with them
// every lock, queue and pool of Python's multiprocessing. iSH now makes the
// directory at boot.
//
// A named semaphore wakes a forked child's sem_wait, and memory from
// shm_open written by the child is seen by the parent. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_dev_shm regress_dev_shm.c

#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed;

static void check(const char *name, int ok) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", name);
    }
}

int main(void) {
    struct stat st;
    check("/dev/shm is a sticky world-writable directory",
          stat("/dev/shm", &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 01777);

    sem_unlink("/regress_dev_shm");
    sem_t *ready = sem_open("/regress_dev_shm", O_CREAT | O_EXCL, 0600, 0);
    check("sem_open", ready != SEM_FAILED);

    shm_unlink("/regress_dev_shm");
    int fd = shm_open("/regress_dev_shm", O_CREAT | O_EXCL | O_RDWR, 0600);
    check("shm_open", fd >= 0);
    char *shared = MAP_FAILED;
    if (fd >= 0 && ftruncate(fd, 4096) == 0)
        shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap of shm", shared != MAP_FAILED);
    if (ready == SEM_FAILED || shared == MAP_FAILED) {
        printf("%d passed, %d failed\n", passed, failed);
        return 1;
    }

    pid_t child = fork();
    if (child == 0) {
        alarm(10);
        strcpy(shared, "from child");
        sem_post(ready);
        _exit(0);
    }
    alarm(10);
    check("sem_wait woken by the child", sem_wait(ready) == 0);
    check("child's write seen in shared memory", strcmp(shared, "from child") == 0);
    int status;
    waitpid(child, &status, 0);

    check("sem_unlink", sem_unlink("/regress_dev_shm") == 0);
    check("shm_unlink", shm_unlink("/regress_dev_shm") == 0);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
