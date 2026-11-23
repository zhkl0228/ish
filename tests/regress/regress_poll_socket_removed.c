// Regression test for a socket taken out of an epoll set while a thread
// waits on it.
//
// iSH marks a task that waits on a socket (sockrestart, for the iOS suspend
// dance), once per fd before the wait and once per fd after it. A socket
// another thread removed from the set meanwhile was marked only before, so
// the task stayed on the list of waiting tasks after it exited and was
// freed, and the next thread to wait on a socket wrote into it: heap
// corruption, which ASan caught in git clone (curl) and which otherwise
// crashed iSH later in free().
//
// Each round, a thread waits on a socket in an epoll set while the main
// thread removes and closes it; the thread wakes at its timeout and exits.
// Then threads wait on a socket again. iSH also says so when a task exits with
// socket waits not ended ("sockrestart: pid N exits with ..."), which the old
// marking did in every round: run.sh fails on that line as on a failed round,
// since the corruption it caused did not always crash. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_poll_socket_removed regress_poll_socket_removed.c

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int epfd;

static void *wait_epoll(void *arg) {
    (void) arg;
    struct epoll_event ev;
    epoll_wait(epfd, &ev, 1, 300);
    return NULL;
}

static void *wait_poll(void *arg) {
    int fd = *(int *) arg;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    poll(&p, 1, 50);
    return NULL;
}

int main(void) {
    const int rounds = 40;
    for (int round = 0; round < rounds; round++) {
        epfd = epoll_create1(0);
        int s[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, s);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = s[0]};
        epoll_ctl(epfd, EPOLL_CTL_ADD, s[0], &ev);

        pthread_t waiter;
        pthread_create(&waiter, NULL, wait_epoll, NULL);
        struct timespec settle = {0, 50 * 1000 * 1000};
        nanosleep(&settle, NULL);
        epoll_ctl(epfd, EPOLL_CTL_DEL, s[0], NULL);
        close(s[0]);
        pthread_join(waiter, NULL);
        close(s[1]);
        close(epfd);

        // Waits on sockets by threads that come and go, each one a new
        // entry on the list the waiter above may have been left on.
        int t[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, t);
        pthread_t pollers[4];
        for (int i = 0; i < 4; i++)
            pthread_create(&pollers[i], NULL, wait_poll, &t[0]);
        for (int i = 0; i < 4; i++)
            pthread_join(pollers[i], NULL);
        close(t[0]);
        close(t[1]);
        // churn the heap, where a write into a freed task shows up
        for (int i = 0; i < 64; i++)
            free(malloc(64 + i * 32));
    }
    printf("%d passed, 0 failed\n", rounds);
    return 0;
}
