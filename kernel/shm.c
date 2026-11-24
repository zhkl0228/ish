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
// A lock (SHM_LOCK) is its mode bit only: nothing is ever paged out here, and
// as mlock, it is not counted against RLIMIT_MEMLOCK.
//
// Lock order: a mem's lock, then shm_lock (data_unref under the mem lock can
// free a segment). So shmat does not hold shm_lock while it maps.

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/resource.h"
#include "kernel/sysvipc.h"
#include "util/list.h"
#include "util/sync.h"

#define SHM_LOCK_ 11
#define SHM_UNLOCK_ 12
#define SHM_STAT_ 13
#define SHM_INFO_ 14
#define SHM_STAT_ANY_ 15
#define SHM_DEST_ 01000
#define SHM_LOCKED_ 02000
#define SHM_RDONLY_ 010000
#define SHM_RND_ 020000
#define SHM_REMAP_ 040000
#define SHM_EXEC_ 0100000
#define SHMLBA_ PAGE_SIZE

// asm-generic's shmid64_ds
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
static_assert(sizeof(struct shmid64_ds_) == 112, "shmid64_ds layout");
static_assert(sizeof(struct shminfo64_) == 72, "shminfo64 layout");
static_assert(sizeof(struct shm_info_) == 48, "shm_info layout");

// Linux's defaults
#define SHMMAX_ (UINT64_MAX - (1ULL << 24))
#define SHMMNI_ 4096

// A removed segment keeps its id, for those who have it attached, with key
// IPC_PRIVATE_ and SHM_DEST_ in its mode
struct shm_segment {
    struct ipc_object ipc;
    struct data *data;
    pages_t pages;
    uint64_t size; // as asked for
    pid_t_ cpid, lpid;
    int64_t atime, dtime, ctime;
    char name[32]; // in /proc/<pid>/maps, as Linux names it
};

static lock_t shm_lock = LOCK_INITIALIZER;
static struct ipc_ids shm_ids = IPC_IDS_INITIALIZER(shm_ids, SHMMNI_);

static struct shm_segment *shm_by_data(struct data *data) {
    struct ipc_object *obj;
    list_for_each_entry(&shm_ids.objects, obj, ids) {
        struct shm_segment *seg = (struct shm_segment *) obj;
        if (seg->data == data)
            return seg;
    }
    return NULL;
}

static uint64_t shm_nattch(struct shm_segment *seg) {
    uint64_t refs = seg->data->refcount;
    if (!(seg->ipc.mode & SHM_DEST_))
        refs--; // the segment's own
    return (refs + seg->pages - 1) / seg->pages;
}

// data->release: the last process attaching a removed segment detached it
static void shm_release(struct data *data) {
    lock(&shm_lock);
    struct shm_segment *seg = shm_by_data(data);
    ipc_remove(&seg->ipc);
    unlock(&shm_lock);
    munmap(data->data, data->size);
    free(data);
    free(seg);
}

int_t sys_shmget(int_t key, addr_t size, int_t flags) {
    STRACE("shmget(%#x, %#llx, %#o)", key, (unsigned long long) size, flags);
    lock(&shm_lock);
    struct ipc_object *obj;
    int_t res = ipc_get(&shm_ids, key, flags, &obj);
    if (res < 0 || obj != NULL) {
        if (obj != NULL)
            res = size > ((struct shm_segment *) obj)->size ? _EINVAL : obj->id;
        unlock(&shm_lock);
        return res;
    }

    if (size == 0) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    pages_t pages = PAGE_ROUND_UP(size);
    void *memory = mmap(NULL, pages << PAGE_BITS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        unlock(&shm_lock);
        return _ENOMEM;
    }
    anon_mmap_count(1, pages);
    struct shm_segment *seg = calloc(1, sizeof(*seg));
    struct data *data = calloc(1, sizeof(*data));
    *data = (struct data) {
        .data = memory,
        .size = pages << PAGE_BITS,
        .refcount = 1, // the segment's own
        .name = seg->name,
        .release = shm_release,
    };
    *seg = (struct shm_segment) {
        .data = data,
        .pages = pages,
        .size = size,
        .cpid = current->tgid,
        .ctime = time(NULL),
    };
    res = ipc_add(&shm_ids, &seg->ipc, key, flags);
    if (res < 0) {
        unlock(&shm_lock);
        munmap(memory, pages << PAGE_BITS);
        free(data);
        free(seg);
        return res;
    }
    snprintf(seg->name, sizeof(seg->name), "/SYSV%08x (deleted)", (uint32_t) key);
    int_t id = seg->ipc.id;
    unlock(&shm_lock);
    return id;
}

addr_t sys_shmat(int_t id, addr_t addr, int_t flags) {
    STRACE("shmat(%d, %#llx, %#o)", id, (unsigned long long) addr, flags);
    lock(&shm_lock);
    struct shm_segment *seg = (struct shm_segment *) ipc_by_id(&shm_ids, id);
    if (seg == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    if (!ipc_permitted(&seg->ipc, flags & SHM_RDONLY_ ? 0444 : 0666)) {
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
    int32_t max = ipc_max_index(&shm_ids);
    int32_t used = 0;
    uint64_t pages = 0;
    struct ipc_object *obj;
    list_for_each_entry(&shm_ids.objects, obj, ids) {
        used++;
        pages += ((struct shm_segment *) obj)->pages;
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
        case SHM_STAT_ANY_:
        case IPC_RMID_:
        case SHM_LOCK_:
        case SHM_UNLOCK_:
            break;
        case IPC_SET_:
            if (user_get(buf_addr, ds))
                return _EFAULT;
            break;
        default:
            return _EINVAL;
    }

    // SHM_STAT takes an index and gives the id, for ipcs; SHM_STAT_ANY too,
    // whatever the permissions
    bool stat = cmd == IPC_STAT_ || cmd == SHM_STAT_ || cmd == SHM_STAT_ANY_;
    lock(&shm_lock);
    struct shm_segment *seg = (struct shm_segment *) (cmd == SHM_STAT_ || cmd == SHM_STAT_ANY_ ?
            ipc_by_index(&shm_ids, id) : ipc_by_id(&shm_ids, id));
    if (seg == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }
    if (stat) {
        if (cmd != SHM_STAT_ANY_ && !ipc_permitted(&seg->ipc, 0444)) {
            unlock(&shm_lock);
            return _EACCES;
        }
        ds = (struct shmid64_ds_) {
            .perm = ipc_perm(&seg->ipc),
            .segsz = seg->size,
            .atime = seg->atime,
            .dtime = seg->dtime,
            .ctime = seg->ctime,
            .cpid = seg->cpid,
            .lpid = seg->lpid,
            .nattch = shm_nattch(seg),
        };
        int_t res = cmd == IPC_STAT_ ? 0 : seg->ipc.id;
        unlock(&shm_lock);
        if (user_put(buf_addr, ds))
            return _EFAULT;
        return res;
    }

    if (cmd == SHM_LOCK_ || cmd == SHM_UNLOCK_) {
        // its owner, its creator or root; to lock, with RLIMIT_MEMLOCK not 0
        if (!superuser() && ((current->euid != seg->ipc.uid && current->euid != seg->ipc.cuid) ||
                    (cmd == SHM_LOCK_ && rlimit(RLIMIT_MEMLOCK_) == 0))) {
            unlock(&shm_lock);
            return _EPERM;
        }
        if (cmd == SHM_LOCK_)
            seg->ipc.mode |= SHM_LOCKED_;
        else
            seg->ipc.mode &= ~SHM_LOCKED_;
        unlock(&shm_lock);
        return 0;
    }

    if (!ipc_owner(&seg->ipc)) {
        unlock(&shm_lock);
        return _EPERM;
    }
    if (cmd == IPC_SET_) {
        int_t res = ipc_set_perm(&seg->ipc, &ds.perm);
        if (res == 0)
            seg->ctime = time(NULL);
        unlock(&shm_lock);
        return res;
    }

    // IPC_RMID: its key is free from now on, its pages once nobody has them
    // mapped
    if (seg->ipc.mode & SHM_DEST_) {
        unlock(&shm_lock);
        return 0;
    }
    seg->ipc.mode |= SHM_DEST_;
    seg->ipc.key = IPC_PRIVATE_;
    struct data *data = seg->data;
    unlock(&shm_lock);
    data_unref(data);
    return 0;
}
