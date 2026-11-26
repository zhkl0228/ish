// Regression test for the floating-point environment: exception flags and
// rounding mode (FPSR and FPCR).
//
// iSH kept the guest's FPCR and FPSR in its CPU state only: its FP
// instructions, which run natively, saw the host's. fetestexcept never found
// a flag raised (numpy's "divide by zero encountered" warnings) and
// fesetround changed nothing. They are now the host's while the guest runs.
// The FPCR trap enables are not: an exception would kill iSH with SIGFPE.
//
// FPSR then moved from each entry to and exit from translated code to the
// run loop's: every indirect branch left translated code, and reading and
// writing FPSR there took a third of the host's time. Cases 7 and 8 hold the
// flags to that: kept over many indirect calls and returns, and each thread's
// its own.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_fenv regress_fenv.c -lm

#define _GNU_SOURCE
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
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

// volatile, so the compiler computes nothing ahead of time
static volatile double one = 1.0, two = 2.0, three = 3.0, zero = 0.0, minus_one = -1.0, big = DBL_MAX,
                       tiny = DBL_MIN, half_up = 2.5;
static volatile double sink;

static void describe_flags(char *out, size_t size, int flags) {
    snprintf(out, size, "flags:%s%s%s%s%s", flags & FE_INVALID ? " invalid" : "",
             flags & FE_DIVBYZERO ? " divbyzero" : "", flags & FE_OVERFLOW ? " overflow" : "",
             flags & FE_UNDERFLOW ? " underflow" : "", flags & FE_INEXACT ? " inexact" : "");
}

// The flags an operation raises, from a clean start.
static int raised(void (*op)(void)) {
    feclearexcept(FE_ALL_EXCEPT);
    op();
    return fetestexcept(FE_ALL_EXCEPT);
}

static void divide_by_zero(void) { sink = one / zero; }
static void invalid(void) { sink = sqrt(minus_one); }
static void overflow(void) { sink = big * two; }
static void underflow(void) { sink = tiny / 1e10; }
static void inexact(void) { sink = one / three; }
static void exact(void) { sink = one + two; }

static void check_raises(const char *name, void (*op)(void), int expected) {
    char detail[100];
    int flags = raised(op);
    describe_flags(detail, sizeof(detail), flags);
    check(name, (flags & FE_ALL_EXCEPT) == expected, detail);
}

static int nothing(int i) { return i + 1; }
static int twice(int i) { return i * 2; }

// The flags a new thread raises, and what it finds raised
static void *thread_raises_invalid(void *arg) {
    (void) arg;
    feclearexcept(FE_ALL_EXCEPT);
    invalid();
    return (void *) (intptr_t) fetestexcept(FE_ALL_EXCEPT);
}

static void on_usr1(int sig) {
    (void) sig;
    feclearexcept(FE_ALL_EXCEPT);
    fesetround(FE_TONEAREST);
}

int main(void) {
    char detail[200];

    // 1. each exception and its flag
    check_raises("1/0 raises divbyzero", divide_by_zero, FE_DIVBYZERO);
    check_raises("sqrt(-1) raises invalid", invalid, FE_INVALID);
    check_raises("DBL_MAX*2 raises overflow and inexact", overflow, FE_OVERFLOW | FE_INEXACT);
    check_raises("DBL_MIN/1e10 raises underflow and inexact", underflow, FE_UNDERFLOW | FE_INEXACT);
    check_raises("1/3 raises inexact", inexact, FE_INEXACT);
    check_raises("1+2 raises nothing", exact, 0);

    // 2. the flags stay raised over system calls
    feclearexcept(FE_ALL_EXCEPT);
    divide_by_zero();
    syscall(SYS_getpid);
    sched_yield();
    int flags = fetestexcept(FE_ALL_EXCEPT);
    describe_flags(detail, sizeof(detail), flags);
    check("flags kept over system calls", flags == FE_DIVBYZERO, detail);

    // 3. the rounding mode, also over a system call
    feclearexcept(FE_ALL_EXCEPT);
    double nearest = one / three;
    fesetround(FE_UPWARD);
    sched_yield();
    double up = one / three, rint_up = rint(half_up);
    fesetround(FE_DOWNWARD);
    double down = one / three, rint_down = rint(half_up);
    int mode = fegetround();
    fesetround(FE_TONEAREST);
    snprintf(detail, sizeof(detail), "1/3 up %a, nearest %a, down %a; rint(2.5) up %g, down %g; mode %#x", up,
             nearest, down, rint_up, rint_down, mode);
    // 1/3 to nearest is one of the two, here the one rounded down
    check("fesetround: upward and downward", up > down && (nearest == up || nearest == down) && rint_up == 3 &&
          rint_down == 2 && mode == FE_DOWNWARD, detail);

    // 4. a child has its parent's flags and rounding mode
    feclearexcept(FE_ALL_EXCEPT);
    divide_by_zero();
    fesetround(FE_TOWARDZERO);
    pid_t child = fork();
    if (child == 0)
        _exit(fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO && fegetround() == FE_TOWARDZERO ? 0 : 1);
    int status;
    waitpid(child, &status, 0);
    fesetround(FE_TONEAREST);
    snprintf(detail, sizeof(detail), "child status %#x", status);
    check("fork: the child's flags and rounding mode", WIFEXITED(status) && WEXITSTATUS(status) == 0, detail);

    // 5. a signal handler's changes undone by sigreturn
    feclearexcept(FE_ALL_EXCEPT);
    divide_by_zero();
    fesetround(FE_UPWARD);
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    flags = fetestexcept(FE_ALL_EXCEPT);
    mode = fegetround();
    fesetround(FE_TONEAREST);
    describe_flags(detail, sizeof(detail), flags);
    snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail), "; mode %#x", mode);
    check("after a handler: the flags and mode it changed are back", flags == FE_DIVBYZERO && mode == FE_UPWARD,
          detail);

    // 6. A trap enable (FPCR.DZE) either sticks and 1/0 traps, natively on a
    // CPU with trapping, or reads back as 0 and 1/0 just raises the flag.
    child = fork();
    if (child == 0) {
        uint64_t fpcr = 1 << 9;
        __asm__ volatile("msr fpcr, %0" ::"r"(fpcr));
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        int trapping = (fpcr >> 9) & 1;
        feclearexcept(FE_ALL_EXCEPT);
        divide_by_zero();
        _exit(!trapping && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO ? 0 : 2 + trapping);
    }
    waitpid(child, &status, 0);
    snprintf(detail, sizeof(detail), "status %#x", status);
    check("FPCR.DZE: a trap, or ignored and the flag raised",
          (WIFSIGNALED(status) && WTERMSIG(status) == SIGFPE) || (WIFEXITED(status) && WEXITSTATUS(status) == 0),
          detail);

    // 7. the flags stay raised over indirect calls and returns, each one a
    // block an indirect branch ends; one raised among them is added
    feclearexcept(FE_ALL_EXCEPT);
    divide_by_zero();
    int (*volatile calls[2])(int) = {nothing, twice};
    void (*volatile raise_inexact)(void) = inexact;
    int acc = 0;
    for (int i = 0; i < 20000; i++) {
        acc = calls[i & 1](acc) & 0xffff;
        if (i == 10000)
            raise_inexact();
    }
    flags = fetestexcept(FE_ALL_EXCEPT);
    describe_flags(detail, sizeof(detail), flags);
    check("flags kept over 20000 indirect calls", flags == (FE_DIVBYZERO | FE_INEXACT) && acc >= 0, detail);

    // 8. a thread's flags are its own
    feclearexcept(FE_ALL_EXCEPT);
    pthread_t thread;
    void *thread_flags = NULL;
    pthread_create(&thread, NULL, thread_raises_invalid, NULL);
    pthread_join(thread, &thread_flags);
    flags = fetestexcept(FE_ALL_EXCEPT);
    char mine[80], theirs[80];
    describe_flags(mine, sizeof(mine), flags);
    describe_flags(theirs, sizeof(theirs), (int) (intptr_t) thread_flags);
    snprintf(detail, sizeof(detail), "the thread's %s; mine %s", theirs, mine);
    check("a thread's flags are its own", (intptr_t) thread_flags == FE_INVALID && flags == 0, detail);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
