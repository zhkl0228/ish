// Regression test: /proc/<pid>/syscall, the system call a thread waits in.
//
// iSH had no /proc/<pid>/syscall: where a hung program's threads waited (in
// futex, poll or read) could not be read. As Linux's: the call's number, its
// six arguments, the stack pointer and the pc, or "running" of a thread that
// is not in one; also /proc/<pid>/task/<tid>/syscall. stat's kstkesp and
// kstkeip (fields 29 and 30), 0 on Linux now, are in iSH where the thread's
// stack and code were as it last left translated code.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_proc_syscall regress_proc_syscall.c

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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

static void read_file(const char *path, char *buf, size_t size) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static void nap(void) {
    struct timespec ts = {0, 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

int main(void) {
    char path[64], text[512];
    int ish = access("/proc/ish", F_OK) == 0;

    // a child waiting in read on a pipe: its number and its fd, a stack and a pc
    int p[2];
    pipe(p);
    pid_t reader = fork();
    if (reader == 0) {
        char c;
        read(p[0], &c, 1);
        _exit(0);
    }
    snprintf(path, sizeof(path), "/proc/%d/syscall", reader);
    long nr = -2;
    unsigned long long args[6], sp = 0, pc = 0;
    for (int tries = 0; tries < 100 && nr != SYS_read; tries++) {
        nap();
        read_file(path, text, sizeof(text));
        nr = -2;
        sscanf(text, "%ld %llx %llx %llx %llx %llx %llx %llx %llx", &nr, &args[0], &args[1], &args[2], &args[3],
               &args[4], &args[5], &sp, &pc);
    }
    char detail[600];
    snprintf(detail, sizeof(detail), "%s", text);
    check("a thread in read: its number, its fd, a stack and a pc",
          nr == SYS_read && args[0] == (unsigned long long) p[0] && args[2] == 1 && sp != 0 && pc != 0, detail);

    snprintf(path, sizeof(path), "/proc/%d/task/%d/syscall", reader, reader);
    char task_text[512];
    read_file(path, task_text, sizeof(task_text));
    snprintf(detail, sizeof(detail), "task's %s, process's %s", task_text, text);
    check("the thread's own file says the same", strcmp(task_text, text) == 0, detail);

    if (ish) {
        // kstkesp and kstkeip, fields 29 and 30 of stat, are the same stack and pc
        snprintf(path, sizeof(path), "/proc/%d/stat", reader);
        read_file(path, text, sizeof(text));
        char *after = strrchr(text, ')');
        unsigned long long field[40] = {0};
        int n = 0;
        for (char *f = strtok(after + 2, " "); f != NULL && n < 40; f = strtok(NULL, " "))
            field[n++] = strtoull(f, NULL, 10);
        // fields 3 onwards are field[0] onwards: 29 is field[26]
        snprintf(detail, sizeof(detail), "kstkesp %llu kstkeip %llu, syscall's sp %llu pc %llu", field[26], field[27],
                 sp, pc);
        check("stat's kstkesp and kstkeip are its stack and pc (iSH)", field[26] == sp && field[27] == pc, detail);
    }
    write(p[1], "x", 1);
    waitpid(reader, NULL, 0);

    // a child running: "running"
    pid_t busy = fork();
    if (busy == 0)
        for (;;)
            ;
    snprintf(path, sizeof(path), "/proc/%d/syscall", busy);
    int running = 0;
    for (int tries = 0; tries < 100 && !running; tries++) {
        nap();
        read_file(path, text, sizeof(text));
        running = strcmp(text, "running\n") == 0;
    }
    check("a thread running: running", running, text);
    kill(busy, SIGKILL);
    waitpid(busy, NULL, 0);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
