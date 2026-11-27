#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>
#include "debug.h"
#include "kernel/calls.h"
#include "emu/interrupt.h"
#include "util/signpost.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "fs/stat.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/real.h"
#include "kernel/inotify.h"
#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
#endif

int64_t syscall_stub(uint64_t UNUSED(a), uint64_t UNUSED(b), uint64_t UNUSED(c), uint64_t UNUSED(d),
        uint64_t UNUSED(e), uint64_t UNUSED(f)) {
    return _ENOSYS;
}
// While identical, this version of the stub doesn't log below. Use this for
// syscalls that are optional (i.e. fallback on something else) but called
// frequently.
dword_t syscall_silent_stub(void) {
    return _ENOSYS;
}
dword_t syscall_success_stub(void) {
    return 0;
}

// Syscall table is defined in arch-specific files (kernel/arch/*/calls.c)
extern syscall_t syscall_table[];
extern size_t syscall_table_size;

void dump_stack(int lines);
void dump_maps(void);

// Fast path syscall handlers (forward declarations)
static inline int fast_fstat64(struct cpu_state *cpu);
static inline int fast_read(struct cpu_state *cpu);
static inline int fast_write(struct cpu_state *cpu);

// Diagnostic: JIT crash info from signal handler
__thread volatile uint64_t jit_last_host_fault = 0;
__thread volatile uint64_t jit_last_x7 = 0;
__thread volatile uint64_t jit_last_x10 = 0;
__thread volatile int jit_crash_count = 0;

static _Atomic(ish_timer_tick_hook_t) g_timer_tick_hook = NULL;

void ish_set_timer_tick_hook(ish_timer_tick_hook_t hook) {
    atomic_store_explicit(&g_timer_tick_hook, hook, memory_order_release);
}

// [T-ish-precise-fault-pc] Deliver / resume a data fault at the faulting
// instruction instead of the start of its block.
static void gpf_use_precise_pc(struct cpu_state *cpu) {
    if (cpu->segfault_precise_pc != 0 && cpu->pc == cpu->segfault_block_pc)
        cpu->pc = cpu->segfault_precise_pc;
    cpu->segfault_precise_pc = 0;
}

// Whether the task takes sig with a handler of its own (not blocked): a fault
// raising it is then the program's business.
static bool task_handles_signal(struct task *task, int sig) {
    if (task->sighand == NULL)
        return false;
    lock(&task->sighand->lock);
    addr_t handler = task->sighand->action[sig].handler;
    bool handles = handler != SIG_DFL_ && handler != SIG_IGN_ && !sigset_has(task->blocked, sig);
    unlock(&task->sighand->lock);
    return handles;
}

void handle_interrupt(int interrupt) {
    struct cpu_state *cpu = &current->cpu;
    if (interrupt == INT_SYSCALL) {
        ISH_SIGNPOST_SCOPE_BEGIN(syscall, "syscall", _sc_spid);
        // ARM64: syscall number in x8, args in x0-x5, return in x0
        unsigned syscall_num = cpu->regs[8];
        // Snapshot for SA_RESTART: x0 (first arg) is about to be clobbered by
        // the return value; save it and the number so an interrupted syscall
        // can be re-executed if a SA_RESTART signal is delivered.
        current->syscall_restart_num = syscall_num;
        current->syscall_restart_arg0 = cpu->regs[0];
        // The svc gadget already advanced cpu.pc to the instruction after the
        // svc. Record it so the SA_RESTART rewind can verify PC is still exactly
        // here (i.e. we really are delivering a signal for THIS interrupted
        // syscall) before doing pc -= 4.
        current->syscall_restart_pc = cpu->pc;
        current->syscall_restartable = false;
        current->syscall_restart_nohandler = false;

        // === FAST PATH: Hot syscalls ===
        int fast_result = -1;  // -1 means fast path not taken
        bool fast_path_taken = false;

        // Fast path 1: fstat64 (syscall 80) - most common during Python import
        if (syscall_num == 80) {
            fast_result = fast_fstat64(cpu);
            if (fast_result != -1)
                fast_path_taken = true;
        }
        // Fast path 2: read (syscall 63) - small buffer reads
        else if (syscall_num == 63) {
            fast_result = fast_read(cpu);
            if (fast_result != -1)
                fast_path_taken = true;
        }
        // Fast path 3: write (syscall 64) - small buffer writes
        else if (syscall_num == 64) {
            fast_result = fast_write(cpu);
            if (fast_result != -1)
                fast_path_taken = true;
        }

        if (fast_path_taken) {
            // Fast path succeeded, return immediately
            // Fast paths return int (small values), safe to use directly
            STRACE("%d call %-3d (fast) = 0x%x\n", current->pid, syscall_num, fast_result);
            int32_t signed_result = (int32_t)fast_result;
            if (signed_result >= -4095 && signed_result < 0) {
                cpu->regs[0] = (uint64_t)(int64_t)signed_result;
            } else {
                cpu->regs[0] = (uint64_t)(uint32_t)fast_result;
            }
        } else {
            // === SLOW PATH: Full syscall ===
            if (syscall_num >= syscall_table_size || syscall_table[syscall_num] == NULL) {
                printk("%d(%s) missing syscall %d\n", current->pid, current->comm, syscall_num);
                cpu->regs[0] = (uint64_t)(int64_t)(int32_t)_ENOSYS;
            } else {
                if (syscall_table[syscall_num] == (syscall_t) syscall_stub) {
                    printk("%d(%s) stub syscall %d\n", current->pid, current->comm, syscall_num);
                }
                STRACE("%d call %-3d ", current->pid, syscall_num);
                // env-gated full syscall trace for golden-diff vs native strace.
                // ISH_SYSTRACE=1 logs every slow-path syscall with timestamp,
                // pid, number, the 6 args, and the return value.
                static int systrace = -1;
                if (systrace == -1) { const char *e = getenv("ISH_SYSTRACE"); systrace = (e && e[0]=='1') ? 1 : 0; }
                uint64_t _a0=cpu->regs[0],_a1=cpu->regs[1],_a2=cpu->regs[2],
                         _a3=cpu->regs[3],_a4=cpu->regs[4],_a5=cpu->regs[5];
                struct timespec _t0; if(systrace) clock_gettime(CLOCK_MONOTONIC, &_t0);
                atomic_store(&current->in_syscall, true);
                int64_t result = syscall_table[syscall_num](
                    cpu->regs[0], cpu->regs[1], cpu->regs[2],
                    cpu->regs[3], cpu->regs[4], cpu->regs[5]);
                atomic_store(&current->in_syscall, false);
                STRACE(" = 0x%llx\n", (unsigned long long)result);
                // Single atomic fprintf so multi-thread output does not
                // interleave the args line with another thread's return value.
                if (systrace)
                    fprintf(stderr, "[sc] %ld.%06ld pid=%d n=%d(%llx,%llx,%llx,%llx,%llx,%llx) = 0x%llx\n",
                            (long)_t0.tv_sec, _t0.tv_nsec/1000, current->pid, syscall_num,
                            (unsigned long long)_a0,(unsigned long long)_a1,(unsigned long long)_a2,
                            (unsigned long long)_a3,(unsigned long long)_a4,(unsigned long long)_a5,
                            (unsigned long long)result);
                // SA_RESTART: mark this syscall restartable if it was
                // interrupted (EINTR). receive_signals will rewind PC if the
                // delivered signal's handler has SA_RESTART. Restricted to
                // syscalls that re-execute correctly by simply re-running with
                // the same args (futex, poll/epoll, read/write, recv/send,
                // accept, connect, wait). Timer syscalls (nanosleep,
                // clock_nanosleep, ppoll/pselect with timeout) must NOT use
                // naive PC-rewind restart: it would restart the FULL duration
                // rather than the remaining time, so a `timeout N` wrapper's
                // sleep would never end. Those keep returning EINTR (userspace
                // handles their own restart with an adjusted timeout).
                //
                // The blocking calls Linux returns ERESTARTSYS from start
                // again also when no handler runs for the signal at all
                // (receive_signals): one that stopped the process, which was
                // then continued (Ctrl-Z, fg), or one another thread took.
                // They returned EINTR, which the program never asked for.
                // A futex wait with a time of its own (FUTEX_WAIT's, relative)
                // and epoll_pwait are left to the program then, which loops
                // on their EINTR anyway, rather than wait their full timeout
                // again; one with none, or to a deadline, starts again
                // (futex_restarts_without_handler): a SIGCHLD another thread
                // took, sent as the process blocked it in fork, made musl's
                // sem_wait return EINTR, which musl passes on once a handler
                // without SA_RESTART is installed (regress_process_signals:
                // a worker went on before it was told to, about one run in a
                // hundred under load).
                if ((int32_t)(uint32_t)result == -EINTR) {
                    switch (syscall_num) {
                        case 98:  // futex
                            current->syscall_restartable = true;
                            current->syscall_restart_nohandler = futex_restarts_without_handler(_a1, _a3);
                            break;
                        case 22:  // epoll_pwait
                            current->syscall_restartable = true;
                            break;
                        case 63: case 64:   // read, write
                        case 65: case 66:   // readv, writev
                        case 67: case 68:   // pread64, pwrite64
                        case 260: case 95:  // wait4, waitid
                        case 202: case 242: // accept, accept4
                        case 206: case 207: // sendto, recvfrom
                        case 211: case 212: // sendmsg, recvmsg
                        case 32:            // flock
                            current->syscall_restartable = true;
                            current->syscall_restart_nohandler = true;
                            break;
                        // ERESTARTNOHAND: again only if no handler ran,
                        // EINTR after one even with SA_RESTART
                        case 188: case 189: // msgrcv, msgsnd
                            current->syscall_restartable = false;
                            current->syscall_restart_nohandler = true;
                            break;
                        // ERESTART_RESTARTBLOCK: the same, as restart_syscall,
                        // which sleeps on to the time they slept to
                        // (kernel/time.c): a stopped `sleep 30` continued
                        // (Ctrl-Z, bg) ended with its nanosleep's EINTR. Not
                        // restart_syscall's with no sleep to go on with.
                        case 101: case 115: // nanosleep, clock_nanosleep
                        case 128:           // restart_syscall
                            current->syscall_restartable = false;
                            current->syscall_restart_nohandler = current->restart_sleep_pending;
                            current->syscall_restart_block = current->restart_sleep_pending;
                            break;
                        default:
                            current->syscall_restartable = false;
                    }
                }
                // sigreturn/rt_sigreturn restore the full CPU state from the
                // signal frame. Do NOT touch regs[0] — it was already restored
                // by restore_sigcontext. Any post-processing could corrupt the
                // restored x0 value (e.g., errno sign-extension).
                if (syscall_num == 139 /* rt_sigreturn */ ||
                    syscall_num == 119 /* sigreturn */) {
                    // x0 already set by restore_sigcontext, skip writeback
                } else {
                    // ARM64 Linux ABI: return value in x0. Errors are negative
                    // (-1 to -4095). Success values are 0 or positive (up to 48-bit
                    // addresses from mmap/brk).
                    // Most syscalls return dword_t (uint32_t). On ARM64 ABI, returning
                    // uint32_t sets w0 only; upper x0 is zero. Error codes like -EFAULT
                    // become 0x00000000FFFFFFF2 instead of 0xFFFFFFFFFFFFFFF2.
                    // Detect this: if low 32 bits are a valid errno (0xFFFFF001-0xFFFFFFFF)
                    // but upper 32 bits are 0, sign-extend the 32-bit value.
                    uint32_t low32 = (uint32_t)result;
                    if ((result >> 32) == 0 && (int32_t)low32 >= -4095 && (int32_t)low32 < 0) {
                        // 32-bit errno from dword_t-returning syscall — sign-extend
                        cpu->regs[0] = (uint64_t)(int64_t)(int32_t)low32;
                    } else {
                        cpu->regs[0] = (uint64_t)result;
                    }
                }
            }
        }
        // [T-ish-group-null-post-syscall-crash] The syscall we just ran may have
        // left this thread orphaned: a blocking syscall (futex/poll/wait) can
        // return right as another thread's do_exit_group() safety valve
        // force-releases us — it sets our `exiting` flag — and, once the group
        // leader is reaped, task_destroy() does memset(task, 0, ...) which
        // zeroes our (thread-local) `current` struct, so both `current->group`
        // and `current->exiting` read back as 0/NULL. The post-syscall
        // bookkeeping below dereferences current->group unconditionally; with a
        // zeroed group that computes &((tgroup*)0)->syscall_count == 0x4fc and
        // the atomic_fetch_add faults (the reported EXC_BAD_ACCESS at 0x4fc on a
        // backgrounded guest thread). task_run_current()'s top-of-loop guard
        // already handles this, but only on the NEXT iteration — we must not
        // touch group before we get back there.
        //
        // Bail the SAME way task_run_current() does for a destroyed/leaked task:
        // pthread_exit() straight out, letting task_run_current()'s
        // pthread_cleanup handler (task_run_cleanup) run — it honors
        // mm_release_deferred, frees the tlb, takes a leaked thread's task out
        // of the pid table and lets go of the struct. (A destroyed task is
        // poisoned with exiting set and no group; this thread's hold keeps its
        // struct alive.) There is nothing for do_exit() to do: the group, its
        // status and the resources are gone or going.
        if (current->exiting || current->group == NULL) {
            pthread_exit(NULL);
        }
        // Update deadlock detection state.
        atomic_fetch_add(&current->group->syscall_count, 1);
        // [T-ish-freeze-watch] Process-wide, readable without pids_lock.
        atomic_fetch_add_explicit(&ish_guest_syscalls, 1, memory_order_relaxed);
        // Update last_unblocked_ns for non-blocking syscalls only.
        // Blocking calls (futex, epoll, ppoll, waitid, nanosleep) that
        // return and retry should NOT refresh the timestamp, as they
        // don't represent real forward progress.
        if (syscall_num != 22 && syscall_num != 73 && syscall_num != 95 &&
            syscall_num != 98 && syscall_num != 101) {
            struct timespec _ts;
            clock_gettime(CLOCK_MONOTONIC, &_ts);
            uint64_t now = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
            current->last_unblocked_ns = now;
            atomic_store_explicit(&current->group->last_progress_ns, now,
                                  memory_order_relaxed);
        }
        // Check for pending group exit. When do_exit_group sends SIGKILL to
        // all threads, a thread stuck in a retrying syscall (e.g., musl's
        // futex_wait loops on EINTR) would never return to the JIT dispatch
        // loop where receive_signals() handles SIGKILL. Catch it here.
        if (current->group->doing_group_exit)
            do_exit(current->group->group_exit_code);
        ISH_SIGNPOST_SCOPE_END(syscall, "syscall", _sc_spid);
    } else if (interrupt == INT_GPF) {
        if (ish_exec_trace())
            fprintf(stderr, "INT_GPF pid=%d pc=0x%llx fault=0x%llx write=%d\n",
                    current->pid,
                    (unsigned long long)cpu->pc,
                    (unsigned long long)cpu->segfault_addr,
                    cpu->segfault_was_write);
        // Instruction-fetch fault: if the guest PC itself points to
        // unmapped memory, the JIT could not even read an instruction
        // to compile. This leaves segfault_addr stale from an earlier
        // data fault. Rewrite segfault_addr to the unmapped PC so the
        // SIGSEGV delivered downstream is meaningful (and any recovery
        // heuristic treats it as a read-from-unmapped-code-page fault
        // rather than retrying a stale data fault that already got
        // resolved by demand-map).
        {
            read_wrlock(&current->mem->lock);
            void *pc_ptr = mem_ptr(current->mem, cpu->pc, MEM_READ);
            read_wrunlock(&current->mem->lock);
            if (pc_ptr == NULL && cpu->pc != 0) {
                cpu->segfault_addr = cpu->pc;
                cpu->segfault_was_write = 0;
            }
        }
        // No limit on faults at one instruction: a program whose SIGSEGV
        // handler resumes elsewhere faults there legitimately, as often as
        // it runs the code. HotSpot's implicit null checks do, and a count
        // that killed the process after 256 of them took down any JVM that
        // threw NullPointerException in a loop.
        read_wrlock(&current->mem->lock);
        void *ptr = mem_ptr(current->mem, cpu->segfault_addr, cpu->segfault_was_write ? MEM_WRITE : MEM_READ);
        read_wrunlock(&current->mem->lock);
        if (ptr == NULL) {
            // Diagnostic output for every INT_GPF is verbose (register dump,
            // block insn dump). Suppress unless ISH_EXEC_TRACE is set — the
            // SIGSEGV will still be delivered, so the shell still reports
            // "Segmentation fault" and exit status, which is what users
            // normally need. Opt-in verbose is for kernel debugging.
            bool trace_enabled = ish_exec_trace();
            if (trace_enabled)
                printk("%d page fault on 0x%llx at 0x%llx (%s)\n", current->pid, (unsigned long long)cpu->segfault_addr, (unsigned long long)cpu->pc, cpu->segfault_was_write ? "write" : "read");
            // Dump instruction bytes and key registers for debugging
            if (trace_enabled) {
                uint32_t fault_insn = 0;
                for (int j = 0; j < 4; j++) {
                    uint8_t b;
                    if (user_get(cpu->pc + j, b)) break;
                    fault_insn |= (uint32_t)b << (j * 8);
                }
                printk("  insn=0x%08x x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
                    fault_insn, (unsigned long long)cpu->regs[0], (unsigned long long)cpu->regs[1],
                    (unsigned long long)cpu->regs[2], (unsigned long long)cpu->regs[3]);
                printk("  sp=0x%llx x29=0x%llx x30=0x%llx brk=0x%llx\n",
                    (unsigned long long)cpu->sp,
                    (unsigned long long)cpu->regs[29], (unsigned long long)cpu->regs[30],
                    (unsigned long long)current->mm->brk);
                printk("  x4=0x%llx x5=0x%llx x8=0x%llx x9=0x%llx x10=0x%llx\n",
                    (unsigned long long)cpu->regs[4], (unsigned long long)cpu->regs[5],
                    (unsigned long long)cpu->regs[8], (unsigned long long)cpu->regs[9],
                    (unsigned long long)cpu->regs[10]);
                printk("  sp+0x3e8=0x%llx jit_crashes=%d\n",
                    (unsigned long long)(cpu->sp + 0x3e8),
                    jit_crash_count);
                // Guest backtrace: walk the x29 frame chain (works for both
                // AAPCS64 and JSC CallFrame layouts — caller frame at [fp],
                // return PC at [fp+8]). Print the first frames and a return-PC
                // histogram so an unbounded recursion shows its cycle.
                if (getenv("ISH_GPF_BT")) {
                    // Watchpoint ring: who wrote the watched window (ISH_WATCH_PAGE)
                    { extern void dump_watch_hits(uint64_t lo, uint64_t hi);
                      extern volatile uint64_t g_watch_lo, g_watch_hi;
                      dump_watch_hits(g_watch_lo, g_watch_hi); }
                    { extern uint64_t g_watch_field_regs[32];
                      extern volatile int g_watch_field_hit_count;
                      if (g_watch_field_hit_count) {
                          printk("  [watch-field] store hit count=%d, regs at first hit:\n", g_watch_field_hit_count);
                          for (int i = 0; i < 31; i += 4)
                              printk("    x%d=0x%llx x%d=0x%llx x%d=0x%llx x%d=0x%llx\n",
                                     i, (unsigned long long)g_watch_field_regs[i],
                                     i+1, i+1 < 31 ? (unsigned long long)g_watch_field_regs[i+1] : 0,
                                     i+2, i+2 < 31 ? (unsigned long long)g_watch_field_regs[i+2] : 0,
                                     i+3, i+3 < 31 ? (unsigned long long)g_watch_field_regs[i+3] : 0);
                          printk("    pc=0x%llx\n", (unsigned long long)g_watch_field_regs[31]);
                          { extern uint64_t g_watch_field_last_regs[32];
                            printk("  [watch-field] LAST hit x8=0x%llx x9=0x%llx x0=0x%llx x19=0x%llx\n",
                                   (unsigned long long)g_watch_field_last_regs[8],
                                   (unsigned long long)g_watch_field_last_regs[9],
                                   (unsigned long long)g_watch_field_last_regs[0],
                                   (unsigned long long)g_watch_field_last_regs[19]); }
                          // Fresh re-reads of candidate source pointers +0x14
                          for (int r = 0; r < 31; r++) {
                              uint64_t p = g_watch_field_regs[r];
                              if (p >= 0x100000 && p < 0x100000000000ULL) {
                                  uint32_t v = 0;
                                  if (!user_get(p + 0x14, v))
                                      printk("    [x%d+0x14] = 0x%x\n", r, v);
                              }
                          }
                          // Block 0x41eee2c reads numVars from [x19+0x18] and
                          // numCalleeLocals from [x19+0x1c]; dump x19 source.
                          { uint64_t base = g_watch_field_regs[19];
                            for (int off = 0; off < 0x40; off += 4) {
                                uint32_t v = 0;
                                if (!user_get(base + off, v))
                                    printk("    [x19 0x%llx+0x%x] = 0x%x\n",
                                           (unsigned long long)base, off, v);
                            }
                            struct pt_entry *pe = mem_pt(current->mem, PAGE(base));
                            if (pe)
                                printk("    x19 page flags=0x%x data=%p off=0x%zx refcnt=%u\n",
                                       pe->flags, pe->data ? pe->data->data : NULL, pe->offset,
                                       pe->data ? pe->data->refcount : 0);
                          }
                      } }
                    // Fresh re-read of the memory x1 points at (LLInt CodeBlock
                    // candidate): distinguishes "load saw stale TLB data" (fresh
                    // read differs from what the gadget loaded) from "memory
                    // really holds this value".
                    for (int off = 0; off <= 0x18; off += 4) {
                        uint32_t v = 0;
                        if (!user_get(cpu->regs[1] + off, v))
                            printk("  [x1+0x%x] = 0x%x\n", off, v);
                    }
                    // Follow CodeBlock::m_unlinkedCode ([x1+0x38]) and dump the
                    // UnlinkedCodeBlock the crashing CodeBlock was built from.
                    {
                        uint64_t unlinked = 0;
                        if (!user_get(cpu->regs[1] + 0x38, unlinked) && unlinked) {
                            printk("  unlinked=[x1+0x38]=0x%llx\n", (unsigned long long)unlinked);
                            { extern void dump_watch_hits_for(uint64_t addr);
                              dump_watch_hits_for(unlinked + 0x10); }
                            for (int off = 0; off <= 0x40; off += 8) {
                                uint64_t v = 0;
                                if (!user_get(unlinked + off, v))
                                    printk("    [ulk+0x%x] = 0x%llx\n", off, (unsigned long long)v);
                            }
                            uint32_t ctr = 0;
                            if (!user_get(unlinked + 0xa8, ctr))
                                printk("    [ulk+0xa8](counter) = 0x%x\n", ctr);
                            struct pt_entry *pe = mem_pt(current->mem, PAGE(unlinked));
                            if (pe != NULL)
                                printk("    ulk page flags=0x%x data=%p offset=0x%zx refcnt=%u\n",
                                       pe->flags, pe->data ? pe->data->data : NULL,
                                       pe->offset, pe->data ? pe->data->refcount : 0);
                        }
                    }
                    uint64_t fp = cpu->regs[29];
                    uint64_t pcs[64]; int n = 0;
                    struct { uint64_t pc; int count; } hist[32]; int nh = 0;
                    for (int i = 0; i < 100000 && fp; i++) {
                        uint64_t next_fp = 0, ret_pc = 0;
                        if (user_get(fp, next_fp) || user_get(fp + 8, ret_pc)) break;
                        if (n < 64) pcs[n++] = ret_pc;
                        for (int h = 0; h < nh; h++)
                            if (hist[h].pc == ret_pc) { hist[h].count++; ret_pc = 0; break; }
                        if (ret_pc && nh < 32) { hist[nh].pc = ret_pc; hist[nh].count = 1; nh++; }
                        if (next_fp <= fp) break;
                        fp = next_fp;
                    }
                    printk("  [bt] first %d return PCs:\n", n);
                    for (int i = 0; i < n; i++) printk("    #%d 0x%llx\n", i, (unsigned long long)pcs[i]);
                    printk("  [bt] return PC histogram (unique among walked frames):\n");
                    for (int h = 0; h < nh; h++)
                        printk("    0x%llx x%d\n", (unsigned long long)hist[h].pc, hist[h].count);
                    // Dump code preceding return PCs that live in JIT-generated
                    // memory (0xc0000000 range) — the caller thunk's tail.
                    for (int i = 0; i < n && i < 12; i++) {
                        if (pcs[i] < 0xc0000000ULL || pcs[i] >= 0xe0000000ULL)
                            continue;
                        printk("  [bt] JIT code before ret pc 0x%llx:\n", (unsigned long long)pcs[i]);
                        for (int k = -24; k <= 2; k++) {
                            uint32_t insn = 0; bool ok = true;
                            for (int j = 0; j < 4; j++) {
                                uint8_t b;
                                if (user_get(pcs[i] + k*4 + j, b)) { ok = false; break; }
                                insn |= (uint32_t)b << (j*8);
                            }
                            if (ok) printk("    0x%llx: 0x%08x\n", (unsigned long long)(pcs[i] + k*4), insn);
                        }
                    }
                }
                // Dump block instructions when fault addr > 4GB (overflow address)
                if (cpu->segfault_addr > 0x100000000ULL) {
                    printk("  block insns from PC=0x%llx:\n", (unsigned long long)cpu->pc);
                    for (int bi = 0; bi < 32; bi++) {
                        uint32_t bi_insn = 0;
                        bool bi_ok = true;
                        for (int j = 0; j < 4; j++) {
                            uint8_t b;
                            if (user_get(cpu->pc + bi * 4 + j, b)) { bi_ok = false; break; }
                            bi_insn |= (uint32_t)b << (j * 8);
                        }
                        if (!bi_ok) break;
                        printk("    0x%llx: 0x%08x\n", (unsigned long long)(cpu->pc + bi * 4), bi_insn);
                        // Stop at RET or unconditional branch
                        if (bi_insn == 0xd65f03c0 || (bi_insn & 0xFC000000) == 0x14000000)
                            break;
                    }
                }
            }
            // A read of unmapped memory is a SIGSEGV, as on Linux. Programs
            // rely on it: HotSpot's implicit null checks and Go's nil checks
            // read near address 0 and expect the fault. A recovery here used
            // to make such reads return 0 and step over the load, so the JVM
            // went on with a null klass and jumped into the bytes of its own
            // code.

            struct siginfo_ info = {
                .code = mem_segv_reason(current->mem, cpu->segfault_addr),
                .fault.addr = cpu->segfault_addr,
            };
            if (getenv("ISH_EXEC_TRACE")) {
                dump_stack(8);
                dump_maps();
            }
            gpf_use_precise_pc(cpu);
            force_signal(current, SIGSEGV_, info, false);
        }
        // Or the page is there now (mem_ptr committed it from a reservation,
        // or another thread mapped it): retry the faulting instruction.
        gpf_use_precise_pc(cpu);
    } else if (interrupt == INT_UNDEFINED) {
        // As Linux (show_unhandled_signals): nothing when the program handles
        // SIGILL itself, as one probing for a CPU feature does; one line when
        // it is about to die of it. An instruction iSH does not implement
        // lands here too, and this line is its sample.
        if (!task_handles_signal(current, SIGILL_)) {
            uint32_t ill_insn = 0;
            bool readable = user_get(cpu->pc, ill_insn) == 0;
            if (readable)
                printk("%s[%d]: unhandled illegal instruction %#010x at %#llx\n",
                       current->comm, current->pid, ill_insn, (unsigned long long) cpu->pc);
            else
                printk("%s[%d]: unhandled illegal instruction at %#llx (unreadable)\n",
                       current->comm, current->pid, (unsigned long long) cpu->pc);
        }
        struct siginfo_ info = {
            .code = SI_KERNEL_,
            .fault.addr = cpu->pc,
        };
        force_signal(current, SIGILL_, info, false);
    } else if (interrupt == INT_BREAKPOINT) {
        force_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = SI_KERNEL_,
        }, false);
    } else if (interrupt == INT_DEBUG) {
        force_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = TRAP_TRACE_,
        }, false);
    } else if (interrupt == INT_ASBESTOS_DETACH || interrupt == INT_ASBESTOS_DIFFERS) {
        // its address space leaves the asbestos it shares before it runs or
        // translates more (asbestos.c, mem_code_written); the guest has not
        // moved, its pc the block's
        write_wrlock(&current->mem->lock);
        mem_asbestos_detach(current->mem, interrupt == INT_ASBESTOS_DIFFERS);
        write_wrunlock(&current->mem->lock);
    } else if (interrupt == INT_TIMER) {
        ish_timer_tick_hook_t hook =
            atomic_load_explicit(&g_timer_tick_hook, memory_order_acquire);
        if (hook) hook();
#ifdef ISH_JIT
    } else if (interrupt == INT_JIT_NATIVE_FAULT) {
        // A host fault in native code not at a guest access: no position to
        // retry from. Killed as by a SIGSEGV no handler can take for its own.
        char text[8192];
        jit_take_unknown_fault(text, sizeof(text), cpu);
        printk("%d(%s): %s", current->pid, current->comm, text);
        do_exit_group(SIGSEGV_);
#endif
    } else {
        printk("EXIT[unhandled-int]: pid=%d interrupt=%d\n", current->pid, interrupt);
        sys_exit(interrupt);
    }

    receive_signals();
    struct tgroup *group = current->group;
    lock(&group->lock);
    while (group->stopped)
        wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
    // continued by SIGCONT: its parent hears of it now, as on Linux from the
    // process as it runs on
    bool continued = group->continued_unnotified;
    group->continued_unnotified = false;
    unlock(&group->lock);
    if (continued)
        notify_parent_stop(CLD_CONTINUED_, SIGCONT_);
}

void dump_maps(void) {
    extern void proc_maps_dump(struct task *task, struct proc_data *buf);
    struct proc_data buf = {};
    proc_maps_dump(current, &buf);
    // go a line at a time because it can be fucking enormous
    char *orig_data = buf.data;
    while (buf.size > 0) {
        size_t chunk_size = buf.size;
        if (chunk_size > 1024)
            chunk_size = 1024;
        printk("%.*s", chunk_size, buf.data);
        buf.data += chunk_size;
        buf.size -= chunk_size;
    }
    free(orig_data);
}

void dump_mem(addr_t start, uint_t len) {
    const int width = 8;
    for (addr_t addr = start; addr < start + len; addr += sizeof(dword_t)) {
        unsigned from_left = (addr - start) / sizeof(dword_t) % width;
        if (from_left == 0)
            printk("%012llx: ", (unsigned long long)addr);
        dword_t word;
        if (user_get(addr, word))
            break;
        printk("%08x ", word);
        if (from_left == width - 1)
            printk("\n");
    }
}

void dump_stack(int lines) {
    printk("stack at %llx, base at %llx, ip at %llx\n",
           (unsigned long long)current->cpu.sp,
           (unsigned long long)current->cpu.regs[29],  // x29 is frame pointer
           (unsigned long long)current->cpu.pc);
    dump_mem(current->cpu.sp, lines * sizeof(uint64_t) * 8);
}

// === Fast Path Implementations ===

// Fast path for fstat64 (syscall 80)
// Bypasses generic_statat path normalization when fd is already validated realfs
static inline int fast_fstat64(struct cpu_state *cpu) {
    fd_t fd_no = (fd_t)cpu->regs[0];
    addr_t statbuf_addr = (addr_t)cpu->regs[1];

    // Quick validation: fd must be valid
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return -1;  // Fall back to slow path

    // Fast path condition: fd is realfs (not adhoc, not procfs, etc.). A
    // pipe has realfs's ops but a stat of its own (fs/pipe.c): its inode,
    // which /proc/<pid>/fd links to, and the guest's owner.
    if (fd->ops != &realfs_fdops || (is_adhoc_fd(fd) && S_ISFIFO(fd->stat.mode)))
        return -1;  // Fall back to slow path

    // Direct host fstat call (bypass generic layers)
    struct stat real_stat;
    if (fstat(fd->real_fd, &real_stat) < 0)
        return errno_map();

    // Convert to guest statbuf
    struct statbuf fake_stat = {};
    fake_stat.dev = dev_fake_from_real(real_stat.st_dev);
    fake_stat.inode = real_stat.st_ino;
    fake_stat.mode = real_stat.st_mode;
    fake_stat.nlink = real_stat.st_nlink;
    fake_stat.uid = real_stat.st_uid;
    fake_stat.gid = real_stat.st_gid;
    fake_stat.rdev = dev_fake_from_real(real_stat.st_rdev);
    fake_stat.size = real_stat.st_size;
    fake_stat.blksize = real_stat.st_blksize;
    fake_stat.blocks = real_stat.st_blocks;
    fake_stat.atime = real_stat.st_atime;
    fake_stat.mtime = real_stat.st_mtime;
    fake_stat.ctime = real_stat.st_ctime;
#if __APPLE__
    fake_stat.atime_nsec = real_stat.st_atimespec.tv_nsec;
    fake_stat.mtime_nsec = real_stat.st_mtimespec.tv_nsec;
    fake_stat.ctime_nsec = real_stat.st_ctimespec.tv_nsec;
#elif __linux__
    fake_stat.atime_nsec = real_stat.st_atim.tv_nsec;
    fake_stat.mtime_nsec = real_stat.st_mtim.tv_nsec;
    fake_stat.ctime_nsec = real_stat.st_ctim.tv_nsec;
#endif

    // Convert to ARM64 stat structure
    struct stat_arm64 arm64stat = {};
    arm64stat.dev = fake_stat.dev;
    arm64stat.ino = fake_stat.inode;
    arm64stat.mode = fake_stat.mode;
    arm64stat.nlink = fake_stat.nlink;
    arm64stat.uid = fake_stat.uid;
    arm64stat.gid = fake_stat.gid;
    arm64stat.rdev = fake_stat.rdev;
    arm64stat.__pad1 = 0;
    arm64stat.size = fake_stat.size;
    arm64stat.blksize = fake_stat.blksize;
    arm64stat.__pad2 = 0;
    arm64stat.blocks = fake_stat.blocks;
    arm64stat.atime_ = fake_stat.atime;
    arm64stat.atime_nsec = fake_stat.atime_nsec;
    arm64stat.mtime_ = fake_stat.mtime;
    arm64stat.mtime_nsec = fake_stat.mtime_nsec;
    arm64stat.ctime_ = fake_stat.ctime;
    arm64stat.ctime_nsec = fake_stat.ctime_nsec;
    arm64stat.__unused4 = 0;
    arm64stat.__unused5 = 0;

    // Copy to user space
    if (user_put(statbuf_addr, arm64stat))
        return _EFAULT;

    return 0;  // Success
}

// Fast path for read (syscall 63) - small buffers only
static inline int fast_read(struct cpu_state *cpu) {
    fd_t fd_no = (fd_t)cpu->regs[0];
    addr_t buf_addr = (addr_t)cpu->regs[1];
    dword_t size = (dword_t)cpu->regs[2];

    // Fast path condition: small buffer (≤ 4KB) and realfs fd
    if (size > 4096)
        return -1;

    // Not a pipe or FIFO: a read there can wait for good, and only
    // realfs_read lets a signal end the wait. Nor while reads are watched
    // (inotify's IN_ACCESS: sys_read_buf reports it).
    if (inotify_wants(IN_ACCESS_))
        return -1;
    struct fd *fd = f_get(fd_no);
    if (fd == NULL || fd->ops != &realfs_fdops || S_ISFIFO(fd->type))
        return -1;

    // Direct host read (with EINTR retry)
    char buf[4096];
    ssize_t res;
    do {
        res = read(fd->real_fd, buf, size);
    } while (res < 0 && errno == EINTR);

    if (res < 0)
        return errno_map();

    // Copy to guest memory
    if (res > 0 && user_write(buf_addr, buf, res))
        return _EFAULT;

    return res;
}

// Fast path for write (syscall 64) - small buffers only
static inline int fast_write(struct cpu_state *cpu) {
    fd_t fd_no = (fd_t)cpu->regs[0];
    addr_t buf_addr = (addr_t)cpu->regs[1];
    dword_t size = (dword_t)cpu->regs[2];

    // Fast path condition: small buffer (≤ 4KB) and realfs fd
    if (size > 4096)
        return -1;

    // as fast_read: not a pipe or FIFO, nor while writes are watched
    if (inotify_wants(IN_MODIFY_))
        return -1;
    struct fd *fd = f_get(fd_no);
    if (fd == NULL || fd->ops != &realfs_fdops || S_ISFIFO(fd->type))
        return -1;

    // Copy from guest memory
    char buf[4096];
    if (user_read(buf_addr, buf, size))
        return _EFAULT;

    // Direct host write (with EINTR retry)
    ssize_t res;
    do {
        res = write(fd->real_fd, buf, size);
    } while (res < 0 && errno == EINTR);

    if (res < 0)
        return errno_map();

    return res;
}

