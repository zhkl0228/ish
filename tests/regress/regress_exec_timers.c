// Regression test for POSIX timers across execve.
//
// Linux deletes a process's POSIX timers when it execs (begin_new_exec), as
// they name the old program's signals and values; setitimer's timer stays.
// iSH kept them: a timer whose signal the old program handled went on firing
// into the new one, where the signal took its default action. Here the new
// image dies of SIGUSR1 if the timer survived.
//
// Prints one line per case and "N passed, M failed". Natively passes.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_exec_timers regress_exec_timers.c

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void on_usr1(int sig) {
    (void) sig;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "after-exec") == 0) {
        // 10 timer periods; nanosleep resumes if a handled signal came
        struct timespec ts = {0, 500 * 1000000};
        while (nanosleep(&ts, &ts) != 0)
            continue;
        timer_t timer;
        struct sigevent sev = {.sigev_notify = SIGEV_NONE};
        int created = timer_create(CLOCK_MONOTONIC, &sev, &timer);
        // the first timer of a process without any: its id is the first one
        printf("after exec: alive, a new timer %s\n", created == 0 ? "created" : "not created");
        return created != 0;
    }

    int passed = 0, failed = 0;
    pid_t child = fork();
    if (child == 0) {
        signal(SIGUSR1, on_usr1);
        timer_t timer;
        struct sigevent sev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
        timer_create(CLOCK_MONOTONIC, &sev, &timer);
        struct itimerspec its = {.it_value = {0, 50 * 1000000}, .it_interval = {0, 50 * 1000000}};
        timer_settime(timer, 0, &its, NULL);
        execl("/proc/self/exe", argv[0], "after-exec", (char *) NULL);
        perror("execl");
        _exit(2);
    }
    int status;
    waitpid(child, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        passed++;
        printf("PASS a POSIX timer does not outlive exec\n");
    } else {
        failed++;
        if (WIFSIGNALED(status))
            printf("FAIL a POSIX timer does not outlive exec: the new image died of signal %d\n", WTERMSIG(status));
        else
            printf("FAIL a POSIX timer does not outlive exec: exit status %d\n", WEXITSTATUS(status));
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
