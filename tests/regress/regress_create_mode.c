// Regression test for the mode of what is made: the mode asked for less the
// process's umask, and nothing more taken.
//
// iSH's realfs made files, directories and FIFOs with the host's umask
// applied too, on top of the guest's: under an umask of 053 a file asked
// 0666 was 0604, not 0624, where the host's umask was 022 (CPython's
// test_subprocess test_umask, through iSH's Linux CLI). fakefs keeps the
// guest's mode itself.
//
// Checked, under an umask of 053: open with O_CREAT of 0666 makes 0624;
// mkdir of 0777 makes 0724; mkfifo of 0666 makes 0624; and with an umask of
// 0, open with O_CREAT of 0777 makes 0777. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_create_mode regress_create_mode.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

static void expect(const char *name, const char *path, int r, mode_t want) {
    int e = errno;
    struct stat st = {0};
    lstat(path, &st);
    char detail[100];
    snprintf(detail, sizeof(detail), "%d (%s), mode %04o", r, r < 0 ? strerror(e) : "ok", st.st_mode & 07777);
    check(name, r >= 0 && (st.st_mode & 07777) == want, detail);
}

int main(void) {
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/regress_create_mode.%d", getpid());
    mkdir(dir, 0700);
    chdir(dir);

    umask(053);
    int fd = open("file", O_CREAT | O_WRONLY, 0666);
    expect("open with O_CREAT of 0666 under umask 053", "file", fd, 0624);
    if (fd >= 0)
        close(fd);
    expect("mkdir of 0777 under umask 053", "dir", mkdir("dir", 0777), 0724);
    expect("mkfifo of 0666 under umask 053", "fifo", mkfifo("fifo", 0666), 0624);

    umask(0);
    fd = open("open", O_CREAT | O_WRONLY, 0777);
    expect("open with O_CREAT of 0777 under umask 0", "open", fd, 0777);
    if (fd >= 0)
        close(fd);

    unlink("file");
    unlink("fifo");
    unlink("open");
    rmdir("dir");
    chdir("/");
    rmdir(dir);
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
