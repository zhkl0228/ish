// Regression test for what /proc says of a process that has exited but is not
// gone, of a thread's open files, and of a directory's . and ..
//
// /proc had no zombie (a child not waited for): no /proc/<pid>, so ps and top
// showed no <defunct>; and a process whose main thread had called
// pthread_exit while another ran had a /proc/<pid> whose every file was ESRCH.
// /proc/<pid>/task/<tid> had no fd. No directory of /proc listed . and ..,
// so what counts the files open in /proc/self/fd less those two came out two
// short. A zombie's status said a SIGKILL was pending that it had none queued
// for. A program's name of 16 bytes or more was its comm whole, 16 bytes with
// no NUL (Linux's is 15), and statm had a space after its last field.
//
// Checked, as Linux has them: a zombie listed in /proc; its stat and status
// in state Z, one thread, none pending; its comm its name's first 15 bytes;
// its cmdline, environ and maps empty, its statm all 0 ("0 0 0 0 0 0 0");
// its exe and cwd links ENOENT; its fd empty, its task its own tid; gone
// once waited for. A process whose main thread has exited while another
// runs: state Z, two threads, both in its task. A thread's task/<tid>/fd lists the file the process opened, and its
// link is to it. /proc, /proc/self and /proc/self/fd list . and .. once.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_proc_zombie regress_proc_zombie.c -lpthread

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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
    fflush(stdout);
}

// The file's contents, at most size - 1 bytes; -1 (errno) if not read
static ssize_t slurp(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    int e = errno;
    close(fd);
    errno = e;
    buf[n > 0 ? n : 0] = '\0';
    return n;
}

static char state_of(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (slurp(path, buf, sizeof(buf)) <= 0)
        return '?';
    char *close_paren = strrchr(buf, ')');
    return close_paren != NULL && close_paren[1] == ' ' ? close_paren[2] : '?';
}

// The value of status's "key:\t" line; "" if none
static void status_line(pid_t pid, const char *key, char *out, size_t size) {
    char path[64], buf[2048];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    out[0] = '\0';
    if (slurp(path, buf, sizeof(buf)) <= 0)
        return;
    for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, key, strlen(key)) == 0 && strncmp(line + strlen(key), ":\t", 2) == 0) {
            snprintf(out, size, "%s", line + strlen(key) + 2);
            return;
        }
    }
}

// The names in a directory, space-separated, and how many were . or ..
static int list(const char *path, char *names, size_t size, int *dots) {
    names[0] = '\0';
    *dots = 0;
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        n++;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            (*dots)++;
        size_t len = strlen(names);
        snprintf(names + len, size - len, "%s%s", len ? " " : "", e->d_name);
    }
    closedir(d);
    return n;
}

static int listed_in_proc(pid_t pid) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL)
        found |= atoi(e->d_name) == pid;
    closedir(d);
    return found;
}

static void zombie(void) {
    char path[96], buf[512], detail[300];
    pid_t child = fork();
    if (child == 0)
        _exit(3);
    // a zombie once it has exited: its state says so
    for (int i = 0; i < 300 && state_of(child) != 'Z'; i++)
        usleep(10000);

    snprintf(detail, sizeof(detail), "pid %d", child);
    check("a zombie is listed in /proc", listed_in_proc(child), detail);

    char state[64], threads[16], pending[32];
    status_line(child, "State", state, sizeof(state));
    status_line(child, "Threads", threads, sizeof(threads));
    status_line(child, "SigPnd", pending, sizeof(pending));
    snprintf(detail, sizeof(detail), "stat state %c; status State '%s', Threads '%s', SigPnd '%s'", state_of(child),
             state, threads, pending);
    check("its stat and status: state Z, one thread, nothing pending",
          state_of(child) == 'Z' && strcmp(state, "Z (zombie)") == 0 && strcmp(threads, "1") == 0 &&
          strcmp(pending, "0000000000000000") == 0, detail);

    snprintf(path, sizeof(path), "/proc/%d/comm", child);
    ssize_t n = slurp(path, buf, sizeof(buf));
    snprintf(detail, sizeof(detail), "%zd '%.64s'", n, buf);
    check("its comm is its name", n > 0 && strcmp(buf, "regress_proc_zo\n") == 0, detail);

    const char *empty[] = {"cmdline", "environ", "maps"};
    char sizes[128] = "";
    int all_empty = 1;
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "/proc/%d/%s", child, empty[i]);
        n = slurp(path, buf, sizeof(buf));
        all_empty &= n == 0;
        size_t len = strlen(sizes);
        snprintf(sizes + len, sizeof(sizes) - len, "%s %zd%s; ", empty[i], n, n < 0 ? strerror(errno) : "");
    }
    snprintf(path, sizeof(path), "/proc/%d/statm", child);
    n = slurp(path, buf, sizeof(buf));
    snprintf(detail, sizeof(detail), "%.128s statm '%.64s'", sizes, buf);
    check("its cmdline, environ and maps are empty, its statm all 0",
          all_empty && strcmp(buf, "0 0 0 0 0 0 0\n") == 0, detail);

    char link[256];
    int enoent = 1;
    sizes[0] = '\0';
    const char *links[] = {"exe", "cwd"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "/proc/%d/%s", child, links[i]);
        n = readlink(path, link, sizeof(link) - 1);
        enoent &= n < 0 && errno == ENOENT;
        size_t len = strlen(sizes);
        snprintf(sizes + len, sizeof(sizes) - len, "%s %zd %s; ", links[i], n, n < 0 ? strerror(errno) : "");
    }
    check("its exe and cwd links are ENOENT", enoent, sizes);

    char fds[256], tasks[256];
    int dots;
    snprintf(path, sizeof(path), "/proc/%d/fd", child);
    int nfd = list(path, fds, sizeof(fds), &dots);
    snprintf(path, sizeof(path), "/proc/%d/task", child);
    list(path, tasks, sizeof(tasks), &dots);
    char want[64];
    snprintf(want, sizeof(want), ". .. %d", child);
    snprintf(detail, sizeof(detail), "fd '%.100s' (%d), task '%.100s'", fds, nfd, tasks);
    check("its fd is empty, its task its own tid", nfd == 2 && strcmp(tasks, want) == 0, detail);

    int status;
    waitpid(child, &status, 0);
    snprintf(path, sizeof(path), "/proc/%d/stat", child);
    n = slurp(path, buf, sizeof(buf));
    snprintf(detail, sizeof(detail), "%zd (%s), listed %d", n, n < 0 ? strerror(errno) : "read",
             listed_in_proc(child));
    check("once waited for it is gone", n < 0 && errno == ENOENT && !listed_in_proc(child), detail);
}

static int go_pipe[2];

static void *stays(void *arg) {
    (void) arg;
    char c;
    read(go_pipe[0], &c, 1);
    return NULL;
}

static void main_thread_exits(void) {
    char detail[300], tasks[256], threads[16];
    int ready[2];
    pipe(ready);
    pipe(go_pipe);
    pid_t child = fork();
    if (child == 0) {
        pthread_t t;
        pthread_create(&t, NULL, stays, NULL);
        write(ready[1], "r", 1);
        pthread_exit(NULL);
    }
    char c;
    read(ready[0], &c, 1);
    for (int i = 0; i < 300 && state_of(child) != 'Z'; i++)
        usleep(10000);
    status_line(child, "Threads", threads, sizeof(threads));
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", child);
    int dots;
    int n = list(path, tasks, sizeof(tasks), &dots);
    snprintf(detail, sizeof(detail), "stat state %c, Threads '%s', task '%.200s' (%d)", state_of(child), threads,
             tasks, n);
    check("its main thread exited, another running: state Z, two threads, both in its task",
          state_of(child) == 'Z' && strcmp(threads, "2") == 0 && n == 4, detail);
    write(go_pipe[1], "g", 1);
    int status;
    waitpid(child, &status, 0);
    close(ready[0]);
    close(ready[1]);
    close(go_pipe[0]);
    close(go_pipe[1]);
}

static volatile pid_t thread_tid;
static int done_pipe[2];

static void *named(void *arg) {
    (void) arg;
    thread_tid = syscall(SYS_gettid);
    char c;
    read(done_pipe[0], &c, 1);
    return NULL;
}

static void thread_fd(void) {
    char detail[400], path[128], link[256], names[1024];
    const char *file = "/tmp/regress_proc_zombie.file";
    int fd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0600);
    pipe(done_pipe);
    pthread_t t;
    pthread_create(&t, NULL, named, NULL);
    while (thread_tid == 0)
        usleep(1000);
    snprintf(path, sizeof(path), "/proc/self/task/%d/fd", thread_tid);
    int dots;
    int n = list(path, names, sizeof(names), &dots);
    char want[16];
    snprintf(want, sizeof(want), "%d", fd);
    int listed = 0;
    for (char *name = strtok(names, " "); name != NULL; name = strtok(NULL, " "))
        listed |= strcmp(name, want) == 0;
    snprintf(path, sizeof(path), "/proc/self/task/%d/fd/%d", thread_tid, fd);
    ssize_t len = readlink(path, link, sizeof(link) - 1);
    link[len > 0 ? len : 0] = '\0';
    snprintf(detail, sizeof(detail), "%d entries, fd %d listed %d; link %zd '%.200s' (%s)", n, fd, listed, len,
             link, len < 0 ? strerror(errno) : "ok");
    check("a thread's task/<tid>/fd lists the process's file, its link to it",
          listed && len > 0 && strcmp(link, file) == 0, detail);
    write(done_pipe[1], "d", 1);
    pthread_join(t, NULL);
    close(done_pipe[0]);
    close(done_pipe[1]);
    close(fd);
    unlink(file);
}

static void dots(void) {
    char names[4096], detail[300] = "";
    const char *dirs[] = {"/proc", "/proc/self", "/proc/self/fd"};
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        int dots;
        int n = list(dirs[i], names, sizeof(names), &dots);
        ok &= n > 0 && dots == 2;
        size_t len = strlen(detail);
        snprintf(detail + len, sizeof(detail) - len, "%s: %d of %d; ", dirs[i], dots, n);
    }
    check("/proc, /proc/self and /proc/self/fd list . and .. once", ok, detail);
}

int main(void) {
    zombie();
    main_thread_exits();
    thread_fd();
    dots();
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
