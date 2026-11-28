// Regression test for futex waits woken across processes, and for futex
// waits ended by signals, the deadline and exit_group with no periodic
// re-check to fall back on.
//
// iSH knew every futex by its address space and address: a wake never
// reached a waiter in another process on a page they share, which went on
// only at the wait's 100 ms re-check (a round trip of Python's
// multiprocessing.Semaphore took 100-117 ms). A process-shared futex (no
// FUTEX_PRIVATE_FLAG) on a shared page is now known by what the page is: a
// host file and the word's offset in it, or the host memory of shared memory
// of no file. And a signal for a thread in a futex wait writes the wait's
// pipe, so the wait re-checks only every 30 s.
//
// Checked:
// - round trips between two processes through a process-shared futex in
//   MAP_SHARED | MAP_ANONYMOUS memory both have from fork: each within 20 ms
//   on average, every wake seen (waits of 2 s, none timed out);
// - the same through a file each process maps for itself, at another offset
//   of the file each (the word at the file's offset 4096 in both);
// - a private futex on shared memory is the address space's own: a wake in
//   one process does not end a wait in the other;
// - a signal for a thread just going into an untimed futex wait, 200 times:
//   each wait ends (EINTR) within 1 s, its handler run;
// - a timed wait of 200 ms: ETIMEDOUT after 200 to 1000 ms;
// - exit_group with a thread in an untimed futex wait: the process gone
//   within 2 s.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_futex_shared regress_futex_shared.c -lpthread

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROUNDS 50

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

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long futex(atomic_int *word, int op, int val, const struct timespec *timeout) {
    return syscall(SYS_futex, word, op, val, timeout, NULL, 0);
}

// Takes turns with the other process on *turn: its own are me, me + 2, ...
// The number of waits that timed out (2 s each), or -1 for another error.
static int play(atomic_int *turn, int me, int private_flag) {
    struct timespec two = {2, 0};
    int timeouts = 0;
    for (int r = 0; r < ROUNDS; r++) {
        int want = 2 * r + me, v;
        while ((v = atomic_load(turn)) != want) {
            if (v > want)
                return -1;
            if (futex(turn, FUTEX_WAIT | private_flag, v, &two) < 0) {
                if (errno == ETIMEDOUT) {
                    if (++timeouts > 3)
                        return timeouts;
                } else if (errno != EAGAIN && errno != EINTR) {
                    return -1;
                }
            }
        }
        atomic_store(turn, want + 1);
        futex(turn, FUTEX_WAKE | private_flag, 1, NULL);
    }
    return timeouts;
}

// Round trips with a child over parent_word and child_word (the same word in
// memory, at the addresses each has for it): the average in ms, and the
// waits that timed out on both sides (in *timeouts), -1 if one side failed.
static double round_trips(atomic_int *parent_word, atomic_int *(*child_word)(void), int *timeouts) {
    atomic_store(parent_word, 0);
    pid_t pid = fork();
    if (pid == 0) {
        atomic_int *word = child_word ? child_word() : parent_word;
        if (word == NULL)
            _exit(255);
        int t = play(word, 1, 0);
        _exit(t < 0 ? 255 : t);
    }
    double start = now();
    int t = play(parent_word, 0, 0);
    double elapsed = now() - start;
    int status = 0;
    waitpid(pid, &status, 0);
    int child_t = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
    *timeouts = t < 0 || child_t == 255 ? -1 : t + child_t;
    return elapsed * 1000 / ROUNDS;
}

#define FILE_PATH "/tmp/regress_futex_shared.file"

// The file's page at its offset 4096, mapped as the parent does not: from
// offset 4096
static atomic_int *child_file_word(void) {
    int fd = open(FILE_PATH, O_RDWR);
    if (fd < 0)
        return NULL;
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
    close(fd);
    return p == MAP_FAILED ? NULL : (atomic_int *) p;
}

static volatile sig_atomic_t got_signal;
static atomic_int sig_word, sig_ready, sig_done;

static void on_usr1(int sig) {
    (void) sig;
    got_signal = 1;
}

static void *sig_waiter(void *arg) {
    (void) arg;
    atomic_store(&sig_ready, 1);
    while (!got_signal)
        futex(&sig_word, FUTEX_WAIT_PRIVATE, 0, NULL);
    atomic_store(&sig_done, 1);
    return NULL;
}

static void *exit_waiter(void *arg) {
    (void) arg;
    atomic_int word = 0;
    for (;;)
        futex(&word, FUTEX_WAIT_PRIVATE, 0, NULL);
    return NULL;
}

int main(void) {
    char detail[300];
    int timeouts;

    // shared anonymous memory, from fork
    atomic_int *anon = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    double ms = round_trips(anon, NULL, &timeouts);
    snprintf(detail, sizeof(detail), "%.2f ms a round trip, %d waits timed out", ms, timeouts);
    check("process-shared futex in MAP_SHARED | MAP_ANONYMOUS memory across fork: woken in time",
          timeouts == 0 && ms < 20, detail);

    // a file each maps for itself, the parent from offset 0, the child from 4096
    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, 8192) < 0) {
        perror(FILE_PATH);
        return 1;
    }
    char *file = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (file == MAP_FAILED) {
        perror("mmap " FILE_PATH);
        return 1;
    }
    ms = round_trips((atomic_int *) (file + 4096), child_file_word, &timeouts);
    snprintf(detail, sizeof(detail), "%.2f ms a round trip, %d waits timed out", ms, timeouts);
    check("process-shared futex in a file each process maps, at offsets of its own: woken in time",
          timeouts == 0 && ms < 20, detail);
    munmap(file, 8192);
    unlink(FILE_PATH);

    // a private futex on shared memory: the address space's own
    atomic_store(anon, 0);
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec t = {0, 400 * 1000000};
        long r = futex(anon, FUTEX_WAIT_PRIVATE, 0, &t);
        _exit(r < 0 && errno == ETIMEDOUT ? 0 : 1);
    }
    usleep(150 * 1000);
    long woke = futex(anon, FUTEX_WAKE_PRIVATE, 1, NULL);
    int status = 0;
    waitpid(pid, &status, 0);
    snprintf(detail, sizeof(detail), "the wake woke %ld, the child's wait %s", woke,
             WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "timed out" : "ended otherwise");
    check("a private futex on shared memory: no wake from another process", woke == 0 &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);

    // a signal for a thread just going into an untimed wait
    struct sigaction sa = {0};
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    double worst = 0;
    int late = 0;
    for (int i = 0; i < 200; i++) {
        got_signal = 0;
        atomic_store(&sig_ready, 0);
        atomic_store(&sig_done, 0);
        pthread_t thread;
        pthread_create(&thread, NULL, sig_waiter, NULL);
        while (!atomic_load(&sig_ready)) {}
        if (i % 2)
            usleep(i % 7 * 100);
        double sent = now();
        pthread_kill(thread, SIGUSR1);
        while (!atomic_load(&sig_done) && now() - sent < 2) {}
        double took = now() - sent;
        if (took > worst)
            worst = took;
        if (!atomic_load(&sig_done)) {
            late++;
            break;   // it is left waiting: no more rounds
        }
        pthread_join(thread, NULL);
    }
    snprintf(detail, sizeof(detail), "the slowest %.1f ms, %d not ended in 2 s", worst * 1000, late);
    check("a signal for a thread going into an untimed futex wait: the wait ends at once", late == 0 && worst < 1,
          detail);

    // a timed wait
    atomic_int word = 0;
    struct timespec t200 = {0, 200 * 1000000};
    double start = now();
    long r = futex(&word, FUTEX_WAIT_PRIVATE, 0, &t200);
    int err = errno;
    double took = (now() - start) * 1000;
    snprintf(detail, sizeof(detail), "returned %ld (%s) after %.1f ms", r, r < 0 ? strerror(err) : "0", took);
    check("a futex wait of 200 ms: ETIMEDOUT after 200 ms", r < 0 && err == ETIMEDOUT && took >= 199 && took < 1000,
          detail);

    // exit_group with a thread in an untimed wait
    pid = fork();
    if (pid == 0) {
        pthread_t thread;
        pthread_create(&thread, NULL, exit_waiter, NULL);
        usleep(100 * 1000);
        exit(7);
    }
    start = now();
    status = 0;
    while (waitpid(pid, &status, WNOHANG) == 0 && now() - start < 5)
        usleep(10 * 1000);
    took = now() - start;
    snprintf(detail, sizeof(detail), "the child %s after %.2f s", WIFEXITED(status) && WEXITSTATUS(status) == 7
             ? "exited 7" : "still there, or ended otherwise", took);
    check("exit_group with a thread in an untimed futex wait: the process gone at once",
          WIFEXITED(status) && WEXITSTATUS(status) == 7 && took < 2, detail);
    if (!(WIFEXITED(status)))
        kill(pid, SIGKILL);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
