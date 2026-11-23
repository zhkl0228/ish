// System V shared memory: shmget, shmat, shmdt, shmctl (arm64 guest).
//
// A segment is pages of its own, one struct data that each process attaching
// it maps with P_SHARED, as a MAP_SHARED anonymous mapping is shared across
// fork. The segment holds a reference to it until IPC_RMID; after that the
// last process to detach it (shmdt, munmap, exit, exec) frees it through
// data->release. Until then it can still be attached by id, as on Linux.
//
// A segment's number of attaches is counted from its references, a page each.
//
// Lock order: a mem's lock, then shm_lock (data_unref under the mem lock can
// free a segment). So shmat does not hold shm_lock while it maps.

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "util/list.h"
#include "util/sync.h"

#define IPC_PRIVATE_ 0
#define IPC_CREAT_ 01000
#define IPC_EXCL_ 02000
#define IPC_RMID_ 0
#define IPC_SET_ 1
#define IPC_STAT_ 2
#define IPC_INFO_ 3
#define SHM_STAT_ 13
#define SHM_INFO_ 14
#define SHM_DEST_ 01000
#define SHM_RDONLY_ 010000
#define SHM_RND_ 020000
#define SHM_REMAP_ 040000
#define SHM_EXEC_ 0100000
#define SHMLBA_ PAGE_SIZE

// asm-generic's ipc64_perm and shmid64_ds
struct ipc64_perm_ {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq, pad2;
    uint64_t unused1, unused2;
};
struct shmid64_ds_ {
    struct ipc64_perm_ perm;
    uint64_t segsz;
    int64_t atime, dtime, ctime;
    int32_t cpid, lpid;
    uint64_t nattch;
    uint64_t unused4, unused5;
};
// IPC_INFO's and SHM_INFO's
struct shminfo64_ {
    uint64_t shmmax, shmmin, shmmni, shmseg, shmall;
    uint64_t unused1, unused2, unused3, unused4;
};
struct shm_info_ {
    int32_t used_ids;
    uint64_t shm_tot, shm_rss, shm_swp;
    uint64_t swap_attempts, swap_successes;
};
static_assert(sizeof(struct ipc64_perm_) == 48, "ipc64_perm layout");
static_assert(sizeof(struct shmid64_ds_) == 112, "shmid64_ds layout");
static_assert(sizeof(struct shminfo64_) == 72, "shminfo64 layout");
static_assert(sizeof(struct shm_info_) == 48, "shm_info layout");

// Linux's defaults
#define SHMMAX_ (UINT64_MAX - (1ULL << 24))
#define SHMMNI_ 4096

// An id is a sequence number times this plus an index, the lowest free;
// SHM_STAT takes an index, as ipcs lists them.
#define SHM_SEQ_MULTIPLIER 32768

struct shm_segment {
    struct list segments;
    int32_t id;
    int32_t index;
    int32_t key; // IPC_PRIVATE_ once removed
    struct data *data;
    pages_t pages;
    uint64_t size; // as asked for
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode; // permissions, SHM_DEST_ once removed
    pid_t_ cpid, lpid;
    int64_t atime, dtime, ctime;
    char name[32]; // in /proc/<pid>/maps, as Linux names it
};

static lock_t shm_lock = LOCK_INITIALIZER;
static struct list shm_segments = LIST_INITIALIZER(shm_segments);
static int32_t shm_seq;

static struct shm_segment *shm_by_id(int32_t id) {
    struct shm_segment *seg;
    list_for_each_entry(&shm_segments, seg, segments) {
        if (seg->id == id)
            return seg;
    }
    return NULL;
}

static struct shm_segment *shm_by_index(int32_t index) {
    struct shm_segment *seg;
    list_for_each_entry(&shm_segments, seg, segments) {
        if (seg->index == index)
            return seg;
    }
    return NULL;
}

static int32_t shm_max_index(void) {
    int32_t max = -1;
    struct shm_segment *seg;
    list_for_each_entry(&shm_segments, seg, segments) {
        if (seg->index > max)
            max = seg->index;
    }
    return max;
}

static struct shm_segment *shm_by_key(int32_t key) {
    struct shm_segment *seg;
    list_for_each_entry(&shm_segments, seg, segments) {
        if (seg->key == key)
            return seg;
    }
    return NULL;
}

static struct shm_segment *shm_by_data(struct data *data) {
    struct shm_segment *seg;
    list_for_each_entry(&shm_segments, seg, segments) {
        if (seg->data == data)
            return seg;
    }
    return NULL;
}

static bool in_group(uint32_t gid) {
    if (current->egid == gid)
        return true;
    for (unsigned i = 0; i < current->ngroups; i++)
        if (current->groups[i] == gid)
            return true;
    return false;
}

// Whether the caller has the access asked for in mode's permission bits
// (any of user, group and other), as Linux's ipcperms
static bool shm_permitted(struct shm_segment *seg, uint32_t mode) {
    if (superuser())
        return true;
    uint32_t requested = (mode >> 6) | (mode >> 3) | mode;
    uint32_t granted = seg->mode;
    if (current->euid == seg->cuid || current->euid == seg->uid)
        granted >>= 6;
    else if (in_group(seg->cgid) || in_group(seg->gid))
        granted >>= 3;
    return (requested & ~granted & 07) == 0;
}

// IPC_SET and IPC_RMID: its owner, its creator or root
static bool shm_owner(struct shm_segment *seg) {
    return superuser() || current->euid == seg->uid || current->euid == seg->cuid;
}

static uint64_t shm_nattch(struct shm_segment *seg) {
    uint64_t refs = seg->data->refcount;
    if (!(seg->mode & SHM_DEST_))
        refs--; // the segment's own
    return (refs + seg->pages - 1) / seg->pages;
}

// data->release: the last process attaching a removed segment detached it
static void shm_release(struct data *data) {
    lock(&shm_lock);
    struct shm_segment *seg = shm_by_data(data);
    list_remove(&seg->segments);
    unlock(&shm_lock);
    munmap(data->data, data->size);
    free(data);
    free(seg);
}

int_t sys_shmget(int_t key, addr_t size, int_t flags) {
    STRACE("shmget(%#x, %#llx, %#o)", key, (unsigned long long) size, flags);
    lock(&shm_lock);
    if (key != IPC_PRIVATE_) {
        struct shm_segment *seg = shm_by_key(key);
        int_t res;
        if (seg == NULL && !(flags & IPC_CREAT_))
            res = _ENOENT;
        else if (seg == NULL)
            goto create;
        else if ((flags & IPC_CREAT_) && (flags & IPC_EXCL_))
            res = _EEXIST;
        else if (!shm_permitted(seg, flags))
            res = _EACCES;
        else if (size > seg->size)
            res = _EINVAL;
        else
            res = seg->id;
        unlock(&shm_lock);
        return res;
    }

create:
    if (size == 0) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    int32_t index = 0;
    while (shm_by_index(index) != NULL)
        index++;
    if (index >= SHMMNI_) {
        unlock(&shm_lock);
        return _ENOSPC;
    }
    pages_t pages = PAGE_ROUND_UP(size);
    void *memory = mmap(NULL, pages << PAGE_BITS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        unlock(&shm_lock);
        return _ENOMEM;
    }
    struct shm_segment *seg = calloc(1, sizeof(*seg));
    struct data *data = calloc(1, sizeof(*data));
    *data = (struct data) {
        .data = memory,
        .size = pages << PAGE_BITS,
        .refcount = 1, // the segment's own
        .name = seg->name,
        .release = shm_release,
    };
    shm_seq = (shm_seq + 1) % (INT32_MAX / SHM_SEQ_MULTIPLIER);
    *seg = (struct shm_segment) {
        .id = shm_seq * SHM_SEQ_MULTIPLIER + index,
        .index = index,
        .key = key,
        .data = data,
        .pages = pages,
        .size = size,
        .uid = current->euid,
        .gid = current->egid,
        .cuid = current->euid,
        .cgid = current->egid,
        .mode = flags & 0777,
        .cpid = current->tgid,
        .ctime = time(NULL),
    };
    snprintf(seg->name, sizeof(seg->name), "/SYSV%08x (deleted)", (uint32_t) key);
    list_add_tail(&shm_segments, &seg->segments);
    int_t id = seg->id;
    unlock(&shm_lock);
    return id;
}

addr_t sys_shmat(int_t id, addr_t addr, int_t flags) {
    STRACE("shmat(%d, %#llx, %#o)", id, (unsigned long long) addr, flags);
    lock(&shm_lock);
    struct shm_segment *seg = shm_by_id(id);
    if (seg == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    if (!shm_permitted(seg, flags & SHM_RDONLY_ ? 0444 : 0666)) {
        unlock(&shm_lock);
        return _EACCES;
    }
    // A reference while it is mapped without shm_lock; none to take once
    // the last went and shm_release waits for the lock to drop it.
    struct data *data = seg->data;
    unsigned refs = atomic_load(&data->refcount);
    do {
        if (refs == 0) {
            unlock(&shm_lock);
            return _EINVAL;
        }
    } while (!atomic_compare_exchange_weak(&data->refcount, &refs, refs + 1));
    pages_t pages = seg->pages;
    seg->atime = time(NULL);
    seg->lpid = current->tgid;
    unlock(&shm_lock);

    unsigned prot = P_READ | P_SHARED;
    if (!(flags & SHM_RDONLY_))
        prot |= P_WRITE;
    if (flags & SHM_EXEC_)
        prot |= P_EXEC;
    addr_t res;
    write_wrlock(&current->mem->lock);
    page_t page;
    if (addr != 0) {
        // fixed even if SHM_RND rounds it down to 0, as on Linux
        if (flags & SHM_RND_)
            addr &= ~(addr_t) (SHMLBA_ - 1);
        page = PAGE(addr);
        if (PGOFFSET(addr) != 0 || (!(flags & SHM_REMAP_) && !pt_is_hole(current->mem, page, pages))) {
            res = _EINVAL;
            goto out;
        }
    } else {
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE) {
            res = _ENOMEM;
            goto out;
        }
    }
    pt_map_data(current->mem, page, pages, data, 0, prot);
    res = page << PAGE_BITS;
out:
    write_wrunlock(&current->mem->lock);
    data_unref(data);
    return res;
}

int_t sys_shmdt(addr_t addr) {
    STRACE("shmdt(%#llx)", (unsigned long long) addr);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    write_wrlock(&current->mem->lock);
    page_t start = PAGE(addr);
    struct pt_entry *pt = mem_pt(current->mem, start);
    if (pt == NULL || pt->data->release != shm_release || pt->offset != 0) {
        write_wrunlock(&current->mem->lock);
        return _EINVAL;
    }
    // held until the segment's times are set, below
    struct data *data = pt->data;
    data->refcount++;
    // The pages mapped from here as shmat mapped them
    page_t end = start;
    pages_t pages = data->size >> PAGE_BITS;
    while (end < start + pages && (pt = mem_pt(current->mem, end)) != NULL && pt->data == data &&
            pt->offset == (end - start) << PAGE_BITS)
        end++;
    pt_unmap_always(current->mem, start, end - start);
    write_wrunlock(&current->mem->lock);

    lock(&shm_lock);
    struct shm_segment *seg = shm_by_data(data);
    seg->dtime = time(NULL);
    seg->lpid = current->tgid;
    unlock(&shm_lock);
    data_unref(data);
    return 0;
}

// IPC_INFO and SHM_INFO (ipcs): the highest index in use, 0 if none
static int_t shm_info(int_t cmd, addr_t buf_addr) {
    lock(&shm_lock);
    int32_t max = shm_max_index();
    int32_t used = 0;
    uint64_t pages = 0;
    struct shm_segment *seg;
    list_for_each_entry(&shm_segments, seg, segments) {
        used++;
        pages += seg->pages;
    }
    unlock(&shm_lock);
    if (cmd == IPC_INFO_) {
        struct shminfo64_ info = {
            .shmmax = SHMMAX_,
            .shmmin = 1,
            .shmmni = SHMMNI_,
            .shmseg = SHMMNI_,
            .shmall = SHMMAX_,
        };
        if (user_put(buf_addr, info))
            return _EFAULT;
    } else {
        struct shm_info_ info = {
            .used_ids = used,
            .shm_tot = pages,
            .shm_rss = pages,
        };
        if (user_put(buf_addr, info))
            return _EFAULT;
    }
    return max < 0 ? 0 : max;
}

int_t sys_shmctl(int_t id, int_t cmd, addr_t buf_addr) {
    STRACE("shmctl(%d, %d, %#llx)", id, cmd, (unsigned long long) buf_addr);
    struct shmid64_ds_ ds;
    switch (cmd) {
        case IPC_INFO_:
        case SHM_INFO_:
            return shm_info(cmd, buf_addr);
        case IPC_STAT_:
        case SHM_STAT_:
        case IPC_RMID_:
            break;
        case IPC_SET_:
            if (user_get(buf_addr, ds))
                return _EFAULT;
            break;
        default:
            FIXME("shmctl(%d, %d, %#llx): command not implemented", id, cmd, (unsigned long long) buf_addr);
            return _EINVAL;
    }

    // SHM_STAT takes an index and gives the id, for ipcs
    lock(&shm_lock);
    struct shm_segment *seg = cmd == SHM_STAT_ ? shm_by_index(id) : shm_by_id(id);
    if (seg == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    if (cmd == IPC_STAT_ || cmd == SHM_STAT_) {
        if (!shm_permitted(seg, 0444)) {
            unlock(&shm_lock);
            return _EACCES;
        }
        ds = (struct shmid64_ds_) {
            .perm = {
                .key = seg->key,
                .uid = seg->uid,
                .gid = seg->gid,
                .cuid = seg->cuid,
                .cgid = seg->cgid,
                .mode = seg->mode,
            },
            .segsz = seg->size,
            .atime = seg->atime,
            .dtime = seg->dtime,
            .ctime = seg->ctime,
            .cpid = seg->cpid,
            .lpid = seg->lpid,
            .nattch = shm_nattch(seg),
        };
        int_t res = cmd == SHM_STAT_ ? seg->id : 0;
        unlock(&shm_lock);
        if (user_put(buf_addr, ds))
            return _EFAULT;
        return res;
    }

    if (!shm_owner(seg)) {
        unlock(&shm_lock);
        return _EPERM;
    }
    seg->ctime = time(NULL);
    if (cmd == IPC_SET_) {
        seg->uid = ds.perm.uid;
        seg->gid = ds.perm.gid;
        seg->mode = (seg->mode & ~0777) | (ds.perm.mode & 0777);
        unlock(&shm_lock);
        return 0;
    }

    // IPC_RMID: its key is free from now on, its pages once nobody has them
    // mapped
    if (seg->mode & SHM_DEST_) {
        unlock(&shm_lock);
        return 0;
    }
    seg->mode |= SHM_DEST_;
    seg->key = IPC_PRIVATE_;
    struct data *data = seg->data;
    unlock(&shm_lock);
    data_unref(data);
    return 0;
}
