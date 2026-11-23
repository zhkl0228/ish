// Regression test for SO_PEERCRED on unix sockets.
//
// Linux takes a socket's peer credentials when the connection is made and
// keeps them: a connecting socket gets those of the process that called
// listen() (at that time), the accepted socket those of the process that
// connected, both sockets of a socketpair those of its creator, and the pid
// is the process's, not the thread's. iSH gave the connecting end the
// credentials of the process that accepted, so getsockopt waited, without
// end and deaf to signals, for an accept that may never come; and it gave
// the thread id. A socket whose other end closed before accept had none.
//
// Also: listen() twice on one socket, which Linux allows (to change the
// backlog), put the socket twice on iSH's list of listening sockets; closing
// it then left a freed socket on the list. And an abstract name anyone had
// connected to could not be bound again after its socket closed (EEXIST).
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_peercred regress_peercred.c

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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

static struct ucred peercred(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    memset(&cred, 0x55, sizeof(cred));
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        perror("getsockopt SO_PEERCRED");
        cred.pid = -2;
    }
    return cred;
}

static int is(struct ucred c, pid_t pid, uid_t uid, gid_t gid, char *detail) {
    snprintf(detail, 128, "got pid=%d uid=%d gid=%d, want pid=%d uid=%d gid=%d",
             c.pid, (int) c.uid, (int) c.gid, pid, (int) uid, (int) gid);
    return c.pid == pid && c.uid == uid && c.gid == gid;
}

// An abstract address: iSH cannot bind a path on a realfs mount, as run.sh's
// rootfs is. Path and abstract names lead to the same listener lookup.
static struct sockaddr_un addr;
static socklen_t addr_len;

static int listener(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(s, (struct sockaddr *) &addr, addr_len) < 0 || listen(s, 8) < 0)
        perror("bind/listen");
    return s;
}

static int connector(void) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(c, (struct sockaddr *) &addr, addr_len) < 0)
        perror("connect");
    return c;
}

// A child that reports its finding as its exit status: 0 for a pass.
static int child_status(pid_t child) {
    int status;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void *connect_thread(void *arg) {
    *(int *) arg = connector();
    return NULL;
}

int main(void) {
    char detail[128];
    pid_t me = getpid();
    uid_t uid = geteuid();
    gid_t gid = getegid();
    addr.sun_family = AF_UNIX;
    int n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "regress_peercred.%d", me);
    addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + n;

    // The connecting end gets the listener's credentials at once, with no
    // accept ever done (the alarm ends a getsockopt that waits for one).
    int l = listener();
    pid_t child = fork();
    if (child == 0) {
        alarm(5);
        int c = connector();
        _exit(is(peercred(c), me, uid, gid, detail) ? 0 : 1);
    }
    int st = child_status(child);
    snprintf(detail, sizeof(detail), "child exit status %d (1: wrong credentials, 142: getsockopt waited for accept)", st);
    check("connecting end, before accept: listener's", st == 0, detail);
    // The child has exited: its end closed before this accept.
    int a = accept(l, NULL, NULL);
    check("accepted end, other end closed before accept: connector's", is(peercred(a), child, uid, gid, detail), detail);
    close(a);

    // ... the process that called listen(), not the one that accepts.
    child = fork();
    if (child == 0) {
        int a = accept(l, NULL, NULL);
        char b;
        read(a, &b, 1);
        _exit(0);
    }
    int c = connector();
    check("connecting end, accepted by another process: listener's", is(peercred(c), me, uid, gid, detail), detail);
    write(c, "x", 1);
    child_status(child);
    close(c);

    // The accepted end gets the connecting process's, and its pid, not the
    // id of the thread that connected.
    c = -1;
    pthread_t t;
    pthread_create(&t, NULL, connect_thread, &c);
    pthread_join(t, NULL);
    a = accept(l, NULL, NULL);
    check("accepted end, connected by a thread: process's", is(peercred(a), me, uid, gid, detail), detail);
    close(a);
    close(c);

    check("listening socket: its own", is(peercred(l), me, uid, gid, detail), detail);
    close(l);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    check("socketpair: creator's", is(peercred(sv[0]), me, uid, gid, detail) && is(peercred(sv[1]), me, uid, gid, detail), detail);
    close(sv[0]);
    close(sv[1]);

    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    check("unconnected socket: none", is(peercred(u), 0, (uid_t) -1, (gid_t) -1, detail), detail);
    close(u);

    // The name, connected to above, is free again; listen() twice, close;
    // then more listening sockets come and go.
    for (int i = 0; i < 4; i++) {
        l = listener();
        listen(l, 16);
        close(l);
    }
    l = listener();
    c = connector();
    a = accept(l, NULL, NULL);
    if (a < 0)
        snprintf(detail, sizeof(detail), "accept: %s", strerror(errno));
    check("listen twice, close, bind and listen again", a >= 0 && is(peercred(c), me, uid, gid, detail), detail);
    close(a);
    close(c);
    close(l);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
