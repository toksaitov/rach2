/* File descriptor tables and the open file descriptions they share. */
#include "fd.h"
#include "../proc/process.h"

#include "vfs.h"
#include "name.h"
#include "lock.h"
#include "../io/inotify.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

/*
 * The record of an open file description, from the heap, on the list of live
 * descriptions that the scans below walk. The wait queue of the description
 * lock is in the record rather than in fd.h, because the wait-queue header
 * includes the process header and the process header includes fd.h; the
 * description is the record's first member, so the one is found from the
 * other.
 */
struct ofd_rec {
    struct open_file ofd;
    wait_queue_t wq;
    struct ofd_rec *next;
};

static struct ofd_rec *ofds;
static uint32_t ofd_serials;
static unsigned long ofd_count; /* records on `ofds` */

int fd_nr_open = FD_NR_OPEN;
unsigned long fd_file_max = ~0ul >> 1;

/* An array a growth replaced, kept until `fd_table_reap` frees it. */
struct fd_retired {
    struct fd_retired *next;
    fd_entry_t *entries;
    int cap;
};

unsigned long fd_files_open(void)
{
    return ofd_count;
}

/** The record whose first member is `ofd`. */
static struct ofd_rec *ofd_rec_of(struct open_file *ofd)
{
    return (struct ofd_rec *)ofd;
}

/**
 * A new open file description on the live list, with one reference, the next
 * serial, zero offsets and no status flags; NULL when the heap has no room.
 */
static struct open_file *ofd_alloc(void)
{
    struct ofd_rec *rec = kzalloc(sizeof(*rec));

    if (!rec) {
        return NULL;
    }
    rec->ofd.refcount = 1;
    rec->ofd.serial = ++ofd_serials;
    wait_queue_init(&rec->wq);
    rec->next = ofds;
    ofds = rec;
    ofd_count++;
    return &rec->ofd;
}

struct open_file *fd_ofd_new(void)
{
    return ofd_alloc();
}

/*
 * The reservations of one file, by the descriptions that hold them: Linux's
 * i_writecount, kept here by the file's mount base and node, since a node is
 * each file system's own. A record lives while either count is above zero.
 */
struct access_count {
    struct access_count *next;
    const struct vfs_mount *base; /* vfs_mount_base of the holders' mount */
    const void *node;
    unsigned long count[2]; /* the writers, the executables */
};

/* The buckets of `access_counts`, a power of two. */
#define ACCESS_BUCKETS 64u

static struct access_count *access_counts[ACCESS_BUCKETS];

/** The bucket of the file `node` under the mount base `base`. */
static uint32_t access_bucket(const struct vfs_mount *base, const void *node)
{
    uintptr_t k = (uintptr_t)node ^ ((uintptr_t)base >> 4);

    return (uint32_t)((k >> 4) ^ (k >> 12)) & (ACCESS_BUCKETS - 1u);
}

/** The record of `node` under `mnt`'s base, made when `make` is set; NULL
 * when there is none, or no room for one. */
static struct access_count *access_find(const struct vfs_mount *mnt, const void *node, int make)
{
    const struct vfs_mount *base = vfs_mount_base(mnt);
    struct access_count **b = &access_counts[access_bucket(base, node)], *a;

    for (a = *b; a; a = a->next) {
        if (a->base == base && a->node == node) {
            return a;
        }
    }
    if (!make || (a = (struct access_count *)kzalloc(sizeof(*a))) == NULL) {
        return NULL;
    }
    a->base = base;
    a->node = node;
    a->next = *b;
    *b = a;
    return a;
}

/** Give back one reservation of kind `access`, 1 or 2, of `node` under
 * `mnt`, freeing the record that holds no more. */
static void access_put(const struct vfs_mount *mnt, const void *node, int access)
{
    struct access_count *a = access_find(mnt, node, 0), **p;

    if (!a || a->count[access - 1] == 0) {
        return;
    }
    if (--a->count[access - 1] > 0 || a->count[2 - access] > 0) {
        return;
    }
    for (p = &access_counts[access_bucket(a->base, node)]; *p != a; p = &(*p)->next) {
    }
    *p = a->next;
    kfree(a);
}

void fd_ofd_access_drop(struct open_file *ofd)
{
    if (ofd->access) {
        access_put(ofd->access_mnt, ofd->access_node, ofd->access);
        ofd->access = 0;
    }
}

int fd_ofd_access(struct open_file *ofd, const struct vfs_ref *ref, int access)
{
    struct access_count *a = access_find(ref->mnt, ref->node, 0);

    if (a && a->count[2 - access] > 0) {
        return -ETXTBSY;
    }
    if (a && ofd->access == access && ofd->access_node == ref->node &&
        vfs_mount_base(ofd->access_mnt) == vfs_mount_base(ref->mnt)) {
        ofd->access_mnt = ref->mnt;
        return 0;
    }
    fd_ofd_access_drop(ofd);
    a = access_find(ref->mnt, ref->node, 1);
    if (!a) {
        return -ENOMEM;
    }
    a->count[access - 1]++;
    ofd->access_mnt = ref->mnt;
    ofd->access_node = ref->node;
    ofd->access = access;
    return 0;
}

void fd_ofd_ref(struct open_file *ofd)
{
    if (ofd) {
        ofd->refcount++;
    }
}

/** The wait condition of the description lock: `arg`'s lock is free. */
static int ofd_idle(void *arg)
{
    return ((struct open_file *)arg)->busy == 0;
}

void fd_ofd_lock(struct open_file *ofd)
{
    if (ofd->vmnt && ofd->vmnt->type->ns_serialised) {
        vfs_ns_lock(ofd->vmnt);
    }
    while (ofd->busy) {
        wq_wait_event_nointr(&ofd_rec_of(ofd)->wq, ofd_idle, ofd);
    }
    ofd->busy = 1;
}

void fd_ofd_unlock(struct open_file *ofd)
{
    ofd->busy = 0;
    wq_wake_all(&ofd_rec_of(ofd)->wq);
    if (ofd->vmnt && ofd->vmnt->type->ns_serialised) {
        vfs_ns_unlock(ofd->vmnt);
    }
}

int fd_node_busy(struct vfs_mount *mnt, void *node)
{
    struct ofd_rec *rec;

    for (rec = ofds; rec; rec = rec->next) {
        if (rec->ofd.vmnt && vfs_mount_base(rec->ofd.vmnt) == vfs_mount_base(mnt) &&
            rec->ofd.vnode == node && rec->ofd.busy) {
            return 1;
        }
    }
    return 0;
}

int fd_ofd_live(const struct open_file *ofd, uint32_t serial)
{
    const struct ofd_rec *rec;

    for (rec = ofds; rec; rec = rec->next) {
        if (&rec->ofd == ofd) {
            return rec->ofd.refcount > 0 && rec->ofd.serial == serial;
        }
    }
    return 0;
}

int fd_ofd_holders(const struct vfs_mount *mnt, const void *node, const struct open_file *going)
{
    struct ofd_rec *rec;
    int held = 0;

    for (rec = ofds; rec; rec = rec->next) {
        if (rec->ofd.vmnt && vfs_mount_base(rec->ofd.vmnt) == vfs_mount_base(mnt) &&
            rec->ofd.vnode == node && (&rec->ofd != going || rec->ofd.refcount > 1)) {
            held++;
        }
    }
    return held;
}

/**
 * The root's adapter, `vfs_krffs_type`, uses the node as the handle of an
 * open too, so the handle moves with the node; a mounted KRFFS volume's
 * handle is an open record of its own, which stays where it is.
 */
void fd_ofd_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    struct ofd_rec *rec;

    for (rec = ofds; rec; rec = rec->next) {
        if (rec->ofd.access && rec->ofd.access_mnt == mnt && rec->ofd.access_node == from) {
            /* With no room for the new node's count, the reservation stays
             * on the old one, whose count still has it. */
            struct access_count *a = access_find(mnt, to, 1);

            if (a) {
                access_put(mnt, from, rec->ofd.access);
                a->count[rec->ofd.access - 1]++;
                rec->ofd.access_node = to;
            }
        } else if (rec->ofd.access_mnt == mnt && rec->ofd.access_node == from) {
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
}

void fd_ofd_unref(struct open_file *ofd)
{
    if (ofd && ofd->refcount > 0) {
        ofd->refcount--;
        if (ofd->refcount == 0) {
            struct ofd_rec **link = &ofds;
            struct ofd_rec *rec = ofd_rec_of(ofd);

            fd_ofd_access_drop(ofd);
            if (lock_any()) {
                lock_ofd_gone(ofd);
            }
            if (ofd->vmnt && (ofd->watch_flags & OFD_WATCH_OPEN)) {
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
            while (*link && *link != rec) {
                link = &(*link)->next;
            }
            if (*link) {
                *link = rec->next;
                ofd_count--;
            }
            kfree(rec);
        }
    }
}

int fd_table_init(fd_table_t *table)
{
    if (!table) {
        return -1;
    }
    table->entries = kzalloc(sizeof(fd_entry_t) * FD_TABLE_FIRST);
    table->cap = table->entries ? FD_TABLE_FIRST : 0;
    table->next_free = 0;
    table->retired = NULL;
    return table->entries ? 0 : -1;
}

/** Free every array on `table`'s retired list. */
static void retired_free(fd_table_t *table)
{
    struct fd_retired *r;

    while ((r = table->retired) != NULL) {
        table->retired = r->next;
        kfree(r->entries);
        kfree(r);
    }
}

void fd_table_free(fd_table_t *table)
{
    if (!table) {
        return;
    }
    retired_free(table);
    kfree(table->entries);
    table->entries = NULL;
    table->cap = 0;
    table->next_free = 0;
}

/**
 * A sharer's call that holds a descriptor may sleep with the address of an
 * entry in an array the table has since replaced; a sharer that holds none
 * has no such address, since every call takes its entries by `fd_get` or
 * `fd_alloc`, which hold them. The caller's own call is ending.
 */
void fd_table_reap(fd_table_t *table)
{
    const process_t *t;

    if (!table || !table->retired) {
        return;
    }
    if (table->refs > 1) {
        for (t = proc_list; t; t = t->next) {
            if (t != current_proc && t->fds == table && t->fd_holding) {
                return;
            }
        }
    }
    retired_free(table);
}

int fd_table_cap(const fd_table_t *table)
{
    return table ? table->cap : 0;
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
        kfree(table);
        return NULL;
    }
    table->refs = 1;
    return table;
}

/**
 * A slot with no type is either free or reserved by an unfinished open of
 * another thread that shares `src`. The copy takes no reference to such a
 * slot's description, so the slot is cleared in the copy.
 */
fd_table_t *fd_table_dup(const fd_table_t *src)
{
    fd_table_t *table = kzalloc(sizeof(*table));
    int i;

    if (!table) {
        return NULL;
    }
    table->entries = kzalloc(sizeof(fd_entry_t) * (size_t)src->cap);
    if (!table->entries) {
        kfree(table);
        return NULL;
    }
    k_memcpy(table->entries, src->entries, sizeof(fd_entry_t) * (size_t)src->cap);
    for (i = 0; i < src->cap; i++) {
        if (table->entries[i].type == FD_TYPE_NONE) {
            k_memset(&table->entries[i], 0, sizeof(fd_entry_t));
        }
    }
    table->cap = src->cap;
    table->refs = 1;
    table->next_free = 0;
    return table;
}

void fd_table_delete(fd_table_t *table)
{
    fd_table_free(table);
    kfree(table);
}

/** Widen the hold map of `t` to cover `cap` descriptors, its bits kept;
 * 0, or -ENOMEM. */
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

/** Widen the hold map of every thread that shares `table` to `cap`; 0, or
 * -ENOMEM, with the maps widened so far left so, which is harmless. */
static int sharers_fit(fd_table_t *table, int cap)
{
    process_t *t;

    for (t = proc_list; t; t = t->next) {
        if (t->fds == table && held_fit(t, cap) != 0) {
            return -ENOMEM;
        }
    }
    return 0;
}

/**
 * Move the entries to an array of `cap`, the new ones free, and put the old
 * array on the retired list, where a call that took the address of one of
 * its entries before the move finds it as it was (`fd_table_t`). The maps of
 * a shared table's sharers widen first. Nothing here sleeps. 0, or -ENOMEM
 * when the heap has no room.
 */
static int fd_table_resize(fd_table_t *table, int cap)
{
    fd_entry_t *grown;
    struct fd_retired *old = NULL;

    if (table->refs > 1 && sharers_fit(table, cap) != 0) {
        return -ENOMEM;
    }
    if (table->entries && (old = kmalloc(sizeof(*old))) == NULL) {
        return -ENOMEM;
    }
    grown = kzalloc(sizeof(fd_entry_t) * (size_t)cap);
    if (!grown) {
        kfree(old);
        return -ENOMEM;
    }
    if (old) {
        k_memcpy(grown, table->entries, sizeof(fd_entry_t) * (size_t)table->cap);
        old->entries = table->entries;
        old->cap = table->cap;
        old->next = table->retired;
        table->retired = old;
    }
    table->entries = grown;
    table->cap = cap;
    return 0;
}

/** Grow the table by doubling to at least `want` entries, and to no more
 * than `fd_nr_open` past that; 0, -EMFILE when `want` is past the caller's
 * limit, or -ENOMEM when the heap has no room. */
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

/** A table already shared has every sharer's map as wide as itself, since
 * each share and each growth of a shared table widened them, so only its
 * first share walks the processes, for the owner's map that a growth of the
 * unshared table left as it was. */
int fd_table_share(fd_table_t *table, struct process *child)
{
    if ((table->refs == 1 && sharers_fit(table, table->cap) != 0) ||
        held_fit(child, table->cap) != 0) {
        return -1;
    }
    table->refs++;
    return 0;
}

int fd_table_fit(fd_table_t *table, int cap)
{
    return table->cap >= cap ? 0 : fd_table_resize(table, cap);
}

/** Mark descriptor `fd` held by the current call when `table` is the
 * caller's and shared, as `fd_get` and `fd_alloc` hold one: a close by
 * another sharer then waits for the call to end, and the arrays a growth
 * replaces stay until it has. `fd_holding` is one more than the highest
 * word of the map with a bit set. */
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

int fd_table_reach(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= fd_limit(table)) {
        return -EBADF;
    }
    if (fd < table->cap) {
        return 0;
    }
    return fd_table_grow(table, fd + 1);
}

int fd_init_stdio(fd_table_t *table, uint32_t hangups)
{
    int i;

    if (!table || !table->entries || table->cap <= FD_STDERR) {
        return -1;
    }

    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        struct open_file *ofd = ofd_alloc();

        if (!ofd) {
            while (i > FD_STDIN) {
                fd_free(table, --i);
            }
            return -1;
        }
        ofd->tty_hangups = hangups;
        table->entries[i].type = FD_TYPE_CONSOLE;
        table->entries[i].node = NULL;
        table->entries[i].sock = NULL;
        table->entries[i].ofd = ofd;
        table->entries[i].ops = NULL;
        table->entries[i].priv = NULL;
        table->entries[i].dev = NULL;
        table->entries[i].flags = FD_FLAG_READ | FD_FLAG_WRITE;
    }
    return 0;
}

/**
 * A slot is free when it has no type and no description; one with a
 * description and no type is reserved by an open that has not finished. The
 * search begins at `next_free`, below which no slot is free, as Linux's
 * `next_fd`, so that a process that opens many descriptors does not walk
 * them all at every open. fs.file-max binds a caller that is not
 * privileged, as Linux's `alloc_empty_file` does.
 */
int fd_alloc_from(fd_table_t *table, int minfd)
{
    int limit = fd_limit(table);
    struct open_file *ofd;
    int i;

    if (!table || minfd < 0 || minfd >= limit) {
        return -EMFILE;
    }
    for (i = minfd > table->next_free ? minfd : table->next_free; i < table->cap && i < limit;
         i++) {
        if (table->entries[i].type == FD_TYPE_NONE && table->entries[i].ofd == NULL) {
            break;
        }
    }
    if (i >= limit) {
        return -EMFILE;
    }
    if (ofd_count >= fd_file_max && current_proc && !proc_privileged(current_proc)) {
        return -ENFILE;
    }
    if (i >= table->cap) {
        int rc = fd_table_grow(table, i + 1);

        if (rc != 0) {
            return rc;
        }
    }
    ofd = ofd_alloc();
    if (!ofd) {
        return -ENOMEM;
    }
    table->entries[i].ofd = ofd;
    table->entries[i].ops = NULL;
    table->entries[i].priv = NULL;
    table->entries[i].dev = NULL;
    if (minfd <= table->next_free) {
        table->next_free = i + 1;
    }
    fd_hold(table, i);
    return i;
}

int fd_alloc(fd_table_t *table)
{
    return fd_alloc_from(table, 0);
}

int fd_free(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= table->cap) {
        return -1;
    }

    if (table->entries[fd].type == FD_TYPE_NONE && table->entries[fd].ofd == NULL) {
        return -1;
    }

    fd_ofd_unref(table->entries[fd].ofd);
    table->entries[fd].type = FD_TYPE_NONE;
    table->entries[fd].node = NULL;
    table->entries[fd].sock = NULL;
    table->entries[fd].ofd = NULL;
    table->entries[fd].ops = NULL;
    table->entries[fd].priv = NULL;
    table->entries[fd].dev = NULL;
    table->entries[fd].flags = 0;
    if (fd < table->next_free) {
        table->next_free = fd;
    }
    return 0;
}

fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    fd_entry_t *e;

    if (!table || fd < 0 || fd >= table->cap) {
        return NULL;
    }
    e = &table->entries[fd];
    if (e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING)) {
        return NULL;
    }
    fd_hold(table, fd);
    return e;
}

/** Whether `e` is the address of one of the `cap` entries at `base`. */
static int in_array(const fd_entry_t *base, int cap, const fd_entry_t *e)
{
    uintptr_t a = (uintptr_t)base, x = (uintptr_t)e;

    return base && x >= a && x < a + sizeof(fd_entry_t) * (size_t)cap &&
           (x - a) % sizeof(fd_entry_t) == 0;
}

int fd_in_table(const fd_table_t *table, const fd_entry_t *e)
{
    const struct fd_retired *r;

    if (!table) {
        return 0;
    }
    if (in_array(table->entries, table->cap, e)) {
        return 1;
    }
    for (r = table->retired; r; r = r->next) {
        if (in_array(r->entries, r->cap, e)) {
            return 1;
        }
    }
    return 0;
}

int fd_is_valid(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= table->cap) {
        return 0;
    }

    return table->entries[fd].type != FD_TYPE_NONE && !(table->entries[fd].flags & FD_FLAG_CLOSING);
}

int fd_can_alloc(const fd_table_t *table)
{
    int limit = fd_limit(table);
    int i;

    for (i = table->next_free; i < table->cap && i < limit; i++) {
        if (table->entries[i].type == FD_TYPE_NONE && table->entries[i].ofd == NULL) {
            return 1;
        }
    }
    return table->cap < limit;
}

fd_entry_t *fd_entry_now(fd_table_t *table, fd_entry_t *e)
{
    const struct fd_retired *r;

    for (r = table ? table->retired : NULL; r; r = r->next) {
        if (in_array(r->entries, r->cap, e)) {
            return &table->entries[e - r->entries];
        }
    }
    return e;
}

/** Repoint each FILE or DIR entry of the `cap` at `entries` that names
 * `from` at `to`. */
static void entries_repoint(fd_entry_t *entries, int cap, const void *from, void *to)
{
    int i;

    for (i = 0; i < cap; i++) {
        if ((entries[i].type == FD_TYPE_FILE || entries[i].type == FD_TYPE_DIR) &&
            (const void *)entries[i].node == from) {
            entries[i].node = to;
        }
    }
}

void fd_table_repoint(fd_table_t *table, const void *from, void *to)
{
    const struct fd_retired *r;

    entries_repoint(table->entries, table->cap, from, to);
    for (r = table->retired; r; r = r->next) {
        entries_repoint(r->entries, r->cap, from, to);
    }
}
