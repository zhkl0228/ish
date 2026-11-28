#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

int futex_wake(addr_t uaddr, dword_t val);
struct task;
// A signal for task: a futex wait it is in ends to take it (its pipe written).
// Caller holds task->waiting_cond_lock.
void futex_signal_kick(struct task *task);
// The current task's futex pipe closed, as it exits
void futex_pipe_close(void);

#endif
