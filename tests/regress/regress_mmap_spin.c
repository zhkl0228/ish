// Regression test for mmap while other threads run guest code without
// syscalls.
//
// A thread running guest code holds its address space's lock for reading
// until it leaves the code, and mmap, munmap and mprotect need it for
// writing. A loop that makes no syscall left the code only when poked, so
// the writer, retrying for a moment without readers, could wait for seconds
// or forever: Go's GC, stopping a goroutine spinning in such a loop, took up
// to 13 s or hung (and iSH injected GOMAXPROCS=2 into every program). A
// waiting writer now pokes the threads running guest code.
//
// With 1 and then 4 threads spinning, 50 mmap and munmap pairs must finish
// within 10 seconds (natively they take well under a millisecond each).
//
// Build: aarch64-linux-musl-gcc -static -O1 -o regress_mmap_spin regress_mmap_spin.c

#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile unsigned long sink;

static void *spin(void *arg) {
    (void) arg;
    unsigned long x = 0;
    for (unsigned long i = 0;; i++) { // no calls, no syscalls
        x += i ^ (x >> 3);
        if (i == ~0UL)
            break;
    }
    sink = x;
    return NULL;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// In a child: with spinners threads spinning, the seconds 50 mmap and munmap
// pairs took, or a kill by SIGALRM after 10.
static int check(int spinners) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        for (int i = 0; i < spinners; i++) {
            pthread_t t;
            pthread_create(&t, NULL, spin, NULL);
        }
        struct timespec settle = {0, 50 * 1000 * 1000};
        nanosleep(&settle, NULL);
        double start = now();
        for (int i = 0; i < 50; i++) {
            void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                _exit(2);
            munmap(p, 1 << 20);
        }
        printf("  %d spinning: %.3f s\n", spinners, now() - start);
        fflush(stdout);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;
    printf("FAIL %d spinning threads: %s %d\n", spinners,
           WIFSIGNALED(status) ? "killed by signal" : "exited", WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    return 0;
}

int main(void) {
    int passed = 0, failed = 0;
    for (int spinners = 1; spinners <= 4; spinners += 3) {
        if (check(spinners))
            passed++;
        else
            failed++;
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
