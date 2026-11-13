/* File descriptor tables and the open file descriptions they share. */
#include "fd.h"
#include "../proc/process.h"
#include "../proc/smp.h"

#include "vfs.h"
#include "name.h"
#include "lock.h"
#include "../io/inotify.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../arch/arch.h"
#include "../mm/kheap.h"

/*
 * The reservations of one file, by the descriptions that hold them: Linux's
 * i_writecount, kept here by the file's mount base and node, since a node is
 * each file system's own. A record lives while either count is above zero.
 * Each description brings one record (`ofd_rec.spare`): the first holder's
 * becomes the file's record and the others' wait on `spares`, one fewer than
 * the holders, so that a holder leaving takes one back and neither reserving
 * nor leaving needs memory, as i_writecount needs none.
 */
struct access_count {
    struct access_count *next;    /* the bucket's chain, or the next spare */
    const struct vfs_mount *base; /* vfs_mount_base of the holders' mount */
    const void *node;
    unsigned long count[2];      /* the writers, the executables */
    struct access_count *spares; /* the other holders' records */
};

/*
 * The record of an open file description, from the heap, on the list of live
 * descriptions that the scans below walk. The wait queue of the description
 * lock is in the record rather than in fd.h, because the wait-queue header
 * includes the process header and the process header includes fd.h; the
 * description is the record's first member, so the one is found from the
 * other. `spare` is the count record the description brings to a
 * reservation, made with it, so that reserving needs no memory: NULL while
 * it holds a reservation, whose record then keeps it.
 */
struct ofd_rec {
    struct open_file ofd;
    wait_queue_t wq;
    struct ofd_rec *next, *prev; /* the live list, newest first */
    struct access_count *spare;
    /* The allocating thread group's initial-namespace pid, or 0 at boot. */
    uint32_t creator;
    /* Nonzero for a reader's place on the list (`fd_files_creators`), which
     * is no description: its `ofd` is zero, so the scans pass it over. */
    int cursor;
};

/*
 * The list of live descriptions, its count and serials, and the access
 * counts are under `ofd_lock`, a spinlock taken with interrupts off below a
 * table's lock; a description's own fields are its holders'. The scans of
 * the list read only what their callers' locks keep still. The list is
 * doubly linked, so that a description leaves it in constant time.
 */
static struct spinlock ofd_lock = SPINLOCK_INIT;
static struct ofd_rec *ofds;
static uint32_t ofd_serials;
static unsigned long ofd_count; /* records on `ofds` */

int fd_nr_open = FD_NR_OPEN;
unsigned long fd_file_max = ~0ul >> 1;

unsigned long fd_files_open(void)
{
    return ofd_count;
}

/** Put `rec` at the head of the live list. `ofd_lock` is held. */
static void ofd_link(struct ofd_rec *rec)
{
    rec->prev = NULL;
    rec->next = ofds;
    if (ofds) {
        ofds->prev = rec;
    }
    ofds = rec;
}

/** Put `rec` on the live list just after `at`. `ofd_lock` is held. */
static void ofd_link_after(struct ofd_rec *rec, struct ofd_rec *at)
{
    rec->prev = at;
    rec->next = at->next;
    if (at->next) {
        at->next->prev = rec;
    }
    at->next = rec;
}

/** Take `rec` off the live list. `ofd_lock` is held. */
static void ofd_unlink(struct ofd_rec *rec)
{
    if (rec->prev) {
        rec->prev->next = rec->next;
    } else {
        ofds = rec->next;
    }
    if (rec->next) {
        rec->next->prev = rec->prev;
    }
    rec->next = NULL;
    rec->prev = NULL;
}

/** Write `n` in decimal and a newline at `len` in `buf`'s `cap` bytes, and
 * return the length after it, counting bytes that did not fit. */
static uint32_t creator_put(char *buf, uint32_t cap, uint32_t len, uint32_t n)
{
    char digits[11];
    unsigned int i = 0;

    do {
        digits[i++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n);
    while (i) {
        if (len < cap) {
            buf[len] = digits[i - 1u];
        }
        len++;
        i--;
    }
    if (len < cap) {
        buf[len] = '\n';
    }
    return len + 1u;
}

/* The records `fd_files_creators` passes in one hold of `ofd_lock`. */
#define CREATORS_BATCH 32u

/**
 * The walk holds `ofd_lock` for at most CREATORS_BATCH records at a time and
 * formats with the lock given back. Between holds a cursor record keeps its
 * place on the list, as a cursor dentry keeps dcache_readdir's among a
 * directory's children in Linux (`scan_positives`, fs/libfs.c:107-140): a
 * description leaving the list unlinks itself around the cursor, and one
 * arriving goes to the head, before it. Linux keeps no list of files at all,
 * only the count (`nr_files`, fs/file_table.c:43 and 103-106).
 */
int32_t fd_files_creators(char *buf, uint32_t cap)
{
    struct ofd_rec *cursor = kzalloc(sizeof(*cursor));
    uint32_t ids[CREATORS_BATCH];
    uint32_t len = 0, flags;
    int more;

    if (!cursor) {
        return -ENOMEM;
    }
    cursor->cursor = 1;
    flags = spin_lock_irqsave(&ofd_lock);
    ofd_link(cursor);
    spin_unlock_irqrestore(&ofd_lock, flags);
    do {
        struct ofd_rec *rec, *last = cursor;
        uint32_t n = 0, steps = 0, i;

        flags = spin_lock_irqsave(&ofd_lock);
        for (rec = cursor->next; rec && steps < CREATORS_BATCH; rec = rec->next, steps++) {
            if (!rec->cursor) {
                ids[n++] = rec->creator;
            }
            last = rec;
        }
        more = rec != NULL;
        if (last != cursor) {
            ofd_unlink(cursor);
            ofd_link_after(cursor, last);
        }
        spin_unlock_irqrestore(&ofd_lock, flags);
        for (i = 0; i < n; i++) {
            len = creator_put(buf, cap, len, ids[i]);
        }
    } while (more);
    flags = spin_lock_irqsave(&ofd_lock);
    ofd_unlink(cursor);
    spin_unlock_irqrestore(&ofd_lock, flags);
    kfree(cursor);
    return (int32_t)len;
}

/** Add `delta`, 1 or -1, to the count at `p` in one atomic step, and return
 * the count after it. */
static uint32_t count_add(uint32_t *p, int delta)
{
    unsigned int old;

    do {
        old = *(volatile unsigned int *)p;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)p, old, old + (unsigned int)delta) !=
             old);
    return old + (unsigned int)delta;
}

/** The record whose first member is `ofd`. */
static struct ofd_rec *ofd_rec_of(struct open_file *ofd)
{
    return (struct ofd_rec *)ofd;
}

/**
 * A new open file description on the live list, with one reference, the next
 * serial, zero offsets, no status flags and no read yet for its readahead;
 * NULL when the heap has no room.
 */
static struct open_file *ofd_alloc(void)
{
    struct ofd_rec *rec = kzalloc(sizeof(*rec));
    uint32_t flags;

    if (!rec) {
        return NULL;
    }
    rec->spare = (struct access_count *)kzalloc(sizeof(*rec->spare));
    if (!rec->spare) {
        kfree(rec);
        return NULL;
    }
    rec->ofd.refcount = 1;
    rec->ofd.ra.prev_pos = ~(uint64_t)0;
    rec->creator = current_proc ? current_proc->tgid : 0;
    wait_queue_init(&rec->wq);
    flags = spin_lock_irqsave(&ofd_lock);
    rec->ofd.serial = ++ofd_serials;
    ofd_link(rec);
    ofd_count++;
    spin_unlock_irqrestore(&ofd_lock, flags);
    return &rec->ofd;
}

struct open_file *fd_ofd_new(void)
{
    return ofd_alloc();
}

/* The buckets of `access_counts`, a power of two. */
#define ACCESS_BUCKETS 64u

static struct access_count *access_counts[ACCESS_BUCKETS];

/** The bucket of the file `node` under the mount base `base`. */
static uint32_t access_bucket(const struct vfs_mount *base, const void *node)
{
    uintptr_t k = (uintptr_t)node ^ ((uintptr_t)base >> 4);

    return (uint32_t)((k >> 4) ^ (k >> 12)) & (ACCESS_BUCKETS - 1u);
}

/** The record of `node` under `mnt`'s base, or NULL when it has none. */
static struct access_count *access_find(const struct vfs_mount *mnt, const void *node)
{
    const struct vfs_mount *base = vfs_mount_base(mnt);
    struct access_count *a;

    for (a = access_counts[access_bucket(base, node)]; a; a = a->next) {
        if (a->base == base && a->node == node) {
            return a;
        }
    }
    return NULL;
}

/** Count `rec`'s reservation of kind `access`, 1 or 2, of `node` under
 * `mnt`, giving the file's record its spare: as the record when the file has
 * none, or onto its spares. `rec` holds no reservation. 0, or -ENOMEM only
 * when `rec` has no spare, which `access_leave` rules out. */
static int access_join(struct ofd_rec *rec, const struct vfs_mount *mnt, const void *node,
                       int access)
{
    struct access_count *a = access_find(mnt, node), *s = rec->spare, **b;

    if (!s) {
        return -ENOMEM;
    }
    rec->spare = NULL;
    if (a) {
        s->next = a->spares;
        a->spares = s;
    } else {
        k_memset(s, 0, sizeof(*s));
        s->base = vfs_mount_base(mnt);
        s->node = node;
        b = &access_counts[access_bucket(s->base, node)];
        s->next = *b;
        *b = s;
        a = s;
    }
    a->count[access - 1]++;
    return 0;
}

/** Give back `rec`'s reservation of kind `access` of `node` under `mnt`,
 * taking a spare back from the file's record, or the record itself when
 * it holds no more. */
static void access_leave(struct ofd_rec *rec, const struct vfs_mount *mnt, const void *node,
                         int access)
{
    struct access_count *a = access_find(mnt, node), **p;

    if (!a || a->count[access - 1] == 0 || (a->count[0] + a->count[1] > 1 && !a->spares)) {
        /* A count out of step with its holders: give `rec` a new spare. */
        rec->spare = (struct access_count *)kzalloc(sizeof(*a));
        return;
    }
    if (--a->count[access - 1] > 0 || a->count[2 - access] > 0) {
        rec->spare = a->spares;
        a->spares = a->spares->next;
        return;
    }
    for (p = &access_counts[access_bucket(a->base, node)]; *p != a; p = &(*p)->next) {
    }
    *p = a->next;
    rec->spare = a;
}

/** `fd_ofd_access_drop` with `ofd_lock` held. */
static void access_drop_locked(struct open_file *ofd)
{
    if (ofd->access) {
        access_leave(ofd_rec_of(ofd), ofd->access_mnt, ofd->access_node, ofd->access);
        ofd->access = 0;
    }
}

void fd_ofd_access_drop(struct open_file *ofd)
{
    uint32_t flags = spin_lock_irqsave(&ofd_lock);

    access_drop_locked(ofd);
    spin_unlock_irqrestore(&ofd_lock, flags);
}

int fd_ofd_access(struct open_file *ofd, const struct vfs_ref *ref, int access)
{
    uint32_t flags = spin_lock_irqsave(&ofd_lock);
    struct access_count *a = access_find(ref->mnt, ref->node);
    int rc = 0;

    if (a && a->count[2 - access] > 0) {
        rc = -ETXTBSY;
    } else if (a && ofd->access == access && ofd->access_node == ref->node &&
               vfs_mount_base(ofd->access_mnt) == vfs_mount_base(ref->mnt)) {
        ofd->access_mnt = ref->mnt;
    } else {
        access_drop_locked(ofd);
        if (access_join(ofd_rec_of(ofd), ref->mnt, ref->node, access) != 0) {
            rc = -ENOMEM;
        } else {
            ofd->access_mnt = ref->mnt;
            ofd->access_node = ref->node;
            ofd->access = access;
        }
    }
    spin_unlock_irqrestore(&ofd_lock, flags);
    return rc;
}

void fd_ofd_ref(struct open_file *ofd)
{
    if (ofd) {
        count_add(&ofd->refcount, 1);
    }
}

void fd_ofd_status_set(struct open_file *ofd, uint32_t clear, uint32_t set)
{
    unsigned int old;

    do {
        old = *(volatile unsigned int *)&ofd->status_flags;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)&ofd->status_flags, old,
                                 (old & ~clear) | set) != old);
}

int fd_ofd_unref_unless_last(struct open_file *ofd)
{
    unsigned int old;

    do {
        old = *(volatile unsigned int *)&ofd->refcount;
        if (old <= 1) {
            return 0;
        }
    } while (arch_atomic_cmpxchg((volatile unsigned int *)&ofd->refcount, old, old - 1) != old);
    return 1;
}

/** The wait condition of the description lock: `arg`'s lock is free. */
static int ofd_idle(void *arg)
{
    return ((struct open_file *)arg)->busy == 0;
}

/**
 * The description lock comes before the namespace lock, as Linux's
 * `f_pos_lock` (fs/file.c:1167) comes before `i_rwsem`: a read(2) holds the
 * description and may fault into a file mapping, whose fill takes the
 * namespace lock (`fs_read_at`). `busy` is taken by an atomic exchange,
 * since a read of a file of a type that sets `reads_unlocked` takes it
 * without the big lock; the waiter is on the queue before it looks again,
 * so the give's wake does not pass it by.
 */
void fd_ofd_lock(struct open_file *ofd)
{
    volatile unsigned int *busy = (volatile unsigned int *)&ofd->busy;

    while (arch_atomic_cmpxchg(busy, 0, 1) != 0) {
        wq_wait_event_nointr(&ofd_rec_of(ofd)->wq, ofd_idle, ofd);
    }
    if (ofd->vmnt && ofd->vmnt->type->ns_serialised) {
        vfs_ns_lock(ofd->vmnt);
    }
}

void fd_ofd_unlock(struct open_file *ofd)
{
    if (ofd->vmnt && ofd->vmnt->type->ns_serialised) {
        vfs_ns_unlock(ofd->vmnt);
    }
    (void)arch_atomic_xchg((volatile unsigned int *)&ofd->busy, 0);
    wq_wake_all(&ofd_rec_of(ofd)->wq);
}

int fd_node_busy(struct vfs_mount *mnt, void *node)
{
    uint32_t flags = spin_lock_irqsave(&ofd_lock);
    struct ofd_rec *rec;
    int busy = 0;

    for (rec = ofds; rec && !busy; rec = rec->next) {
        busy = rec->ofd.vmnt && vfs_mount_base(rec->ofd.vmnt) == vfs_mount_base(mnt) &&
               rec->ofd.vnode == node && rec->ofd.busy;
    }
    spin_unlock_irqrestore(&ofd_lock, flags);
    return busy;
}

int fd_ofd_live(const struct open_file *ofd, uint32_t serial)
{
    uint32_t flags = spin_lock_irqsave(&ofd_lock);
    const struct ofd_rec *rec;
    int live = 0;

    for (rec = ofds; rec; rec = rec->next) {
        if (&rec->ofd == ofd) {
            live = rec->ofd.refcount > 0 && rec->ofd.serial == serial;
            break;
        }
    }
    spin_unlock_irqrestore(&ofd_lock, flags);
    return live;
}

int fd_ofd_holders(const struct vfs_mount *mnt, const void *node, const struct open_file *going)
{
    uint32_t flags = spin_lock_irqsave(&ofd_lock);
    struct ofd_rec *rec;
    int held = 0;

    for (rec = ofds; rec; rec = rec->next) {
        if (rec->ofd.vmnt && vfs_mount_base(rec->ofd.vmnt) == vfs_mount_base(mnt) &&
            rec->ofd.vnode == node && (&rec->ofd != going || rec->ofd.refcount > 1)) {
            held++;
        }
    }
    spin_unlock_irqrestore(&ofd_lock, flags);
    return held;
}

/**
 * The root's adapter, `vfs_krffs_type`, uses the node as the handle of an
 * open too, so the handle moves with the node; a mounted KRFFS volume's
 * handle is an open record of its own, which stays where it is.
 */
void fd_ofd_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    uint32_t flags = spin_lock_irqsave(&ofd_lock);
    struct ofd_rec *rec;

    for (rec = ofds; rec; rec = rec->next) {
        if (rec->ofd.access_mnt == mnt && rec->ofd.access_node == from) {
            if (rec->ofd.access) {
                access_leave(rec, mnt, from, rec->ofd.access);
                if (access_join(rec, mnt, to, rec->ofd.access) != 0) {
                    rec->ofd.access = 0;
                }
            }
            rec->ofd.access_node = to;
        }
        if (rec->ofd.vmnt != mnt) {
            continue;
        }
        if (rec->ofd.vnode == from) {
            rec->ofd.vnode = to;
        }
        if (mnt->type == &vfs_krffs_type && rec->ofd.fhandle == from) {
            rec->ofd.fhandle = to;
        }
    }
    spin_unlock_irqrestore(&ofd_lock, flags);
}

/**
 * Whether the end of description `ofd`, whose count reached 0, needs the
 * big lock: unless it names no file, or names a file or a directory of a
 * type that sets `reads_unlocked`, as ext2 does, with no pin, no access
 * claim and no device or synthetic node, and in either case no inotify
 * watch is on its file system (`inotify_mount_watched`). Such a type's
 * `release` and `put` and the name record (`vfs_name_put`) take the big
 * lock where they need it, as Linux's last `fput` of a file takes no lock
 * but its own (`__fput`, fs/file_table.c:442). The description's own record
 * locks go under their list's lock (`lock_ofd_gone`).
 */
static int ofd_end_big(const struct open_file *ofd)
{
    if (inotify_mount_watched(ofd->vmnt) && (ofd->watch_flags & OFD_WATCH_OPEN)) {
        return 1;
    }
    if (ofd->vmnt) {
        return ofd->dev_mnt || ofd->synth_mnt || ofd->pin || ofd->access ||
               !vfs_mount_base(ofd->vmnt)->type->reads_unlocked;
    }
    return ofd->dev_mnt || ofd->synth_mnt || ofd->name || ofd->pin || ofd->access || ofd->dirpath ||
           ofd->leaf;
}

/**
 * The count falls in one atomic step, so of two holders that let go at once
 * on different processors exactly one sees it reach 0 and ends the
 * description. The end may sleep, in a file system's release. It takes the
 * big lock, which most file systems and inotify still need, where
 * `ofd_end_big` says. With the count at 0 no descriptor names the
 * description, so no lock of it can be taken after `lock_any` looks, as
 * Linux's locks_remove_file looks at the file's lists without their lock
 * (fs/locks.c:2698-2706).
 */
void fd_ofd_unref(struct open_file *ofd)
{
    if (ofd && ofd->refcount > 0) {
        if (count_add(&ofd->refcount, -1) == 0) {
            struct ofd_rec *rec = ofd_rec_of(ofd);
            int big = ofd_end_big(ofd);
            uint32_t flags;

            if (big) {
                bkl_enter();
            }

            flags = spin_lock_irqsave(&ofd_lock);
            access_drop_locked(ofd);
            spin_unlock_irqrestore(&ofd_lock, flags);
            kfree(rec->spare);
            rec->spare = NULL;
            if (lock_any()) {
                lock_ofd_gone(ofd);
            }
            if (big && ofd->vmnt && (ofd->watch_flags & OFD_WATCH_OPEN)) {
                inotify_desc(ofd, (ofd->watch_flags & OFD_WATCH_WRITE) ? IN_CLOSE_WRITE
                                                                       : IN_CLOSE_NOWRITE);
            }
            vfs_name_put(ofd->name);
            ofd->name = NULL;
            kfree(ofd->leaf);
            ofd->leaf = NULL;
            kfree(ofd->dirpath);
            ofd->dirpath = NULL;
            if (ofd->vmnt) {
                struct vfs_mount *mnt = ofd->vmnt;
                void *node = ofd->vnode;
                void *handle = ofd->fhandle;
                void *pin = ofd->pin;

                ofd->vmnt = NULL;
                ofd->vnode = NULL;
                ofd->fhandle = NULL;
                ofd->pin = NULL;
                if (pin) {
                    mnt->type->ops->unpin(mnt, pin);
                }
                vfs_desc_close(mnt, node, handle);
            }
            if (ofd->dev_mnt) {
                struct vfs_mount *mnt = ofd->dev_mnt;
                void *node = ofd->dev_node;

                ofd->dev_mnt = NULL;
                ofd->dev_node = NULL;
                vfs_desc_close(mnt, node, NULL);
            }
            if (ofd->synth_mnt) {
                struct vfs_ref ref;

                ref.mnt = ofd->synth_mnt;
                ref.node = ref.mnt->root;
                ofd->synth_mnt = NULL;
                vfs_desc_close(ref.mnt, ref.node, NULL);
            }
            flags = spin_lock_irqsave(&ofd_lock);
            ofd_unlink(rec);
            ofd_count--;
            spin_unlock_irqrestore(&ofd_lock, flags);
            kfree(rec);
            if (big) {
                bkl_exit();
            }
        }
    }
}

/*
 * A table's entries are in chunks that never move: chunk 0 holds the first
 * FD_TABLE_FIRST descriptors and chunk k > 0 the FD_TABLE_FIRST << (k - 1)
 * after those of the chunks before it, so each growth past a chunk's end
 * adds one chunk as large as all before it. An entry's address is its slot's
 * for the table's life, which lets a call keep it across a sleep while
 * another sharer grows the table, and a chunk is published before the `cap`
 * that reaches into it, so that a reader of a slot below `cap` needs no
 * lock to find it.
 */

/** The chunk that holds descriptor `fd`, and the index of its first. */
static int chunk_of(int fd, int *first)
{
    unsigned int q = (unsigned int)fd / FD_TABLE_FIRST;
    int k = 0;

    while (q != 0) {
        q >>= 1;
        k++;
    }
    *first = k == 0 ? 0 : FD_TABLE_FIRST << (k - 1);
    return k;
}

/** How many entries chunk `k` holds. */
static int chunk_size(int k)
{
    return k == 0 ? FD_TABLE_FIRST : FD_TABLE_FIRST << (k - 1);
}

fd_entry_t *fd_slot(const fd_table_t *table, int fd)
{
    int first;
    int k = chunk_of(fd, &first);

    return &table->chunks[k][fd - first];
}

uint32_t fd_table_lock(fd_table_t *table)
{
    return spin_lock_irqsave(&table->lock);
}

void fd_table_unlock(fd_table_t *table, uint32_t flags)
{
    spin_unlock_irqrestore(&table->lock, flags);
}

/**
 * Give `table` the chunks that reach descriptor `cap` - 1, publishing each
 * before the `cap` that covers it, and raise `cap`. The caller holds the
 * lock, or is the only one that has the table; the heap does not sleep.
 * 0, or -ENOMEM with `cap` as it was.
 */
static int chunks_reach(fd_table_t *table, int cap)
{
    int first;
    int last = chunk_of(cap - 1, &first);
    int k;

    for (k = 0; k <= last; k++) {
        if (!table->chunks[k]) {
            fd_entry_t *c = kzalloc(sizeof(fd_entry_t) * (size_t)chunk_size(k));

            if (!c) {
                return -ENOMEM;
            }
            *(fd_entry_t *volatile *)(void *)&table->chunks[k] = c;
        }
    }
    *(volatile int *)&table->cap = cap;
    return 0;
}

int fd_table_init(fd_table_t *table)
{
    if (!table) {
        return -1;
    }
    k_memset(table->chunks, 0, sizeof(table->chunks));
    table->cap = 0;
    table->next_free = 0;
    table->sharers = NULL;
    spin_lock_init(&table->lock, "fd_table->lock");
    return chunks_reach(table, FD_TABLE_FIRST) == 0 ? 0 : -1;
}

void fd_table_free(fd_table_t *table)
{
    int k;

    if (!table) {
        return;
    }
    for (k = 0; k < FD_CHUNKS; k++) {
        kfree(table->chunks[k]);
        table->chunks[k] = NULL;
    }
    table->cap = 0;
    table->next_free = 0;
}

int fd_table_cap(const fd_table_t *table)
{
    return table ? *(const volatile int *)&table->cap : 0;
}

int fd_limit(const fd_table_t *table)
{
    unsigned long cur;

    if (!current_proc || table != current_proc->fds) {
        return fd_nr_open;
    }
    cur = current_proc->rlim[RLIM_NOFILE].cur;
    return cur < (unsigned long)fd_nr_open ? (int)cur : fd_nr_open;
}

fd_table_t *fd_table_new(void)
{
    fd_table_t *table = kzalloc(sizeof(*table));

    if (!table) {
        return NULL;
    }
    if (fd_table_init(table) != 0) {
        fd_table_delete(table);
        return NULL;
    }
    table->refs = 1;
    return table;
}

/** Whether entry `e` is a slot no lookup finds and no copy keeps: free,
 * reserved by an open that has not finished, or being filled. */
static int slot_unready(const fd_entry_t *e)
{
    return e->type == FD_TYPE_NONE || e->pending;
}

/**
 * Copy `src`'s entries into `dst`, a table nobody else has, under `src`'s
 * lock, first raising `dst` to `src`'s size; with `take`, clear in the copy
 * each slot that is not ready or is closing and add a reference to each
 * description the copy names, before the lock goes, so that no closer of a
 * sharer frees one in between. A slot with no type is free or reserved by
 * an unfinished open of a sharer, whose description the copy has no
 * reference to, so it is cleared in the copy either way. 0, or -ENOMEM when
 * the heap has no room for `dst`'s growth.
 */
static int entries_copy(fd_table_t *dst, fd_table_t *src, int take)
{
    uint32_t flags = fd_table_lock(src);
    int i;

    if (dst->cap < src->cap && chunks_reach(dst, src->cap) != 0) {
        fd_table_unlock(src, flags);
        return -ENOMEM;
    }
    for (i = 0; i < src->cap; i++) {
        fd_entry_t *d = fd_slot(dst, i);

        *d = *fd_slot(src, i);
        if (d->type == FD_TYPE_NONE || (take && (d->pending || (d->flags & FD_FLAG_CLOSING)))) {
            k_memset(d, 0, sizeof(*d));
        } else if (take) {
            fd_ofd_ref(d->ofd);
        }
    }
    fd_table_unlock(src, flags);
    return 0;
}

/** A table from the heap with `src`'s entries and one reference, or NULL
 * when the heap has no room; `take` as `entries_copy` has it. */
static fd_table_t *table_copy(fd_table_t *src, int take)
{
    fd_table_t *table = kzalloc(sizeof(*table));

    if (!table) {
        return NULL;
    }
    spin_lock_init(&table->lock, "fd_table->lock");
    if (entries_copy(table, src, take) != 0) {
        fd_table_delete(table);
        return NULL;
    }
    table->refs = 1;
    return table;
}

fd_table_t *fd_table_dup(fd_table_t *src)
{
    return table_copy(src, 0);
}

fd_table_t *fd_table_copy(fd_table_t *src)
{
    return table_copy(src, 1);
}

int fd_table_take(fd_table_t *dst, fd_table_t *src)
{
    int k;

    for (k = 0; k < FD_CHUNKS && dst->chunks[k]; k++) {
        k_memset(dst->chunks[k], 0, sizeof(fd_entry_t) * (size_t)chunk_size(k));
    }
    dst->next_free = 0;
    return entries_copy(dst, src, 1);
}

void fd_table_delete(fd_table_t *table)
{
    fd_table_free(table);
    kfree(table);
}

/** Widen the hold map of `t` to cover `cap` descriptors, its bits kept;
 * 0, or -ENOMEM. The caller holds the lock of `t`'s table, or `t` shares
 * none yet. */
static int held_fit(process_t *t, int cap)
{
    int words = (cap + 31) / 32;
    uint32_t *map;

    if (t->fd_held_words >= words) {
        return 0;
    }
    map = kzalloc(sizeof(uint32_t) * (size_t)words);
    if (!map) {
        return -ENOMEM;
    }
    if (t->fd_held) {
        k_memcpy(map, t->fd_held, sizeof(uint32_t) * (size_t)t->fd_held_words);
        kfree(t->fd_held);
    }
    t->fd_held = map;
    t->fd_held_words = words;
    return 0;
}

/** Widen the hold map of every sharer of `table` to `cap`, under its lock;
 * 0, or -ENOMEM, with the maps widened so far left so, which is harmless. */
static int sharers_fit(fd_table_t *table, int cap)
{
    process_t *t;

    for (t = table->sharers; t; t = t->fd_sharer_next) {
        if (held_fit(t, cap) != 0) {
            return -ENOMEM;
        }
    }
    return 0;
}

/**
 * Raise `table` to `cap` entries under its lock, the maps of a shared
 * table's sharers widened first, so that each sharer's map is as wide as
 * the table. Nothing here sleeps, and no entry moves. 0, or -ENOMEM when the
 * heap has no room.
 */
static int fd_table_resize(fd_table_t *table, int cap)
{
    if (cap <= table->cap) {
        return 0;
    }
    if (table->refs > 1 && sharers_fit(table, cap) != 0) {
        return -ENOMEM;
    }
    return chunks_reach(table, cap);
}

/** Grow the table, under its lock, by doubling to at least `want` entries,
 * and to no more than `fd_nr_open` past that; 0, -EMFILE when `want` is
 * past the caller's limit, or -ENOMEM when the heap has no room. */
static int fd_table_grow(fd_table_t *table, int want)
{
    int cap = table->cap ? table->cap : FD_TABLE_FIRST;

    if (want > fd_limit(table)) {
        return -EMFILE;
    }
    while (cap < want) {
        cap *= 2;
    }
    if (cap > fd_nr_open && fd_nr_open >= want) {
        cap = fd_nr_open;
    }
    return fd_table_resize(table, cap);
}

void fd_table_join(fd_table_t *table, struct process *p)
{
    uint32_t flags = fd_table_lock(table);

    p->fds = table;
    p->fd_sharer_next = table->sharers;
    table->sharers = p;
    fd_table_unlock(table, flags);
}

int fd_table_put(struct process *p)
{
    fd_table_t *table = p->fds;
    process_t **link;
    uint32_t flags;

    if (!table) {
        return 0;
    }
    flags = fd_table_lock(table);
    if (table->refs <= 1) {
        fd_table_unlock(table, flags);
        return 1;
    }
    for (link = &table->sharers; *link && *link != p; link = &(*link)->fd_sharer_next) {
    }
    if (*link) {
        *link = p->fd_sharer_next;
    }
    p->fd_sharer_next = NULL;
    table->refs--;
    p->fds = NULL;
    fd_table_unlock(table, flags);
    return 0;
}

/** A table already shared has every sharer's map as wide as itself, since
 * each share and each growth of a shared table widened them, so only its
 * first share widens the owner's map, which a growth of the unshared table
 * left as it was. */
int fd_table_share(fd_table_t *table, struct process *child)
{
    uint32_t flags = fd_table_lock(table);
    int rc = 0;

    if ((table->refs == 1 && sharers_fit(table, table->cap) != 0) ||
        held_fit(child, table->cap) != 0) {
        rc = -1;
    } else {
        table->refs++;
        child->fds = table;
        child->fd_sharer_next = table->sharers;
        table->sharers = child;
    }
    fd_table_unlock(table, flags);
    return rc;
}

int fd_table_fit(fd_table_t *table, int cap)
{
    uint32_t flags = fd_table_lock(table);
    int rc = cap <= table->cap ? 0 : chunks_reach(table, cap);

    fd_table_unlock(table, flags);
    return rc;
}

/** Mark descriptor `fd` held by the current call when `table` is the
 * caller's and shared, as `fd_get` and `fd_alloc` hold one: a close by
 * another sharer then waits for the call to end. `fd_holding` is one more
 * than the highest word of the map with a bit set. The caller holds the
 * table's lock. */
static void fd_hold(fd_table_t *table, int fd)
{
    process_t *p = current_proc;
    int w = fd / 32;

    if (table->refs > 1 && p && table == p->fds && w < p->fd_held_words) {
        p->fd_held[w] |= 1u << (fd % 32);
        if (p->fd_holding <= w) {
            p->fd_holding = w + 1;
        }
    }
}

int fd_held_elsewhere(fd_table_t *table, int fd)
{
    const process_t *t;

    if (table->refs < 2) {
        return 0;
    }
    for (t = table->sharers; t; t = t->fd_sharer_next) {
        if (t != current_proc && fd / 32 < t->fd_holding &&
            (t->fd_held[fd / 32] & (1u << (fd % 32)))) {
            return 1;
        }
    }
    return 0;
}

int fd_close_claim(fd_table_t *table, int fd)
{
    uint32_t flags = fd_table_lock(table);
    fd_entry_t *e;
    int rc;

    if (fd < 0 || fd >= table->cap) {
        fd_table_unlock(table, flags);
        return -EBADF;
    }
    e = fd_slot(table, fd);
    if (slot_unready(e) || (e->flags & FD_FLAG_CLOSING)) {
        rc = -EBADF;
    } else if (fd_held_elsewhere(table, fd)) {
        e->flags |= FD_FLAG_CLOSING;
        rc = 0;
    } else {
        e->flags |= FD_FLAG_CLOSING | FD_FLAG_RELEASING;
        rc = 1;
    }
    fd_table_unlock(table, flags);
    return rc;
}

int fd_close_finish(fd_table_t *table, int fd)
{
    uint32_t flags = fd_table_lock(table);
    fd_entry_t *e;
    int rc = 0;

    if (fd >= 0 && fd < table->cap) {
        e = fd_slot(table, fd);
        if ((e->flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)) == FD_FLAG_CLOSING &&
            !fd_held_elsewhere(table, fd)) {
            e->flags |= FD_FLAG_RELEASING;
            rc = 1;
        }
    }
    fd_table_unlock(table, flags);
    return rc;
}

int fd_dup_target(fd_table_t *table, int fd)
{
    uint32_t flags = fd_table_lock(table);
    fd_entry_t *e;
    int rc;

    if (fd < 0 || fd >= table->cap) {
        fd_table_unlock(table, flags);
        return -EBADF;
    }
    e = fd_slot(table, fd);
    if (e->type == FD_TYPE_NONE && !e->ofd && !e->pending) {
        e->pending = 1;
        fd_hold(table, fd);
        rc = 0;
    } else if (slot_unready(e) || (e->flags & FD_FLAG_CLOSING) || fd_held_elsewhere(table, fd)) {
        rc = -EBUSY;
    } else {
        e->flags |= FD_FLAG_CLOSING | FD_FLAG_RELEASING;
        rc = 1;
    }
    fd_table_unlock(table, flags);
    return rc;
}

int fd_holds_claim(int from)
{
    process_t *p = current_proc;
    fd_table_t *table = p ? p->fds : NULL;
    uint32_t flags;
    int fd;

    if (!table) {
        if (p) {
            p->fd_holding = 0;
        }
        return -1;
    }
    flags = fd_table_lock(table);
    for (fd = from; fd / 32 < p->fd_holding; fd++) {
        uint32_t *word = &p->fd_held[fd / 32];
        fd_entry_t *e;

        if (*word == 0) {
            fd = (fd / 32) * 32 + 31;
            continue;
        }
        if (!(*word & (1u << (fd % 32)))) {
            continue;
        }
        *word &= ~(1u << (fd % 32));
        if (fd >= table->cap) {
            continue;
        }
        e = fd_slot(table, fd);
        if ((e->flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)) == FD_FLAG_CLOSING &&
            !fd_held_elsewhere(table, fd)) {
            e->flags |= FD_FLAG_RELEASING;
            fd_table_unlock(table, flags);
            return fd;
        }
    }
    p->fd_holding = 0;
    fd_table_unlock(table, flags);
    return -1;
}

int fd_table_reach(fd_table_t *table, int fd)
{
    uint32_t flags;
    int rc;

    if (!table || fd < 0 || fd >= fd_limit(table)) {
        return -EBADF;
    }
    flags = fd_table_lock(table);
    rc = fd < table->cap ? 0 : fd_table_grow(table, fd + 1);
    fd_table_unlock(table, flags);
    return rc;
}

int fd_init_stdio(fd_table_t *table, uint32_t hangups)
{
    int i;

    if (!table || table->cap <= FD_STDERR) {
        return -1;
    }

    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        struct open_file *ofd = ofd_alloc();
        fd_entry_t *e = fd_slot(table, i);

        if (!ofd) {
            while (i > FD_STDIN) {
                fd_free(table, --i);
            }
            return -1;
        }
        ofd->tty_hangups = hangups;
        k_memset(e, 0, sizeof(*e));
        e->type = FD_TYPE_CONSOLE;
        e->ofd = ofd;
        e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    }
    return 0;
}

/** The lowest free slot of `table` at or above `from` and below `limit`,
 * which may be `cap` when every slot below it is taken; the caller holds the
 * lock. */
static int free_slot(fd_table_t *table, int from, int limit)
{
    int i;

    for (i = from; i < table->cap && i < limit; i++) {
        const fd_entry_t *e = fd_slot(table, i);

        if (e->type == FD_TYPE_NONE && e->ofd == NULL && !e->pending) {
            break;
        }
    }
    return i;
}

/**
 * A slot is free when it has no type and no description; one with a
 * description and no type is reserved by an open that has not finished. The
 * search begins at `next_free`, below which no slot is free, as Linux's
 * `next_fd`, so that a process that opens many descriptors does not walk
 * them all at every open. fs.file-max binds a caller that is not
 * privileged, as Linux's `alloc_empty_file` does.
 *
 * The description comes from the heap before the slot is chosen, and goes
 * back when there is no slot; the slot is chosen, grown to and reserved
 * under the table's lock in one step, so that two sharers never reserve
 * one slot.
 */
int fd_alloc_from(fd_table_t *table, int minfd)
{
    int limit = fd_limit(table);
    struct open_file *ofd;
    fd_entry_t *e;
    uint32_t flags;
    int i, rc;

    if (!table || minfd < 0 || minfd >= limit) {
        return -EMFILE;
    }
    flags = fd_table_lock(table);
    i = free_slot(table, minfd > table->next_free ? minfd : table->next_free, limit);
    fd_table_unlock(table, flags);
    if (i >= limit) {
        return -EMFILE;
    }
    if (ofd_count >= fd_file_max && current_proc && !proc_privileged(current_proc)) {
        return -ENFILE;
    }
    ofd = ofd_alloc();
    if (!ofd) {
        return -ENOMEM;
    }
    flags = fd_table_lock(table);
    i = free_slot(table, minfd > table->next_free ? minfd : table->next_free, limit);
    rc = i >= limit ? -EMFILE : i >= table->cap ? fd_table_grow(table, i + 1) : 0;
    if (rc != 0) {
        fd_table_unlock(table, flags);
        fd_ofd_unref(ofd);
        return rc;
    }
    e = fd_slot(table, i);
    k_memset(e, 0, sizeof(*e));
    e->ofd = ofd;
    e->pending = 1;
    if (minfd <= table->next_free) {
        table->next_free = i + 1;
    }
    fd_hold(table, i);
    fd_table_unlock(table, flags);
    return i;
}

int fd_alloc(fd_table_t *table)
{
    return fd_alloc_from(table, 0);
}

void fd_install(fd_table_t *table, int fd)
{
    uint32_t flags = fd_table_lock(table);

    fd_slot(table, fd)->pending = 0;
    fd_table_unlock(table, flags);
}

/**
 * The description's reference is dropped first, while the slot still names
 * it, as the release of a KRFFS file counts the slot among the node's
 * holders until it ends; the slot is then cleared under the lock.
 */
int fd_free(fd_table_t *table, int fd)
{
    struct open_file *ofd;
    fd_entry_t *e;
    uint32_t flags;

    if (!table || fd < 0 || fd >= fd_table_cap(table)) {
        return -1;
    }
    e = fd_slot(table, fd);
    flags = fd_table_lock(table);
    ofd = e->ofd;
    if (e->type == FD_TYPE_NONE && ofd == NULL && !e->pending) {
        fd_table_unlock(table, flags);
        return -1;
    }
    e->pending = 1;
    fd_table_unlock(table, flags);
    fd_ofd_unref(ofd);
    flags = fd_table_lock(table);
    k_memset(e, 0, sizeof(*e));
    if (fd < table->next_free) {
        table->next_free = fd;
    }
    fd_table_unlock(table, flags);
    return 0;
}

void fd_free_after(fd_table_t *table, int fd)
{
    fd_entry_t *e = fd_slot(table, fd);
    uint32_t flags = fd_table_lock(table);

    k_memset(e, 0, sizeof(*e));
    if (fd < table->next_free) {
        table->next_free = fd;
    }
    fd_table_unlock(table, flags);
}

/**
 * Whether `table` is the calling thread's own and no other thread shares
 * it, so that only the caller changes its slots, `cap` and `refs` while it
 * runs: a table is shared only by a clone of one of its sharers
 * (`fd_table_share`), every reservation, publication, close and growth is
 * made by a sharer's call, and another process's call reads the table
 * (`fd_peek`) or changes no more than a slot's node (`fd_table_repoint`).
 * A sharer that left made its last change under the lock before it dropped
 * `refs` there, and x86 does not move a load ahead of an earlier one, so a
 * read of a slot after `refs` reads 1 finds what it left, as Linux's
 * `__fget_light` reads a table whose count is 1 with no lock and no
 * reference after an acquiring read of the count (fs/file.c:1114-1118).
 */
static int fd_table_alone(const fd_table_t *table)
{
    int alone =
        current_proc && table == current_proc->fds && *(const volatile int *)&table->refs == 1;

    k_barrier();
    return alone;
}

/**
 * A lookup in a table the caller alone has (`fd_table_alone`) takes no lock
 * and marks no hold, since no other thread can close the slot during the
 * call; any other lookup takes the table's lock.
 */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    fd_entry_t *e = NULL;
    uint32_t flags;

    if (!table || fd < 0) {
        return NULL;
    }
    if (fd_table_alone(table)) {
        if (fd >= table->cap) {
            return NULL;
        }
        e = fd_slot(table, fd);
        return slot_unready(e) || (e->flags & FD_FLAG_CLOSING) ? NULL : e;
    }
    flags = fd_table_lock(table);
    if (fd < table->cap) {
        e = fd_slot(table, fd);
        if (slot_unready(e) || (e->flags & FD_FLAG_CLOSING)) {
            e = NULL;
        } else {
            fd_hold(table, fd);
        }
    }
    fd_table_unlock(table, flags);
    return e;
}

int fd_peek(fd_table_t *table, int fd, fd_entry_t *out, int ref)
{
    uint32_t flags;
    int rc = -EBADF;

    if (!table || fd < 0) {
        return -EBADF;
    }
    flags = fd_table_lock(table);
    if (fd < table->cap) {
        const fd_entry_t *e = fd_slot(table, fd);

        if (fd_entry_open(e)) {
            *out = *e;
            if (ref == FD_PEEK_REF) {
                fd_ofd_ref(out->ofd);
            }
            rc = 0;
        }
    }
    fd_table_unlock(table, flags);
    return rc;
}

int fd_entry_open(const fd_entry_t *e)
{
    return !slot_unready(e) && !(e->flags & FD_FLAG_CLOSING);
}

int fd_next_open(fd_table_t *table, int from)
{
    uint32_t flags;
    int fd, found = -1;

    if (!table || from < 0) {
        return -1;
    }
    flags = fd_table_lock(table);
    for (fd = from; fd < table->cap; fd++) {
        if (fd_entry_open(fd_slot(table, fd))) {
            found = fd;
            break;
        }
    }
    fd_table_unlock(table, flags);
    return found;
}

void fd_set_flags(fd_table_t *table, fd_entry_t *e, uint32_t clear, uint32_t set)
{
    uint32_t flags = fd_table_lock(table);

    e->flags = (e->flags & ~clear) | set;
    fd_table_unlock(table, flags);
}

int fd_in_table(const fd_table_t *table, const fd_entry_t *e)
{
    uintptr_t x = (uintptr_t)e;
    int k;

    if (!table) {
        return 0;
    }
    for (k = 0; k < FD_CHUNKS && table->chunks[k]; k++) {
        uintptr_t a = (uintptr_t)table->chunks[k];

        if (x >= a && x < a + sizeof(fd_entry_t) * (size_t)chunk_size(k) &&
            (x - a) % sizeof(fd_entry_t) == 0) {
            return 1;
        }
    }
    return 0;
}

int fd_is_valid(fd_table_t *table, int fd)
{
    fd_entry_t e;

    return fd_peek(table, fd, &e, 0) == 0;
}

int fd_can_alloc(fd_table_t *table)
{
    int limit = fd_limit(table);
    uint32_t flags = fd_table_lock(table);
    int i = free_slot(table, table->next_free, limit);

    fd_table_unlock(table, flags);
    return i < limit;
}

void fd_table_repoint(fd_table_t *table, const void *from, void *to)
{
    uint32_t flags = fd_table_lock(table);
    int i;

    for (i = 0; i < table->cap; i++) {
        fd_entry_t *e = fd_slot(table, i);

        if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && (const void *)e->node == from) {
            e->node = to;
        }
    }
    fd_table_unlock(table, flags);
}
