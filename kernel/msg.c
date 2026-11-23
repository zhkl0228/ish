// System V message queues: msgget, msgsnd, msgrcv, msgctl (arm64 guest).
//
// A queue is a list of messages under msg_lock. A sender waits while it is
// full, a receiver while it has nothing for it, both on the queue's condition:
// every change wakes them all to look again. IPC_RMID takes the queue off its
// id at once; whoever was waiting on it gets EIDRM, and the last of them frees
// it.
//
// msgsnd and msgrcv start again after a signal no handler ran for (stopped
// and continued), and fail with EINTR after one a handler ran for, whatever
// its SA_RESTART, as Linux's ERESTARTNOHAND (kernel/calls.c).

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/sysvipc.h"
#include "util/list.h"
#include "util/sync.h"

#define MSG_STAT_ 11
#define MSG_INFO_ 12
#define MSG_STAT_ANY_ 13
#define MSG_NOERROR_ 010000
#define MSG_EXCEPT_ 020000
#define MSG_COPY_ 040000

// asm-generic's msqid64_ds, and IPC_INFO's and MSG_INFO's msginfo
struct msqid64_ds_ {
    struct ipc64_perm_ perm;
    int64_t stime, rtime, ctime;
    uint64_t cbytes, qnum, qbytes;
    int32_t lspid, lrpid;
    uint64_t unused4, unused5;
};
struct msginfo_ {
    int32_t msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz, msgtql;
    uint16_t msgseg;
};
static_assert(sizeof(struct msqid64_ds_) == 120, "msqid64_ds layout");
static_assert(sizeof(struct msginfo_) == 32, "msginfo layout");

// Linux's defaults
#define MSGMAX_ 8192
#define MSGMNB_ 16384
#define MSGMNI_ 32000
#define MSGPOOL_ (MSGMNI_ * MSGMNB_ / 1024)
#define MSGMAP_ MSGMNB_
#define MSGTQL_ MSGMNB_
#define MSGSSZ_ 16
#define MSGSEG_ 0xffff

struct message {
    struct list queue;
    int64_t type;
    uint64_t size;
    char text[];
};

struct msg_queue {
    struct ipc_object ipc;
    struct list messages;
    uint64_t cbytes, qnum, qbytes;
    pid_t_ lspid, lrpid;
    int64_t stime, rtime, ctime;
    cond_t changed;
    unsigned waiting;
    bool removed;
};

static lock_t msg_lock = LOCK_INITIALIZER;
static struct ipc_ids msg_ids = IPC_IDS_INITIALIZER(msg_ids, MSGMNI_);
// in all queues, for MSG_INFO
static uint64_t msg_count, msg_bytes;

static void msg_take(struct msg_queue *queue, struct message *msg) {
    list_remove(&msg->queue);
    queue->qnum--;
    queue->cbytes -= msg->size;
    msg_count--;
    msg_bytes -= msg->size;
}

// Waits for a change to queue: 0, _EINTR, or _EIDRM once it is removed (and
// then freed, by the last one waiting)
static int_t msg_wait(struct msg_queue *queue) {
    queue->waiting++;
    int_t err = wait_for(&queue->changed, &msg_lock, NULL);
    queue->waiting--;
    if (queue->removed) {
        if (queue->waiting == 0) {
            cond_destroy(&queue->changed);
            free(queue);
        }
        return _EIDRM;
    }
    return err;
}

int_t sys_msgget(int_t key, int_t flags) {
    STRACE("msgget(%#x, %#o)", key, flags);
    lock(&msg_lock);
    struct ipc_object *obj;
    int_t res = ipc_get(&msg_ids, key, flags, &obj);
    if (res < 0 || obj != NULL) {
        if (obj != NULL)
            res = obj->id;
        unlock(&msg_lock);
        return res;
    }
    struct msg_queue *queue = calloc(1, sizeof(*queue));
    list_init(&queue->messages);
    cond_init(&queue->changed);
    queue->qbytes = MSGMNB_;
    queue->ctime = time(NULL);
    res = ipc_add(&msg_ids, &queue->ipc, key, flags);
    if (res < 0) {
        cond_destroy(&queue->changed);
        free(queue);
    } else {
        res = queue->ipc.id;
    }
    unlock(&msg_lock);
    return res;
}

int_t sys_msgsnd(int_t id, addr_t msgp, addr_t size, int_t flags) {
    STRACE("msgsnd(%d, %#llx, %llu, %#o)", id, (unsigned long long) msgp, (unsigned long long) size, flags);
    int64_t type;
    if (user_get(msgp, type))
        return _EFAULT;
    if (size > MSGMAX_ || id < 0 || type < 1)
        return _EINVAL;
    struct message *msg = malloc(sizeof(*msg) + size);
    msg->type = type;
    msg->size = size;
    if (user_read(msgp + sizeof(type), msg->text, size)) {
        free(msg);
        return _EFAULT;
    }

    lock(&msg_lock);
    struct msg_queue *queue = (struct msg_queue *) ipc_by_id(&msg_ids, id);
    int_t res = _EINVAL;
    while (queue != NULL) {
        if (!ipc_permitted(&queue->ipc, 0222)) {
            res = _EACCES;
            break;
        }
        if (queue->cbytes + size <= queue->qbytes && queue->qnum + 1 <= queue->qbytes) {
            list_add_tail(&queue->messages, &msg->queue);
            msg = NULL;
            queue->cbytes += size;
            queue->qnum++;
            msg_count++;
            msg_bytes += size;
            queue->lspid = current->tgid;
            queue->stime = time(NULL);
            notify(&queue->changed);
            res = 0;
            break;
        }
        if (flags & IPC_NOWAIT_) {
            res = _EAGAIN;
            break;
        }
        res = msg_wait(queue);
        if (res < 0)
            break;
    }
    unlock(&msg_lock);
    free(msg);
    return res;
}

// The message msgrcv takes: the first; for a type, the first of it (or with
// MSG_EXCEPT, of another); for a negative type, the first of the lowest type
// up to -type
static struct message *msg_find(struct msg_queue *queue, int64_t type, bool except) {
    int64_t up_to = type == INT64_MIN ? INT64_MAX : -type;
    struct message *msg, *found = NULL;
    list_for_each_entry(&queue->messages, msg, queue) {
        if (type == 0)
            return msg;
        if (type > 0 && (msg->type == type) != except)
            return msg;
        if (type < 0 && msg->type <= up_to && (found == NULL || msg->type < found->type))
            found = msg;
    }
    return found;
}

int_t sys_msgrcv(int_t id, addr_t msgp, addr_t size, int64_t type, int_t flags) {
    STRACE("msgrcv(%d, %#llx, %llu, %lld, %#o)", id, (unsigned long long) msgp, (unsigned long long) size, (long long) type, flags);
    if (id < 0 || (int64_t) size < 0)
        return _EINVAL;
    if (flags & MSG_COPY_) {
        // as Linux without CONFIG_CHECKPOINT_RESTORE: it is for CRIU
        FIXME("msgrcv(%d, %#llx, %llu, %lld, %#o): MSG_COPY not implemented", id, (unsigned long long) msgp, (unsigned long long) size, (long long) type, flags);
        return _ENOSYS;
    }

    lock(&msg_lock);
    struct msg_queue *queue = (struct msg_queue *) ipc_by_id(&msg_ids, id);
    int_t res = _EINVAL;
    struct message *msg = NULL;
    while (queue != NULL) {
        if (!ipc_permitted(&queue->ipc, 0444)) {
            res = _EACCES;
            break;
        }
        msg = msg_find(queue, type, flags & MSG_EXCEPT_);
        if (msg != NULL) {
            if (size < msg->size && !(flags & MSG_NOERROR_)) {
                msg = NULL;
                res = _E2BIG;
                break;
            }
            msg_take(queue, msg);
            queue->lrpid = current->tgid;
            queue->rtime = time(NULL);
            notify(&queue->changed);
            break;
        }
        if (flags & IPC_NOWAIT_) {
            res = _ENOMSG;
            break;
        }
        res = msg_wait(queue);
        if (res < 0)
            break;
    }
    unlock(&msg_lock);
    if (msg == NULL)
        return res;

    // Taken off the queue already, as on Linux: lost if it can't be copied
    uint64_t copied = size < msg->size ? size : msg->size;
    res = user_put(msgp, msg->type) || user_write(msgp + sizeof(msg->type), msg->text, copied) ? _EFAULT : (int_t) copied;
    free(msg);
    return res;
}

// IPC_INFO and MSG_INFO (ipcs): the highest index in use, 0 if none
static int_t msg_info(int_t cmd, addr_t buf_addr) {
    struct msginfo_ info = {
        .msgmax = MSGMAX_,
        .msgmnb = MSGMNB_,
        .msgmni = MSGMNI_,
        .msgssz = MSGSSZ_,
        .msgseg = MSGSEG_,
        .msgpool = MSGPOOL_,
        .msgmap = MSGMAP_,
        .msgtql = MSGTQL_,
    };
    lock(&msg_lock);
    if (cmd == MSG_INFO_) {
        // queues, messages and bytes in use
        info.msgpool = ipc_count(&msg_ids);
        info.msgmap = msg_count < INT32_MAX ? (int32_t) msg_count : INT32_MAX;
        info.msgtql = msg_bytes < INT32_MAX ? (int32_t) msg_bytes : INT32_MAX;
    }
    int32_t max = ipc_max_index(&msg_ids);
    unlock(&msg_lock);
    if (user_put(buf_addr, info))
        return _EFAULT;
    return max < 0 ? 0 : max;
}

int_t sys_msgctl(int_t id, int_t cmd, addr_t buf_addr) {
    STRACE("msgctl(%d, %d, %#llx)", id, cmd, (unsigned long long) buf_addr);
    struct msqid64_ds_ ds;
    switch (cmd) {
        case IPC_INFO_:
        case MSG_INFO_:
            return msg_info(cmd, buf_addr);
        case IPC_STAT_:
        case MSG_STAT_:
        case MSG_STAT_ANY_:
        case IPC_RMID_:
            break;
        case IPC_SET_:
            if (user_get(buf_addr, ds))
                return _EFAULT;
            break;
        default:
            return _EINVAL;
    }

    // MSG_STAT takes an index and gives the id, for ipcs; MSG_STAT_ANY too,
    // whatever the permissions
    lock(&msg_lock);
    struct msg_queue *queue = (struct msg_queue *) (cmd == MSG_STAT_ || cmd == MSG_STAT_ANY_ ?
            ipc_by_index(&msg_ids, id) : ipc_by_id(&msg_ids, id));
    if (queue == NULL) {
        unlock(&msg_lock);
        return _EINVAL;
    }
    if (cmd == IPC_STAT_ || cmd == MSG_STAT_ || cmd == MSG_STAT_ANY_) {
        if (cmd != MSG_STAT_ANY_ && !ipc_permitted(&queue->ipc, 0444)) {
            unlock(&msg_lock);
            return _EACCES;
        }
        ds = (struct msqid64_ds_) {
            .perm = ipc_perm(&queue->ipc),
            .stime = queue->stime,
            .rtime = queue->rtime,
            .ctime = queue->ctime,
            .cbytes = queue->cbytes,
            .qnum = queue->qnum,
            .qbytes = queue->qbytes,
            .lspid = queue->lspid,
            .lrpid = queue->lrpid,
        };
        int_t res = cmd == IPC_STAT_ ? 0 : queue->ipc.id;
        unlock(&msg_lock);
        if (user_put(buf_addr, ds))
            return _EFAULT;
        return res;
    }

    if (!ipc_owner(&queue->ipc)) {
        unlock(&msg_lock);
        return _EPERM;
    }
    if (cmd == IPC_SET_) {
        // more room than MSGMNB is root's to give
        int_t res = ds.qbytes > MSGMNB_ && !superuser() ? _EPERM : ipc_set_perm(&queue->ipc, &ds.perm);
        if (res == 0) {
            queue->qbytes = ds.qbytes;
            queue->ctime = time(NULL);
            // for senders the room, receivers the permissions
            notify(&queue->changed);
        }
        unlock(&msg_lock);
        return res;
    }

    // IPC_RMID: its messages go now, it once nobody waits on it
    ipc_remove(&queue->ipc);
    struct message *msg, *tmp;
    list_for_each_entry_safe(&queue->messages, msg, tmp, queue) {
        msg_take(queue, msg);
        free(msg);
    }
    queue->removed = true;
    if (queue->waiting == 0) {
        cond_destroy(&queue->changed);
        free(queue);
    } else {
        notify(&queue->changed);
    }
    unlock(&msg_lock);
    return 0;
}
