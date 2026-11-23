// Regression test for inotify.
//
// inotify_init1 and inotify_add_watch succeeded but no event ever came: a
// read blocked for good, so node's fs.watch, vite, tsc --watch, nodemon and
// Python's watchdog never saw a file change. Watches are now on inodes, and
// the filesystem operations report their events as Linux's do.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_inotify regress_inotify.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
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

struct ev {
    int wd;
    unsigned mask, cookie;
    char name[64];
};
static struct ev evs[64];
static int nevs;

// Every event queued now (the fd is non-blocking)
static void take(int in) {
    static char buf[1 << 16] __attribute__((aligned(8)));
    nevs = 0;
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *e = (struct inotify_event *) p;
            if (nevs < 64) {
                evs[nevs].wd = e->wd;
                evs[nevs].mask = e->mask;
                evs[nevs].cookie = e->cookie;
                snprintf(evs[nevs].name, sizeof(evs[nevs].name), "%s", e->len ? e->name : "");
                nevs++;
            }
            p += sizeof(*e) + e->len;
        }
    }
}

static const char *mask_name(unsigned mask) {
    static char out[128];
    static const struct { unsigned bit; const char *name; } names[] = {
        {IN_ACCESS, "ACCESS"}, {IN_MODIFY, "MODIFY"}, {IN_ATTRIB, "ATTRIB"}, {IN_CLOSE_WRITE, "CLOSE_WRITE"},
        {IN_CLOSE_NOWRITE, "CLOSE_NOWRITE"}, {IN_OPEN, "OPEN"}, {IN_MOVED_FROM, "MOVED_FROM"},
        {IN_MOVED_TO, "MOVED_TO"}, {IN_CREATE, "CREATE"}, {IN_DELETE, "DELETE"}, {IN_DELETE_SELF, "DELETE_SELF"},
        {IN_MOVE_SELF, "MOVE_SELF"}, {IN_Q_OVERFLOW, "Q_OVERFLOW"}, {IN_IGNORED, "IGNORED"}, {IN_ISDIR, "ISDIR"},
    };
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (mask & names[i].bit)
            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s", out[0] ? "|" : "", names[i].name);
    return out;
}

// The events taken, as "MASK name, MASK name"
static const char *describe(void) {
    static char out[1024];
    out[0] = '\0';
    for (int i = 0; i < nevs; i++)
        snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s %s", i ? ", " : "", mask_name(evs[i].mask),
                 evs[i].name);
    return out;
}

static int equals(const char *expected) {
    return strcmp(describe(), expected) == 0;
}

static void touch(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    close(fd);
}

int main(void) {
    char detail[1200];
    char dir[] = "/tmp/inotify-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    chdir(dir);
    mkdir("d", 0755);
    int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    // 1. a directory: create, write, close, attrib, rename, delete
    int wd = inotify_add_watch(in, "d", IN_ALL_EVENTS);
    int fd = open("d/f", O_CREAT | O_WRONLY, 0644);
    write(fd, "a", 1);
    write(fd, "b", 1); // the same event again: merged
    close(fd);
    chmod("d/f", 0600);
    take(in);
    snprintf(detail, sizeof(detail), "wd %d: %s", wd, describe());
    check("a watched directory: create, open, modify (merged), close, attrib",
          wd > 0 && equals("CREATE f, OPEN f, MODIFY f, CLOSE_WRITE f, ATTRIB f") && evs[0].wd == wd, detail);

    rename("d/f", "d/g");
    take(in);
    snprintf(detail, sizeof(detail), "%s; cookies %u %u", describe(), evs[0].cookie, evs[1].cookie);
    check("rename: MOVED_FROM and MOVED_TO with one cookie",
          equals("MOVED_FROM f, MOVED_TO g") && evs[0].cookie != 0 && evs[0].cookie == evs[1].cookie, detail);

    unlink("d/g");
    mkdir("d/sub", 0755);
    rmdir("d/sub");
    take(in);
    snprintf(detail, sizeof(detail), "%s", describe());
    check("delete, mkdir and rmdir: DELETE, CREATE|ISDIR, DELETE|ISDIR",
          equals("DELETE g, CREATE|ISDIR sub, DELETE|ISDIR sub"), detail);

    fd = open("d", O_RDONLY | O_DIRECTORY);
    char ents[4096];
    syscall(SYS_getdents64, fd, ents, sizeof(ents));
    close(fd);
    take(in);
    snprintf(detail, sizeof(detail), "%s", describe());
    check("reading the directory: OPEN, ACCESS, CLOSE_NOWRITE, each |ISDIR",
          equals("OPEN|ISDIR , ACCESS|ISDIR , CLOSE_NOWRITE|ISDIR "), detail);

    // 2. a watched directory renamed is still watched
    rename("d", "d2");
    touch("d2/x");
    take(in);
    snprintf(detail, sizeof(detail), "%s", describe());
    check("renamed: MOVE_SELF, then still watched",
          equals("MOVE_SELF , CREATE x, OPEN x, CLOSE_WRITE x"), detail);
    inotify_rm_watch(in, wd);
    take(in);
    int again = inotify_rm_watch(in, wd);
    int again_errno = errno;
    snprintf(detail, sizeof(detail), "%s; again %d (%s)", describe(), again, strerror(again_errno));
    check("rm_watch: IGNORED; again: EINVAL", equals("IGNORED ") && again < 0 && again_errno == EINVAL, detail);

    // 3. a file: modify, then unlinked: ATTRIB, DELETE_SELF, IGNORED
    touch("d2/y");
    wd = inotify_add_watch(in, "d2/y", IN_ALL_EVENTS);
    fd = open("d2/y", O_WRONLY);
    write(fd, "z", 1);
    close(fd);
    unlink("d2/y");
    take(in);
    snprintf(detail, sizeof(detail), "%s", describe());
    check("a watched file: its events without a name; unlinked: ATTRIB, DELETE_SELF, IGNORED",
          equals("OPEN , MODIFY , CLOSE_WRITE , ATTRIB , DELETE_SELF , IGNORED "), detail);

    // 4. unlinked while open: DELETE_SELF waits for the last close
    touch("d2/z");
    wd = inotify_add_watch(in, "d2/z", IN_ATTRIB | IN_DELETE_SELF);
    fd = open("d2/z", O_RDONLY);
    unlink("d2/z");
    take(in);
    char before_close[256];
    snprintf(before_close, sizeof(before_close), "%s", describe());
    int was = equals("ATTRIB ");
    close(fd);
    take(in);
    snprintf(detail, sizeof(detail), "while open: %s; after close: %s", before_close, describe());
    check("unlinked while open: ATTRIB, then at the close DELETE_SELF, IGNORED",
          was && equals("DELETE_SELF , IGNORED "), detail);

    // 5. watch flags
    touch("d2/o");
    int w1 = inotify_add_watch(in, "d2/o", IN_MODIFY);
    int w2 = inotify_add_watch(in, "d2/o", IN_ATTRIB | IN_MASK_ADD);
    int w3 = inotify_add_watch(in, "d2/o", IN_ATTRIB | IN_MASK_CREATE);
    int w3_errno = errno;
    int w4 = inotify_add_watch(in, "d2/o", IN_ATTRIB | IN_ONLYDIR);
    int w4_errno = errno;
    chmod("d2/o", 0600);
    fd = open("d2/o", O_WRONLY);
    write(fd, "q", 1);
    close(fd);
    take(in);
    snprintf(detail, sizeof(detail), "wds %d %d, MASK_CREATE %d (%s), ONLYDIR %d (%s); %s", w1, w2, w3,
             strerror(w3_errno), w4, strerror(w4_errno), describe());
    check("MASK_ADD on the same file: its wd, both events; MASK_CREATE: EEXIST; ONLYDIR on a file: ENOTDIR",
          w1 > 0 && w2 == w1 && w3 < 0 && w3_errno == EEXIST && w4 < 0 && w4_errno == ENOTDIR &&
          equals("ATTRIB , MODIFY "), detail);
    inotify_rm_watch(in, w1);
    take(in);

    wd = inotify_add_watch(in, "d2/o", IN_ATTRIB | IN_ONESHOT);
    chmod("d2/o", 0644);
    chmod("d2/o", 0600);
    take(in);
    snprintf(detail, sizeof(detail), "%s", describe());
    check("ONESHOT: one event, then IGNORED", equals("ATTRIB , IGNORED "), detail);

    // 6. reading: EAGAIN, poll, FIONREAD, a buffer too small
    char small[sizeof(struct inotify_event)];
    ssize_t empty = read(in, small, sizeof(small));
    int empty_errno = errno;
    wd = inotify_add_watch(in, "d2", IN_CREATE);
    struct pollfd p = {.fd = in, .events = POLLIN};
    int before = poll(&p, 1, 0);
    touch("d2/a-long-name");
    p.revents = 0;
    int after = poll(&p, 1, 0);
    int queued = -1;
    ioctl(in, FIONREAD, &queued);
    ssize_t too_small = read(in, small, sizeof(small));
    int small_errno = errno;
    take(in);
    snprintf(detail, sizeof(detail), "empty %zd (%s), poll %d then %d (%#x), FIONREAD %d, small %zd (%s); %s", empty,
             strerror(empty_errno), before, after, p.revents, queued, too_small, strerror(small_errno), describe());
    check("EAGAIN, poll, FIONREAD (16 + the name padded to 16), a buffer too small: EINVAL",
          empty < 0 && empty_errno == EAGAIN && before == 0 && after == 1 && (p.revents & POLLIN) &&
          queued == 32 && too_small < 0 && small_errno == EINVAL && equals("CREATE a-long-name"), detail);
    inotify_rm_watch(in, wd);
    take(in);

    // 7. the order of a directory's and a file's own events, as Linux's
    touch("d2/m");
    touch("d2/n");
    mkdir("d2/k", 0755);
    wd = inotify_add_watch(in, "d2", IN_ALL_EVENTS);
    int wm = inotify_add_watch(in, "d2/m", IN_ALL_EVENTS);
    int wk = inotify_add_watch(in, "d2/k", IN_ALL_EVENTS);
    chmod("d2/m", 0600);
    link("d2/m", "d2/m2");
    unlink("d2/m2");
    take(in);
    char first[400];
    snprintf(first, sizeof(first), "%s", describe());
    // (the link's ATTRIB of m is the same event as chmod's: merged)
    int ok_first = equals("ATTRIB m, ATTRIB , CREATE m2, ATTRIB , DELETE m2");
    rename("d2/n", "d2/m"); // over m, its last link
    take(in);
    char second[400];
    snprintf(second, sizeof(second), "%s", describe());
    int ok_second = equals("MOVED_FROM n, MOVED_TO m, ATTRIB , DELETE_SELF , IGNORED ");
    rmdir("d2/k");
    take(in);
    snprintf(detail, sizeof(detail), "chmod, link, unlink: %s; rename over m: %s; rmdir k: %s (wds %d %d %d)", first,
             second, describe(), wd, wm, wk);
    check("the directory's event before the file's own; a rename over a file; rmdir of a watched directory",
          ok_first && ok_second && equals("DELETE_SELF , IGNORED , DELETE|ISDIR k"), detail);
    inotify_rm_watch(in, wd);
    take(in);
    unlink("d2/m");

    // 8. a unix socket bound to a path: bind makes the file
    wd = inotify_add_watch(in, "d2", IN_CREATE | IN_DELETE);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "d2/sock");
    int bound = bind(sock, (struct sockaddr *) &addr, sizeof(addr));
    close(sock);
    unlink("d2/sock");
    take(in);
    snprintf(detail, sizeof(detail), "bind %d: %s", bound, describe());
    check("bind makes a socket file: CREATE", bound == 0 && equals("CREATE sock, DELETE sock"), detail);
    inotify_rm_watch(in, wd);
    take(in);

    // 9. the queue's end: 16384 events, then Q_OVERFLOW
    touch("d2/p");
    touch("d2/q");
    wd = inotify_add_watch(in, "d2", IN_MODIFY);
    int fp = open("d2/p", O_WRONLY), fq = open("d2/q", O_WRONLY);
    for (int i = 0; i < 8300; i++) {
        write(fp, "1", 1);
        write(fq, "1", 1);
    }
    close(fp);
    close(fq);
    static char big[1 << 20] __attribute__((aligned(8)));
    ssize_t n = read(in, big, sizeof(big));
    int count = 0, overflow_last = 0;
    for (char *q = big; q < big + n;) {
        struct inotify_event *e = (struct inotify_event *) q;
        count++;
        overflow_last = e->mask == IN_Q_OVERFLOW && e->wd == -1;
        q += sizeof(*e) + e->len;
    }
    snprintf(detail, sizeof(detail), "%d events, the last Q_OVERFLOW %d", count, overflow_last);
    check("a full queue: 16384 events and Q_OVERFLOW", count == 16385 && overflow_last, detail);

    close(in);
    unlink("d2/x");
    unlink("d2/o");
    unlink("d2/a-long-name");
    unlink("d2/p");
    unlink("d2/q");
    rmdir("d2");
    chdir("/");
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
