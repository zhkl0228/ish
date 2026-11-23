// System V semaphores: semget, semop, semtimedop, semctl (arm64 guest).
//
// A semop applies all its operations or none: it tries them in order on the
// values and backs out if one would block, then waits on the set's condition
// and tries again; every change to the values wakes all those waiting. Each
// waiter keeps the operation it is blocked on, for GETNCNT and GETZCNT.
// IPC_RMID takes the set off its id at once; whoever was waiting on it gets
// EIDRM, and the last of them frees it.
//
// SEM_UNDO's adjustments are kept per process (thread group) and set, and
// applied when its last thread exits (sem_exit), as Linux's for threads that
// share them with CLONE_SYSVSEM, which pthreads do. A child of fork starts
// without any; exec keeps them.
//
// semop and semtimedop fail with EINTR after any signal, as on Linux.

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel/calls.h"
#include "kernel/sysvipc.h"
#include "kernel/time.h"
#include "util/list.h"
#include "util/sync.h"

#define GETPID_ 11
#define GETVAL_ 12
#define GETALL_ 13
#define GETNCNT_ 14
#define GETZCNT_ 15
#define SETVAL_ 16
#define SETALL_ 17
#define SEM_STAT_ 18
#define SEM_INFO_ 19
#define SEM_STAT_ANY_ 20
#define SEM_UNDO_ 0x1000

// asm-generic's sembuf, semid64_ds, and IPC_INFO's and SEM_INFO's seminfo
struct sembuf_ {
    uint16_t num;
    int16_t op;
    int16_t flg;
};
struct semid64_ds_ {
    struct ipc64_perm_ perm;
    int64_t otime, ctime;
    uint64_t nsems;
    uint64_t unused3, unused4;
};
struct seminfo_ {
    int32_t semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem;
};
static_assert(sizeof(struct sembuf_) == 6, "sembuf layout");
static_assert(sizeof(struct semid64_ds_) == 88, "semid64_ds layout");
static_assert(sizeof(struct seminfo_) == 40, "seminfo layout");

// Linux's defaults
#define SEMMNI_ 32000
#define SEMMSL_ 32000
#define SEMMNS_ (SEMMNI_ * SEMMSL_)
#define SEMOPM_ 500
#define SEMVMX_ 32767
#define SEMAEM_ SEMVMX_
#define SEMUME_ SEMOPM_
#define SEMMNU_ SEMMNS_
#define SEMMAP_ SEMMNS_
#define SEMUSZ_ 20

struct sem {
    int val;
    pid_t_ pid;
};

// A process's SEM_UNDO adjustments in a set
struct sem_undo {
    struct list undos;
    struct tgroup *group;
    int adj[];
};

struct sem_waiter {
    struct list waiters;
    struct sembuf_ blocking;
};

struct sem_set {
    struct ipc_object ipc;
    unsigned nsems;
    int64_t otime, ctime;
    struct list undos;
    struct list waiters;
    cond_t changed;
    bool removed;
    struct sem sems[];
};

static lock_t sem_lock = LOCK_INITIALIZER;
static struct ipc_ids sem_ids = IPC_IDS_INITIALIZER(sem_ids, SEMMNI_);
// in all sets, for SEM_INFO
static int32_t sem_count;

static void sem_free(struct sem_set *set) {
    cond_destroy(&set->changed);
    free(set);
}

int_t sys_semget(int_t key, int_t nsems, int_t flags) {
    STRACE("semget(%#x, %d, %#o)", key, nsems, flags);
    if (nsems < 0 || nsems > SEMMSL_)
        return _EINVAL;
    lock(&sem_lock);
    struct ipc_object *obj;
    int_t res = ipc_get(&sem_ids, key, flags, &obj);
    if (res < 0 || obj != NULL) {
        if (obj != NULL)
            res = (unsigned) nsems > ((struct sem_set *) obj)->nsems ? _EINVAL : obj->id;
        unlock(&sem_lock);
        return res;
    }
    if (nsems == 0) {
        unlock(&sem_lock);
        return _EINVAL;
    }
    struct sem_set *set = calloc(1, sizeof(*set) + nsems * sizeof(struct sem));
    set->nsems = nsems;
    set->ctime = time(NULL);
    list_init(&set->undos);
    list_init(&set->waiters);
    cond_init(&set->changed);
    res = ipc_add(&sem_ids, &set->ipc, key, flags);
    if (res < 0) {
        sem_free(set);
    } else {
        sem_count += nsems;
        res = set->ipc.id;
    }
    unlock(&sem_lock);
    return res;
}

static struct sem_undo *sem_find_undo(struct sem_set *set, struct tgroup *group) {
    struct sem_undo *undo;
    list_for_each_entry(&set->undos, undo, undos) {
        if (undo->group == group)
            return undo;
    }
    return NULL;
}

// SETVAL and SETALL: a value set is no one's to undo
static void sem_clear_undos(struct sem_set *set, int first, int count) {
    struct sem_undo *undo;
    list_for_each_entry(&set->undos, undo, undos)
        memset(&undo->adj[first], 0, count * sizeof(undo->adj[0]));
}

// Applies ops to set: 0; 1 if one would block, *blocking that one; _EAGAIN if
// that one has IPC_NOWAIT; or _ERANGE for a value or an adjustment out of
// range. Nothing applied unless 0.
static int_t sem_try(struct sem_set *set, struct sembuf_ *ops, unsigned nops, struct sem_undo *undo, struct sembuf_ **blocking) {
    int_t res = 0;
    unsigned i;
    for (i = 0; i < nops; i++) {
        struct sembuf_ *op = &ops[i];
        struct sem *sem = &set->sems[op->num];
        int val = sem->val + op->op;
        if ((op->op == 0 && sem->val != 0) || val < 0) {
            *blocking = op;
            res = op->flg & IPC_NOWAIT_ ? _EAGAIN : 1;
            break;
        }
        if (val > SEMVMX_) {
            res = _ERANGE;
            break;
        }
        if (op->flg & SEM_UNDO_) {
            int adj = undo->adj[op->num] - op->op;
            if (adj < -SEMAEM_ - 1 || adj > SEMAEM_) {
                res = _ERANGE;
                break;
            }
            undo->adj[op->num] = adj;
        }
        sem->val = val;
    }
    if (res == 0) {
        for (i = 0; i < nops; i++)
            set->sems[ops[i].num].pid = current->tgid;
        return 0;
    }
    while (i-- > 0) {
        set->sems[ops[i].num].val -= ops[i].op;
        if (ops[i].flg & SEM_UNDO_)
            undo->adj[ops[i].num] += ops[i].op;
    }
    return res;
}

int_t sys_semtimedop(int_t id, addr_t sops_addr, uint_t nsops, addr_t timeout_addr) {
    STRACE("semtimedop(%d, %#llx, %u, %#llx)", id, (unsigned long long) sops_addr, nsops, (unsigned long long) timeout_addr);
    if (nsops < 1 || id < 0)
        return _EINVAL;
    if (nsops > SEMOPM_)
        return _E2BIG;
    struct sembuf_ ops[nsops];
    if (user_read(sops_addr, ops, sizeof(ops)))
        return _EFAULT;
    struct timespec deadline;
    if (timeout_addr != 0) {
        struct timespec_ timeout;
        if (user_get(timeout_addr, timeout))
            return _EFAULT;
        if (timeout.sec < 0 || timeout.nsec < 0 || timeout.nsec >= 1000000000)
            return _EINVAL;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout.sec;
        deadline.tv_nsec += timeout.nsec;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    unsigned max = 0;
    bool alter = false, undoing = false;
    for (unsigned i = 0; i < nsops; i++) {
        if (ops[i].num > max)
            max = ops[i].num;
        if (ops[i].op != 0)
            alter = true;
        if (ops[i].flg & SEM_UNDO_)
            undoing = true;
    }

    lock(&sem_lock);
    struct sem_set *set = (struct sem_set *) ipc_by_id(&sem_ids, id);
    int_t res;
    if (set == NULL)
        res = _EINVAL;
    else if (max >= set->nsems)
        res = _EFBIG;
    else if (!ipc_permitted(&set->ipc, alter ? 0222 : 0444))
        res = _EACCES;
    else
        res = 1;
    struct sem_undo *undo = NULL;
    if (res == 1 && undoing) {
        undo = sem_find_undo(set, current->group);
        if (undo == NULL) {
            undo = calloc(1, sizeof(*undo) + set->nsems * sizeof(undo->adj[0]));
            undo->group = current->group;
            list_add(&set->undos, &undo->undos);
        }
    }
    struct sem_waiter waiter;
    while (res == 1) {
        struct sembuf_ *blocking;
        res = sem_try(set, ops, nsops, undo, &blocking);
        if (res == 0) {
            set->otime = time(NULL);
            if (alter)
                notify(&set->changed);
            break;
        }
        if (res < 0)
            break;

        struct timespec left, *timeout = NULL;
        if (timeout_addr != 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            left.tv_sec = deadline.tv_sec - now.tv_sec;
            left.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (left.tv_nsec < 0) {
                left.tv_sec--;
                left.tv_nsec += 1000000000;
            }
            if (left.tv_sec < 0) {
                res = _EAGAIN;
                break;
            }
            timeout = &left;
        }
        waiter.blocking = *blocking;
        list_add_tail(&set->waiters, &waiter.waiters);
        int err = wait_for(&set->changed, &sem_lock, timeout);
        list_remove(&waiter.waiters);
        if (set->removed) {
            if (list_empty(&set->waiters))
                sem_free(set);
            res = _EIDRM;
            break;
        }
        if (err == _EINTR)
            res = _EINTR;
        else if (err == _ETIMEDOUT)
            res = _EAGAIN;
        else
            res = 1;
    }
    unlock(&sem_lock);
    return res;
}

int_t sys_semop(int_t id, addr_t sops_addr, uint_t nsops) {
    return sys_semtimedop(id, sops_addr, nsops, 0);
}

// GETNCNT and GETZCNT: those waiting with an operation blocked on the
// semaphore, to decrease it or for it to be zero
static int_t sem_waiting(struct sem_set *set, int num, bool for_zero) {
    int_t count = 0;
    struct sem_waiter *waiter;
    list_for_each_entry(&set->waiters, waiter, waiters) {
        if (waiter->blocking.num == num && (for_zero ? waiter->blocking.op == 0 : waiter->blocking.op < 0))
            count++;
    }
    return count;
}

// IPC_INFO and SEM_INFO (ipcs): the highest index in use, 0 if none
static int_t sem_info(int_t cmd, addr_t buf_addr) {
    struct seminfo_ info = {
        .semmni = SEMMNI_,
        .semmns = SEMMNS_,
        .semmsl = SEMMSL_,
        .semopm = SEMOPM_,
        .semvmx = SEMVMX_,
        .semmnu = SEMMNU_,
        .semmap = SEMMAP_,
        .semume = SEMUME_,
        .semusz = SEMUSZ_,
        .semaem = SEMAEM_,
    };
    lock(&sem_lock);
    if (cmd == SEM_INFO_) {
        // sets and semaphores in use
        info.semusz = ipc_count(&sem_ids);
        info.semaem = sem_count;
    }
    int32_t max = ipc_max_index(&sem_ids);
    unlock(&sem_lock);
    if (user_put(buf_addr, info))
        return _EFAULT;
    return max < 0 ? 0 : max;
}

int_t sys_semctl(int_t id, int_t num, int_t cmd, addr_t arg) {
    STRACE("semctl(%d, %d, %d, %#llx)", id, num, cmd, (unsigned long long) arg);
    if (id < 0)
        return _EINVAL;
    struct semid64_ds_ ds;
    switch (cmd) {
        case IPC_INFO_:
        case SEM_INFO_:
            return sem_info(cmd, arg);
        case IPC_SET_:
            if (user_get(arg, ds))
                return _EFAULT;
            break;
        case SETVAL_:
            // an int, in the union's low bytes
            if ((int32_t) arg < 0 || (int32_t) arg > SEMVMX_)
                return _ERANGE;
            break;
        case IPC_STAT_:
        case SEM_STAT_:
        case SEM_STAT_ANY_:
        case IPC_RMID_:
        case GETPID_:
        case GETVAL_:
        case GETALL_:
        case GETNCNT_:
        case GETZCNT_:
        case SETALL_:
            break;
        default:
            return _EINVAL;
    }

    // SEM_STAT takes an index and gives the id, for ipcs; SEM_STAT_ANY too,
    // whatever the permissions
    lock(&sem_lock);
    struct sem_set *set = (struct sem_set *) (cmd == SEM_STAT_ || cmd == SEM_STAT_ANY_ ?
            ipc_by_index(&sem_ids, id) : ipc_by_id(&sem_ids, id));
    int_t res = 0;
    if (set == NULL) {
        res = _EINVAL;
        goto out;
    }

    if (cmd == IPC_STAT_ || cmd == SEM_STAT_ || cmd == SEM_STAT_ANY_) {
        if (cmd != SEM_STAT_ANY_ && !ipc_permitted(&set->ipc, 0444)) {
            res = _EACCES;
            goto out;
        }
        ds = (struct semid64_ds_) {
            .perm = ipc_perm(&set->ipc),
            .otime = set->otime,
            .ctime = set->ctime,
            .nsems = set->nsems,
        };
        res = cmd == IPC_STAT_ ? 0 : set->ipc.id;
        unlock(&sem_lock);
        if (user_put(arg, ds))
            return _EFAULT;
        return res;
    }

    if (cmd == IPC_SET_ || cmd == IPC_RMID_) {
        if (!ipc_owner(&set->ipc)) {
            res = _EPERM;
            goto out;
        }
        if (cmd == IPC_SET_) {
            res = ipc_set_perm(&set->ipc, &ds.perm);
            if (res == 0)
                set->ctime = time(NULL);
            goto out;
        }
        // IPC_RMID: its undos go now, it once nobody waits on it
        ipc_remove(&set->ipc);
        sem_count -= set->nsems;
        struct sem_undo *undo, *tmp;
        list_for_each_entry_safe(&set->undos, undo, tmp, undos) {
            list_remove(&undo->undos);
            free(undo);
        }
        set->removed = true;
        if (list_empty(&set->waiters))
            sem_free(set);
        else
            notify(&set->changed);
        goto out;
    }

    // SETVAL looks at num first, the others at the permissions, as Linux
    unsigned nsems = set->nsems;
    if (cmd == SETVAL_ && (num < 0 || (unsigned) num >= nsems)) {
        res = _EINVAL;
        goto out;
    }
    bool setting = cmd == SETVAL_ || cmd == SETALL_;
    if (!ipc_permitted(&set->ipc, setting ? 0222 : 0444)) {
        res = _EACCES;
        goto out;
    }

    // GETALL and SETALL: every value, and num means nothing
    if (cmd == GETALL_ || cmd == SETALL_) {
        uint16_t vals[nsems];
        if (cmd == GETALL_) {
            for (unsigned i = 0; i < nsems; i++)
                vals[i] = set->sems[i].val;
            unlock(&sem_lock);
            return user_write(arg, vals, sizeof(vals)) ? _EFAULT : 0;
        }
        unlock(&sem_lock);
        if (user_read(arg, vals, sizeof(vals)))
            return _EFAULT;
        for (unsigned i = 0; i < nsems; i++)
            if (vals[i] > SEMVMX_)
                return _ERANGE;
        // as it was, unless removed meanwhile
        lock(&sem_lock);
        if ((struct sem_set *) ipc_by_id(&sem_ids, id) != set) {
            res = _EIDRM;
            goto out;
        }
        for (unsigned i = 0; i < nsems; i++) {
            set->sems[i].val = vals[i];
            set->sems[i].pid = current->tgid;
        }
        sem_clear_undos(set, 0, nsems);
        set->ctime = time(NULL);
        notify(&set->changed);
        goto out;
    }

    if (num < 0 || (unsigned) num >= nsems) {
        res = _EINVAL;
        goto out;
    }
    struct sem *sem = &set->sems[num];
    switch (cmd) {
        case GETVAL_:
            res = sem->val;
            break;
        case GETPID_:
            res = sem->pid;
            break;
        case GETNCNT_:
            res = sem_waiting(set, num, false);
            break;
        case GETZCNT_:
            res = sem_waiting(set, num, true);
            break;
        case SETVAL_:
            sem->val = (int32_t) arg;
            sem->pid = current->tgid;
            sem_clear_undos(set, num, 1);
            set->ctime = time(NULL);
            notify(&set->changed);
            break;
    }
out:
    unlock(&sem_lock);
    return res;
}

void sem_exit(struct tgroup *group) {
    lock(&sem_lock);
    struct ipc_object *obj;
    list_for_each_entry(&sem_ids.objects, obj, ids) {
        struct sem_set *set = (struct sem_set *) obj;
        struct sem_undo *undo = sem_find_undo(set, group);
        if (undo == NULL)
            continue;
        // within 0 and SEMVMX, as Linux keeps them
        for (unsigned i = 0; i < set->nsems; i++) {
            if (undo->adj[i] == 0)
                continue;
            struct sem *sem = &set->sems[i];
            sem->val += undo->adj[i];
            if (sem->val < 0)
                sem->val = 0;
            if (sem->val > SEMVMX_)
                sem->val = SEMVMX_;
            sem->pid = current->tgid;
        }
        list_remove(&undo->undos);
        free(undo);
        set->otime = time(NULL);
        notify(&set->changed);
    }
    unlock(&sem_lock);
}
