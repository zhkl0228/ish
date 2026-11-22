// Run as init: the main thread leaves first with pthread_exit, and the other
// thread ends the process, with exit(5) ("exit") or by returning ("return").
// iSH took init whose last thread was not pid 1 for an orphan process and
// never halted: the command line iSH hung instead of exiting (5 and 0 here).
// Java hit it: after exit_group its last thread is a VM thread.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_init_thread_exit regress_init_thread_exit.c
// Run:   ish -r <rootfs> /tmp/regress_init_thread_exit exit|return

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int do_exit;

static void *last_thread(void *arg) {
    (void) arg;
    struct timespec ts = {.tv_nsec = 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
    if (do_exit)
        exit(5);
    return NULL;
}

int main(int argc, char **argv) {
    do_exit = argc > 1 && strcmp(argv[1], "exit") == 0;
    pthread_t thread;
    pthread_create(&thread, NULL, last_thread, NULL);
    pthread_exit(NULL);
}
