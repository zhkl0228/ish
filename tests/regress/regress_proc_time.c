// Regression test for task state and CPU time: /proc/<pid>/stat and
// getrusage.
//
// /proc/<pid>/stat called every task running (R) and gave 0 for its CPU time
// (ps's TIME and %CPU, top's) and its children's. getrusage(RUSAGE_SELF)
// gave the calling thread's time, not the process's, and RUSAGE_THREAD was
// EINVAL; clock_gettime's CLOCK_THREAD_CPUTIME_ID was EINVAL too, and its
// CLOCK_PROCESS_CPUTIME_ID the calling thread's user time. A task in a
// system call is now taken for sleeping (S), as near as
// iSH can tell; the times are its threads', those running and those that
// exited, and those of its children waited for.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_proc_time regress_proc_time.c

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
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

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0)
        continue;
}

// the calling thread's CPU time; -1 if the clock fails
static double cpu_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) < 0)
        return -1;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// computes for that much CPU time, or 5 s of wall time if the clock fails
static volatile unsigned long sink;
static void burn(double seconds) {
    double end = cpu_seconds() + seconds;
    time_t give_up = time(NULL) + 5;
    while (cpu_seconds() < end && time(NULL) < give_up)
        for (int i = 0; i < 100000; i++)
            sink += i;
}

// state and utime + stime (ticks) of /proc/<pid>/stat; cutime + cstime too
static char proc_stat(pid_t pid, unsigned long *time, unsigned long *children) {
    char path[64], buf[1024] = "";
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return '?';
    fgets(buf, sizeof(buf), f);
    fclose(f);
    char *p = strrchr(buf, ')') + 2;
    char state = *p;
    unsigned long v[20] = {0};
    int n = 3;
    for (char *tok = strtok(p, " "); tok != NULL && n < 20; tok = strtok(NULL, " "))
        sscanf(tok, "%lu", &v[n++]);
    *time = v[14] + v[15];
    *children = v[16] + v[17];
    return state;
}

static double rusage_seconds(int who) {
    struct rusage ru;
    if (getrusage(who, &ru) < 0)
        return -1;
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static pthread_barrier_t burned, done;
static void *burner(void *arg) {
    (void) arg;
    burn(0.3);
    pthread_barrier_wait(&burned);
    pthread_barrier_wait(&done);
    return NULL;
}

int main(void) {
    char detail[200];
    unsigned long time, children;
    long hz = sysconf(_SC_CLK_TCK);

    // 1. a sleeping task and a running one
    pid_t sleeper = fork();
    if (sleeper == 0) {
        sleep_ms(5000);
        _exit(0);
    }
    pid_t spinner = fork();
    if (spinner == 0) {
        burn(5);
        _exit(0);
    }
    sleep_ms(300);
    char sleeping = proc_stat(sleeper, &time, &children);
    char running = proc_stat(spinner, &time, &children);
    snprintf(detail, sizeof(detail), "in nanosleep %c, computing %c", sleeping, running);
    check("state: sleeping S, computing R", sleeping == 'S' && running == 'R', detail);
    kill(sleeper, SIGKILL);
    kill(spinner, SIGKILL);
    waitpid(sleeper, NULL, 0);
    waitpid(spinner, NULL, 0);

    // 2. a child's CPU time, while it lives: read once it says it computed
    // (waiting for it to sleep, a loaded host read it in the middle)
    int computed[2];
    pipe(computed);
    pid_t child = fork();
    if (child == 0) {
        burn(0.3);
        write(computed[1], "c", 1);
        sleep_ms(5000);
        _exit(0);
    }
    char c;
    read(computed[0], &c, 1);
    close(computed[0]);
    close(computed[1]);
    proc_stat(child, &time, &children);
    snprintf(detail, sizeof(detail), "utime + stime %lu ticks of %ld a second", time, hz);
    check("a process's CPU time in /proc/<pid>/stat", time >= (unsigned long) hz / 5 && time < (unsigned long) hz * 3,
          detail);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    // 3. getrusage(RUSAGE_SELF): another thread's time, running and exited
    double before = rusage_seconds(RUSAGE_SELF);
    pthread_barrier_init(&burned, NULL, 2);
    pthread_barrier_init(&done, NULL, 2);
    pthread_t t;
    pthread_create(&t, NULL, burner, NULL);
    pthread_barrier_wait(&burned);
    double running_thread = rusage_seconds(RUSAGE_SELF) - before;
    double self_thread = rusage_seconds(RUSAGE_THREAD);
    pthread_barrier_wait(&done);
    pthread_join(t, NULL);
    double exited_thread = rusage_seconds(RUSAGE_SELF) - before;
    snprintf(detail, sizeof(detail), "with the thread running %.2fs, after it exited %.2fs; RUSAGE_THREAD %.2fs",
             running_thread, exited_thread, self_thread);
    check("getrusage(RUSAGE_SELF) counts every thread", running_thread >= 0.25 && exited_thread >= 0.25 &&
          self_thread >= 0 && self_thread < running_thread, detail);

    // 4. the CPU-time clocks: the process's has the thread's that exited
    struct timespec process_ts, thread_ts, res;
    int process_rc = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process_ts);
    int thread_rc = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread_ts);
    int res_rc = clock_getres(CLOCK_THREAD_CPUTIME_ID, &res);
    double process_clock = process_ts.tv_sec + process_ts.tv_nsec / 1e9;
    double thread_clock = thread_ts.tv_sec + thread_ts.tv_nsec / 1e9;
    snprintf(detail, sizeof(detail), "process %d %.2fs, thread %d %.2fs, getres %d", process_rc, process_clock,
             thread_rc, thread_clock, res_rc);
    check("clock_gettime: the process's and the thread's CPU time", process_rc == 0 && thread_rc == 0 &&
          res_rc == 0 && process_clock >= 0.25 && thread_clock < process_clock, detail);

    // 5. a child waited for: its time in cutime + cstime
    child = fork();
    if (child == 0) {
        burn(0.3);
        _exit(0);
    }
    waitpid(child, NULL, 0);
    proc_stat(getpid(), &time, &children);
    snprintf(detail, sizeof(detail), "cutime + cstime %lu ticks", children);
    check("children's time after waiting", children >= (unsigned long) hz / 5, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
