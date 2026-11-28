// Regression test for readdir on a fakefs root (run.sh runs it as the /init
// of a fakefs of its own).
//
// fakefs gives each entry its meta.db inode, found by the entry's path. It
// used to take a read transaction for each entry. Now it reads the host
// directory up to 64 entries ahead and looks them up in one transaction. The
// host's position after each entry is kept with it: telldir and getdents'
// d_off report it, and lseek and seekdir drop what was read ahead.
//
// Checked, on a directory of 200 files, more than three read-aheads:
// - every entry, each d_ino the inode stat gives ("." and ".." too);
// - telldir after the 100th entry, the rest read, seekdir back: the same rest;
// - rewinddir: all of them again;
// - getdents64 with room for one or two entries a call: the same entries,
//   and an lseek to an entry's d_off: the entries after it;
// - files deleted while the directory is read: no crash, each entry one of
//   the directory's.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_fakefs_readdir regress_fakefs_readdir.c

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FILES 200

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

static int named(const char *name) {   // f000..f199: its number, else -1
    if (name[0] != 'f' || strlen(name) != 4)
        return -1;
    int n = atoi(name + 1);
    return n >= 0 && n < FILES ? n : -1;
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

int main(void) {
    char path[300], detail[600] = "";
    mkdir("/tmp/d", 0755);
    for (int i = 0; i < FILES; i++) {
        snprintf(path, sizeof(path), "/tmp/d/f%03d", i);
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            perror(path);
            return 1;
        }
        close(fd);
    }
    struct stat dot, dotdot;
    stat("/tmp/d", &dot);
    stat("/tmp", &dotdot);

    // every entry, its inode stat's
    DIR *dir = opendir("/tmp/d");
    if (dir == NULL) {
        perror("opendir");
        return 1;
    }
    int seen[FILES] = {0}, others = 0, wrong = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        struct stat s;
        int n = named(e->d_name);
        if (strcmp(e->d_name, ".") == 0)
            s = dot;
        else if (strcmp(e->d_name, "..") == 0)
            s = dotdot;
        else if (n >= 0) {
            seen[n]++;
            snprintf(path, sizeof(path), "/tmp/d/%s", e->d_name);
            stat(path, &s);
        } else {
            others++;
            continue;
        }
        if (s.st_ino != e->d_ino && wrong++ == 0)
            snprintf(detail, sizeof(detail), "%s: d_ino %llu, stat's %llu", e->d_name, (unsigned long long) e->d_ino,
                     (unsigned long long) s.st_ino);
    }
    int missing = 0, twice = 0;
    for (int i = 0; i < FILES; i++) {
        missing += seen[i] == 0;
        twice += seen[i] > 1;
    }
    if (!wrong)
        snprintf(detail, sizeof(detail), "%d missing, %d twice, %d others", missing, twice, others);
    check("200 files: each entry once, its d_ino the inode stat gives", !wrong && !missing && !twice && !others, detail);

    // telldir after the 100th, the rest, seekdir back: the same rest
    rewinddir(dir);
    static char rest[FILES + 2][NAME_MAX + 1];
    int k = 0, nrest = 0;
    long pos = -1;
    while ((e = readdir(dir)) != NULL) {
        if (++k == 100)
            pos = telldir(dir);
        else if (k > 100 && nrest < FILES + 2)
            strcpy(rest[nrest++], e->d_name);
    }
    seekdir(dir, pos);
    int same = 0, again = 0;
    while ((e = readdir(dir)) != NULL) {
        if (again < nrest && strcmp(rest[again], e->d_name) == 0)
            same++;
        again++;
    }
    snprintf(detail, sizeof(detail), "%d after the 100th, %d after seekdir back, %d of them the same", nrest, again, same);
    check("seekdir to telldir after the 100th entry: the same entries after it", nrest > 0 && again == nrest && same == nrest,
          detail);

    rewinddir(dir);
    int count = 0, dots = 0;
    int seen2[FILES] = {0};
    while ((e = readdir(dir)) != NULL) {
        count++;
        int n = named(e->d_name);
        if (n >= 0)
            seen2[n]++;
        else
            dots++;
    }
    int first_missing = -1;
    for (int i = 0; i < FILES && first_missing < 0; i++)
        if (!seen2[i])
            first_missing = i;
    snprintf(detail, sizeof(detail), "%d entries, want %d (%d of them . or .., the first file missing %d)", count,
             FILES + 2, dots, first_missing);
    check("rewinddir: all of them again", count == FILES + 2, detail);
    closedir(dir);

    // getdents64 with room for one or two entries a call; lseek to a d_off
    int fd = open("/tmp/d", O_RDONLY | O_DIRECTORY);
    char buf[48];
    int64_t offs[FILES + 2];
    static char names[FILES + 2][NAME_MAX + 1];
    int got = 0, calls = 0;
    for (;;) {
        long r = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (r <= 0)
            break;
        calls++;
        for (long o = 0; o < r;) {
            struct linux_dirent64 *d = (struct linux_dirent64 *) (buf + o);
            if (got < FILES + 2) {
                offs[got] = d->d_off;
                strcpy(names[got], d->d_name);
            }
            got++;
            o += d->d_reclen;
        }
    }
    snprintf(detail, sizeof(detail), "%d entries in %d calls, want %d", got, calls, FILES + 2);
    check("getdents64 with room for an entry or two: every entry", got == FILES + 2 && calls >= got / 2, detail);
    int at = 150;
    lseek(fd, offs[at], SEEK_SET);
    long r = syscall(SYS_getdents64, fd, buf, sizeof(buf));
    struct linux_dirent64 *d = (struct linux_dirent64 *) buf;
    snprintf(detail, sizeof(detail), "after lseek to entry %d's d_off: %s, want %s", at, r > 0 ? d->d_name : "nothing",
             names[at + 1]);
    check("lseek to an entry's d_off: the entry after it next", r > 0 && strcmp(d->d_name, names[at + 1]) == 0, detail);
    close(fd);

    // files deleted while the directory is read
    dir = opendir("/tmp/d");
    k = 0;
    int bad = 0;
    while ((e = readdir(dir)) != NULL) {
        if (++k == 50)
            for (int i = 0; i < FILES; i += 3) {
                snprintf(path, sizeof(path), "/tmp/d/f%03d", i);
                unlink(path);
            }
        if (named(e->d_name) < 0 && strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            bad++;
    }
    closedir(dir);
    snprintf(detail, sizeof(detail), "%d entries read, %d not the directory's", k, bad);
    check("files deleted while the directory is read: each entry one of its", bad == 0 && k > 50, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
