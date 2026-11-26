#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include "util/timer.h"
#include "misc.h"

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data) {
//    assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);
    struct timer *timer = malloc(sizeof(struct timer));
    timer->clockid = clockid;
    timer->callback = callback;
    timer->data = data;
    timer->active = false;
    timer->silent = false;
    timer->thread_running = false;
    lock_init(&timer->lock);
    timer->dead = false;
    return timer;
}

void timer_free(struct timer *timer) {
    lock(&timer->lock);
    timer->active = false;
    if (timer->thread_running) {
        timer->dead = true;
        pthread_kill(timer->thread, SIGUSR1);
        unlock(&timer->lock);
    } else {
        unlock(&timer->lock);
        free(timer);
    }
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    lock(&timer->lock);
    while (true) {
        struct timespec remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        while (timer->active && timespec_positive(remaining)) {
            unlock(&timer->lock);
            nanosleep(&remaining, NULL);
            lock(&timer->lock);
            remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        }
        if (timer->active)
            timer->callback(timer->data);
        if (timer->active && timespec_positive(timer->interval)) {
            timer->start = timer->end;
            timer->end = timespec_add(timer->start, timer->interval);
        } else {
            // a one-shot timer that fired is disarmed, and says so (timer_get)
            timer->active = false;
            break;
        }
    }
    timer->thread_running = false;
    if (timer->dead)
        free(timer);
    else
        unlock(&timer->lock);
    return NULL;
}

// What is left of it, as Linux says: 0 if it is disarmed, which a one-shot
// timer is once it fired (its end, in the past, said a time below 0: what
// setitimer, timer_settime and alarm said of one); 1ns if it is due and its
// callback has yet to run, as an armed timer with a signal to deliver never
// says 0. A silent one-shot is 0 from its end (Linux's common_timer_get: "A
// single shot SIGEV_NONE timer must return 0, when it is expired!"): on a
// loaded host the thread wakes up late, and it said 1ns 80ms after its end.
static struct timer_spec timer_get_locked(struct timer *timer, struct timespec now) {
    struct timer_spec spec = {.interval = timer->interval};
    if (timer->active) {
        spec.value = timespec_subtract(timer->end, now);
        if (!timespec_positive(spec.value)) {
            if (timer->silent && !timespec_positive(timer->interval))
                spec.value = (struct timespec) {0};
            else
                spec.value = (struct timespec) {.tv_sec = 0, .tv_nsec = 1};
        }
    }
    return spec;
}

void timer_get(struct timer *timer, struct timer_spec *spec) {
    lock(&timer->lock);
    *spec = timer_get_locked(timer, timespec_now(timer->clockid));
    unlock(&timer->lock);
}

int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec) {
    return timer_set_with(timer, spec, oldspec, NULL);
}

int timer_set_with(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec,
                   timer_callback_t reset) {
    lock(&timer->lock);
    struct timespec now = timespec_now(timer->clockid);
    if (oldspec != NULL)
        *oldspec = timer_get_locked(timer, now);
    if (reset != NULL)
        reset(timer->data);

    timer->start = now;
    timer->end = timespec_add(timer->start, spec.value);
    timer->interval = spec.interval;
    timer->active = !timespec_is_zero(spec.value);
    if (timer->thread_running) {
        pthread_kill(timer->thread, SIGUSR1);
    } else if (timer->active) {
        timer->thread_running = true;
        pthread_create(&timer->thread, NULL, timer_thread, timer);
        pthread_detach(timer->thread);
    }
    unlock(&timer->lock);
    return 0;
}
