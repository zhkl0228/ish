#ifndef KERNEL_SYSVIPC_H
#define KERNEL_SYSVIPC_H

// What System V IPC's three kinds share (kernel/sysvipc.c): shared memory
// (kernel/shm.c), message queues (kernel/msg.c) and semaphores
// (kernel/sem.c), arm64 guest. An object has an id, a key and permissions, as
// Linux's kern_ipc_perm; each kind has ids of its own, under its own lock,
// which the functions here expect held.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "misc.h"
#include "util/list.h"

#define IPC_PRIVATE_ 0
#define IPC_CREAT_ 01000
#define IPC_EXCL_ 02000
#define IPC_NOWAIT_ 04000
#define IPC_RMID_ 0
#define IPC_SET_ 1
#define IPC_STAT_ 2
#define IPC_INFO_ 3

// asm-generic's ipc64_perm
struct ipc64_perm_ {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq, pad2;
    uint64_t unused1, unused2;
};
static_assert(sizeof(struct ipc64_perm_) == 48, "ipc64_perm layout");

struct ipc_object {
    struct list ids;
    int32_t id;
    int32_t index; // what *_STAT takes, as ipcs lists them
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode; // permissions, and a kind's own bits above them
};

struct ipc_ids {
    struct list objects;
    int32_t seq;
    int32_t max; // the kind's MNI
};
#define IPC_IDS_INITIALIZER(name, mni) {.objects = LIST_INITIALIZER((name).objects), .max = (mni)}

// *get's key and flags: 0 with *obj the object key names, which the caller
// may use (IPC_CREAT without IPC_EXCL, and the access flags asks for); 0 with
// *obj NULL for the caller to make one; or the error
int_t ipc_get(struct ipc_ids *ids, int32_t key, int_t flags, struct ipc_object **obj);
// Gives obj the lowest free index and an id, current for its owner and
// creator, and flags' permissions: 0, or _ENOSPC with the kind's MNI in use
int_t ipc_add(struct ipc_ids *ids, struct ipc_object *obj, int32_t key, int_t flags);
// Off its kind's ids: its id names nothing from now on
void ipc_remove(struct ipc_object *obj);

struct ipc_object *ipc_by_id(struct ipc_ids *ids, int32_t id);
struct ipc_object *ipc_by_index(struct ipc_ids *ids, int32_t index);
// For IPC_INFO and *_INFO: the highest index in use, -1 if none
int32_t ipc_max_index(struct ipc_ids *ids);
int32_t ipc_count(struct ipc_ids *ids);

// Whether the caller has the access mode's permission bits ask for (any of
// user, group and other), as Linux's ipcperms
bool ipc_permitted(struct ipc_object *obj, uint32_t mode);
// IPC_SET and IPC_RMID: its owner, its creator or root
bool ipc_owner(struct ipc_object *obj);
// For IPC_STAT and *_STAT
struct ipc64_perm_ ipc_perm(struct ipc_object *obj);
// IPC_SET's owner and permissions: 0, or _EINVAL for an owner of -1
int_t ipc_set_perm(struct ipc_object *obj, struct ipc64_perm_ *perm);

#endif
