// Regression test for the device nodes of a filesystem imported on iOS before
// fakefs_import stored device numbers in iSH's encoding: they held Darwin's
// makedev (major << 24 | minor), read as major 0, and failed to open with
// ENXIO ("Couldn't open /dev/null: No such device or address").
//
// run.sh imports a tarball with /dev/null (1,3), /dev/zero (1,5) and /dev/tty7
// (1,3, not its usual 4,7), rewrites the first two in meta.db as such an import
// stored them, and runs this as init. Boot must have recreated those two, and
// left /dev/tty7 alone: it is a valid node, just not the one iSH would make.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_dev_nodes regress_dev_nodes.c

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int passed, failed;

static void check(const char *name, int ok) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", name);
    }
}

static int is_device(const char *path, unsigned major_, unsigned minor_) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISCHR(st.st_mode) &&
        major(st.st_rdev) == major_ && minor(st.st_rdev) == minor_;
}

int main(void) {
    int fd = open("/dev/null", O_RDWR);
    check("/dev/null opens", fd >= 0);
    check("/dev/null takes a write", fd >= 0 && write(fd, "x", 1) == 1);
    close(fd);

    char buf[4] = {1, 1, 1, 1};
    fd = open("/dev/zero", O_RDONLY);
    check("/dev/zero opens", fd >= 0);
    check("/dev/zero reads zeros", fd >= 0 && read(fd, buf, sizeof(buf)) == sizeof(buf) &&
            memcmp(buf, "\0\0\0\0", sizeof(buf)) == 0);
    close(fd);

    check("/dev/null is 1,3", is_device("/dev/null", 1, 3));
    check("/dev/zero is 1,5", is_device("/dev/zero", 1, 5));
    check("/dev/tty7 is left 1,3", is_device("/dev/tty7", 1, 3));

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
