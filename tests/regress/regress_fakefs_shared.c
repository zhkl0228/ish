// Regression test for fakefs's lookup cache under a second process of the
// same meta.db (run.sh runs two ish on one fakefs of its own: the app and its
// file provider share a database the same way).
//
// A stat of a fakefs path takes the path's meta.db row from a cache while no
// connection has committed since it was read: the version is the WAL index
// header's, which every commit of every connection changes.
//
// "watch": makes /tmp/shared/f (0644), says so (/tmp/shared/ready), then
// stats it over and over (from the cache, mostly) until its mode is 0600:
// exit 0, or 1 after 20 s. "change": chmod 0600 of it, from the other ish.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_fakefs_shared regress_fakefs_shared.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "change") == 0) {
        if (chmod("/tmp/shared/f", 0600) < 0) {
            printf("chmod: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }
    if (argc != 2 || strcmp(argv[1], "watch") != 0) {
        printf("usage: %s watch|change\n", argv[0]);
        return 2;
    }
    mkdir("/tmp/shared", 0755);
    int fd = open("/tmp/shared/f", O_CREAT | O_WRONLY, 0644);
    if (fd < 0 || fchmod(fd, 0644) < 0) {
        printf("/tmp/shared/f: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    struct stat s;
    if (stat("/tmp/shared/f", &s) < 0 || (s.st_mode & 07777) != 0644) {
        printf("FAIL the file's mode before: %o\n", (unsigned) (s.st_mode & 07777));
        return 1;
    }
    fd = open("/tmp/shared/ready", O_CREAT | O_WRONLY, 0644);
    close(fd);
    double start = now();
    long stats = 0;
    while (now() - start < 20) {
        if (stat("/tmp/shared/f", &s) < 0) {
            printf("FAIL stat: %s\n", strerror(errno));
            return 1;
        }
        stats++;
        if ((s.st_mode & 07777) == 0600) {
            printf("PASS another process's chmod seen by stat, after %ld of them\n", stats);
            printf("1 passed, 0 failed\n");
            return 0;
        }
    }
    printf("FAIL another process's chmod seen by stat: the mode still %o after %ld stats in 20 s\n",
           (unsigned) (s.st_mode & 07777), stats);
    printf("0 passed, 1 failed\n");
    return 1;
}
