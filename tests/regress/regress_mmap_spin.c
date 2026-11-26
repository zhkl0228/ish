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
// One that does not is shown before it is killed: the step it was at (the
// child writes it to a shared page), and each thread's state and
// /proc/<pid>/task/<tid>/syscall, twice half a second apart, so that a thread
// spinning (its pc moving) tells from one stuck. A run of the whole suite in
// an iSH in iSH hung once with 1 spinning; 180 runs of this alone did not.
//
// Build: aarch64-linux-musl-gcc -static -O1 -o regress_mmap_spin regress_mmap_spin.c

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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

// Where the child is: what it does, and which pair (shared with the parent)
enum { STARTING, THREADS, SETTLING, MAPPING, UNMAPPING, DONE };
static const char *const steps[] = {"starting", "creating threads", "settling", "in mmap", "in munmap", "done"};
static volatile int *step;

static void read_line(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    buf[0] = '\0';
    if (f == NULL)
        return;
    if (fgets(buf, (int) size, f) == NULL)
        buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
}

// Each thread of pid: its state (stat's third field) and its syscall line
static void show_threads(pid_t pid) {
    char path[64], stat[512], syscall[256];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *dir = opendir(path);
    if (dir == NULL)
        return;
    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        if (d->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/proc/%d/task/%s/stat", pid, d->d_name);
        read_line(path, stat, sizeof(stat));
        snprintf(path, sizeof(path), "/proc/%d/task/%s/syscall", pid, d->d_name);
        read_line(path, syscall, sizeof(syscall));
        char *end = strrchr(stat, ')');
        printf("    thread %s: state %c, syscall %s\n", d->d_name, end != NULL && end[1] ? end[2] : '?', syscall);
    }
    closedir(dir);
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// In a child: with spinners threads spinning, the seconds 50 mmap and munmap
// pairs took; after 10 the child is shown and killed.
static int check(int spinners) {
    fflush(stdout);
    step[0] = STARTING;
    step[1] = 0;
    pid_t pid = fork();
    if (pid == 0) {
        step[0] = THREADS;
        for (int i = 0; i < spinners; i++) {
            pthread_t t;
            pthread_create(&t, NULL, spin, NULL);
        }
        step[0] = SETTLING;
        struct timespec settle = {0, 50 * 1000 * 1000};
        nanosleep(&settle, NULL);
        double start = now();
        for (int i = 0; i < 50; i++) {
            step[1] = i;
            step[0] = MAPPING;
            void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                _exit(2);
            step[0] = UNMAPPING;
            munmap(p, 1 << 20);
        }
        step[0] = DONE;
        printf("  %d spinning: %.3f s\n", spinners, now() - start);
        fflush(stdout);
        _exit(0);
    }
    int status;
    double deadline = now() + 10;
    struct timespec tick = {0, 10 * 1000 * 1000}, half = {0, 500 * 1000 * 1000};
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (now() < deadline) {
            nanosleep(&tick, NULL);
            continue;
        }
        printf("FAIL %d spinning threads: not done in 10 s, %s (pair %d)\n", spinners, steps[step[0]], step[1]);
        show_threads(pid);
        nanosleep(&half, NULL);
        printf("  half a second later: %s (pair %d)\n", steps[step[0]], step[1]);
        show_threads(pid);
        fflush(stdout);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;
    printf("FAIL %d spinning threads: %s %d\n", spinners,
           WIFSIGNALED(status) ? "killed by signal" : "exited", WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    return 0;
}

int main(void) {
    int passed = 0, failed = 0;
    step = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    for (int spinners = 1; spinners <= 4; spinners += 3) {
        if (check(spinners))
            passed++;
        else
            failed++;
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
