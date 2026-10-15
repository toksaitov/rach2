/* File descriptor tables and the open-file descriptions they share. */
#include "fd.h"
#include "../proc/process.h"

#include "vfs.h"
#include "lock.h"
#include "../io/inotify.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

/*
 * The record of an open-file description, from the heap, on the list of live
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

/** The record whose first member is `ofd`. */
static struct ofd_rec *ofd_rec_of(struct open_file *ofd)
{
    return (struct ofd_rec *)ofd;
}

/**
 * A new open-file description on the live list, with one reference, the next
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
    return &rec->ofd;
}

struct open_file *fd_ofd_new(void)
{
    return ofd_alloc();
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

            if (lock_any()) {
                lock_ofd_gone(ofd);
            }
            if (ofd->vmnt && (ofd->watch_flags & OFD_WATCH_OPEN)) {
                inotify_desc(ofd, (ofd->watch_flags & OFD_WATCH_WRITE) ? IN_CLOSE_WRITE
                                                                       : IN_CLOSE_NOWRITE);
            }
            kfree(ofd->leaf);
            ofd->leaf = NULL;
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
            while (*link && *link != rec) {
                link = &(*link)->next;
            }
            if (*link) {
                *link = rec->next;
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
    return table->entries ? 0 : -1;
}

void fd_table_free(fd_table_t *table)
{
    if (!table) {
        return;
    }
    kfree(table->entries);
    table->entries = NULL;
    table->cap = 0;
}

int fd_table_cap(const fd_table_t *table)
{
    return table ? table->cap : 0;
}

int fd_limit(const fd_table_t *table)
{
    unsigned long cur;

    if (!current_proc || table != current_proc->fds) {
        return FD_TABLE_LIMIT;
    }
    cur = current_proc->rlim[RLIM_NOFILE].cur;
    return cur < FD_TABLE_LIMIT ? (int)cur : FD_TABLE_LIMIT;
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
    return table;
}

void fd_table_delete(fd_table_t *table)
{
    fd_table_free(table);
    kfree(table);
}

/** Move the entries to an array of `cap`, the new ones free; 0, or -ENOMEM
 * when the heap has no room. */
static int fd_table_resize(fd_table_t *table, int cap)
{
    fd_entry_t *grown = kzalloc(sizeof(fd_entry_t) * (size_t)cap);

    if (!grown) {
        return -ENOMEM;
    }
    if (table->cap) {
        k_memcpy(grown, table->entries, sizeof(fd_entry_t) * (size_t)table->cap);
    }
    kfree(table->entries);
    table->entries = grown;
    table->cap = cap;
    return 0;
}

/** Grow the table by doubling to at least `want` entries; 0, -EMFILE when
 * `want` is past the caller's limit, or -ENOMEM when the heap has no room.
 * A shared table has every entry already. */
static int fd_table_grow(fd_table_t *table, int want)
{
    int cap = table->cap ? table->cap : FD_TABLE_FIRST;

    if (want > fd_limit(table)) {
        return -EMFILE;
    }
    while (cap < want) {
        cap *= 2;
    }
    if (cap > FD_TABLE_LIMIT) {
        cap = FD_TABLE_LIMIT;
    }
    return fd_table_resize(table, cap);
}

int fd_table_share(fd_table_t *table)
{
    if (table->cap < FD_TABLE_LIMIT && fd_table_resize(table, FD_TABLE_LIMIT) != 0) {
        return -1;
    }
    table->refs++;
    return 0;
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

int fd_init_stdio(fd_table_t *table)
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
 * description and no type is reserved by an open that has not finished.
 */
int fd_alloc_from(fd_table_t *table, int minfd)
{
    int limit = fd_limit(table);
    int i;

    if (!table || minfd < 0 || minfd >= limit) {
        return -EMFILE;
    }
    for (i = minfd; i < table->cap && i < limit; i++) {
        if (table->entries[i].type == FD_TYPE_NONE && table->entries[i].ofd == NULL) {
            break;
        }
    }
    if (i >= limit) {
        return -EMFILE;
    }
    if (i >= table->cap) {
        int rc = fd_table_grow(table, i + 1);

        if (rc != 0) {
            return rc;
        }
    }
    {
        struct open_file *ofd = ofd_alloc();

        if (!ofd) {
            return -ENOMEM;
        }
        table->entries[i].ofd = ofd;
        table->entries[i].ops = NULL;
        table->entries[i].priv = NULL;
        table->entries[i].dev = NULL;
    }
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
    if (table->refs > 1 && current_proc && table == current_proc->fds) {
        current_proc->fd_held[fd / 32] |= 1u << (fd % 32);
        current_proc->fd_holding = 1;
    }
    return e;
}

int fd_in_table(const fd_table_t *table, const fd_entry_t *e)
{
    int i;

    for (i = 0; table && i < table->cap; i++) {
        if (&table->entries[i] == e) {
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

    for (i = 0; i < table->cap && i < limit; i++) {
        if (table->entries[i].type == FD_TYPE_NONE && table->entries[i].ofd == NULL) {
            return 1;
        }
    }
    return table->cap < limit;
}
