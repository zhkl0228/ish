// Regression test for set_tid_address's return value.
//
// Linux returns the caller's thread id; iSH returned the process's (tgid),
// which is the same in a process's main thread, where musl and glibc call it,
// but not in another thread.
//
// The main thread and another thread each call it. A thread whose clear_tid
// is not the C library's must not leave by itself (the thread list lock its
// exit would release is musl's), so the other thread waits until the process
// ends with exit. Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -pthread -o regress_set_tid_address regress_set_tid_address.c

#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
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

static int main_word, thread_word;
static long thread_returned, thread_tid;
static sem_t done, never;

static void *thread(void *arg) {
    (void) arg;
    thread_tid = syscall(SYS_gettid);
    thread_returned = syscall(SYS_set_tid_address, &thread_word);
    sem_post(&done);
    sem_wait(&never);
    return NULL;
}

int main(void) {
    char detail[128];
    sem_init(&done, 0, 0);
    sem_init(&never, 0, 0);

    pthread_t t;
    pthread_create(&t, NULL, thread, NULL);
    sem_wait(&done);
    snprintf(detail, sizeof(detail), "returned %ld, the thread's id %ld, the process's %d", thread_returned,
             thread_tid, getpid());
    check("in another thread: that thread's id", thread_returned == thread_tid && thread_tid != getpid(), detail);

    long returned = syscall(SYS_set_tid_address, &main_word);
    snprintf(detail, sizeof(detail), "returned %ld, the process's id %d", returned, getpid());
    check("in the main thread: the process's id", returned == getpid(), detail);

    printf("%d passed, %d failed\n", passed, failed);
    exit(failed != 0);
}
