// Regression test for a page another thread remaps while one runs guest
// code: what the first thread does after the change must not go through its
// TLB's old entry for the page.
//
// A thread running guest code keeps its TLB entries: a page's host address
// is looked up once and used till the entry goes. A mapping changed under the
// address space's lock held to write (mmap, mprotect, a copy on write) has
// every thread out of guest code, and each flushes its TLB as it comes back.
// One changed with it held to read, the others running, has to take their
// entries of the page out itself: a page becoming code as its first block is
// translated (W^X: the others' writes to it must come back through the miss
// path, which invalidates the blocks translated from it).
//
// Checked, each in rounds:
// - a page made code: a function's first run translates it while another
//   thread writes a word of its page all the time, through a writable entry
//   of its TLB; told to, the same store of that thread (its loop does not
//   leave guest code) writes the function a new instruction, with no cache
//   maintenance, and the function's next run must run it (iSH's W^X, which
//   code from x86 relies on; hardware wants the cache cleaned). A thread
//   comes back to its run loop every 1024 blocks or so, and flushes its TLB
//   if the mappings changed: the entry is kept only that long, so a round
//   catches a missing invalidation only if the write comes before (some do:
//   500 rounds).
// - a page copied on write: after a fork the main thread writes a private page
//   another thread reads all the while, then a flag on a shared page (which no
//   copy on write moves); the reader, seeing the flag, must read the new
//   value, not the old copy, which is the child's now (200 rounds).
//
// Build: aarch64-linux-musl-gcc -static -O1 -o regress_tlb_cross_thread regress_tlb_cross_thread.c

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
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

static void pause_a_moment(long us) {
    struct timespec ts = {0, us * 1000};
    nanosleep(&ts, NULL);
}

// ---- a page made code

#define CODE_ROUNDS 500
#define MOV_W0(imm) (0x52800000u | ((uint32_t) (imm) << 5))
#define RET 0xd65f03c0u

static volatile uint32_t *code;   // a page of its own: the function at [0], a word written all the time at [512]
static int *ctl;                  // [0]: the round whose instruction the writer is to write, [1]: the round it wrote
static volatile uint32_t what;    // that instruction

static void *code_writer(void *arg) {
    (void) arg;
    for (int r = 1; r <= CODE_ROUNDS; r++) {
        // one store, through a writable entry for the page: the word at 512,
        // then (told to) the function's instruction, through it if it stayed
        for (;;) {
            int told = __atomic_load_n(&ctl[0], __ATOMIC_ACQUIRE) == r;
            code[told ? 0 : 512] = what;
            if (told)
                break;
        }
        __atomic_store_n(&ctl[1], r, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void page_made_code(void) {
    code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ctl = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || ctl == MAP_FAILED) {
        perror("mmap");
        failed++;
        return;
    }
    int (*fn)(void) = (int (*)(void)) (uintptr_t) code;
    pthread_t t;
    if (pthread_create(&t, NULL, code_writer, NULL) != 0) {
        perror("pthread_create");
        failed++;
        return;
    }
    int wrong = 0, first = 0, first_before = 0, first_after = 0;
    for (int r = 1; r <= CODE_ROUNDS; r++) {
        code[0] = MOV_W0(r);
        code[1] = RET;
        __builtin___clear_cache((char *) code, (char *) (code + 2));
        int before = fn();   // translated: the page is code now
        what = MOV_W0(r + 1);
        __atomic_store_n(&ctl[0], r, __ATOMIC_RELEASE);
        while (__atomic_load_n(&ctl[1], __ATOMIC_ACQUIRE) != r)
            ;
        int after = fn();
        if ((before != r || after != r + 1) && wrong++ == 0) {
            first = r;
            first_before = before;
            first_after = after;
        }
    }
    pthread_join(t, NULL);
    char detail[200];
    snprintf(detail, sizeof(detail), "%d of %d rounds ran the old code (round %d returned %d then %d, want %d then %d)",
             wrong, CODE_ROUNDS, first, first_before, first_after, first, first + 1);
    check("a page made code while another thread writes it: that thread's next write takes the new code in",
          wrong == 0, detail);
}

// ---- a page copied on write

#define COW_ROUNDS 200

static volatile int *data;   // a private page of its own: copied on write after a fork
static int *shared;          // [0]: the round's flag, [1]: the reader's acknowledgement
static int got[COW_ROUNDS + 1];
static volatile unsigned long sink;

static void *cow_reader(void *arg) {
    (void) arg;
    unsigned long s = 0;
    for (int r = 1; r <= COW_ROUNDS; r++) {
        // the private page read all the while: its entry stays
        while (__atomic_load_n(&shared[0], __ATOMIC_ACQUIRE) != r)
            s += *data;
        got[r] = *data;
        __atomic_store_n(&shared[1], r, __ATOMIC_RELEASE);
    }
    sink = s;
    return NULL;
}

static void page_copied_on_write(void) {
    shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    data = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED || data == MAP_FAILED) {
        perror("mmap");
        failed++;
        return;
    }
    *data = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, cow_reader, NULL) != 0) {
        perror("pthread_create");
        failed++;
        return;
    }
    int wrong = 0, first = 0;
    for (int r = 1; r <= COW_ROUNDS; r++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            failed++;
            return;
        }
        if (pid == 0) {
            // holds the old copy of data's page till killed
            for (;;)
                pause();
        }
        pause_a_moment(500);   // the reader back in guest code, data's page in its TLB again
        *data = r;             // a copy of the page for the parent
        __atomic_store_n(&shared[0], r, __ATOMIC_RELEASE);
        while (__atomic_load_n(&shared[1], __ATOMIC_ACQUIRE) != r)
            ;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        if (got[r] != r && wrong++ == 0)
            first = r;
    }
    pthread_join(t, NULL);
    char detail[200];
    snprintf(detail, sizeof(detail), "%d of %d rounds read the old copy (round %d read %d)", wrong, COW_ROUNDS, first,
             got[first]);
    check("a page copied on write by another thread: the new value after the flag", wrong == 0, detail);
}

int main(void) {
    page_made_code();
    page_copied_on_write();
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
