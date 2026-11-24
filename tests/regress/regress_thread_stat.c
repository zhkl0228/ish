// Regression test for /proc/<pid>/task/<tid>: a directory of that thread, its
// stat with the thread's own CPU time.
//
// task/<tid> was a symlink to /proc/<tid>, whose stat has the process's CPU
// time (as Linux's): every thread of a process said the same, the process's,
// and top -H and ps -L could not tell which thread took it.
//
// Checked, with a thread that runs until it has taken 0.3 s of CPU and one
// that waits: task/<tid> is a directory; the busy thread's stat says at least
// 0.2 s of its own, the waiting one's at most 0.05 s, the process's at least
// the busy one's; each thread's stat starts with its tid, its comm is its
// name, and its status says its Pid and the process's Tgid. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_thread_stat regress_thread_stat.c

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

static volatile pid_t busy_tid, idle_tid;
static volatile int busy_done, finish;

static void *busy(void *arg) {
    (void) arg;
    prctl(PR_SET_NAME, "busy");
    busy_tid = syscall(SYS_gettid);
    struct timespec cpu;
    do {
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    } while (cpu.tv_sec == 0 && cpu.tv_nsec < 300000000);
    busy_done = 1;
    while (!finish)
        usleep(10000);
    return NULL;
}

static void *idle(void *arg) {
    (void) arg;
    prctl(PR_SET_NAME, "idle");
    idle_tid = syscall(SYS_gettid);
    while (!finish)
        usleep(10000);
    return NULL;
}

// A stat file's first field, its name in parentheses and its utime + stime,
// in ticks; -1 if it could not be read
static long stat_ticks(const char *path, long *first, char *name, size_t name_size) {
    char buf[1024];
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    *first = atol(buf);
    char *open = strchr(buf, '('), *close = strrchr(buf, ')');
    snprintf(name, name_size, "%.*s", (int) (close - open - 1), open + 1);
    // after ") ": state, then fields 4 to 15; utime and stime are 14 and 15
    char *p = close + 2;
    long fields[16] = {0};
    for (int i = 3; i <= 15; i++) {
        fields[i] = strtol(p, &p, 10);
        if (i == 3) // the state is a letter, not a number
            p = strchr(close + 2, ' ');
    }
    return fields[14] + fields[15];
}

static int status_has(const char *path, const char *line) {
    char buf[4096];
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, line) != NULL;
}

int main(void) {
    char detail[300], path[128], name[64];
    pthread_t b, i;
    pthread_create(&b, NULL, busy, NULL);
    pthread_create(&i, NULL, idle, NULL);
    while (!busy_done || !idle_tid)
        usleep(10000);

    struct stat st;
    snprintf(path, sizeof(path), "/proc/self/task/%d", busy_tid);
    int r = lstat(path, &st);
    check("task/<tid> is a directory", r == 0 && S_ISDIR(st.st_mode), r == 0 ? "not a directory" : "lstat failed");

    long hz = sysconf(_SC_CLK_TCK), first;
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", busy_tid);
    long busy_ticks = stat_ticks(path, &first, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%ld ticks (%ld a second), first field %ld, name '%s'", busy_ticks, hz, first, name);
    check("the busy thread's stat: its own CPU time", busy_ticks >= hz / 5, detail);
    check("the busy thread's stat: its tid and its name", first == busy_tid && strcmp(name, "busy") == 0, detail);

    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", idle_tid);
    long idle_ticks = stat_ticks(path, &first, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%ld ticks, first field %ld, name '%s'", idle_ticks, first, name);
    check("the waiting thread's stat: little CPU time", idle_ticks >= 0 && idle_ticks <= hz / 20, detail);

    long process_ticks = stat_ticks("/proc/self/stat", &first, name, sizeof(name));
    snprintf(detail, sizeof(detail), "process %ld ticks, busy thread %ld", process_ticks, busy_ticks);
    check("the process's stat: at least the busy thread's", process_ticks >= busy_ticks, detail);

    char comm[32] = "", line[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", idle_tid);
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        fgets(comm, sizeof(comm), f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "/proc/self/task/%d/status", idle_tid);
    snprintf(line, sizeof(line), "\nPid:\t%d\n", idle_tid);
    int pid_ok = status_has(path, line);
    snprintf(line, sizeof(line), "\nTgid:\t%d\n", getpid());
    int tgid_ok = status_has(path, line);
    snprintf(detail, sizeof(detail), "comm '%.*s', Pid %s, Tgid %s", (int) strcspn(comm, "\n"), comm,
             pid_ok ? "the tid" : "not the tid", tgid_ok ? "the process's" : "not the process's");
    check("the waiting thread's comm and status", strcmp(comm, "idle\n") == 0 && pid_ok && tgid_ok, detail);

    finish = 1;
    pthread_join(b, NULL);
    pthread_join(i, NULL);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
