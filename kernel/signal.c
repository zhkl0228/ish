#include "debug.h"
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/native_offload.h"
#include "kernel/vdso.h"
#include "emu/interrupt.h"

#if is_gcc(9)
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif


int xsave_extra = 0;
int fxsave_extra = 0;
static void sigmask_set(sigset_t_ set);
static void altstack_to_user(struct stack_t_ *user_stack);
static bool is_on_altstack(addr_t sp);

static int signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
}

#define UNBLOCKABLE_MASK (sig_mask(SIGKILL_) | sig_mask(SIGSTOP_))

#define SIGNAL_IGNORE 0
#define SIGNAL_KILL 1
#define SIGNAL_CALL_HANDLER 2
#define SIGNAL_STOP 3

static int signal_action(struct sighand *sighand, int sig) {
    if (signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if (action->handler == SIG_IGN_)
            return SIGNAL_IGNORE;
        if (action->handler != SIG_DFL_)
            return SIGNAL_CALL_HANDLER;
    }

    switch (sig) {
        case SIGURG_: case SIGCONT_: case SIGCHLD_:
        case SIGIO_: case SIGWINCH_:
            return SIGNAL_IGNORE;

        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            return SIGNAL_STOP;

        default:
            return SIGNAL_KILL;
    }
}

// Real-time signals (>= SIGRTMIN) queue: each delivery is a distinct instance
// and must not be coalesced. Standard signals coalesce (one pending bit).
// JSC's thread resume path uses RT signals (RTMIN/RT_1/RT_2); dropping a
// duplicate as "already pending" loses a resume and wedges the GC handshake.
#define SIGRTMIN_ 32
static void deliver_signal_unlocked(struct task *task, int sig, struct siginfo_ info, bool forced) {
    if (sig < SIGRTMIN_ && sigset_has(task->pending, sig))
        return;

    sigset_add(&task->pending, sig);
    struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
    sigqueue->info = info;
    sigqueue->info.sig = sig;
    sigqueue->forced = forced;
    list_add_tail(&task->queue, &sigqueue->queue);

    if (sigset_has(task->blocked & ~task->waiting, sig) && signal_is_blockable(sig))
        return;

    if (task != current) {
        pthread_kill(task->thread, SIGUSR1);

        // wake up any pthread condition waiters
        // actual madness, I hope to god it's correct
        // must release the sighand lock while going insane, to avoid a deadlock
        unlock(&task->sighand->lock);
        // Wake up the target thread's condvar wait.
        // We don't acquire waiting_lock (the associated mutex) because:
        // 1. It may be held by the target thread inside pthread_cond_wait,
        //    and spinning on it blocks the condvar from returning.
        // 2. pthread_cond_broadcast() is safe without the mutex — worst case
        //    is a spurious wakeup, which wait_for() handles via re-checking.
        lock(&task->waiting_cond_lock);
        if (task->waiting_cond != NULL)
            notify(task->waiting_cond);
        unlock(&task->waiting_cond_lock);
        lock(&task->sighand->lock);
    }
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    if (task->sighand == NULL)
        return;
    lock(&task->sighand->lock);
    deliver_signal_unlocked(task, sig, info, true);
    unlock(&task->sighand->lock);
    cpu_poke(&task->cpu);
}

// A signal the task brought on itself synchronously: a fault, a breakpoint,
// a single step. As Linux's force_sig_info_to_task: if the task blocks or
// ignores it, it is unblocked and its action reset to the default, since the
// faulting instruction would otherwise run again and fault again forever (or
// a blocked one stay pending with the task going on as if nothing happened).
// to_default resets the action even to a handler, as force_sigsegv does when
// the handler of the signal that failed to be delivered is SIGSEGV's own.
void force_signal(struct task *task, int sig, struct siginfo_ info, bool to_default) {
    if (task->sighand == NULL)
        return;
    lock(&task->sighand->lock);
    struct sigaction_ *action = &task->sighand->action[sig];
    if (to_default || sigset_has(task->blocked, sig) || action->handler == SIG_IGN_) {
        action->handler = SIG_DFL_;
        sigset_del(&task->blocked, sig);
    }
    deliver_signal_unlocked(task, sig, info, true);
    unlock(&task->sighand->lock);
    cpu_poke(&task->cpu);
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    // signal zero is for testing whether a process exists
    if (sig == 0)
        return;
    if (task->zombie || task->exiting)
        return;
#ifdef GUEST_ARM64
    if ((sig == SIGTRAP_ || sig == SIGABRT_ || sig == SIGILL_ || sig == SIGSEGV_ || sig == SIGBUS_)
        && ish_exec_trace())
        fprintf(stderr, "SIGNAL_TRACE: sig=%d pc=0x%llx pid=%d sp=0x%llx lr=0x%llx fault=0x%llx\n",
                sig, (unsigned long long)task->cpu.pc, task->pid,
                (unsigned long long)task->cpu.sp,
                (unsigned long long)task->cpu.regs[30],
                (unsigned long long)task->cpu.segfault_addr);
#endif

    // Native offload: forward signal to the host native process
    if (native_offload_forward_signal(task, sig))
        return;

    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    lock(&sighand->lock);
    int action = signal_action(sighand, sig);
    // init, like the init of a Linux PID namespace (SIGNAL_UNKILLABLE), only
    // gets the signals it has a handler for, SIGKILL and SIGSTOP included, so
    // `kill -9 1` does nothing. A signal it blocks still queues, as on Linux:
    // it may be taken by sigwait or get a handler before it is unblocked
    // (receive_signals drops it if it does not). Faults are delivered with
    // deliver_signal and still end it.
    bool blocked = sigset_has(task->blocked | task->waiting, sig);
    if (task->tgid == 1 && action != SIGNAL_CALL_HANDLER && !blocked)
        action = SIGNAL_IGNORE;
    if (action != SIGNAL_IGNORE) {
        deliver_signal_unlocked(task, sig, info, false);
        // Poke the target CPU so it exits the JIT loop at the next gadget
        // boundary instead of waiting for INT_TIMER (up to 1024 cycles).
        // This reduces signal delivery latency from ~1024 to ~1 gadget.
        cpu_poke(&task->cpu);
    }
    unlock(&sighand->lock);

    if (sig == SIGCONT_ || sig == SIGKILL_) {
        lock(&task->group->lock);
        task->group->stopped = false;
        notify(&task->group->stopped_cond);
        unlock(&task->group->lock);
    }
}

bool try_self_signal(int sig) {
    assert(sig == SIGTTIN_ || sig == SIGTTOU_);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    bool can_send = signal_action(sighand, sig) != SIGNAL_IGNORE &&
        !sigset_has(current->blocked, sig);
    if (can_send)
        deliver_signal_unlocked(current, sig, SIGINFO_NIL, false);
    unlock(&sighand->lock);
    return can_send;
}

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    lock(&pids_lock);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        send_signal(tgroup->leader, sig, info);
    }
    unlock(&pids_lock);
    return 0;
}

static addr_t sigreturn_trampoline(const char *name) {
    addr_t sigreturn_addr = vdso_symbol(name);
    if (sigreturn_addr == 0) {
        die("sigreturn not found in vdso, this should never happen");
    }
    return current->mm->vdso + sigreturn_addr;
}

int sigset_size_valid(dword_t size) {
#if defined(GUEST_ARM64)
    return size >= sizeof(sigset_t_) && (size % sizeof(sigset_t_) == 0);
#else
    return size == sizeof(sigset_t_);
#endif
}

int user_get_sigset(addr_t addr, dword_t size, sigset_t_ *out) {
    if (!sigset_size_valid(size))
        return _EINVAL;
#if defined(GUEST_ARM64)
    uint64_t word = 0;
    if (user_read(addr, &word, sizeof(word)))
        return _EFAULT;
    *out = (sigset_t_) word;
    return 0;
#else
    if (user_get(addr, *out))
        return _EFAULT;
    return 0;
#endif
}

int user_put_sigset(addr_t addr, dword_t size, sigset_t_ set) {
    if (!sigset_size_valid(size))
        return _EINVAL;
#if defined(GUEST_ARM64)
    uint64_t word = (uint64_t) set;
    if (user_write(addr, &word, sizeof(word)))
        return _EFAULT;
    dword_t remaining = size - sizeof(word);
    addr_t pos = addr + sizeof(word);
    static const byte_t zeros[16] = {0};
    while (remaining > 0) {
        dword_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;
        if (user_write(pos, zeros, chunk))
            return _EFAULT;
        pos += chunk;
        remaining -= chunk;
    }
    return 0;
#else
    if (user_put(addr, set))
        return _EFAULT;
    return 0;
#endif
}

#if defined(GUEST_ARM64)
struct sigaction_arm64_kernel {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask[1];
};

static int user_get_sigaction_arm64(addr_t addr, dword_t sigset_size, struct sigaction_ *out) {
    uint64_t handler = 0;
    uint64_t flags = 0;
    uint64_t restorer = 0;
    if (user_read(addr + offsetof(struct sigaction_arm64_kernel, handler), &handler, sizeof(handler)))
        return _EFAULT;
    if (user_read(addr + offsetof(struct sigaction_arm64_kernel, flags), &flags, sizeof(flags)))
        return _EFAULT;
    if (user_read(addr + offsetof(struct sigaction_arm64_kernel, restorer), &restorer, sizeof(restorer)))
        return _EFAULT;
    sigset_t_ mask = 0;
    int err = user_get_sigset(addr + offsetof(struct sigaction_arm64_kernel, mask), sigset_size, &mask);
    if (err)
        return err;
    *out = (struct sigaction_) {
        .handler = (addr_t) handler,
        .flags = (dword_t) flags,
        .restorer = (addr_t) restorer,
        .mask = mask,
    };
    return 0;
}

static int user_put_sigaction_arm64(addr_t addr, dword_t sigset_size, const struct sigaction_ *in) {
    uint64_t handler = (uint64_t) in->handler;
    uint64_t flags = (uint64_t) in->flags;
    uint64_t restorer = (uint64_t) in->restorer;
    if (user_write(addr + offsetof(struct sigaction_arm64_kernel, handler), &handler, sizeof(handler)))
        return _EFAULT;
    if (user_write(addr + offsetof(struct sigaction_arm64_kernel, flags), &flags, sizeof(flags)))
        return _EFAULT;
    if (user_write(addr + offsetof(struct sigaction_arm64_kernel, restorer), &restorer, sizeof(restorer)))
        return _EFAULT;
    return user_put_sigset(addr + offsetof(struct sigaction_arm64_kernel, mask), sigset_size, in->mask);
}
#endif

#if defined(GUEST_ARM64)
// ARM64 signal handling - matches Linux kernel sigcontext layout

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu) {
    // Store full ARM64 state needed for sigreturn
    // NOTE: Do NOT call arm64_sync_nzcv() here!
    // The JIT maintains cpu->nzcv directly (via mrs/str in store_flags).
    // The individual byte flags (nf/zf/cf/vf) are stale and must not
    // overwrite the correct nzcv value. See also: MEMORY.md NZCV lesson.

    // Clear the entire structure first
    memset(sc, 0, sizeof(*sc));

    sc->fault_address = cpu->segfault_addr;
    memcpy(sc->regs, cpu->regs, sizeof(cpu->regs));
    sc->sp = cpu->sp;
    sc->pc = cpu->pc;
    sc->pstate = cpu->nzcv;

    // Set up FPSIMD context in the extension area
    struct fpsimd_context_ *fpsimd = (struct fpsimd_context_ *)sc->__reserved;
    fpsimd->magic = FPSIMD_MAGIC;
    fpsimd->size = sizeof(struct fpsimd_context_);
    fpsimd->fpsr = cpu->fpsr;
    fpsimd->fpcr = cpu->fpcr;
    // Copy vector registers
    for (int i = 0; i < 32; i++) {
        fpsimd->vregs[i] = cpu->fp[i].q;
    }

    // Terminate the extension list with null entry
    uint32_t *term = (uint32_t *)(sc->__reserved + sizeof(struct fpsimd_context_));
    term[0] = 0;  // magic = 0 terminates
    term[1] = 0;  // size = 0
}
#else
static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu) {
    sc->ax = cpu->eax;
    sc->bx = cpu->ebx;
    sc->cx = cpu->ecx;
    sc->dx = cpu->edx;
    sc->di = cpu->edi;
    sc->si = cpu->esi;
    sc->bp = cpu->ebp;
    sc->sp = sc->sp_at_signal = cpu->esp;
    sc->ip = cpu->eip;
    collapse_flags(cpu);
    sc->flags = cpu->eflags;
    sc->trapno = cpu->trapno;
    if (cpu->trapno == INT_GPF)
        sc->cr2 = cpu->segfault_addr;
    // TODO more shit
    sc->oldmask = current->blocked & 0xffffffff;
}
#endif

static void setup_sigframe(struct siginfo_ *info, struct sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_sigreturn");
    frame->sig = info->sig;
    setup_sigcontext(&frame->sc, &current->cpu);
    frame->extramask = current->blocked >> 32;

    static const struct {
        uint16_t popmov;
        uint32_t nr_sigreturn;
        uint16_t int80;
    } __attribute__((packed)) retcode = {
        .popmov = 0xb858,
        .nr_sigreturn = 113,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &retcode, sizeof(retcode));
}

static void setup_rt_sigframe(struct siginfo_ *info, struct rt_sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_rt_sigreturn");
    frame->sig = info->sig;
    frame->info = *info;
    frame->uc.flags = 0;
    frame->uc.link = 0;
    altstack_to_user(&frame->uc.stack);
    setup_sigcontext(&frame->uc.mcontext, &current->cpu);
    frame->uc.sigmask = current->blocked;

    static const struct {
        uint8_t mov;
        uint32_t nr_rt_sigreturn;
        uint16_t int80;
        uint8_t pad;
    } __attribute__((packed)) rt_retcode = {
        .mov = 0xb8,
        .nr_rt_sigreturn = 173,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}

static void receive_signal(struct sighand *sighand, struct siginfo_ *info) {
    int sig = info->sig;
    STRACE("%d receiving signal %d\n", current->pid, sig);

    int action_type = signal_action(sighand, sig);
    switch (action_type) {
        case SIGNAL_IGNORE:
            return;

        case SIGNAL_STOP:
            lock(&current->group->lock);
            current->group->stopped = true;
            current->group->group_exit_code = sig << 8 | 0x7f;
            unlock(&current->group->lock);
            return;

        case SIGNAL_KILL:
            unlock(&sighand->lock); // do_exit must be called without this lock
            do_exit_group(sig);
    }

    struct sigaction_ *action = &sighand->action[info->sig];
    bool need_siginfo = action->flags & SA_SIGINFO_;
#if defined(GUEST_ARM64)
    // ARM64 only provides __kernel_rt_sigreturn in the VDSO.
    // Always use an rt_sigframe for signal delivery.
    need_siginfo = true;

    // SA_RESTART: if this handler asked for automatic restart and the syscall
    // we're interrupting returned EINTR, rewind PC to the `svc #0` instruction
    // (4 bytes back) and restore the original first argument (x0), which the
    // return value clobbered. The signal frame we save next captures this
    // rewound PC, so when the handler returns via rt_sigreturn the syscall
    // re-executes instead of surfacing EINTR. Load-bearing for JSC's SIGPWR
    // GC-safepoint handshake, which registers SA_RESTART and otherwise
    // livelocks re-signalling a thread whose futex keeps returning EINTR.
    // syscall_restartable is a sticky flag: it stays set if an earlier EINTR
    // futex/epoll/read/write delivered a signal whose handler lacked SA_RESTART
    // (no rewind happened). Guarding the rewind on the flag alone is unsafe —
    // a later non-syscall interrupt (INT_TIMER every ~1024 blocks, INT_GPF on
    // demand-map/CoW) runs receive_signals() too, and if a NEW SA_RESTART signal
    // is pending it would rewind PC into the middle of ordinary code. Require
    // cpu.pc to still equal the interrupted syscall's return PC (svc_addr+4), so
    // the rewind can only fire when we truly are just past that svc.
    if ((action->flags & SA_RESTART_) && current->syscall_restartable &&
            current->cpu.pc == current->syscall_restart_pc) {
        current->cpu.pc -= 4;
        current->cpu.regs[0] = current->syscall_restart_arg0;
        current->syscall_restartable = false;
    }
#endif

    // setup the frame
    union {
        struct sigframe_ sigframe;
        struct rt_sigframe_ rt_sigframe;
    } frame = {};
    size_t frame_size;
    if (need_siginfo) {
        setup_rt_sigframe(info, &frame.rt_sigframe);
        frame_size = sizeof(frame.rt_sigframe);
    } else {
        setup_sigframe(info, &frame.sigframe);
        frame_size = sizeof(frame.sigframe);
    }

    // set up registers for signal handler
#if defined(GUEST_ARM64)
    // ARM64 spec: exceptions clear the exclusive monitor
    current->cpu.excl_addr = UINT64_MAX;
    current->cpu.regs[0] = info->sig;
    current->cpu.pc = sighand->action[info->sig].handler;
    addr_t sp = current->cpu.sp;  // Use addr_t (64-bit) for ARM64
#else
    current->cpu.eax = info->sig;
    current->cpu.eip = sighand->action[info->sig].handler;
    dword_t sp = current->cpu.esp;
#endif
    // Only switch to the alternate signal stack when the handler was
    // registered with SA_ONSTACK. iSH previously switched whenever an altstack
    // existed at all, ignoring the flag — that put EVERY handler on the
    // altstack. JSC's GC installs an altstack (for SIGSEGV) but registers its
    // SIGPWR thread-suspend handler WITHOUT SA_ONSTACK: that handler compares
    // the interrupted sp against the thread's own stack bounds to decide
    // whether to actually suspend (sem_post + sigsuspend) or bail. Running it
    // on the altstack made sp fall outside those bounds, so the handler took
    // the bail path and never parked — the GC suspender then re-signalled
    // forever (the claude-cli startup deadlock).
    if ((action->flags & SA_ONSTACK_) && current->altstack &&
            !is_on_altstack(sp)) {
        sp = current->altstack + current->altstack_size;
    }
    if (xsave_extra) {
        // do as the kernel does
        // this is superhypermega condensed version of fpu__alloc_mathframe in
        // arch/x86/kernel/fpu/signal.c
        sp -= xsave_extra;
        sp &=~ 0x3f;
        sp -= fxsave_extra;
    }
    sp -= frame_size;
#if defined(GUEST_ARM64)
    // ARM64 requires the stack to be 16-byte aligned.
    sp &= ~0xf;
#else
    // align sp + 4 on a 16-byte boundary because that's what the abi says
    sp = ((sp + 4) & ~0xf) - 4;
#endif
#if defined(GUEST_ARM64)
    current->cpu.sp = sp;
#else
    current->cpu.esp = sp;
#endif

    // Update the mask. By default the signal will be blocked while in the
    // handler, but sigaction is allowed to customize this.
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    // these have to be filled in after the location of the frame is known
    if (need_siginfo) {
        frame.rt_sigframe.pinfo = sp + offsetof(struct rt_sigframe_, info);
        frame.rt_sigframe.puc = sp + offsetof(struct rt_sigframe_, uc);
#if defined(GUEST_ARM64)
        // ARM64 ABI: x0=signum, x1=siginfo*, x2=ucontext*
        current->cpu.regs[1] = frame.rt_sigframe.pinfo;
        current->cpu.regs[2] = frame.rt_sigframe.puc;
#else
        current->cpu.edx = frame.rt_sigframe.pinfo;
        current->cpu.ecx = frame.rt_sigframe.puc;
#endif
    }

    // install frame
    if (user_write(sp, &frame, frame_size)) {
        printk("failed to install frame for %d at %#x\n", info->sig, sp);
        force_signal(current, SIGSEGV_, SIGINFO_NIL, info->sig == SIGSEGV_);
    }

#if defined(GUEST_ARM64)
    // Set LR to the signal return trampoline.
    if (need_siginfo)
        current->cpu.regs[30] = frame.rt_sigframe.restorer;
    else
        current->cpu.regs[30] = frame.sigframe.restorer;
#endif

    if (action->flags & SA_RESETHAND_)
        *action = (struct sigaction_) {.handler = SIG_DFL_};
}

void signal_delivery_stop(int sig, struct siginfo_ *info) {
    lock(&current->ptrace.lock);
    current->ptrace.stopped = true;
    current->ptrace.signal = sig;
    current->ptrace.info = *info;
    unlock(&current->ptrace.lock);
    notify(&current->parent->group->child_exit);
    // TODO add siginfo
    send_signal(current->parent, current->group->leader->exit_signal, SIGINFO_NIL);

    unlock(&current->sighand->lock);
    lock(&current->ptrace.lock);
    while (current->ptrace.stopped) {
        wait_for_ignore_signals(&current->ptrace.cond, &current->ptrace.lock, NULL);
        lock(&current->sighand->lock);
        bool got_sigkill = sigset_has(current->pending, SIGKILL_);
        unlock(&current->sighand->lock);
        if (got_sigkill) {
            STRACE("%d received a SIGKILL in signal delivery stop\n", current->pid);
            unlock(&current->ptrace.lock);
            do_exit_group(SIGKILL_);
        }
    }
    unlock(&current->ptrace.lock);
    lock(&current->sighand->lock);
}

void receive_signals(void) {
    lock(&current->group->lock);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    struct sighand *sighand = current->sighand;
    if (sighand == NULL)
        return;
    lock(&sighand->lock);

    // A saved mask means that the last system call was a call like sigsuspend
    // that changes the mask during the call. Only ignore a signal right now if
    // it was both blocked during the call and should still be blocked after
    // the call.
    sigset_t_ blocked = current->blocked;
    if (current->has_saved_mask) {
        blocked &= current->saved_mask;
        current->has_saved_mask = false;
        current->blocked = current->saved_mask;
    }

    struct sigqueue *sigqueue, *tmp;
    list_for_each_entry_safe(&current->queue, sigqueue, tmp, queue) {
        int sig = sigqueue->info.sig;
        if (sigset_has(blocked, sig))
            continue;
        list_remove(&sigqueue->queue);
        sigset_del(&current->pending, sig);

        // init drops a signal that was not forced and would take its default
        // action, as Linux's get_signal does for an unkillable init: one it
        // had blocked when it was sent, unblocked without a handler.
        int action = signal_action(sighand, sig);
        if (current->tgid == 1 && !sigqueue->forced &&
                (action == SIGNAL_KILL || action == SIGNAL_STOP)) {
            free(sigqueue);
            continue;
        }

        if (current->ptrace.traced && sig != SIGKILL_) {
            // This notifies the parent, goes to sleep, and waits for the
            // parent to tell it to continue.
            // Any signals received while waiting are left on the queue, except
            // for SIGKILL_, which causes an immediate exit.
            signal_delivery_stop(sig, &sigqueue->info);
        } else {
            receive_signal(sighand, &sigqueue->info);
        }
        free(sigqueue);
    }

    unlock(&sighand->lock);

    // this got moved out of the switch case in receive_signal to fix locking problems
    if (!was_stopped) {
        lock(&current->group->lock);
        bool now_stopped = current->group->stopped;
        unlock(&current->group->lock);
        if (now_stopped) {
            lock(&pids_lock);
            notify(&current->parent->group->child_exit);
            // TODO add siginfo
            send_signal(current->parent, current->group->leader->exit_signal, SIGINFO_NIL);
            unlock(&pids_lock);
        }
    }
}

#if defined(GUEST_ARM64)
static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    memcpy(cpu->regs, context->regs, sizeof(cpu->regs));
    cpu->sp = context->sp;
    cpu->pc = context->pc;
    arm64_set_nzcv(cpu, (uint32_t)context->pstate);

    // Restore FPSIMD context from extension area
    struct fpsimd_context_ *fpsimd = (struct fpsimd_context_ *)context->__reserved;
    if (fpsimd->magic == FPSIMD_MAGIC) {
        cpu->fpsr = fpsimd->fpsr;
        cpu->fpcr = fpsimd->fpcr;
        for (int i = 0; i < 32; i++) {
            cpu->fp[i].q = fpsimd->vregs[i];
        }
    }
}
#else
static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    cpu->eax = context->ax;
    cpu->ebx = context->bx;
    cpu->ecx = context->cx;
    cpu->edx = context->dx;
    cpu->edi = context->di;
    cpu->esi = context->si;
    cpu->ebp = context->bp;
    cpu->esp = context->sp;
    cpu->eip = context->ip;
    collapse_flags(cpu);

    // Use AC, RF, OF, DF, TF, SF, ZF, AF, PF, CF
#define USE_FLAGS 0b1010000110111010101
    cpu->eflags = (context->flags & USE_FLAGS) | (cpu->eflags & ~USE_FLAGS);
}
#endif

int64_t sys_rt_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_ frame;
#if defined(GUEST_ARM64)
    addr_t sp = cpu->sp;  // Use addr_t (64-bit) for ARM64
#else
    dword_t sp = cpu->esp;
#endif
#if defined(GUEST_ARM64)
    // ARM64 keeps SP pointing at the start of the frame.
    addr_t frame_addr = sp;
#else
    // esp/sp points past the first field of the frame
    addr_t frame_addr = sp - offsetof(struct rt_sigframe_, sig);
#endif
    if (user_get(frame_addr, frame)) {
        force_signal(current, SIGSEGV_, SIGINFO_NIL, false);
        return _EFAULT;
    }
    restore_sigcontext(&frame.uc.mcontext, cpu);

    lock(&current->sighand->lock);
    // FIXME this duplicates logic from sys_sigaltstack
    if (!is_on_altstack(sp) &&
            frame.uc.stack.size >= MINSIGSTKSZ_) {
        current->altstack = frame.uc.stack.stack;
        current->altstack_size = frame.uc.stack.size;
    }
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
#if defined(GUEST_ARM64)
    // Return the full 64-bit x0 from the restored context.
    // IMPORTANT: The caller (handle_interrupt) must NOT modify x0 after
    // this returns, because the full register state was restored from the
    // signal frame. We return int64_t to preserve all 64 bits.
    return (int64_t)cpu->regs[0];
#else
    return cpu->eax;
#endif
}

int64_t sys_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct sigframe_ frame;
#if defined(GUEST_ARM64)
    addr_t sp = cpu->sp;  // Use addr_t (64-bit) for ARM64
#else
    dword_t sp = cpu->esp;
#endif
#if defined(GUEST_ARM64)
    // ARM64 keeps SP pointing at the start of the frame.
    addr_t frame_addr = sp;
#else
    // esp/sp points past the first two fields of the frame
    addr_t frame_addr = sp - offsetof(struct sigframe_, sc);
#endif
    if (user_get(frame_addr, frame)) {
        force_signal(current, SIGSEGV_, SIGINFO_NIL, false);
        return _EFAULT;
    }
    restore_sigcontext(&frame.sc, cpu);

    lock(&current->sighand->lock);
#if defined(GUEST_ARM64)
    sigmask_set(current->blocked);
#else
    sigset_t_ oldmask = ((sigset_t_) frame.extramask << 32) | frame.sc.oldmask;
    sigmask_set(oldmask);
#endif
    unlock(&current->sighand->lock);
#if defined(GUEST_ARM64)
    return (int64_t)cpu->regs[0];
#else
    return cpu->eax;
#endif
}

struct sighand *sighand_new(void) {
    struct sighand *sighand = malloc(sizeof(struct sighand));
    if (sighand == NULL)
        return NULL;
    memset(sighand, 0, sizeof(struct sighand));
    sighand->refcount = 1;
    lock_init(&sighand->lock);
    return sighand;
}

struct sighand *sighand_copy(struct sighand *sighand) {
    struct sighand *new_sighand = sighand_new();
    if (new_sighand == NULL)
        return NULL;
    memcpy(new_sighand->action, sighand->action, sizeof(new_sighand->action));
    return new_sighand;
}

void sighand_release(struct sighand *sighand) {
    if (--sighand->refcount == 0) {
        free(sighand);
    }
}

static int do_sigaction(int sig, const struct sigaction_ *action, struct sigaction_ *oldaction) {
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (!signal_is_blockable(sig))
        return _EINVAL;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    if (oldaction)
        *oldaction = sighand->action[sig];
    if (action)
        sighand->action[sig] = *action;
    unlock(&sighand->lock);
    return 0;
}

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size) {
    if (!sigset_size_valid(sigset_size))
        return _EINVAL;
    struct sigaction_ action = {}, oldaction;
    if (action_addr != 0) {
#if defined(GUEST_ARM64)
        int err = user_get_sigaction_arm64(action_addr, sigset_size, &action);
        if (err)
            return err;
#else
        if (user_get(action_addr, action))
            return _EFAULT;
#endif
    }
    STRACE("rt_sigaction(%d, %#x {handler=%#x, flags=%#x, restorer=%#x, mask=%#llx}, 0x%x, %d)", signum,
            action_addr, action.handler, action.flags, action.restorer,
            (unsigned long long) action.mask, oldaction_addr, sigset_size);

    int err = do_sigaction(signum,
            action_addr ? &action : NULL,
            oldaction_addr ? &oldaction : NULL);
    if (err < 0)
        return err;

    if (oldaction_addr != 0)
    {
#if defined(GUEST_ARM64)
        int err = user_put_sigaction_arm64(oldaction_addr, sigset_size, &oldaction);
        if (err)
            return err;
#else
        if (user_put(oldaction_addr, oldaction))
            return _EFAULT;
#endif
    }
    return err;
}

dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, 1);
}

static void sigmask_set(sigset_t_ set) {
    current->blocked = (set & ~UNBLOCKABLE_MASK);
}

static void sigmask_set_temp_unlocked(sigset_t_ mask) {
    current->saved_mask = current->blocked;
    current->has_saved_mask = true;
    sigmask_set(mask);
}

void sigmask_set_temp(sigset_t_ mask) {
    lock(&current->sighand->lock);
    sigmask_set_temp_unlocked(mask);
    unlock(&current->sighand->lock);
}

static int do_sigprocmask(dword_t how, sigset_t_ set) {
    if (how == SIG_BLOCK_)
        sigmask_set(current->blocked | set);
    else if (how == SIG_UNBLOCK_)
        sigmask_set(current->blocked & ~set);
    else if (how == SIG_SETMASK_)
        sigmask_set(set);
    else
        return _EINVAL;
    return 0;
}

dword_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, dword_t size) {
    if (!sigset_size_valid(size))
        return _EINVAL;

    sigset_t_ set = 0;
    if (set_addr != 0) {
        int err = user_get_sigset(set_addr, size, &set);
        if (err)
            return err;
    }
    STRACE("rt_sigprocmask(%s, %#llx, %#x, %d)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (long long) set : -1, oldset_addr, size);

    if (oldset_addr != 0) {
        int err = user_put_sigset(oldset_addr, size, current->blocked);
        if (err)
            return err;
    }
    if (set_addr != 0) {
        struct sighand *sighand = current->sighand;
        lock(&sighand->lock);
        int err = do_sigprocmask(how, set);
        unlock(&sighand->lock);
        if (err < 0)
            return err;
    }
    return 0;
}

int_t sys_rt_sigpending(addr_t set_addr, dword_t size) {
    STRACE("rt_sigpending(%#x, %d)", set_addr, size);
    // as defined by the standard
    sigset_t_ pending = current->pending & current->blocked;
    return user_put_sigset(set_addr, size, pending);
}

static addr_t current_sp(void) {
#if defined(GUEST_ARM64)
    return current->cpu.sp;
#else
    return current->cpu.esp;
#endif
}

static bool is_on_altstack(addr_t sp) {
    return sp > current->altstack && sp <= current->altstack + current->altstack_size;
}

static void altstack_to_user(struct stack_t_ *user_stack) {
    user_stack->stack = current->altstack;
    user_stack->size = current->altstack_size;
    user_stack->flags = 0;
    if (current->altstack == 0)
        user_stack->flags |= SS_DISABLE_;
    if (is_on_altstack(current_sp()))
        user_stack->flags |= SS_ONSTACK_;
}

dword_t sys_sigaltstack(addr_t ss_addr, addr_t old_ss_addr) {
    STRACE("sigaltstack(0x%x, 0x%x)", ss_addr, old_ss_addr);
    if (old_ss_addr != 0) {
        struct stack_t_ old_ss;
        altstack_to_user(&old_ss);
        if (user_put(old_ss_addr, old_ss))
            return _EFAULT;
    }
    if (ss_addr != 0) {
        if (is_on_altstack(current_sp()))
            return _EPERM;
        struct stack_t_ ss;
        if (user_get(ss_addr, ss))
            return _EFAULT;
        if (ss.flags & SS_DISABLE_) {
            current->altstack = 0;
            current->altstack_size = 0;
        } else {
            if (ss.size < MINSIGSTKSZ_)
                return _ENOMEM;
            current->altstack = ss.stack;
            current->altstack_size = ss.size;
        }
    }
    return 0;
}

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size) {
    sigset_t_ mask;
    int err = user_get_sigset(mask_addr, size, &mask);
    if (err)
        return err;
    STRACE("sigsuspend(0x%llx) = ...\n", (long long) mask);

    lock(&current->sighand->lock);
    sigmask_set_temp_unlocked(mask);
    while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&current->sighand->lock);
    STRACE("%d done sigsuspend", current->pid);
    return _EINTR;
}

int_t sys_pause(void) {
    lock(&current->sighand->lock);
    while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&current->sighand->lock);
    return _EINTR;
}

int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    sigset_t_ set;
    int err = user_get_sigset(set_addr, set_size, &set);
    if (err)
        return err;
    struct timespec timeout;
    if (timeout_addr != 0) {
        struct timespec_ fake_timeout;
        if (user_get(timeout_addr, fake_timeout))
            return _EFAULT;
        timeout.tv_sec = fake_timeout.sec;
        timeout.tv_nsec = fake_timeout.nsec;
    }
    STRACE("sigtimedwait(%#llx, %#x, %#x) = ...\n", (long long) set, info_addr, timeout_addr);

    lock(&current->sighand->lock);
    assert(current->waiting == 0);
    current->waiting = set;
    // A signal in `set` is normally BLOCKED by the caller — that is how
    // sigwait/sigtimedwait are used (bind9's isc_app_run blocks SIGTERM/SIGINT
    // in every thread and waits for them here). wait_for() classifies a wakeup
    // as "real" only when (pending & ~blocked) != 0, so a delivery of a blocked
    // signal — the only kind this syscall exists to consume — always reads as a
    // spurious wakeup: deliver_signal queues it and notifies waiting_cond, we
    // wake, wait_for returns 0, and the old `while (wait_err == 0)` loop went
    // straight back to sleep. Forever. Field case: bind-tools nslookup prints
    // its answer, isc_app_shutdown raises SIGTERM at the sigwaiting main
    // thread, the wake is discarded, and the process hangs with
    // pending=0x4002 blocked=0x4003 (SIGTERM+SIGINT delivered, never consumed);
    // Ctrl-C dies the same way, and only unblockable SIGKILL gets through.
    // The loop must therefore check for awaited-signal arrival itself, before
    // every sleep — which also consumes a signal that was already pending on
    // entry, as POSIX requires.
    int wait_err = 0;
    while (!(current->pending & set)) {
        wait_err = wait_for(&current->pause, &current->sighand->lock, timeout_addr == 0 ? NULL : &timeout);
        if (wait_err != 0)
            break;
    }
    current->waiting = 0;
    if (wait_err == _ETIMEDOUT) {
        unlock(&current->sighand->lock);
        STRACE("sigtimedwait timed out\n");
        return _EAGAIN;
    }

    struct sigqueue *sigqueue;
    bool found = false;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        if (sigset_has(set, sigqueue->info.sig)) {
            found = true;
            list_remove(&sigqueue->queue);
            break;
        }
    }
    unlock(&current->sighand->lock);
    if (!found)
        return _EINTR;
    struct siginfo_ info = sigqueue->info;
    free(sigqueue);
    if (info_addr != 0)
        if (user_put(info_addr, info))
            return _EFAULT;
    STRACE("done sigtimedwait = %d\n", info.sig);
    return info.sig;
}

// The thread a signal sent to task's process goes to: one that can take it,
// because it does not block it or waits for it in sigwait. The thread named is
// preferred. With none, it stays pending on the named thread. Linux keeps such
// a signal pending for the whole process, so a thread that unblocks it later
// takes it; iSH has no process-wide pending set. Caller holds pids_lock (the
// thread list).
static struct task *process_signal_target(struct task *task, int sig) {
    struct task *target = task;
    lock(&task->sighand->lock);
    if (sigset_has(task->blocked & ~task->waiting, sig) && signal_is_blockable(sig)) {
        struct task *thread;
        list_for_each_entry(&task->group->threads, thread, group_links) {
            if (!sigset_has(thread->blocked & ~thread->waiting, sig)) {
                target = thread;
                break;
            }
        }
    }
    unlock(&task->sighand->lock);
    return target;
}

// process_directed: kill() (and its process group forms) address the whole
// process; tkill() and tgkill() address one thread.
static int kill_task(struct task *task, dword_t sig, bool process_directed) {
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid)
        return _EPERM;
    struct siginfo_ info = {
        .code = SI_USER_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    if (process_directed && sig != 0)
        task = process_signal_target(task, sig);
    send_signal(task, sig, info);
    return 0;
}

static int kill_group(pid_t_ pgid, dword_t sig) {
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    int err = _EPERM;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        int kill_err = kill_task(tgroup->leader, sig, true);
        // killing a group should return an error only if no process can be signaled
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int kill_everything(dword_t sig) {
    int err = _EPERM;
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task == NULL || task == current || !task_is_leader(task))
            continue;
        int kill_err = kill_task(task, sig, true);
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int do_kill(pid_t_ pid, dword_t sig, pid_t_ tgid, bool process_directed) {
    STRACE("kill(%d, %d)", pid, sig);
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (pid == 0)
        pid = -current->group->pgid;

    int err;
    lock(&pids_lock);

    if (pid == -1) {
        err = kill_everything(sig);
    } else if (pid < 0) {
        err = kill_group(-pid, sig);
    } else {
        struct task *task = pid_get_task(pid);
        if (task == NULL) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        // If tgid is nonzero, it must be correct
        if (tgid != 0 && task->tgid != tgid) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        err = kill_task(task, sig, process_directed);
    }

    unlock(&pids_lock);
    return err;
}

dword_t sys_kill(pid_t_ pid, dword_t sig) {
    return do_kill(pid, sig, 0, true);
}
dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    if (tid <= 0 || tgid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, tgid, false);
}
dword_t sys_tkill(pid_t_ tid, dword_t sig) {
    if (tid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, 0, false);
}
