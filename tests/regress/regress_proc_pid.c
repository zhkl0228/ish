// Regression test for the /proc files procps reads for ps, top, free, uptime
// and vmstat.
//
// iSH had no /proc/<pid>/status (ps took VSZ and RSS from it: 0), put 0 for
// /proc/<pid>/stat's start time (ps showed every process started at boot),
// vsize and rss, and its ignored and caught signals one bit off. On a Linux
// host /proc/meminfo was 1024 times too small (7.9 MB of memory for 7.9 GB).
// There was no /proc/loadavg (load averages all 0.00) and no /proc/vmstat
// (vmstat: "Unable to create vmstat structure"). setfsuid and setfsgid were
// stubs.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_proc_pid regress_proc_pid.c

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/fsuid.h>
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
}

// The value after "key" in a file of "key value" lines; -1 if missing.
static long long field(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[512];
    long long value = -1;
    size_t len = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, len) == 0 && (line[len] == ' ' || line[len] == '\t'))
            sscanf(line + len, "%lld", &value);
    }
    fclose(f);
    return value;
}

static int status_line(const char *key, char *out, size_t size) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL)
        return 0;
    char line[512];
    int found = 0;
    size_t len = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, len) == 0 && line[len] == ':') {
            snprintf(out, size, "%s", line + len + 1);
            out[strcspn(out, "\n")] = '\0';
            found = 1;
        }
    }
    fclose(f);
    return found;
}

static void on_usr2(int sig) {
    (void) sig;
}

int main(void) {
    char detail[400];
    // signals to see in the masks: SIGUSR1 (10) blocked, SIGPIPE (13)
    // ignored, SIGUSR2 (12) caught
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR2, on_usr2);

    // /proc/self/stat: fields after the ")" of the name; starttime is the
    // 22nd field, vsize 23rd, rss 24th, sigignore 33rd, sigcatch 34th.
    FILE *f = fopen("/proc/self/stat", "r");
    char stat[1024] = "";
    fgets(stat, sizeof(stat), f);
    fclose(f);
    char *p = strrchr(stat, ')') + 2;
    unsigned long long v[40] = {0};
    int n = 3;
    for (char *tok = strtok(p, " "); tok != NULL && n < 40; tok = strtok(NULL, " "))
        sscanf(tok, "%llu", &v[n++]);
    long hz = sysconf(_SC_CLK_TCK);
    long long btime = field("/proc/stat", "btime");
    time_t now = time(NULL);
    long long started = btime + (long long) (v[22] / hz);
    snprintf(detail, sizeof(detail), "starttime %llu ticks: started %lld, now %lld", v[22], started, (long long) now);
    check("stat: start time is now", started >= now - 5 && started <= now + 1, detail);
    snprintf(detail, sizeof(detail), "vsize %llu bytes, rss %llu pages", v[23], v[24]);
    check("stat: vsize and rss", v[23] > 0 && v[24] > 0 && v[24] * 4096 <= v[23], detail);
    snprintf(detail, sizeof(detail), "sigignore %#llx (want bit %d), sigcatch %#llx (want bit %d)",
             v[33], SIGPIPE - 1, v[34], SIGUSR2 - 1);
    check("stat: ignored and caught signals", (v[33] & (1ull << (SIGPIPE - 1))) && (v[34] & (1ull << (SIGUSR2 - 1))), detail);

    // /proc/self/status
    char name[64] = "", pid[64] = "", ppid[64] = "", uid[64] = "", vmsize[64] = "", vmrss[64] = "",
         threads[64] = "", sigblk[64] = "", sigign[64] = "", sigcgt[64] = "";
    int have = status_line("Name", name, sizeof(name)) + status_line("Pid", pid, sizeof(pid)) +
               status_line("PPid", ppid, sizeof(ppid)) + status_line("Uid", uid, sizeof(uid)) +
               status_line("VmSize", vmsize, sizeof(vmsize)) + status_line("VmRSS", vmrss, sizeof(vmrss)) +
               status_line("Threads", threads, sizeof(threads)) + status_line("SigBlk", sigblk, sizeof(sigblk)) +
               status_line("SigIgn", sigign, sizeof(sigign)) + status_line("SigCgt", sigcgt, sizeof(sigcgt));
    int pid_v = -1, ppid_v = -1, threads_v = -1;
    unsigned ruid = 1, euid = 1, suid = 1, fsuid = 1;
    unsigned long vmsize_v = 0, vmrss_v = 0;
    unsigned long long blk = 0, ign = 0, cgt = 0;
    sscanf(pid, "%d", &pid_v);
    sscanf(ppid, "%d", &ppid_v);
    sscanf(uid, "%u %u %u %u", &ruid, &euid, &suid, &fsuid);
    sscanf(vmsize, "%lu", &vmsize_v);
    sscanf(vmrss, "%lu", &vmrss_v);
    sscanf(threads, "%d", &threads_v);
    sscanf(sigblk, "%llx", &blk);
    sscanf(sigign, "%llx", &ign);
    sscanf(sigcgt, "%llx", &cgt);
    snprintf(detail, sizeof(detail), "%d of 10 lines; Name '%s' Pid %d PPid %d Uid '%s' Threads %d", have, name,
             pid_v, ppid_v, uid, threads_v);
    check("status: ids and threads", have == 10 && pid_v == getpid() && ppid_v == getppid() &&
          ruid == getuid() && euid == geteuid() && fsuid == geteuid() && threads_v == 1 &&
          strstr(name, "regress_proc") != NULL, detail);
    snprintf(detail, sizeof(detail), "VmSize %lu kB, VmRSS %lu kB, stat vsize %llu", vmsize_v, vmrss_v, v[23]);
    check("status: VmSize and VmRSS", vmsize_v > 0 && vmrss_v > 0 && vmrss_v <= vmsize_v, detail);
    snprintf(detail, sizeof(detail), "SigBlk %llx SigIgn %llx SigCgt %llx", blk, ign, cgt);
    check("status: signal masks", (blk & (1ull << (SIGUSR1 - 1))) && (ign & (1ull << (SIGPIPE - 1))) &&
          (cgt & (1ull << (SIGUSR2 - 1))), detail);

    long long total = field("/proc/meminfo", "MemTotal:");
    snprintf(detail, sizeof(detail), "MemTotal %lld kB", total);
    check("meminfo: MemTotal in kB (over 64 MB)", total > 64 * 1024, detail);

    double load[3] = {-1, -1, -1};
    int running = -1, tasks = -1, last = -1;
    f = fopen("/proc/loadavg", "r");
    int fields = f != NULL ? fscanf(f, "%lf %lf %lf %d/%d %d", &load[0], &load[1], &load[2], &running, &tasks, &last) : -1;
    if (f != NULL)
        fclose(f);
    snprintf(detail, sizeof(detail), "%d fields: %.2f %.2f %.2f %d/%d %d", fields, load[0], load[1], load[2],
             running, tasks, last);
    check("loadavg: Linux's six fields", fields == 6 && load[0] >= 0 && running >= 1 && tasks >= running && last > 0,
          detail);

    snprintf(detail, sizeof(detail), "pgpgin %lld pgpgout %lld pswpin %lld pswpout %lld",
             field("/proc/vmstat", "pgpgin"), field("/proc/vmstat", "pgpgout"),
             field("/proc/vmstat", "pswpin"), field("/proc/vmstat", "pswpout"));
    check("vmstat: what vmstat reads", field("/proc/vmstat", "pgpgin") >= 0 && field("/proc/vmstat", "pgpgout") >= 0 &&
          field("/proc/vmstat", "pswpin") >= 0 && field("/proc/vmstat", "pswpout") >= 0, detail);

    int fs_uid = setfsuid(-1), fs_gid = setfsgid(-1);
    snprintf(detail, sizeof(detail), "setfsuid(-1) %d (euid %d), setfsgid(-1) %d (egid %d)", fs_uid, geteuid(),
             fs_gid, getegid());
    check("setfsuid and setfsgid: the filesystem ids", fs_uid == (int) geteuid() && fs_gid == (int) getegid(), detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
