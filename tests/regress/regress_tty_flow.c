// Regression test for tcdrain, tcsendbreak and tcflow on a pseudo-terminal.
//
// iSH's tty said ENOTTY of TCSBRK (tcdrain, tcsendbreak) and TCXONC
// (tcflow): CPython's test_termios.
//
// Checked: tcdrain and tcsendbreak succeed; tcflow with TCOOFF suspends the
// slave's output, a write waiting (a non-blocking one EAGAIN), and TCOON lets
// it through to the master; TCIOFF and TCION send the STOP and START
// characters to the master; tcflow with -1 is EINVAL. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_tty_flow regress_tty_flow.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
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

static const char *result(int r, int e) {
    static char buf[80];
    snprintf(buf, sizeof(buf), "%d (%s)", r, r < 0 ? strerror(e) : "ok");
    return buf;
}

static int slave;
static volatile int written;

static void *writer(void *arg) {
    (void) arg;
    write(slave, "def", 3);
    written = 1;
    return NULL;
}

// what the master has to read within the time given, in ms
static ssize_t read_master(int master, char *buf, size_t size, int ms) {
    struct pollfd p = {.fd = master, .events = POLLIN};
    if (poll(&p, 1, ms) <= 0)
        return 0;
    return read(master, buf, size);
}

int main(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(master);
    unlockpt(master);
    slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    struct termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    char buf[64];
    char detail[120];

    int r = tcdrain(slave);
    int e = errno;
    check("tcdrain", r == 0, result(r, e));
    r = tcsendbreak(slave, 0);
    e = errno;
    check("tcsendbreak", r == 0, result(r, e));

    r = tcflow(slave, TCOOFF);
    e = errno;
    check("tcflow TCOOFF", r == 0, result(r, e));
    int flags = fcntl(slave, F_GETFL);
    fcntl(slave, F_SETFL, flags | O_NONBLOCK);
    r = write(slave, "x", 1);
    e = errno;
    fcntl(slave, F_SETFL, flags);
    check("a non-blocking write while it is suspended", r == -1 && e == EAGAIN, result(r, e));
    pthread_t t1;
    pthread_create(&t1, NULL, writer, NULL);
    ssize_t n = read_master(master, buf, sizeof(buf), 300);
    snprintf(detail, sizeof(detail), "the master read %zd, the write %s", n, written ? "done" : "waiting");
    check("a write waits while output is suspended", n == 0 && !written, detail);
    r = tcflow(slave, TCOON);
    e = errno;
    pthread_join(t1, NULL);
    n = read_master(master, buf, sizeof(buf), 2000);
    snprintf(detail, sizeof(detail), "%s, the master read %zd: '%.*s'", result(r, e), n, (int) (n > 0 ? n : 0), buf);
    check("tcflow TCOON lets it through", r == 0 && n == 3 && memcmp(buf, "def", 3) == 0, detail);

    r = tcflow(slave, TCIOFF);
    int r2 = tcflow(slave, TCION);
    e = errno;
    n = read_master(master, buf, sizeof(buf), 2000);
    if (n == 1)
        n += read_master(master, buf + 1, sizeof(buf) - 1, 2000);
    snprintf(detail, sizeof(detail), "%d and %d, the master read %zd: %#x %#x", r, r2, n, n > 0 ? buf[0] : 0,
             n > 1 ? buf[1] : 0);
    check("tcflow TCIOFF and TCION send STOP and START", r == 0 && r2 == 0 && n == 2 && buf[0] == t.c_cc[VSTOP] &&
          buf[1] == t.c_cc[VSTART], detail);

    r = tcflow(slave, -1);
    e = errno;
    check("tcflow -1", r == -1 && e == EINVAL, result(r, e));

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
