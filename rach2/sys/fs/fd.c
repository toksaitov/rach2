/* File descriptor tables and the open file descriptions they share. */
#include "fd.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../proc/tgroup.h"

#include "vfs.h"
#include "name.h"
#include "lock.h"
#include "../io/inotify.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/rcu.h"
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
    /* The identity of the opener's credentials (`cred_id` in process.h),
     * which linkat's AT_EMPTY_PATH compares (`fd_ofd_opener_is_current`);
     * 0, which no process has, at boot. */
    uint64_t opener_cred;
    /* Nonzero for a reader's place on the list (`fd_files_creators`), which
     * is no description: its `ofd` is zero, so the scans pass it over. */
    int cursor;
};

/*
 * The list of live descriptions, its count, and the access
 * counts are under `ofd_lock`, a spinlock taken with interrupts off below a
 * table's lock; a description's own fields are its holders'. The scans of
 * the list read only what their callers' locks keep still, and a
 * description's mount and node, which its end withdraws under this lock
 * before it gives them back (`fd_ofd_unref`), so that a scan reads either
 * none or a mount still held, whatever lock the end holds. The list is
 * doubly linked, so that a description leaves it in constant time.
 */
static struct spinlock ofd_lock = SPINLOCK_INIT;
static struct ofd_rec *ofds;
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
    rec->opener_cred = current_proc ? current_proc->cred_id : 0;
    wait_queue_init(&rec->wq);
    flags = spin_lock_irqsave(&ofd_lock);
    ofd_link(rec);
    ofd_count++;
    spin_unlock_irqrestore(&ofd_lock, flags);
    return &rec->ofd;
}

struct open_file *fd_ofd_new(void)
{
    return ofd_alloc();
}

int fd_ofd_opener_is_current(struct open_file *ofd)
{
    const struct ofd_rec *rec = ofd_rec_of(ofd);

    return current_proc && rec->opener_cred == current_proc->cred_id;
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

    if (access == 1 &&
        (ref->mnt->readonly_pending || vfs_mount_base(ref->mnt)->readonly_pending ||
         (ref->mnt->flags & VFS_MS_RDONLY) || (vfs_mount_base(ref->mnt)->flags & VFS_MS_RDONLY))) {
        rc = -EROFS;
    } else if (a && a->count[2 - access] > 0) {
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

int fd_mount_readonly_begin(struct vfs_mount *mnt)
{
    struct ofd_rec *rec;
    uint32_t flags = spin_lock_irqsave(&ofd_lock);
    int rc = mnt->readonly_pending ? -EBUSY : 0;

    for (rec = ofds; rec && rc == 0; rec = rec->next) {
        struct open_file *f = &rec->ofd;

        if (f->access == 1 &&
            (mnt->bind_of ? f->access_mnt == mnt : vfs_mount_base(f->access_mnt) == mnt)) {
            rc = -EBUSY;
        }
    }
    if (rc == 0) {
        mnt->readonly_pending = 1;
    }
    spin_unlock_irqrestore(&ofd_lock, flags);
    return rc;
}

void fd_mount_readonly_end(struct vfs_mount *mnt)
{
    uint32_t flags = spin_lock_irqsave(&ofd_lock);

    mnt->readonly_pending = 0;
    spin_unlock_irqrestore(&ofd_lock, flags);
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

                flags = spin_lock_irqsave(&ofd_lock);
                ofd->vmnt = NULL;
                ofd->vnode = NULL;
                spin_unlock_irqrestore(&ofd_lock, flags);
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

/*
 * An entry dup2 put in place of one that was open or closing
 * (`fd_dup_swap`): the number's entry until it is free and the slot's own
 * entry is no longer kept, on its table's `spares`. The entry is the first
 * member, so that its address is the spare's.
 */
struct fd_spare {
    fd_entry_t entry;
    struct fd_spare *next;
};

/*
 * An entry dup2 replaced, kept for the calls that held it, as Linux's
 * calls keep the file by the reference `fdget` took: on its table's `gone`
 * until the last of `holders` ends and its release, claimed then or under
 * way when dup2 came, clears it (`fd_free_after`).
 */
struct fd_gone {
    struct fd_gone *next;
    fd_entry_t *entry; /* the slot's own entry or a spare */
    int fd;
    int holders;   /* the calls that keep it and have not ended */
    int releasing; /* its release has been claimed */
};

/*
 * A call's hold on an entry dup2 replaced, on its thread's `fd_gone`:
 * pushed by the replacing thread under the table's lock and taken off only
 * by the thread itself, so the thread reads its list with no lock. `fd` is
 * the descriptor whose number leads the thread's `fd_slot` to the kept
 * entry, or -1 once the call has looked the number up again or reserved it.
 */
struct fd_gone_hold {
    struct fd_gone_hold *next;
    struct fd_gone *gone;
    fd_table_t *table;
    fd_entry_t *entry;
    int fd;
};

/** The slot's own entry for descriptor `fd`. */
static fd_entry_t *slot_home(const fd_table_t *table, int fd)
{
    int first;
    int k = chunk_of(fd, &first);

    return &table->chunks[k][fd - first];
}

fd_entry_t *fd_table_entry(const fd_table_t *table, int fd)
{
    int first;
    int k = chunk_of(fd, &first);
    fd_entry_t **moved = *(fd_entry_t * *const volatile *)&table->moved[k];

    if (moved) {
        fd_entry_t *e = *(fd_entry_t *const volatile *)&moved[fd - first];

        if (e) {
            return e;
        }
    }
    return &table->chunks[k][fd - first];
}

/**
 * A call that held a descriptor dup2 then replaced reads what it held
 * through the number for the rest of the call, as it did before the
 * replacement, until it looks the number up again (`gone_shadow`): code
 * that publishes a descriptor and then fills in its description by number
 * keeps writing to its own.
 */
fd_entry_t *fd_slot(const fd_table_t *table, int fd)
{
    const process_t *p = current_proc;
    const struct fd_gone_hold *h;

    if (p && p->fd_gone && table == p->fds) {
        for (h = p->fd_gone; h; h = h->next) {
            if (h->fd == fd && h->table == table) {
                return h->entry;
            }
        }
    }
    return fd_table_entry(table, fd);
}

const fd_entry_t *fd_gone_entry(const fd_table_t *table, int i)
{
    const struct fd_gone *g = table->gone;

    while (g && i-- > 0) {
        g = g->next;
    }
    return g ? g->entry : NULL;
}

/** The calling thread's holds on kept entries of `fd` in `table` lead its
 * `fd_slot` there no longer, since its call now looks the number up or
 * reserves it. The thread alone writes the holds' `fd`. */
static void gone_shadow(fd_table_t *table, int fd)
{
    struct fd_gone_hold *h;

    for (h = current_proc ? current_proc->fd_gone : NULL; h; h = h->next) {
        if (h->fd == fd && h->table == table) {
            h->fd = -1;
        }
    }
}

/** The record of `e` on `table`'s `gone`, or NULL; the caller holds the
 * lock. */
static struct fd_gone *gone_of(const fd_table_t *table, const fd_entry_t *e)
{
    struct fd_gone *g;

    for (g = table->gone; g && g->entry != e; g = g->next) {
    }
    return g;
}

/** Take `e` off `table`'s `spares` and free it when it is a spare, and
 * return 1; 0 for a slot's own entry. The caller holds the lock. */
static int spare_drop(fd_table_t *table, fd_entry_t *e)
{
    struct fd_spare **link;

    for (link = &table->spares; *link; link = &(*link)->next) {
        if (&(*link)->entry == e) {
            struct fd_spare *sp = *link;

            *link = sp->next;
            kfree(sp);
            return 1;
        }
    }
    return 0;
}

/** Whether `e` is a free entry: no type, no description and no
 * reservation. */
static int entry_free(const fd_entry_t *e)
{
    return e->type == FD_TYPE_NONE && e->ofd == NULL && !e->pending;
}

/** Clear entry `e` under the table's lock: its type first, so that a
 * lookup without the lock, which reads the flags and the reservation
 * before the type (`slot_open_unlocked`), never takes an entry half
 * cleared for open. */
static void slot_clear(fd_entry_t *e)
{
    ((volatile fd_entry_t *)e)->type = FD_TYPE_NONE;
    k_barrier();
    k_memset(e, 0, sizeof(*e));
}

/** Fill free entry `e` with `nw` under the table's lock, its type last,
 * so that a lookup without the lock never takes it for open before the
 * rest is in place; x86 keeps the stores in order. */
static void slot_fill(fd_entry_t *e, const fd_entry_t *nw)
{
    fd_entry_t v = *nw;

    v.type = FD_TYPE_NONE;
    *e = v;
    k_barrier();
    ((volatile fd_entry_t *)e)->type = nw->type;
}

/** Lead the number `fd` back to its slot's own entry when it leads to a
 * spare that is free and the slot's own entry is no longer kept, and free
 * the spare. The caller holds the lock, and no call holds a free spare. */
static void slot_settle(fd_table_t *table, int fd)
{
    int first;
    int k = chunk_of(fd, &first);
    fd_entry_t *cur;

    if (!table->moved[k] || !(cur = table->moved[k][fd - first]) || !entry_free(cur) ||
        gone_of(table, &table->chunks[k][fd - first])) {
        return;
    }
    *(fd_entry_t *volatile *)(void *)&table->moved[k][fd - first] = NULL;
    spare_drop(table, cur);
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
    k_memset(table->moved, 0, sizeof(table->moved));
    table->spares = NULL;
    table->gone = NULL;
    table->cap = 0;
    table->next_free = 0;
    table->sharers = NULL;
    spin_lock_init(&table->lock, "fd_table->lock");
    return chunks_reach(table, FD_TABLE_FIRST) == 0 ? 0 : -1;
}

/**
 * The spares and the records of kept entries go too; a release of the table
 * has released every entry they hold (`fd_gone_take`).
 */
void fd_table_free(fd_table_t *table)
{
    int k;

    if (!table) {
        return;
    }
    while (table->gone) {
        struct fd_gone *g = table->gone;

        table->gone = g->next;
        kfree(g);
    }
    while (table->spares) {
        struct fd_spare *sp = table->spares;

        table->spares = sp->next;
        kfree(sp);
    }
    for (k = 0; k < FD_CHUNKS; k++) {
        kfree(table->chunks[k]);
        table->chunks[k] = NULL;
        kfree(table->moved[k]);
        table->moved[k] = NULL;
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
        fd_entry_t *d = slot_home(dst, i);

        *d = *fd_table_entry(src, i);
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

/* The fewest words a hold map has: 512 descriptors, and with its header
 * more than a cache line, so that two threads' maps, each written by its
 * own thread at every call, seldom share one. */
#define HOLD_MIN_WORDS 16

/** Free a hold map a growth replaced, once a grace period has passed: its
 * `rcu` is its first member, so `head` is the map. */
static void hold_map_free(struct rcu_head *head)
{
    kfree(head);
}

/** Set the bits `set` and clear the bits `clear` of hold map word `*w` in
 * one locked compare-and-exchange, a full barrier on x86: the thread whose
 * map it is changes its bits without the table's lock while a dup2 by
 * another sharer may clear one of them under it (`gone_make`), so neither
 * may write back a word it read before the other's change. */
static void hold_word_change(volatile unsigned int *w, unsigned int set, unsigned int clear)
{
    unsigned int old;

    do {
        old = *w;
    } while (arch_atomic_cmpxchg(w, old, (old | set) & ~clear) != old);
}

/**
 * Give `t` a map of held descriptors that covers `cap`, its bits kept; 0,
 * or -ENOMEM. The caller holds the lock of `t`'s table, or `t` shares none
 * yet. The thread may be setting or clearing a bit of the old map without
 * the lock as it is replaced, so the old map is marked stale with a full
 * barrier before it is read: a bit changed before the mark is seen by the
 * copy, and the thread finds the mark after a change and makes the change
 * again in the new map under the lock (`hold_set`, `fd_holds_claim`). The
 * old map is freed after a grace period, since the thread may still be
 * writing it, as Linux's `expand_fdtable` frees a table's old array by
 * `call_rcu` (fs/file.c:207).
 */
static int held_fit(process_t *t, int cap)
{
    int words = (cap + 31) / 32;
    struct fd_hold_map *old = t->fd_held, *map;
    int w;

    if (old && old->words >= words) {
        return 0;
    }
    if (words < HOLD_MIN_WORDS) {
        words = HOLD_MIN_WORDS;
    }
    map = kzalloc(sizeof(*map) + sizeof(map->bits[0]) * (size_t)(words - 1));
    if (!map) {
        return -ENOMEM;
    }
    map->words = words;
    if (old) {
        (void)arch_atomic_xchg(&old->stale, 1u);
        for (w = 0; w < old->words; w++) {
            map->bits[w] = old->bits[w];
        }
    }
    t->fd_held = map;
    if (old) {
        call_rcu(&old->rcu, hold_map_free);
    }
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

/** Let go of `p`'s holds on entries of `table` that dup2 replaced, as `p`
 * leaves the table, so that a release of the table frees them
 * (`fd_gone_take`). The caller holds the lock. */
static void holds_forget(fd_table_t *table, process_t *p)
{
    struct fd_gone_hold **link = &p->fd_gone;

    while (*link) {
        struct fd_gone_hold *h = *link;

        if (h->table != table) {
            link = &h->next;
            continue;
        }
        *link = h->next;
        h->gone->holders--;
        kfree(h);
    }
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
    holds_forget(table, p);
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
    int rc = fd_table_resize(table, cap);

    fd_table_unlock(table, flags);
    return rc;
}

/** Mark descriptor `fd` held by the current call when `table` is the
 * caller's and shared, as `fd_get` and `fd_alloc` hold one: a close by
 * another sharer then waits for the call to end. `fd_holding` is one more
 * than the highest word of the map with a bit set. The call's `fd_slot`
 * then finds the entry the number has now (`gone_shadow`). The caller
 * holds the table's lock. */
static void fd_hold(fd_table_t *table, int fd)
{
    process_t *p = current_proc;
    int w = fd / 32;

    if (p && p->fd_gone) {
        gone_shadow(table, fd);
    }
    if (table->refs > 1 && p && table == p->fds && p->fd_held && w < p->fd_held->words) {
        if (p->fd_holding <= w) {
            p->fd_holding = w + 1;
        }
        p->fd_held->bits[w] |= 1u << (fd % 32);
    }
}

/**
 * Mark descriptor `fd` of `table`, the shared table of the calling thread
 * `p`, held by its call, without the table's lock, inside the caller's read
 * section, since a growth frees a map it replaced after a grace period.
 * `fd_holding` is stored before the bit, and the bit is set by a locked
 * compare-and-exchange, which orders it before the caller's second look at
 * the slot. A close or a dup2 stores its mark of the slot and takes a full
 * barrier before it reads the maps (`fd_close_claim`, `gone_make`), so of
 * the two, either it finds the bit or the second look finds the mark, as
 * Linux's `__fget_files_rcu` takes the file's reference before it reads the
 * slot again. A growth that replaced the map meanwhile marked the old one
 * stale before it copied it, so the bit is set again in the new map under
 * the lock. 0, or -1 when the map does not reach `fd`, which a growth of
 * the table never leaves.
 */
static int hold_set(fd_table_t *table, process_t *p, int fd)
{
    struct fd_hold_map *map = p->fd_held;
    unsigned int bit = 1u << (fd % 32);
    int w = fd / 32;
    uint32_t flags;

    if (!map || w >= map->words) {
        return -1;
    }
    if (p->fd_holding <= w) {
        p->fd_holding = w + 1;
    }
    hold_word_change(&map->bits[w], bit, 0);
    if (map->stale) {
        flags = fd_table_lock(table);
        p->fd_held->bits[w] |= bit;
        fd_table_unlock(table, flags);
    }
    return 0;
}

/** Whether a call of a thread other than the caller that shares `table`
 * holds `fd`. The caller holds the lock, so each map is its thread's
 * newest, and has stored its own mark of the slot with a full barrier
 * first. */

int fd_held_elsewhere(fd_table_t *table, int fd)
{
    const process_t *t;

    if (table->refs < 2) {
        return 0;
    }
    for (t = table->sharers; t; t = t->fd_sharer_next) {
        if (t != current_proc && fd / 32 < t->fd_holding &&
            (t->fd_held->bits[fd / 32] & (1u << (fd % 32)))) {
            return 1;
        }
    }
    return 0;
}

int fd_close_claim(fd_table_t *table, int fd, fd_entry_t **claimed)
{
    uint32_t flags = fd_table_lock(table);
    fd_entry_t *e;
    int rc;

    if (fd < 0 || fd >= table->cap) {
        fd_table_unlock(table, flags);
        return -EBADF;
    }
    e = fd_table_entry(table, fd);
    if (slot_unready(e) || (e->flags & FD_FLAG_CLOSING)) {
        rc = -EBADF;
    } else {
        (void)arch_atomic_xchg(&e->flags, e->flags | FD_FLAG_CLOSING);
        if (fd_held_elsewhere(table, fd)) {
            rc = 0;
        } else {
            e->flags |= FD_FLAG_RELEASING;
            rc = 1;
        }
    }
    if (rc >= 0) {
        *claimed = e;
    }
    fd_table_unlock(table, flags);
    return rc;
}

/** Whether thread `t` marks descriptor `fd` held; the caller holds the
 * lock of `t`'s table. */
static int held_by(const process_t *t, int fd)
{
    return fd / 32 < t->fd_holding && (t->fd_held->bits[fd / 32] & (1u << (fd % 32)));
}

/** A hold of `t`'s call on `g`'s entry, pushed on its `fd_gone` with the
 * number `fd` leading its `fd_slot` there, and counted; the caller holds
 * the lock of `g`'s table, and `h` is from the heap. */
static void gone_push(process_t *t, struct fd_gone *g, struct fd_gone_hold *h, fd_table_t *table,
                      int fd)
{
    h->gone = g;
    h->table = table;
    h->entry = g->entry;
    h->fd = fd;
    h->next = t->fd_gone;
    k_barrier();
    *(struct fd_gone_hold *volatile *)(void *)&t->fd_gone = h;
    g->holders++;
}

/**
 * Take from the heap what `gone_make` needs to replace an entry of chunk
 * `k` of `table` that `need` - 1 calls hold: the chunk's `moved` array when
 * it has none, the spare `*sp`, the record `*g` and `need` holds on
 * `*holds`. 0, or -ENOMEM with nothing taken but the array, which stays
 * for a later dup2. The caller holds the lock, under which the heap may be
 * called.
 */
static int gone_alloc(fd_table_t *table, int k, int need, struct fd_spare **sp, struct fd_gone **g,
                      struct fd_gone_hold **holds)
{
    struct fd_gone_hold *h;

    *holds = NULL;
    if (!table->moved[k]) {
        fd_entry_t **moved = kzalloc(sizeof(fd_entry_t *) * (size_t)chunk_size(k));

        if (!moved) {
            return -ENOMEM;
        }
        *(fd_entry_t * *volatile *)(void *)&table->moved[k] = moved;
    }
    *sp = kzalloc(sizeof(**sp));
    *g = kzalloc(sizeof(**g));
    while (*sp && *g && need-- > 0 && (h = kzalloc(sizeof(*h))) != NULL) {
        h->next = *holds;
        *holds = h;
    }
    if (*sp && *g && need < 0) {
        return 0;
    }
    kfree(*sp);
    kfree(*g);
    while (*holds) {
        h = *holds;
        *holds = h->next;
        kfree(h);
    }
    return -ENOMEM;
}

/**
 * Put a spare holding `nw` in place of `old`, the entry of `fd`, for dup2,
 * in one step, and keep `old` for the calls that hold it: each sharer whose
 * call holds `fd` lets the number go and keeps `old` instead, its `fd_slot`
 * still leading there, and the caller keeps it too when it was open, so
 * that it may drop its record locks after the table's lock goes and release
 * it (`fd_gone_claim`), or when no call or release would end it otherwise.
 * An entry whose release is under way is that release's alone: the calls
 * that hold it let the number go and keep nothing. A lookup finds `old`
 * before the step and the copy after it, never a slot that is empty or
 * reserved, as Linux's do_dup2 swaps the file in the slot under the
 * table's lock. An open `old` is marked closing with a full barrier
 * before the holds are read, as a close marks it (`fd_close_claim`), so
 * that a lookup without the lock either holds it before the reading or
 * finds the mark at its second look (`hold_set`). Everything is taken from
 * the heap first, under the lock, which the heap allows. 0, or -ENOMEM
 * with nothing changed. The caller holds the lock.
 */
static int gone_make(fd_table_t *table, int fd, fd_entry_t *old, const fd_entry_t *nw)
{
    process_t *self = current_proc;
    int first, k = chunk_of(fd, &first);
    int releasing = (old->flags & FD_FLAG_RELEASING) != 0;
    int was_open = !(old->flags & FD_FLAG_CLOSING);
    struct fd_gone_hold *holds, *h;
    struct fd_spare *sp;
    struct fd_gone *g;
    process_t *t;
    int need = 1, self_held = 0;

    if (was_open) {
        (void)arch_atomic_xchg(&old->flags, old->flags | FD_FLAG_CLOSING);
    }
    for (t = table->sharers; t; t = t->fd_sharer_next) {
        if (held_by(t, fd)) {
            need++;
        }
    }
    if (gone_alloc(table, k, need, &sp, &g, &holds) != 0) {
        old->flags &= ~(uint32_t)(was_open ? FD_FLAG_CLOSING : 0);
        return -ENOMEM;
    }
    g->entry = old;
    g->fd = fd;
    g->releasing = releasing;
    for (t = table->sharers; t; t = t->fd_sharer_next) {
        if (!held_by(t, fd)) {
            continue;
        }
        hold_word_change(&t->fd_held->bits[fd / 32], 0, 1u << (fd % 32));
        if (releasing) {
            continue;
        }
        h = holds;
        holds = h->next;
        gone_push(t, g, h, table, fd);
        self_held |= t == self;
    }
    if (!releasing && !self_held && self && (was_open || g->holders == 0)) {
        h = holds;
        holds = h->next;
        gone_push(self, g, h, table, -1);
    }
    while (holds) {
        h = holds;
        holds = h->next;
        kfree(h);
    }
    g->next = table->gone;
    table->gone = g;
    sp->entry = *nw;
    sp->next = table->spares;
    table->spares = sp;
    k_barrier();
    *(fd_entry_t *volatile *)(void *)&table->moved[k][fd - first] = &sp->entry;
    if (self && self->fd_gone) {
        gone_shadow(table, fd);
    }
    return 0;
}

int fd_dup_swap(fd_table_t *table, int fd, const fd_entry_t *nw, fd_entry_t **was)
{
    uint32_t flags = fd_table_lock(table);
    fd_entry_t *e;
    int rc;

    *was = NULL;
    if (fd < 0 || fd >= table->cap) {
        fd_table_unlock(table, flags);
        return -EBADF;
    }
    e = fd_table_entry(table, fd);
    if (entry_free(e)) {
        slot_fill(e, nw);
        if (current_proc && current_proc->fd_gone) {
            gone_shadow(table, fd);
        }
        rc = 0;
    } else if (slot_unready(e)) {
        rc = -EBUSY;
    } else {
        int open = !(e->flags & FD_FLAG_CLOSING);

        rc = gone_make(table, fd, e, nw);
        if (rc == 0 && open) {
            *was = e;
        }
    }
    fd_table_unlock(table, flags);
    return rc;
}

int fd_gone_claim(fd_table_t **tablep, fd_entry_t **e)
{
    process_t *p = current_proc;
    struct fd_gone_hold *h;

    while (p && p->fd_gone) {
        fd_table_t *table;
        struct fd_gone *g;
        uint32_t flags;

        h = p->fd_gone;
        table = h->table;
        flags = fd_table_lock(table);
        p->fd_gone = h->next;
        g = h->gone;
        kfree(h);
        if (--g->holders == 0 && !g->releasing) {
            g->releasing = 1;
            g->entry->flags |= FD_FLAG_RELEASING;
            *tablep = table;
            *e = g->entry;
            fd_table_unlock(table, flags);
            return g->fd;
        }
        fd_table_unlock(table, flags);
    }
    return -1;
}

int fd_gone_take(fd_table_t *table, fd_entry_t **e)
{
    uint32_t flags = fd_table_lock(table);
    struct fd_gone *g;
    int fd = -1;

    for (g = table->gone; g && g->releasing; g = g->next) {
    }
    if (g) {
        g->releasing = 1;
        g->entry->flags |= FD_FLAG_RELEASING;
        *e = g->entry;
        fd = g->fd;
    }
    fd_table_unlock(table, flags);
    return fd;
}

/** Whether the entry descriptor `fd` of `table` leads to may be closing
 * with no one releasing it yet, read without the lock: its number leads to
 * a spare, which only the lock may read, or its slot's own entry bears the
 * mark a close left for the calls that hold it. */
static int slot_left(fd_table_t *table, int fd)
{
    fd_entry_t **moved;
    uint32_t flags;
    int first, k;

    if (fd >= fd_table_cap(table)) {
        return 0;
    }
    k_barrier();
    k = chunk_of(fd, &first);
    moved = *(fd_entry_t * *const volatile *)&table->moved[k];
    if (moved && *(fd_entry_t *const volatile *)&moved[fd - first]) {
        return 1;
    }
    flags = ((const volatile fd_entry_t *)&table->chunks[k][fd - first])->flags;
    return (flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)) == FD_FLAG_CLOSING;
}

/** Under the table's lock, clear bit `fd` of the newest map of the
 * calling thread `p` and claim the release of the entry its number leads
 * to when a close left it to the calls that held it and no other holds it:
 * 1, with the entry marked releasing and in `*claimed`, or 0. */
static int hold_drop_locked(fd_table_t *table, process_t *p, int fd, fd_entry_t **claimed)
{
    uint32_t flags = fd_table_lock(table);
    fd_entry_t *e;
    int rc = 0;

    p->fd_held->bits[fd / 32] &= ~(1u << (fd % 32));
    if (fd < table->cap) {
        e = fd_table_entry(table, fd);
        if ((e->flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)) == FD_FLAG_CLOSING &&
            !fd_held_elsewhere(table, fd)) {
            e->flags |= FD_FLAG_RELEASING;
            *claimed = e;
            rc = 1;
        }
    }
    fd_table_unlock(table, flags);
    return rc;
}

/**
 * Each bit is cleared by a locked compare-and-exchange and the slot's mark
 * read after it: a close that stored its mark after this read finds the
 * bit clear and releases the slot itself (`fd_close_claim`), and a dup2
 * that read the bit first moved the hold to the entry it replaced
 * (`fd_gone_claim`), so only a slot found marked or leading to a spare, or
 * a map a growth replaced, needs the lock, as Linux's `fput` at a call's
 * end takes no table lock.
 */
int fd_holds_claim(int from, fd_entry_t **claimed)
{
    process_t *p = current_proc;
    fd_table_t *table = p ? p->fds : NULL;
    struct fd_hold_map *map;
    unsigned int word, bit;
    int fd;

    if (!table) {
        if (p) {
            p->fd_holding = 0;
        }
        return -1;
    }
    rcu_read_lock();
    for (fd = from; fd / 32 < p->fd_holding; fd++) {
        map = p->fd_held;
        word = map->bits[fd / 32];
        bit = 1u << (fd % 32);
        if (word == 0) {
            fd = (fd / 32) * 32 + 31;
            continue;
        }
        if (!(word & bit)) {
            continue;
        }
        hold_word_change(&map->bits[fd / 32], 0, bit);
        if ((map->stale || slot_left(table, fd)) && hold_drop_locked(table, p, fd, claimed)) {
            rcu_read_unlock();
            return fd;
        }
    }
    p->fd_holding = 0;
    rcu_read_unlock();
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
        fd_entry_t *e = fd_table_entry(table, i);

        if (!ofd) {
            while (i > FD_STDIN) {
                fd_free(table, --i);
            }
            return -1;
        }
        ofd->tty_hangups = hangups;
        ofd->tty_console = 1;
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
        const fd_entry_t *e = fd_table_entry(table, i);

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
    e = fd_table_entry(table, i);
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

    fd_table_entry(table, fd)->pending = 0;
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
    flags = fd_table_lock(table);
    e = fd_table_entry(table, fd);
    ofd = e->ofd;
    if (entry_free(e)) {
        fd_table_unlock(table, flags);
        return -1;
    }
    e->pending = 1;
    fd_table_unlock(table, flags);
    fd_ofd_unref(ofd);
    flags = fd_table_lock(table);
    slot_clear(e);
    if (fd < table->next_free) {
        table->next_free = fd;
    }
    slot_settle(table, fd);
    fd_table_unlock(table, flags);
    return 0;
}

/**
 * An entry dup2 replaced leaves `gone` and is freed when it is a spare or
 * cleared when it is the slot's own, which the number may then lead back
 * to (`slot_settle`); the number's own entry is cleared in place.
 */
void fd_free_after(fd_table_t *table, int fd, fd_entry_t *e)
{
    uint32_t flags = fd_table_lock(table);
    struct fd_gone **link;

    for (link = &table->gone; *link && (*link)->entry != e; link = &(*link)->next) {
    }
    if (*link) {
        struct fd_gone *g = *link;

        *link = g->next;
        kfree(g);
        if (!spare_drop(table, e)) {
            slot_clear(e);
        }
    } else {
        slot_clear(e);
        if (fd < table->next_free) {
            table->next_free = fd;
        }
    }
    slot_settle(table, fd);
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
 * Whether entry `e` is open, read without the table's lock: published, not
 * closing and of a type. The flags and the reservation are read before the
 * type, and a release clears the type before the rest (`slot_clear`) and a
 * dup2 into a free entry fills it last (`slot_fill`), so an entry being
 * cleared or filled is never taken for open. An entry being filled after
 * `fd_alloc` keeps its reservation until `fd_install` publishes it, after
 * the fill's stores.
 */
static int slot_open_unlocked(const fd_entry_t *e)
{
    const volatile fd_entry_t *v = e;
    uint32_t flags = v->flags;
    uint32_t pending = v->pending;

    return !(flags & FD_FLAG_CLOSING) && !pending && v->type != FD_TYPE_NONE;
}

/** The slot's own entry of descriptor `fd` < `fd_table_cap(table)` when
 * its number leads there, read without the lock, or NULL when it leads to
 * a spare dup2 made, which a release may free under the lock. */
static fd_entry_t *slot_home_unlocked(const fd_table_t *table, int fd)
{
    fd_entry_t **moved;
    int first, k;

    k = chunk_of(fd, &first);
    moved = *(fd_entry_t * *const volatile *)&table->moved[k];
    if (moved && *(fd_entry_t *const volatile *)&moved[fd - first]) {
        return NULL;
    }
    return &table->chunks[k][fd - first];
}

/**
 * A lookup in a table the caller alone has (`fd_table_alone`) takes no lock
 * and marks no hold, since no other thread can close the slot during the
 * call, as Linux's `__fget_light`. A lookup in the caller's shared table
 * finds the slot's own entry open, holds it (`hold_set`) and finds it open
 * and still the number's again, with no lock, as Linux's `__fget_files_rcu`
 * reads the slot, takes the file's reference and reads the slot again: a
 * close or a dup2 that the second look misses finds the hold. The slot's
 * address is stable, since a growth adds chunks and moves no entry, and is
 * read after `cap`, which a growth raises after its chunk is published. A
 * slot found changing, by a close, a release or a dup2, a number that
 * leads to a spare, and a call that keeps an entry dup2 replaced, whose
 * `fd_slot` must follow the lookup (`gone_shadow`), are looked up again
 * under the lock; so is any other process's table.
 */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    process_t *p = current_proc;
    fd_entry_t *e = NULL;
    uint32_t flags;

    if (!table || fd < 0) {
        return NULL;
    }
    if (fd_table_alone(table)) {
        if (fd >= table->cap) {
            return NULL;
        }
        e = fd_table_entry(table, fd);
        if (slot_unready(e) || (e->flags & FD_FLAG_CLOSING)) {
            return NULL;
        }
        if (current_proc->fd_gone) {
            gone_shadow(table, fd);
        }
        return e;
    }
    if (p && table == p->fds && !p->fd_gone) {
        if (fd >= fd_table_cap(table)) {
            return NULL;
        }
        k_barrier();
        rcu_read_lock();
        e = slot_home_unlocked(table, fd);
        if (e && slot_open_unlocked(e) && hold_set(table, p, fd) == 0 &&
            slot_home_unlocked(table, fd) == e && slot_open_unlocked(e)) {
            rcu_read_unlock();
            return e;
        }
        rcu_read_unlock();
        e = NULL;
    }
    flags = fd_table_lock(table);
    if (fd < table->cap) {
        e = fd_table_entry(table, fd);
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
        const fd_entry_t *e = fd_table_entry(table, fd);

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
        if (fd_entry_open(fd_table_entry(table, fd))) {
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
    const struct fd_spare *sp;
    uintptr_t x = (uintptr_t)e;
    int k;

    if (!table) {
        return 0;
    }
    for (sp = table->spares; sp; sp = sp->next) {
        if (&sp->entry == e) {
            return 1;
        }
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
    struct fd_gone *g;
    int i;

    for (i = 0; i < table->cap; i++) {
        fd_entry_t *e = fd_table_entry(table, i);

        if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && (const void *)e->node == from) {
            e->node = to;
        }
    }
    for (g = table->gone; g; g = g->next) {
        fd_entry_t *e = g->entry;

        if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && (const void *)e->node == from) {
            e->node = to;
        }
    }
    fd_table_unlock(table, flags);
}
