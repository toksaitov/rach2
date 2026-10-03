/*
 * sysv.c: System V interprocess communication, as sysv.h and ipc/Readme.md
 * describe it.
 *
 * Each kind has a table of SYSV_SLOTS places. An object's id is its place
 * plus its sequence number times 32768, as Linux builds it, so an id names
 * one object and not a later one in the same place. Every object begins with
 * its permissions, `struct sysv_perm`, which the lookup, the permission
 * check and the control calls share. A segment's memory is a file on the
 * tmpfs of memfd_create, which shmat maps shared; a set and a queue are in
 * the heap. A caller that sleeps on a set or a queue holds a reference to
 * it, so removing it wakes the sleeper with EIDRM rather than freeing what
 * it waits on.
 */

#include "../arch/uabi.h"
#include "sysv.h"
#include "mqueue.h"

#include "../irq.h"
#include "../proc/process.h"
#include "../proc/tgroup.h"
#include "../proc/ns.h"
#include "../proc/wait_queue.h"
#include "../mm/mm.h"
#include "../fs/fd.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../fs/tmpfs.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../syscall_internal.h"

/* The flags of every get and control call, and the bit the C library adds
 * to a command to ask for the 64-bit layouts, which are the only ones. */
#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3
#define IPC_64      0x100

#define SYSV_SLOTS 64
#define SYSV_SEQ   32768

/* The limits, Linux's defaults but for the number of objects. SHMMAX is
 * Linux's ULONG_MAX less 16 MiB on each width: 0xFEFFFFFF on i686, and on
 * x86-64 more than a tmpfs file holds, so there the file's own limit,
 * SHMEM_MAX_SIZE, refuses a larger segment with ENOMEM. */
#define SHMMAX (~0UL - (1UL << 24))
#define SHMMIN 1u
#define SEMMSL 32000u
#define SEMOPM 500u
#define SEMVMX 32767
#define SEMAEM 32767
#define MSGMAX 8192u
#define MSGMNB 16384u

/* The permissions every object begins with. `mode` has the nine bits and,
 * for a segment, SHM_DEST once it is removed while attached. `ns` is the
 * IPC namespace it was made in (proc/ns.h), which alone sees it, and
 * `ipc_gone` for a segment its namespace's end left attached. */
struct sysv_perm {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint32_t seq;
    int place;
    uint32_t refs;
    int removed;
    const struct ipcns *ns;
};

/* The namespace of the segments an IPC namespace's end left attached, which
 * no process is in. */
static struct ipcns ipc_gone;

static struct sysv_perm *tables[3][SYSV_SLOTS];
static uint16_t seqs[3];

/** The current time in seconds since the epoch. */
static int64_t sysv_now(void)
{
    int64_t sec, nsec;

    get_current_time(&sec, &nsec);
    return sec;
}

/** Whether the caller's effective or supplementary groups have `gid`. */
static int sysv_in_group(uint32_t gid)
{
    uint32_t i;

    if (gid == current_proc->egid) {
        return 1;
    }
    for (i = 0; i < current_proc->ngroups; i++) {
        if (current_proc->groups[i] == gid) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether the caller may use `p` as the low nine bits of `flag` ask, Linux's
 * ipcperms: the owner's or the creator's bits for either user, else the
 * group's for either group, else the others'; a privileged caller may
 * always.
 */
static int sysv_may(const struct sysv_perm *p, uint32_t flag)
{
    uint32_t want = (flag >> 6 | flag >> 3 | flag) & 7u;
    uint32_t bits = p->mode;

    if (current_proc->euid == p->uid || current_proc->euid == p->cuid) {
        bits >>= 6;
    } else if (sysv_in_group(p->gid) || sysv_in_group(p->cgid)) {
        bits >>= 3;
    }
    return (want & ~bits & 7u) == 0 || proc_privileged(current_proc);
}

/** Whether the caller owns or made `p`, or is privileged: who may set and
 * remove it. */
static int sysv_owner(const struct sysv_perm *p)
{
    return current_proc->euid == p->uid || current_proc->euid == p->cuid ||
           proc_privileged(current_proc);
}

/** Whether `p` is in the caller's IPC namespace. */
static int sysv_mine(const struct sysv_perm *p)
{
    return p->ns == proc_ipcns(current_proc);
}

/** The id of `p`. */
static int32_t sysv_id(const struct sysv_perm *p)
{
    return (int32_t)(p->seq * SYSV_SEQ + (uint32_t)p->place);
}

/** The object of `kind` that `id` names, or NULL. */
static struct sysv_perm *sysv_find(int kind, int32_t id)
{
    struct sysv_perm *p;

    if (id < 0) {
        return NULL;
    }
    p = tables[kind][(uint32_t)id % SYSV_SEQ < SYSV_SLOTS ? (uint32_t)id % SYSV_SEQ : 0];
    return p && (uint32_t)id % SYSV_SEQ < SYSV_SLOTS && sysv_id(p) == id && sysv_mine(p) ? p : NULL;
}

/** The object of `kind` with key `key`, not removed, or NULL. */
static struct sysv_perm *sysv_find_key(int kind, int32_t key)
{
    int i;

    for (i = 0; i < SYSV_SLOTS; i++) {
        struct sysv_perm *p = tables[kind][i];

        if (p && !p->removed && p->key == key && sysv_mine(p)) {
            return p;
        }
    }
    return NULL;
}

/** Enter `p`, of `kind`, made by the caller with key `key` and the nine
 * bits of `flag`, in a free place; its id, or -ENOSPC with none free. */
static int32_t sysv_enter(int kind, struct sysv_perm *p, int32_t key, uint32_t flag)
{
    int i;

    for (i = 0; i < SYSV_SLOTS && tables[kind][i]; i++) {
    }
    if (i == SYSV_SLOTS) {
        return -ENOSPC;
    }
    p->key = key;
    p->uid = p->cuid = current_proc->euid;
    p->gid = p->cgid = current_proc->egid;
    p->mode = flag & 0777u;
    p->seq = seqs[kind]++ % (0x7FFFFFFFu / SYSV_SEQ);
    p->place = i;
    p->ns = proc_ipcns(current_proc);
    tables[kind][i] = p;
    return sysv_id(p);
}

/** Take `p` out of its kind's table; it is freed by whoever drops its last
 * reference, the table's included. */
static void sysv_leave(int kind, struct sysv_perm *p)
{
    tables[kind][p->place] = NULL;
    p->removed = 1;
}

/**
 * The lookup the three get calls share, Linux's ipcget: an existing object
 * of `key` unless the key is IPC_PRIVATE, refused with EEXIST under
 * IPC_CREAT|IPC_EXCL and EACCES when `flag` asks more than the caller may;
 * NULL with -ENOENT in `*rc` when there is none and IPC_CREAT is not given,
 * and NULL with 0 when one is to be made.
 */
static struct sysv_perm *sysv_lookup(int kind, int32_t key, uint32_t flag, int32_t *rc)
{
    struct sysv_perm *p = key == IPC_PRIVATE ? NULL : sysv_find_key(kind, key);

    *rc = 0;
    if (!p) {
        if (key != IPC_PRIVATE && !(flag & IPC_CREAT)) {
            *rc = -ENOENT;
        }
        return NULL;
    }
    if ((flag & IPC_CREAT) && (flag & IPC_EXCL)) {
        *rc = -EEXIST;
    } else if (!sysv_may(p, flag)) {
        *rc = -EACCES;
    }
    return p;
}

/** The pid `pid` of an object's record as the caller sees it, 0 kept. */
static int32_t sysv_pid_seen(int32_t pid)
{
    return pid > 0 ? (int32_t)pid_seen((uint32_t)pid) : pid;
}

/** Fill the 64-bit permissions `out` from `p`, with the ids as the caller
 * sees them. */
static void sysv_perm_out(const struct sysv_perm *p, struct ipc64_perm *out)
{
    k_memset(out, 0, sizeof(*out));
    out->key = p->key;
    out->uid = uid_seen(p->uid);
    out->gid = gid_seen(p->gid);
    out->cuid = uid_seen(p->cuid);
    out->cgid = gid_seen(p->cgid);
    out->mode = (uint16_t)p->mode;
    out->seq = (uint16_t)p->seq;
}

/** IPC_SET's change of `p` from `in`: the owner, the group and the nine
 * bits. 0, or -EINVAL for an id the caller's user namespace does not map. */
static int32_t sysv_perm_set(struct sysv_perm *p, const struct ipc64_perm *in)
{
    int64_t uid = uid_named(in->uid);
    int64_t gid = gid_named(in->gid);

    if (uid < 0 || gid < 0) {
        return -EINVAL;
    }
    p->uid = (uint32_t)uid;
    p->gid = (uint32_t)gid;
    p->mode = (p->mode & ~0777u) | (in->mode & 0777u);
    return 0;
}

/** The highest place of `kind`'s table in use, which IPC_INFO and the
 * kinds' INFO commands return, or 0. */
static int32_t sysv_highest(int kind)
{
    int32_t i;

    for (i = SYSV_SLOTS - 1; i > 0 && !(tables[kind][i] && sysv_mine(tables[kind][i])); i--) {
    }
    return i;
}

/**
 * The object a control command names: by id, or by place for the STAT
 * commands that take one (`by_place`), which the caller may read unless
 * `any`. NULL with the error in `*rc`: EINVAL for an id or a place with no
 * object, EACCES when the caller may not read it.
 */
static struct sysv_perm *sysv_target(int kind, int32_t id, int by_place, int any, int32_t *rc)
{
    struct sysv_perm *p;

    if (by_place) {
        p = id >= 0 && id < SYSV_SLOTS ? tables[kind][id] : NULL;
        p = p && sysv_mine(p) ? p : NULL;
    } else {
        p = sysv_find(kind, id);
    }
    *rc = !p ? -EINVAL : !any && !sysv_may(p, 0444) ? -EACCES : 0;
    return *rc == 0 ? p : NULL;
}

/* ------------------------------------------------------------------ */
/* Shared memory */

#define SHM_RDONLY   010000
#define SHM_RND      020000
#define SHM_REMAP    040000
#define SHM_EXEC     0100000
#define SHM_DEST     01000
#define SHM_STAT     13
#define SHM_INFO     14
#define SHM_STAT_ANY 15
#define SHM_LOCK     11
#define SHM_UNLOCK   12

struct shm_seg {
    struct sysv_perm perm;
    size_t size;
    struct open_file *ofd;
    int64_t atime, dtime, ctime;
    int32_t cpid, lpid;
};

/* IPC_INFO's struct shminfo64 and SHM_INFO's struct shm_info. */
struct shminfo64 {
    unsigned long shmmax, shmmin, shmmni, shmseg, shmall;
    unsigned long unused[4];
};

struct shm_info {
    int32_t used_ids;
    unsigned long shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes;
};

/** The segment's memory's record of sharing, which its mappings have. */
static struct shmem *shm_mem(const struct shm_seg *s)
{
    return tmpfs_shmem(s->ofd->vnode);
}

/** Free `s`, which no table and no caller has. */
static void shm_free(struct shm_seg *s)
{
    fd_ofd_unref(s->ofd);
    kfree(s);
}

/**
 * Free each segment removed while attached that has no attachment left:
 * Linux frees one at its last detach, and this kernel, which does not see
 * an address space's end, frees it at the next call that reads the table.
 */
static void shm_reap(void)
{
    int i;

    for (i = 0; i < SYSV_SLOTS; i++) {
        struct shm_seg *s = (struct shm_seg *)tables[SYSV_SHM][i];

        if (s && (s->perm.mode & SHM_DEST) && shm_attaches(shm_mem(s)) == 0) {
            sysv_leave(SYSV_SHM, &s->perm);
            shm_free(s);
        }
    }
}

/** A new segment of `size` bytes in a tmpfs file; NULL with no memory. */
static struct shm_seg *shm_new(size_t size)
{
    struct shm_seg *s = kzalloc(sizeof(*s));
    struct vfs_mount *mnt = vfs_memfd_mount();
    struct vfs_stat st;
    void *node;

    if (!s || tmpfs_memfd(mnt, "SYSV", 0600u, 0u, &node) != 0) {
        kfree(s);
        return NULL;
    }
    s->ofd = fd_ofd_new();
    k_memset(&st, 0, sizeof(st));
    st.size = size;
    if (!s->ofd || vfs_open_node(mnt, node, 2, &s->ofd->fhandle) != 0 ||
        vfs_setattr(mnt, node, VFS_SET_SIZE, &st) != 0) {
        if (s->ofd) {
            fd_ofd_unref(s->ofd);
        } else {
            mnt->type->ops->put(mnt, node);
        }
        kfree(s);
        return NULL;
    }
    s->ofd->vmnt = mnt;
    s->ofd->vnode = node;
    s->ofd->ino = vfs_getattr(mnt, node, &st) == 0 ? st.ino : 0;
    vfs_desc_open(mnt);
    s->size = size;
    s->cpid = (int32_t)current_proc->tgid;
    s->ctime = sysv_now();
    return s;
}

/**
 * shmget: the segment of `key`, or a new one of `size` bytes made with
 * IPC_CREAT or for IPC_PRIVATE. EINVAL for a size outside SHMMIN to SHMMAX,
 * or past an existing segment's; ENOSPC with no place free; ENOMEM with no
 * memory; and the refusals of `sysv_lookup`.
 */
static int32_t shm_get(int32_t key, size_t size, uint32_t flag)
{
    struct shm_seg *s;
    int32_t rc;

    shm_reap();
    s = (struct shm_seg *)sysv_lookup(SYSV_SHM, key, flag, &rc);
    if (rc != 0) {
        return rc;
    }
    if (s) {
        return size > s->size ? -EINVAL : sysv_id(&s->perm);
    }
    if (size < SHMMIN || size > SHMMAX) {
        return -EINVAL;
    }
    s = shm_new(size);
    if (!s) {
        return -ENOMEM;
    }
    rc = sysv_enter(SYSV_SHM, &s->perm, key, flag);
    if (rc < 0) {
        shm_free(s);
    }
    return rc;
}

/** The name /proc/PID/maps gives a segment of key `key`, as Linux's:
 * "/SYSV" and the key in eight hex digits, deleted; in the heap, or NULL. */
static char *shm_path(int32_t key)
{
    static const char hex[] = "0123456789abcdef";
    char *path = kmalloc(sizeof("/SYSV00000000 (deleted)"));
    int i;

    if (path) {
        k_memcpy(path, "/SYSV00000000 (deleted)", sizeof("/SYSV00000000 (deleted)"));
        for (i = 0; i < 8; i++) {
            path[5 + i] = hex[((uint32_t)key >> (28 - 4 * i)) & 15u];
        }
    }
    return path;
}

/**
 * shmat: map segment `id` into the caller at `addr`, or where the kernel
 * finds room when it is 0, and return the address. `addr` must be page
 * aligned unless SHM_RND rounds it down, and may not cover a mapping unless
 * SHM_REMAP replaces it, both EINVAL. SHM_RDONLY maps it read-only, which
 * mprotect cannot undo, and SHM_EXEC executable; the caller must be allowed
 * to read the segment, and to write it without SHM_RDONLY, else EACCES. A
 * removed segment may still be attached, as on Linux.
 */
static int32_t shm_at(int32_t id, uintptr_t addr, uint32_t flag, uintptr_t *base)
{
    struct shm_seg *s = (struct shm_seg *)sysv_find(SYSV_SHM, id);
    char *path;
    long at;

    if (!s) {
        return -EINVAL;
    }
    if (!sysv_may(&s->perm, (flag & SHM_RDONLY) ? 0444u : 0666u)) {
        return -EACCES;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        if (!(flag & SHM_RND)) {
            return -EINVAL;
        }
        addr &= ~(uintptr_t)(PAGE_SIZE - 1);
    }
    if (addr == 0 && (flag & SHM_REMAP)) {
        return -EINVAL;
    }
    path = shm_path(s->perm.key);
    at = mmap_shm(s->ofd, addr, s->size,
                  ((flag & SHM_RDONLY) ? 0u : SHM_MAP_WRITE) |
                      ((flag & SHM_EXEC) ? SHM_MAP_EXEC : 0u),
                  addr == 0            ? 0
                  : (flag & SHM_REMAP) ? 2
                                       : 1,
                  path);
    if (USER_ADDR_ERRNO(at)) {
        return at == -EEXIST ? -EINVAL : (int32_t)at;
    }
    *base = (uintptr_t)at;
    s->atime = sysv_now();
    s->lpid = (int32_t)current_proc->tgid;
    return 0;
}

/** shmdt: unmap the segment attached at `addr`. EINVAL when no attachment
 * begins there. */
static int32_t shm_dt(uintptr_t addr)
{
    int i;

    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    for (i = 0; i < SYSV_SLOTS; i++) {
        struct shm_seg *s = (struct shm_seg *)tables[SYSV_SHM][i];

        if (s && munmap_shm(addr, shm_mem(s), s->size) == 0) {
            s->dtime = sysv_now();
            s->lpid = (int32_t)current_proc->tgid;
            shm_reap();
            return 0;
        }
    }
    return -EINVAL;
}

/** IPC_STAT and the STAT commands' record of `s`. */
static void shm_stat_out(struct shm_seg *s, struct shmid64_ds *ds)
{
    k_memset(ds, 0, sizeof(*ds));
    sysv_perm_out(&s->perm, &ds->perm);
    ds->segsz = s->size;
    IPC_TIME_SET(ds, atime, s->atime);
    IPC_TIME_SET(ds, dtime, s->dtime);
    IPC_TIME_SET(ds, ctime, s->ctime);
    ds->cpid = sysv_pid_seen(s->cpid);
    ds->lpid = sysv_pid_seen(s->lpid);
    ds->nattch = shm_attaches(shm_mem(s));
}

/** IPC_INFO and SHM_INFO into `buf`; the highest place in use. */
static int32_t shm_info_out(int cmd, void __user *buf)
{
    struct shminfo64 info;
    struct shm_info used;
    int i;

    if (cmd == IPC_INFO) {
        k_memset(&info, 0, sizeof(info));
        info.shmmax = SHMMAX;
        info.shmmin = SHMMIN;
        info.shmmni = SYSV_SLOTS;
        info.shmseg = SYSV_SLOTS;
        info.shmall = SHMMAX;
        return copy_to_user(buf, &info, sizeof(info)) != 0 ? -EFAULT : sysv_highest(SYSV_SHM);
    }
    k_memset(&used, 0, sizeof(used));
    for (i = 0; i < SYSV_SLOTS; i++) {
        const struct shm_seg *s = (const struct shm_seg *)tables[SYSV_SHM][i];

        if (s && sysv_mine(&s->perm)) {
            used.used_ids++;
            used.shm_tot += (s->size + 4095u) / 4096u;
        }
    }
    used.shm_rss = used.shm_tot;
    return copy_to_user(buf, &used, sizeof(used)) != 0 ? -EFAULT : sysv_highest(SYSV_SHM);
}

/**
 * shmctl: IPC_STAT, SHM_STAT and SHM_STAT_ANY copy a segment's record out,
 * the last two by place, returning its id; IPC_SET changes its owner, group
 * and bits; IPC_RMID removes it, at once when nothing is attached and else
 * at its last detach, giving it the key IPC_PRIVATE meanwhile; IPC_INFO and
 * SHM_INFO give the limits and the use. SHM_LOCK and SHM_UNLOCK do nothing,
 * since no page is ever swapped. EPERM for a caller that neither owns nor
 * made the segment, EINVAL for another command.
 */
static int32_t shm_ctl(int32_t id, int cmd, void __user *buf)
{
    struct shmid64_ds ds;
    struct shm_seg *s;
    int32_t rc;

    cmd &= ~IPC_64;
    shm_reap();
    if (cmd == IPC_INFO || cmd == SHM_INFO) {
        return shm_info_out(cmd, buf);
    }
    s = (struct shm_seg *)sysv_target(SYSV_SHM, id, cmd == SHM_STAT || cmd == SHM_STAT_ANY,
                                      cmd != IPC_STAT && cmd != SHM_STAT, &rc);
    if (!s) {
        return rc;
    }
    switch (cmd) {
    case IPC_STAT:
    case SHM_STAT:
    case SHM_STAT_ANY:
        shm_stat_out(s, &ds);
        if (copy_to_user(buf, &ds, sizeof(ds)) != 0) {
            return -EFAULT;
        }
        return cmd == IPC_STAT ? 0 : sysv_id(&s->perm);
    case IPC_SET:
        if (copy_from_user(&ds, buf, sizeof(ds)) != 0) {
            return -EFAULT;
        }
        if (!sysv_owner(&s->perm)) {
            return -EPERM;
        }
        if (sysv_perm_set(&s->perm, &ds.perm) != 0) {
            return -EINVAL;
        }
        s->ctime = sysv_now();
        return 0;
    case IPC_RMID:
        if (!sysv_owner(&s->perm)) {
            return -EPERM;
        }
        s->perm.mode |= SHM_DEST;
        s->perm.key = IPC_PRIVATE;
        shm_reap();
        return 0;
    case SHM_LOCK:
    case SHM_UNLOCK:
        return sysv_owner(&s->perm) ? 0 : -EPERM;
    default:
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* Semaphores */

#define SEM_UNDO     0x1000
#define GETPID       11
#define GETVAL       12
#define GETALL       13
#define GETNCNT      14
#define GETZCNT      15
#define SETVAL       16
#define SETALL       17
#define SEM_STAT     18
#define SEM_INFO     19
#define SEM_STAT_ANY 20

struct sem {
    uint16_t val;
    uint16_t ncnt, zcnt;
    int32_t pid;
};

struct sem_set {
    struct sysv_perm perm;
    uint32_t nsems;
    struct sem *sems;
    int64_t otime, ctime;
    wait_queue_t wq;
    uint32_t gen;
};

/* A process's adjustments of one set, which its end applies: kept in one
 * list for every process, so SETVAL and SETALL can clear them. */
struct sem_undo {
    struct sem_undo *next;
    struct tgroup *tg;
    int32_t semid;
    uint32_t nsems;
    int16_t *adj;
};

struct seminfo {
    int32_t semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem;
};

/* A semaphore operation as semop takes it. */
struct sembuf {
    uint16_t num;
    int16_t op;
    int16_t flg;
};

static struct sem_undo *undos;

/** Drop a reference to `set`, freeing it with the last. */
static void sem_put(struct sem_set *set)
{
    if (--set->perm.refs == 0) {
        kfree(set->sems);
        kfree(set);
    }
}

/** semget: the set of `key`, or a new one of `nsems` semaphores, each 0.
 * EINVAL for a count past SEMMSL, 0 for a new set, or past an existing
 * set's; ENOSPC and ENOMEM as for shmget. */
static int32_t sem_get(int32_t key, int32_t nsems, uint32_t flag)
{
    struct sem_set *set;
    int32_t rc;

    if (nsems < 0 || (uint32_t)nsems > SEMMSL) {
        return -EINVAL;
    }
    set = (struct sem_set *)sysv_lookup(SYSV_SEM, key, flag, &rc);
    if (rc != 0) {
        return rc;
    }
    if (set) {
        return (uint32_t)nsems > set->nsems ? -EINVAL : sysv_id(&set->perm);
    }
    if (nsems == 0) {
        return -EINVAL;
    }
    set = kzalloc(sizeof(*set));
    if (set) {
        set->sems = kzalloc((uint32_t)nsems * sizeof(struct sem));
    }
    if (!set || !set->sems) {
        kfree(set);
        return -ENOMEM;
    }
    set->nsems = (uint32_t)nsems;
    set->perm.refs = 1;
    set->ctime = sysv_now();
    wait_queue_init(&set->wq);
    rc = sysv_enter(SYSV_SEM, &set->perm, key, flag);
    if (rc < 0) {
        sem_put(set);
    }
    return rc;
}

/** The caller's adjustments of `set`, made empty when it has none; NULL
 * with no memory. */
static struct sem_undo *sem_undo_of(struct sem_set *set)
{
    struct sem_undo *u;
    int32_t id = sysv_id(&set->perm);

    for (u = undos; u; u = u->next) {
        if (u->tg == current_proc->tg && u->semid == id) {
            return u;
        }
    }
    u = kzalloc(sizeof(*u));
    if (u) {
        u->adj = kzalloc(set->nsems * sizeof(int16_t));
    }
    if (!u || !u->adj) {
        kfree(u);
        return NULL;
    }
    u->tg = current_proc->tg;
    u->semid = id;
    u->nsems = set->nsems;
    u->next = undos;
    undos = u;
    return u;
}

/** Clear every process's adjustment of semaphore `num` of `set`, or of all
 * its semaphores when `num` is negative, as SETVAL and SETALL do. */
static void sem_undo_clear(const struct sem_set *set, int num)
{
    struct sem_undo *u;
    int32_t id = sysv_id(&set->perm);
    uint32_t i;

    for (u = undos; u; u = u->next) {
        for (i = 0; u->semid == id && i < u->nsems; i++) {
            if (num < 0 || i == (uint32_t)num) {
                u->adj[i] = 0;
            }
        }
    }
}

/**
 * Try `ops` on `set` at once, Linux's perform_atomic_semop: each in order,
 * an increment, a decrement that leaves no negative value, or a wait for 0.
 * 0 when every one was applied; 1 when one must wait, with its index in
 * `*blocked` and nothing applied; -ERANGE for a value past SEMVMX or an
 * adjustment past SEMAEM, with nothing applied.
 */
static int sem_try(struct sem_set *set, const struct sembuf *ops, uint32_t n, struct sem_undo *u,
                   uint32_t *blocked)
{
    uint32_t i;
    int rc = 0;

    for (i = 0; i < n && rc == 0; i++) {
        struct sem *s = &set->sems[ops[i].num];
        int32_t v = (int32_t)s->val + ops[i].op;
        int32_t adj = u && (ops[i].flg & SEM_UNDO) ? u->adj[ops[i].num] - ops[i].op : 0;

        if ((ops[i].op == 0 && s->val != 0) || v < 0) {
            *blocked = i;
            rc = 1;
        } else if (v > SEMVMX || adj < -SEMAEM - 1 || adj > SEMAEM) {
            rc = -ERANGE;
        } else {
            s->val = (uint16_t)v;
            continue;
        }
        break;
    }
    while (i-- > 0) {
        if (rc != 0) {
            set->sems[ops[i].num].val = (uint16_t)(set->sems[ops[i].num].val - ops[i].op);
        }
    }
    return rc;
}

/** Record the success of `ops`: the caller's pid on each semaphore, its
 * adjustments, the time, and a wake for every waiter when a value moved. */
static void sem_done(struct sem_set *set, const struct sembuf *ops, uint32_t n, struct sem_undo *u)
{
    uint32_t i;
    int moved = 0;

    for (i = 0; i < n; i++) {
        set->sems[ops[i].num].pid = (int32_t)current_proc->tgid;
        if (u && (ops[i].flg & SEM_UNDO)) {
            u->adj[ops[i].num] = (int16_t)(u->adj[ops[i].num] - ops[i].op);
        }
        moved |= ops[i].op != 0;
    }
    set->otime = sysv_now();
    if (moved) {
        set->gen++;
        wq_wake_all(&set->wq);
    }
}

/* What a sleeper on a set or a queue waits for: a change, the object's
 * removal or the deadline. */
struct sysv_wait {
    const struct sysv_perm *perm;
    const uint32_t *gen;
    uint32_t from;
    int timed;
    uint32_t target;
};

static int sysv_wait_done(void *arg)
{
    const struct sysv_wait *w = arg;

    return *w->gen != w->from || w->perm->removed ||
           (w->timed && (int32_t)(kernel_ticks - w->target) >= 0);
}

/**
 * Sleep on `wq` until what `w` waits for: 0 for a change, -EIDRM when the
 * object was removed, -EAGAIN at the deadline, as semtimedop and msgrcv
 * report it, and -EINTR for a signal.
 */
static int32_t sysv_sleep(wait_queue_t *wq, struct sysv_wait *w)
{
    int rc;

    current_proc->wakeup_tick = w->timed ? (w->target != 0 ? w->target : 1u) : 0;
    rc = wq_wait_event(wq, sysv_wait_done, w);
    current_proc->wakeup_tick = 0;
    if (w->perm->removed) {
        return -EIDRM;
    }
    if (*w->gen != w->from) {
        return 0;
    }
    return rc != 0 ? -EINTR : -EAGAIN;
}

/** Apply `ops` to `set`, sleeping while one must wait as `sem_op` says,
 * with the waiting semaphore's count of waiters raised meanwhile. */
static int32_t sem_wait_apply(struct sem_set *set, const struct sembuf *ops, uint32_t n,
                              struct sem_undo *u, struct sysv_wait *w)
{
    uint32_t blocked;
    int32_t rc;

    for (;;) {
        struct sem *s;

        rc = sem_try(set, ops, n, u, &blocked);
        if (rc == 0) {
            sem_done(set, ops, n, u);
        }
        if (rc <= 0) {
            return rc;
        }
        if (ops[blocked].flg & IPC_NOWAIT) {
            return -EAGAIN;
        }
        s = &set->sems[ops[blocked].num];
        if (ops[blocked].op == 0) {
            s->zcnt++;
        } else {
            s->ncnt++;
        }
        w->from = set->gen;
        rc = sysv_sleep(&set->wq, w);
        if (ops[blocked].op == 0) {
            s->zcnt--;
        } else {
            s->ncnt--;
        }
        if (rc != 0) {
            return rc;
        }
    }
}

/**
 * semop and semtimedop: apply the `n` operations at `uops` to set `id` at
 * once, waiting while one cannot be applied unless its IPC_NOWAIT gives
 * EAGAIN, and at most `ticks` ticks when `timed`, EAGAIN after. E2BIG for
 * more than SEMOPM, EINVAL for none or an unknown set, EFBIG for a
 * semaphore past the set's, EACCES for an alteration the caller may not
 * write or a wait it may not read, EIDRM when the set is removed meanwhile,
 * ERANGE and ENOMEM as `sem_try` and `sem_undo_of` give them.
 */
static int32_t sem_op(int32_t id, const void __user *uops, uint32_t n, int timed, uint32_t ticks)
{
    struct sysv_wait w;
    struct sembuf *ops;
    struct sem_set *set;
    struct sem_undo *u = NULL;
    uint32_t i, alter = 0, undo = 0;
    int32_t rc = 0;

    if (n == 0) {
        return -EINVAL;
    }
    if (n > SEMOPM) {
        return -E2BIG;
    }
    ops = kmalloc(n * sizeof(*ops));
    if (!ops) {
        return -ENOMEM;
    }
    if (copy_from_user(ops, uops, n * sizeof(*ops)) != 0) {
        kfree(ops);
        return -EFAULT;
    }
    set = (struct sem_set *)sysv_find(SYSV_SEM, id);
    for (i = 0; set && i < n; i++) {
        alter |= ops[i].op != 0;
        undo |= (ops[i].flg & SEM_UNDO) != 0;
        if (ops[i].num >= set->nsems) {
            rc = -EFBIG;
        }
    }
    if (!set) {
        rc = -EINVAL;
    }
    if (rc == 0 && !sysv_may(&set->perm, alter ? 0222u : 0444u)) {
        rc = -EACCES;
    }
    if (rc == 0 && undo && !(u = sem_undo_of(set))) {
        rc = -ENOMEM;
    }
    if (rc == 0) {
        set->perm.refs++;
        w.perm = &set->perm;
        w.gen = &set->gen;
        w.timed = timed;
        w.target = kernel_ticks + ticks + 1u;
        rc = sem_wait_apply(set, ops, n, u, &w);
        sem_put(set);
    }
    kfree(ops);
    return rc;
}

/** IPC_STAT and the STAT commands' record of `set`. */
static void sem_stat_out(const struct sem_set *set, struct semid64_ds *ds)
{
    k_memset(ds, 0, sizeof(*ds));
    sysv_perm_out(&set->perm, &ds->perm);
    IPC_TIME_SET(ds, otime, set->otime);
    IPC_TIME_SET(ds, ctime, set->ctime);
    ds->nsems = set->nsems;
}

/** IPC_INFO and SEM_INFO into `buf`, SEM_INFO with the sets in use as
 * `semusz` and their semaphores as `semaem`; the highest place in use. */
static int32_t sem_info_out(int cmd, void __user *buf)
{
    struct seminfo info;
    int i;

    k_memset(&info, 0, sizeof(info));
    info.semmni = SYSV_SLOTS;
    info.semmsl = (int32_t)SEMMSL;
    info.semmns = SYSV_SLOTS * (int32_t)SEMMSL;
    info.semopm = (int32_t)SEMOPM;
    info.semvmx = SEMVMX;
    info.semaem = SEMAEM;
    info.semmnu = info.semmns;
    info.semume = (int32_t)SEMOPM;
    info.semmap = info.semmns;
    info.semusz = 20;
    for (i = 0; cmd != IPC_INFO && i < SYSV_SLOTS; i++) {
        const struct sem_set *set = (const struct sem_set *)tables[SYSV_SEM][i];

        if (i == 0) {
            info.semusz = info.semaem = 0;
        }
        if (set && sysv_mine(&set->perm)) {
            info.semusz++;
            info.semaem += (int32_t)set->nsems;
        }
    }
    return copy_to_user(buf, &info, sizeof(info)) != 0 ? -EFAULT : sysv_highest(SYSV_SEM);
}

/** SETVAL and SETALL: semaphore `num` of `set` to `val`, or every one from
 * the array at `uarr` when `num` is negative; each process's adjustment of
 * what changed is cleared and every waiter woken. ERANGE for a value past
 * SEMVMX. */
static int32_t sem_set_values(struct sem_set *set, int num, int32_t val, const void __user *uarr)
{
    uint16_t *vals = NULL;
    uint32_t i;

    if (num < 0) {
        vals = kmalloc(set->nsems * sizeof(uint16_t));
        if (!vals) {
            return -ENOMEM;
        }
        if (copy_from_user(vals, uarr, set->nsems * sizeof(uint16_t)) != 0) {
            kfree(vals);
            return -EFAULT;
        }
    }
    for (i = 0; i < set->nsems; i++) {
        if ((num < 0 && vals[i] > SEMVMX) || (num >= 0 && (val < 0 || val > SEMVMX))) {
            kfree(vals);
            return -ERANGE;
        }
    }
    for (i = 0; i < set->nsems; i++) {
        if (num < 0 || i == (uint32_t)num) {
            set->sems[i].val = (uint16_t)(num < 0 ? vals[i] : (uint16_t)val);
            set->sems[i].pid = (int32_t)current_proc->tgid;
        }
    }
    kfree(vals);
    sem_undo_clear(set, num);
    set->ctime = sysv_now();
    set->gen++;
    wq_wake_all(&set->wq);
    return 0;
}

/** The per-semaphore commands of semctl on semaphore `num` of `set`: its
 * value, the last pid and the counts of waiters, and SETVAL. EINVAL for a
 * semaphore past the set's, EACCES for one the caller may not read, or with
 * SETVAL write. */
static int32_t sem_ctl_one(struct sem_set *set, int num, int cmd, uintptr_t arg)
{
    const struct sem *s;

    if (num < 0 || (uint32_t)num >= set->nsems) {
        return -EINVAL;
    }
    if (!sysv_may(&set->perm, cmd == SETVAL ? 0222u : 0444u)) {
        return -EACCES;
    }
    s = &set->sems[num];
    switch (cmd) {
    case GETVAL:
        return s->val;
    case GETPID:
        return sysv_pid_seen(s->pid);
    case GETNCNT:
        return s->ncnt;
    case GETZCNT:
        return s->zcnt;
    default:
        return sem_set_values(set, num, (int32_t)arg, NULL);
    }
}

/** GETALL: every value of `set` into the array at `uarr`. */
static int32_t sem_get_all(const struct sem_set *set, void __user *uarr)
{
    uint16_t *vals;
    uint32_t i;
    int32_t rc;

    if (!sysv_may(&set->perm, 0444u)) {
        return -EACCES;
    }
    vals = kmalloc(set->nsems * sizeof(uint16_t));
    if (!vals) {
        return -ENOMEM;
    }
    for (i = 0; i < set->nsems; i++) {
        vals[i] = set->sems[i].val;
    }
    rc = copy_to_user(uarr, vals, set->nsems * sizeof(uint16_t)) != 0 ? -EFAULT : 0;
    kfree(vals);
    return rc;
}

/** IPC_SET and IPC_RMID on `set`, for its owner or its maker: the owner,
 * group and bits from the record at `buf`, or the set's removal, which
 * wakes every waiter with EIDRM. */
static int32_t sem_ctl_owner(struct sem_set *set, int cmd, void __user *buf)
{
    struct semid64_ds ds;

    if (cmd == IPC_SET && copy_from_user(&ds, buf, sizeof(ds)) != 0) {
        return -EFAULT;
    }
    if (!sysv_owner(&set->perm)) {
        return -EPERM;
    }
    if (cmd == IPC_SET) {
        if (sysv_perm_set(&set->perm, &ds.perm) != 0) {
            return -EINVAL;
        }
        set->ctime = sysv_now();
        return 0;
    }
    sysv_leave(SYSV_SEM, &set->perm);
    wq_wake_all(&set->wq);
    sem_put(set);
    return 0;
}

/**
 * semctl: the per-semaphore commands (`sem_ctl_one`), GETALL and SETALL
 * through the array `arg` points at, IPC_STAT, SEM_STAT and SEM_STAT_ANY
 * into the record it points at, the last two by place and returning the
 * id, IPC_SET and IPC_RMID (`sem_ctl_owner`), and IPC_INFO and SEM_INFO.
 * EINVAL for another command.
 */
static int32_t sem_ctl(int32_t id, int num, int cmd, uintptr_t arg)
{
    struct semid64_ds ds;
    struct sem_set *set;
    int32_t rc;

    cmd &= ~IPC_64;
    if (cmd == IPC_INFO || cmd == SEM_INFO) {
        return sem_info_out(cmd, (void __user *)arg);
    }
    set = (struct sem_set *)sysv_target(SYSV_SEM, id, cmd == SEM_STAT || cmd == SEM_STAT_ANY,
                                        cmd != IPC_STAT && cmd != SEM_STAT, &rc);
    if (!set) {
        return rc;
    }
    switch (cmd) {
    case IPC_STAT:
    case SEM_STAT:
    case SEM_STAT_ANY:
        sem_stat_out(set, &ds);
        if (copy_to_user((void __user *)arg, &ds, sizeof(ds)) != 0) {
            return -EFAULT;
        }
        return cmd == IPC_STAT ? 0 : sysv_id(&set->perm);
    case IPC_SET:
    case IPC_RMID:
        return sem_ctl_owner(set, cmd, (void __user *)arg);
    case GETALL:
        return sem_get_all(set, (void __user *)arg);
    case SETALL:
        return sysv_may(&set->perm, 0222u) ? sem_set_values(set, -1, 0, (const void __user *)arg)
                                           : -EACCES;
    case GETVAL:
    case GETPID:
    case GETNCNT:
    case GETZCNT:
    case SETVAL:
        return sem_ctl_one(set, num, cmd, arg);
    default:
        return -EINVAL;
    }
}

void sysv_exit(struct tgroup *tg)
{
    struct sem_undo **link = &undos;

    while (*link) {
        struct sem_undo *u = *link;
        struct sem_set *set;
        uint32_t i;

        if (u->tg != tg) {
            link = &u->next;
            continue;
        }
        *link = u->next;
        set = (struct sem_set *)sysv_find(SYSV_SEM, u->semid);
        for (i = 0; set && i < u->nsems; i++) {
            int32_t v = (int32_t)set->sems[i].val + u->adj[i];

            set->sems[i].val = (uint16_t)(v < 0 ? 0 : v > SEMVMX ? SEMVMX : v);
            set->sems[i].pid = (int32_t)tg->leader->tgid;
        }
        if (set) {
            set->gen++;
            wq_wake_all(&set->wq);
        }
        kfree(u->adj);
        kfree(u);
    }
}

/* ------------------------------------------------------------------ */
/* Message queues */

#define MSG_NOERROR  010000
#define MSG_EXCEPT   020000
#define MSG_COPY     040000
#define MSG_STAT     11
#define MSG_INFO     12
#define MSG_STAT_ANY 13

struct msg {
    struct msg *next;
    long type;
    uint32_t size;
};

struct msg_queue {
    struct sysv_perm perm;
    struct msg *head;
    uint32_t cbytes, qnum, qbytes;
    int64_t stime, rtime, ctime;
    int32_t lspid, lrpid;
    wait_queue_t wq;
    uint32_t gen;
};

struct msginfo {
    int32_t msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz, msgtql;
    uint16_t msgseg;
};

/** Drop a reference to `q`, freeing it and its messages with the last. */
static void msg_put(struct msg_queue *q)
{
    if (--q->perm.refs == 0) {
        while (q->head) {
            struct msg *m = q->head;

            q->head = m->next;
            kfree(m);
        }
        kfree(q);
    }
}

/** msgget: the queue of `key`, or a new empty one of MSGMNB bytes. */
static int32_t msg_get(int32_t key, uint32_t flag)
{
    struct msg_queue *q;
    int32_t rc;

    q = (struct msg_queue *)sysv_lookup(SYSV_MSG, key, flag, &rc);
    if (rc != 0 || q) {
        return rc != 0 ? rc : sysv_id(&q->perm);
    }
    q = kzalloc(sizeof(*q));
    if (!q) {
        return -ENOMEM;
    }
    q->qbytes = MSGMNB;
    q->perm.refs = 1;
    q->ctime = sysv_now();
    wait_queue_init(&q->wq);
    rc = sysv_enter(SYSV_MSG, &q->perm, key, flag);
    if (rc < 0) {
        msg_put(q);
    }
    return rc;
}

/** The queue `id` names, held, when the caller may use it as `flag` asks;
 * NULL with EINVAL or EACCES in `*rc`. */
static struct msg_queue *msg_hold(int32_t id, uint32_t flag, int32_t *rc)
{
    struct msg_queue *q = (struct msg_queue *)sysv_find(SYSV_MSG, id);

    *rc = !q ? -EINVAL : !sysv_may(&q->perm, flag) ? -EACCES : 0;
    if (*rc != 0) {
        return NULL;
    }
    q->perm.refs++;
    return q;
}

/**
 * msgsnd: append the message at `umsg`, a long type of at least 1 and
 * `size` bytes of text, to queue `id`, waiting while the queue's bytes or
 * its count of messages would pass its limit, unless IPC_NOWAIT gives
 * EAGAIN. EINVAL for a size past MSGMAX or a type below 1, EACCES when the
 * caller may not write the queue, EIDRM when it is removed meanwhile.
 */
static int32_t msg_snd(int32_t id, const void __user *umsg, uint32_t size, uint32_t flag)
{
    struct sysv_wait w;
    struct msg_queue *q;
    struct msg *m, **tail;
    int32_t rc = 0;

    if (size > MSGMAX) {
        return -EINVAL;
    }
    m = kmalloc(sizeof(*m) + size);
    if (!m) {
        return -ENOMEM;
    }
    if (copy_from_user(&m->type, umsg, sizeof(m->type)) != 0 ||
        copy_from_user(m + 1, (const char __user *)umsg + sizeof(m->type), size) != 0) {
        kfree(m);
        return -EFAULT;
    }
    m->size = size;
    m->next = NULL;
    q = m->type < 1 ? NULL : msg_hold(id, 0222u, &rc);
    if (!q) {
        kfree(m);
        return rc != 0 ? rc : -EINVAL;
    }
    w.perm = &q->perm;
    w.gen = &q->gen;
    w.timed = 0;
    while (rc == 0 && (q->cbytes + size > q->qbytes || q->qnum + 1 > q->qbytes)) {
        w.from = q->gen;
        rc = (flag & IPC_NOWAIT) ? -EAGAIN : sysv_sleep(&q->wq, &w);
    }
    if (rc == 0) {
        for (tail = &q->head; *tail; tail = &(*tail)->next) {
        }
        *tail = m;
        q->cbytes += size;
        q->qnum++;
        q->lspid = (int32_t)current_proc->tgid;
        q->stime = sysv_now();
        q->gen++;
        wq_wake_all(&q->wq);
    } else {
        kfree(m);
    }
    msg_put(q);
    return rc;
}

/** The link to the message of `q` msgrcv takes for `type` and `flag`: the
 * first for 0, the first of `type` or with MSG_EXCEPT not of it for a
 * positive one, and the first of the lowest type up to -`type` for a
 * negative one; NULL for none. */
static struct msg **msg_pick(struct msg_queue *q, long type, uint32_t flag)
{
    struct msg **link, **best = NULL;

    for (link = &q->head; *link; link = &(*link)->next) {
        long t = (*link)->type;

        if (type == 0 || (type > 0 && ((flag & MSG_EXCEPT) ? t != type : t == type))) {
            return link;
        }
        if (type < 0 && t <= -type && (!best || t < (*best)->type)) {
            best = link;
        }
    }
    return best;
}

/** Copy `m` out to `umsg`, its text cut to `size` bytes; the bytes of text
 * copied. */
static int32_t msg_copy_out(const struct msg *m, void __user *umsg, uint32_t size)
{
    uint32_t n = m->size < size ? m->size : size;

    if (copy_to_user(umsg, &m->type, sizeof(m->type)) != 0 ||
        copy_to_user((char __user *)umsg + sizeof(m->type), m + 1, n) != 0) {
        return -EFAULT;
    }
    return (int32_t)n;
}

/**
 * msgrcv: take the message `msg_pick` finds in queue `id`, its type and at
 * most `size` bytes of its text, into `umsg`, waiting while there is none
 * unless IPC_NOWAIT gives ENOMSG. A longer message is E2BIG and stays
 * unless MSG_NOERROR cuts it. MSG_COPY is ENOSYS, as from a Linux built
 * without checkpoint and restore. Returns the bytes of text.
 */
static int32_t msg_rcv(int32_t id, void __user *umsg, int32_t size, long type, uint32_t flag)
{
    struct sysv_wait w;
    struct msg_queue *q;
    struct msg **link = NULL;
    int32_t rc = 0;

    if (size < 0) {
        return -EINVAL;
    }
    if (flag & MSG_COPY) {
        return -ENOSYS;
    }
    q = msg_hold(id, 0444u, &rc);
    if (!q) {
        return rc;
    }
    w.perm = &q->perm;
    w.gen = &q->gen;
    w.timed = 0;
    while (rc == 0 && !(link = msg_pick(q, type, flag))) {
        w.from = q->gen;
        rc = (flag & IPC_NOWAIT) ? -ENOMSG : sysv_sleep(&q->wq, &w);
    }
    if (rc == 0 && (*link)->size > (uint32_t)size && !(flag & MSG_NOERROR)) {
        rc = -E2BIG;
    }
    if (rc == 0) {
        struct msg *m = *link;

        rc = msg_copy_out(m, umsg, (uint32_t)size);
        if (rc >= 0) {
            *link = m->next;
            q->cbytes -= m->size;
            q->qnum--;
            q->lrpid = (int32_t)current_proc->tgid;
            q->rtime = sysv_now();
            q->gen++;
            wq_wake_all(&q->wq);
            kfree(m);
        }
    }
    msg_put(q);
    return rc;
}

/** IPC_STAT and the STAT commands' record of `q`. */
static void msg_stat_out(const struct msg_queue *q, struct msqid64_ds *ds)
{
    k_memset(ds, 0, sizeof(*ds));
    sysv_perm_out(&q->perm, &ds->perm);
    IPC_TIME_SET(ds, stime, q->stime);
    IPC_TIME_SET(ds, rtime, q->rtime);
    IPC_TIME_SET(ds, ctime, q->ctime);
    ds->cbytes = q->cbytes;
    ds->qnum = q->qnum;
    ds->qbytes = q->qbytes;
    ds->lspid = sysv_pid_seen(q->lspid);
    ds->lrpid = sysv_pid_seen(q->lrpid);
}

/** IPC_INFO and MSG_INFO into `buf`, MSG_INFO with the queues in use as
 * `msgpool`, their messages as `msgmap` and their bytes as `msgtql`; the
 * highest place in use. */
static int32_t msg_info_out(int cmd, void __user *buf)
{
    struct msginfo info;
    int i;

    k_memset(&info, 0, sizeof(info));
    info.msgmax = (int32_t)MSGMAX;
    info.msgmnb = (int32_t)MSGMNB;
    info.msgmni = SYSV_SLOTS;
    info.msgssz = 16;
    info.msgseg = 0xFFFF;
    info.msgpool = SYSV_SLOTS * (int32_t)MSGMNB / 1024;
    info.msgmap = info.msgtql = (int32_t)MSGMNB;
    for (i = 0; cmd != IPC_INFO && i < SYSV_SLOTS; i++) {
        const struct msg_queue *q = (const struct msg_queue *)tables[SYSV_MSG][i];

        if (i == 0) {
            info.msgpool = info.msgmap = info.msgtql = 0;
        }
        if (q && sysv_mine(&q->perm)) {
            info.msgpool++;
            info.msgmap += (int32_t)q->qnum;
            info.msgtql += (int32_t)q->cbytes;
        }
    }
    return copy_to_user(buf, &info, sizeof(info)) != 0 ? -EFAULT : sysv_highest(SYSV_MSG);
}

/** IPC_SET and IPC_RMID on `q`, for its owner or its maker: the owner,
 * group, bits and byte limit from the record at `buf`, a limit past MSGMNB
 * needing a privileged caller, or the queue's removal, which wakes every
 * waiter with EIDRM. */
static int32_t msg_ctl_owner(struct msg_queue *q, int cmd, void __user *buf)
{
    struct msqid64_ds ds;

    if (cmd == IPC_SET && copy_from_user(&ds, buf, sizeof(ds)) != 0) {
        return -EFAULT;
    }
    if (!sysv_owner(&q->perm) ||
        (cmd == IPC_SET && ds.qbytes > MSGMNB && !proc_privileged(current_proc))) {
        return -EPERM;
    }
    if (cmd == IPC_SET) {
        if (sysv_perm_set(&q->perm, &ds.perm) != 0) {
            return -EINVAL;
        }
        q->qbytes = USER_LEN32(ds.qbytes);
        q->ctime = sysv_now();
        q->gen++;
        wq_wake_all(&q->wq);
        return 0;
    }
    sysv_leave(SYSV_MSG, &q->perm);
    wq_wake_all(&q->wq);
    msg_put(q);
    return 0;
}

/** msgctl: IPC_STAT, MSG_STAT and MSG_STAT_ANY, IPC_SET and IPC_RMID, and
 * IPC_INFO and MSG_INFO, as semctl has them. */
static int32_t msg_ctl(int32_t id, int cmd, void __user *buf)
{
    struct msqid64_ds ds;
    struct msg_queue *q;
    int32_t rc;

    cmd &= ~IPC_64;
    if (cmd == IPC_INFO || cmd == MSG_INFO) {
        return msg_info_out(cmd, buf);
    }
    q = (struct msg_queue *)sysv_target(SYSV_MSG, id, cmd == MSG_STAT || cmd == MSG_STAT_ANY,
                                        cmd != IPC_STAT && cmd != MSG_STAT, &rc);
    if (!q) {
        return rc;
    }
    switch (cmd) {
    case IPC_STAT:
    case MSG_STAT:
    case MSG_STAT_ANY:
        msg_stat_out(q, &ds);
        if (copy_to_user(buf, &ds, sizeof(ds)) != 0) {
            return -EFAULT;
        }
        return cmd == IPC_STAT ? 0 : sysv_id(&q->perm);
    case IPC_SET:
    case IPC_RMID:
        return msg_ctl_owner(q, cmd, buf);
    default:
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* The calls */

/* ipc(2)'s calls. */
#define SEMOP      1
#define SEMGET     2
#define SEMCTL     3
#define SEMTIMEDOP 4
#define MSGSND     11
#define MSGRCV     12
#define MSGGET     13
#define MSGCTL     14
#define SHMAT      21
#define SHMDT      22
#define SHMGET     23
#define SHMCTL     24

/** The ticks of the relative 32-bit timespec at `uts`, the form ipc's
 * semtimedop takes, into `*ticks`; EINVAL for a negative or malformed one. */
static int32_t sysv_ticks32(const void __user *uts, uint32_t *ticks)
{
    int32_t ts[2];

    if (copy_from_user(ts, uts, sizeof(ts)) != 0) {
        return -EFAULT;
    }
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) {
        return -EINVAL;
    }
    *ticks = (uint32_t)(ts[0] > 21474836 ? 21474836 : ts[0]) * 100u +
             ((uint32_t)ts[1] + 9999999u) / 10000000u;
    return 0;
}

/** ipc's SEMTIMEDOP and SEMOP: the operations and the optional relative
 * timeout at `uts`. */
static int32_t sem_timed(int32_t id, const void __user *uops, uint32_t n, const void __user *uts)
{
    uint32_t ticks = 0;
    int32_t rc = uts ? sysv_ticks32(uts, &ticks) : 0;

    return rc != 0 ? rc : sem_op(id, uops, n, uts != NULL, ticks);
}

/** ipc's SHMAT, which stores the address at `third` and returns 0. */
static int32_t shm_at_ipc(int32_t id, uintptr_t addr, uint32_t flag, void __user *third)
{
    uintptr_t base;
    int32_t rc = shm_at(id, addr, flag, &base);

    if (rc == 0 && copy_to_user(third, &base, sizeof(base)) != 0) {
        rc = -EFAULT;
    }
    return rc;
}

/** ipc's MSGRCV: version 0 has the buffer and the type in a pair at `ptr`,
 * version 1 the buffer at `ptr` and the type in `fifth`. */
static int32_t msg_rcv_ipc(int version, int32_t id, int32_t size, uint32_t flag, void __user *ptr,
                           long fifth)
{
    struct {
        uintptr_t msgp;
        long msgtyp;
    } kludge; /* Linux's struct ipc_kludge */

    if (version != 0) {
        return msg_rcv(id, ptr, size, fifth, flag);
    }
    if (!ptr) {
        return -EINVAL;
    }
    if (copy_from_user(&kludge, ptr, sizeof(kludge)) != 0) {
        return -EFAULT;
    }
    return msg_rcv(id, (void __user *)kludge.msgp, size, kludge.msgtyp, flag);
}

long sys_ipc(struct isr_params *params)
{
    uint32_t call = SC_ARG(params, 0) & 0xFFFFu;
    int version = (int)(SC_ARG(params, 0) >> 16);
    int32_t first = (int32_t)SC_ARG(params, 1);
    uint32_t second = (uint32_t)SC_ARG(params, 2);
    uintptr_t third = SC_ARG(params, 3), ptr = SC_ARG(params, 4);
    unsigned long arg;

    switch (call) {
    case SEMOP:
        return sem_timed(first, (const void __user *)ptr, second, NULL);
    case SEMTIMEDOP:
        return sem_timed(first, (const void __user *)ptr, second,
                         (const void __user *)SC_ARG(params, 5));
    case SEMGET:
        return sem_get(first, (int32_t)second, (uint32_t)third);
    case SEMCTL:
        if (!ptr) {
            return -EINVAL;
        }
        if (copy_from_user(&arg, (const void __user *)ptr, sizeof(arg)) != 0) {
            return -EFAULT;
        }
        return sem_ctl(first, (int)second, (int)third, arg);
    case MSGSND:
        return msg_snd(first, (const void __user *)ptr, second, (uint32_t)third);
    case MSGRCV:
        return msg_rcv_ipc(version, first, (int32_t)second, (uint32_t)third, (void __user *)ptr,
                           (long)SC_ARG(params, 5));
    case MSGGET:
        return msg_get(first, second);
    case MSGCTL:
        return msg_ctl(first, (int)second, (void __user *)ptr);
    case SHMAT:
        return version == 1 ? -EINVAL : shm_at_ipc(first, ptr, second, (void __user *)third);
    case SHMDT:
        return shm_dt(ptr);
    case SHMGET:
        return shm_get(first, second, (uint32_t)third);
    case SHMCTL:
        return shm_ctl(first, (int)second, (void __user *)ptr);
    default:
        return -ENOSYS;
    }
}

long sys_semget(struct isr_params *params)
{
    return sem_get((int32_t)SC_ARG(params, 0), (int32_t)SC_ARG(params, 1),
                   (uint32_t)SC_ARG(params, 2));
}

long sys_semctl(struct isr_params *params)
{
    return sem_ctl((int32_t)SC_ARG(params, 0), (int)SC_ARG(params, 1), (int)SC_ARG(params, 2),
                   SC_ARG(params, 3));
}

long sys_shmget(struct isr_params *params)
{
    return shm_get((int32_t)SC_ARG(params, 0), (size_t)SC_ARG(params, 1),
                   (uint32_t)SC_ARG(params, 2));
}

long sys_shmctl(struct isr_params *params)
{
    return shm_ctl((int32_t)SC_ARG(params, 0), (int)SC_ARG(params, 1),
                   (void __user *)SC_ARG(params, 2));
}

long sys_shmat(struct isr_params *params)
{
    uintptr_t base;
    int32_t rc =
        shm_at((int32_t)SC_ARG(params, 0), SC_ARG(params, 1), (uint32_t)SC_ARG(params, 2), &base);

    return rc != 0 ? rc : (long)base;
}

long sys_shmdt(struct isr_params *params)
{
    return shm_dt(SC_ARG(params, 0));
}

long sys_msgget(struct isr_params *params)
{
    return msg_get((int32_t)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1));
}

long sys_msgsnd(struct isr_params *params)
{
    return msg_snd((int32_t)SC_ARG(params, 0), (const void __user *)SC_ARG(params, 1),
                   USER_LEN32(SC_ARG(params, 2)), (uint32_t)SC_ARG(params, 3));
}

long sys_msgrcv(struct isr_params *params)
{
    return msg_rcv((int32_t)SC_ARG(params, 0), (void __user *)SC_ARG(params, 1),
                   (int32_t)SC_ARG(params, 2), (long)SC_ARG(params, 3),
                   (uint32_t)SC_ARG(params, 4));
}

long sys_msgctl(struct isr_params *params)
{
    return msg_ctl((int32_t)SC_ARG(params, 0), (int)SC_ARG(params, 1),
                   (void __user *)SC_ARG(params, 2));
}

/** semtimedop_time64: the timeout a 64-bit timespec. */
long sys_semtimedop_time64(struct isr_params *params)
{
    int64_t ts[2];
    uint32_t ticks = 0;

    if (SC_ARG(params, 3)) {
        if (copy_from_user(ts, (const void __user *)SC_ARG(params, 3), sizeof(ts)) != 0) {
            return -EFAULT;
        }
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) {
            return -EINVAL;
        }
        ticks = (uint32_t)(ts[0] > 21474836 ? 21474836 : ts[0]) * 100u +
                ((uint32_t)ts[1] + 9999999u) / 10000000u;
    }
    return sem_op((int32_t)SC_ARG(params, 0), (const void __user *)SC_ARG(params, 1),
                  (uint32_t)SC_ARG(params, 2), SC_ARG(params, 3) != 0, ticks);
}

int sysv_row_at(int kind, int i, struct sysv_row *row)
{
    const struct sysv_perm *p;

    if (i >= SYSV_SLOTS) {
        return -1;
    }
    if (kind == SYSV_SHM && i == 0) {
        shm_reap();
    }
    p = tables[kind][i];
    if (!p || !sysv_mine(p)) {
        return 0;
    }
    k_memset(row, 0, sizeof(*row));
    row->key = p->key;
    row->id = sysv_id(p);
    row->mode = p->mode;
    row->uid = uid_seen(p->uid);
    row->gid = gid_seen(p->gid);
    row->cuid = uid_seen(p->cuid);
    row->cgid = gid_seen(p->cgid);
    if (kind == SYSV_SHM) {
        const struct shm_seg *s = (const struct shm_seg *)p;

        row->a = (uint32_t)s->size == s->size ? (uint32_t)s->size : 0xFFFFFFFFu;
        row->b = (uint32_t)sysv_pid_seen(s->cpid);
        row->c = (uint32_t)sysv_pid_seen(s->lpid);
        row->d = shm_attaches(shm_mem(s));
        row->t0 = s->atime;
        row->t1 = s->dtime;
        row->t2 = s->ctime;
    } else if (kind == SYSV_SEM) {
        const struct sem_set *set = (const struct sem_set *)p;

        row->a = set->nsems;
        row->t0 = set->otime;
        row->t1 = set->ctime;
    } else {
        const struct msg_queue *q = (const struct msg_queue *)p;

        row->a = q->cbytes;
        row->b = q->qnum;
        row->c = (uint32_t)sysv_pid_seen(q->lspid);
        row->e = (uint32_t)sysv_pid_seen(q->lrpid);
        row->t0 = q->stime;
        row->t1 = q->rtime;
        row->t2 = q->ctime;
    }
    return 1;
}

void ipc_ns_gone(struct ipcns *ns)
{
    int i;

    for (i = 0; i < SYSV_SLOTS; i++) {
        struct shm_seg *s = (struct shm_seg *)tables[SYSV_SHM][i];
        struct sem_set *set = (struct sem_set *)tables[SYSV_SEM][i];
        struct msg_queue *q = (struct msg_queue *)tables[SYSV_MSG][i];

        if (s && s->perm.ns == ns) {
            s->perm.mode |= SHM_DEST;
            s->perm.key = IPC_PRIVATE;
            s->perm.ns = &ipc_gone;
        }
        if (set && set->perm.ns == ns) {
            sysv_leave(SYSV_SEM, &set->perm);
            wq_wake_all(&set->wq);
            sem_put(set);
        }
        if (q && q->perm.ns == ns) {
            sysv_leave(SYSV_MSG, &q->perm);
            wq_wake_all(&q->wq);
            msg_put(q);
        }
    }
    shm_reap();
    mq_ns_gone(ns);
}
