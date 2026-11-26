// Regression test for /proc/stat's cpu line and btime.
//
// iSH's cpu line had four of Linux's ten fields. procps-ng 4 refuses one with
// fewer than eight, and then ps fails: "Unable to get system boot time",
// with the first process half printed. On a Linux host three of the four
// were the host's in the wrong order (its idle time as nice). There were no
// per-CPU lines (htop and psutil count CPUs by them), and none of intr,
// ctxt, processes, procs_running, procs_blocked and softirq.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_proc_stat regress_proc_stat.c

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

static long long processes_now(void) {
    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    long long processes = -1;
    while (fgets(line, sizeof(line), f) != NULL)
        sscanf(line, "processes %lld", &processes);
    fclose(f);
    return processes;
}

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

int main(void) {
    char detail[600];
    long long processes_before = processes_now();
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    waitpid(child, NULL, 0);

    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    int fields = 0, cpu_lines = 0, cpu_lines_short = 0;
    char cpu_line[sizeof(line)] = "";
    unsigned long long idle = 0, idle_sum = 0;
    long long btime = -1, processes = -1, running = -1, blocked = -1, ctxt = -1, intr = -1, softirq = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long v[11];
        unsigned cpu;
        if (strncmp(line, "cpu ", 4) == 0) {
            fields = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]);
            idle = v[3];
            strcpy(cpu_line, line);
        } else if (strncmp(line, "cpu", 3) == 0) {
            cpu_lines++;
            if (sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &cpu,
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) != 11)
                cpu_lines_short++;
            idle_sum += v[3];
        }
        sscanf(line, "btime %lld", &btime);
        sscanf(line, "processes %lld", &processes);
        sscanf(line, "procs_running %lld", &running);
        sscanf(line, "procs_blocked %lld", &blocked);
        sscanf(line, "ctxt %lld", &ctxt);
        sscanf(line, "intr %lld", &intr);
        sscanf(line, "softirq %lld", &softirq);
    }
    fclose(f);
    snprintf(detail, sizeof(detail), "%d fields: %s", fields, cpu_line);
    check("cpu line: Linux's ten fields", fields == 10, detail);

    long online = sysconf(_SC_NPROCESSORS_ONLN);
    snprintf(detail, sizeof(detail), "%d cpuN lines (%d short), %ld CPUs online", cpu_lines, cpu_lines_short, online);
    check("a cpuN line per CPU, ten fields each", cpu_lines == online && cpu_lines_short == 0, detail);

    // read moments apart: a few ticks per CPU
    unsigned long long slack = 10 * cpu_lines + idle / 100;
    snprintf(detail, sizeof(detail), "cpu idle %llu, sum of cpuN idle %llu", idle, idle_sum);
    check("cpu line's idle is the CPUs' idle (fields in order)",
          idle + slack >= idle_sum && idle_sum + slack >= idle, detail);

    snprintf(detail, sizeof(detail), "intr %lld ctxt %lld processes %lld procs_running %lld procs_blocked %lld softirq %lld",
             intr, ctxt, processes, running, blocked, softirq);
    check("intr, ctxt, processes, procs_running, procs_blocked, softirq",
          intr >= 0 && ctxt > 0 && processes >= 0 && running >= 1 && blocked >= 0 && softirq >= 0, detail);
    snprintf(detail, sizeof(detail), "processes %lld before a fork, %lld after", processes_before, processes);
    check("processes counts forks", processes > processes_before, detail);

    struct timespec now, up;
    clock_gettime(CLOCK_REALTIME, &now);
    clock_gettime(CLOCK_BOOTTIME, &up);
    long long boot = now.tv_sec - up.tv_sec;
    snprintf(detail, sizeof(detail), "btime %lld, realtime - boottime %lld", btime, boot);
    check("btime: realtime less time since boot", btime >= boot - 2 && btime <= boot + 2, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
