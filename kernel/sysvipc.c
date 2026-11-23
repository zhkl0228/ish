// What System V IPC's kinds share: see kernel/sysvipc.h.

#include "kernel/calls.h"
#include "kernel/sysvipc.h"

// An id is a sequence number times this plus an index, the lowest free
#define IPC_SEQ_MULTIPLIER 32768

static struct ipc_object *ipc_by_key(struct ipc_ids *ids, int32_t key) {
    struct ipc_object *obj;
    list_for_each_entry(&ids->objects, obj, ids) {
        if (obj->key == key)
            return obj;
    }
    return NULL;
}

struct ipc_object *ipc_by_id(struct ipc_ids *ids, int32_t id) {
    struct ipc_object *obj;
    list_for_each_entry(&ids->objects, obj, ids) {
        if (obj->id == id)
            return obj;
    }
    return NULL;
}

struct ipc_object *ipc_by_index(struct ipc_ids *ids, int32_t index) {
    struct ipc_object *obj;
    list_for_each_entry(&ids->objects, obj, ids) {
        if (obj->index == index)
            return obj;
    }
    return NULL;
}

int32_t ipc_max_index(struct ipc_ids *ids) {
    int32_t max = -1;
    struct ipc_object *obj;
    list_for_each_entry(&ids->objects, obj, ids) {
        if (obj->index > max)
            max = obj->index;
    }
    return max;
}

int32_t ipc_count(struct ipc_ids *ids) {
    int32_t count = 0;
    struct ipc_object *obj;
    list_for_each_entry(&ids->objects, obj, ids)
        count++;
    return count;
}

int_t ipc_get(struct ipc_ids *ids, int32_t key, int_t flags, struct ipc_object **obj) {
    *obj = NULL;
    if (key == IPC_PRIVATE_)
        return 0;
    struct ipc_object *found = ipc_by_key(ids, key);
    if (found == NULL)
        return flags & IPC_CREAT_ ? 0 : _ENOENT;
    if ((flags & IPC_CREAT_) && (flags & IPC_EXCL_))
        return _EEXIST;
    if (!ipc_permitted(found, flags))
        return _EACCES;
    *obj = found;
    return 0;
}

int_t ipc_add(struct ipc_ids *ids, struct ipc_object *obj, int32_t key, int_t flags) {
    int32_t index = 0;
    while (ipc_by_index(ids, index) != NULL)
        index++;
    if (index >= ids->max)
        return _ENOSPC;
    ids->seq = (ids->seq + 1) % (INT32_MAX / IPC_SEQ_MULTIPLIER);
    obj->id = ids->seq * IPC_SEQ_MULTIPLIER + index;
    obj->index = index;
    obj->key = key;
    obj->uid = obj->cuid = current->euid;
    obj->gid = obj->cgid = current->egid;
    obj->mode = flags & 0777;
    list_add_tail(&ids->objects, &obj->ids);
    return 0;
}

void ipc_remove(struct ipc_object *obj) {
    list_remove(&obj->ids);
}

static bool in_group(uint32_t gid) {
    if (current->egid == gid)
        return true;
    for (unsigned i = 0; i < current->ngroups; i++)
        if (current->groups[i] == gid)
            return true;
    return false;
}

bool ipc_permitted(struct ipc_object *obj, uint32_t mode) {
    if (superuser())
        return true;
    uint32_t requested = (mode >> 6) | (mode >> 3) | mode;
    uint32_t granted = obj->mode;
    if (current->euid == obj->cuid || current->euid == obj->uid)
        granted >>= 6;
    else if (in_group(obj->cgid) || in_group(obj->gid))
        granted >>= 3;
    return (requested & ~granted & 07) == 0;
}

bool ipc_owner(struct ipc_object *obj) {
    return superuser() || current->euid == obj->uid || current->euid == obj->cuid;
}

struct ipc64_perm_ ipc_perm(struct ipc_object *obj) {
    return (struct ipc64_perm_) {
        .key = obj->key,
        .uid = obj->uid,
        .gid = obj->gid,
        .cuid = obj->cuid,
        .cgid = obj->cgid,
        .mode = obj->mode,
        .seq = obj->id / IPC_SEQ_MULTIPLIER,
    };
}

int_t ipc_set_perm(struct ipc_object *obj, struct ipc64_perm_ *perm) {
    if (perm->uid == (uint32_t) -1 || perm->gid == (uint32_t) -1)
        return _EINVAL;
    obj->uid = perm->uid;
    obj->gid = perm->gid;
    obj->mode = (obj->mode & ~0777) | (perm->mode & 0777);
    return 0;
}
