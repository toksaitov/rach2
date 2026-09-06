#include "fd.h"

#include "vfs.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

/*
 * An open-file description has one shared seek offset and a refcount of how
 * many descriptors point at it (dup/dup2/fork add references). It is
 * allocated from the heap at open and freed by the last unref, on the list
 * of live descriptions, which the two scans below walk. The wait queue of
 * its lock lives beside it in the record rather than in fd.h, because the
 * wait-queue header includes the process header and the process header
 * includes fd.h; the description is the record's first member, so the one
 * is found from the other.
 */
struct ofd_rec {
    struct open_file ofd;
    wait_queue_t wq;
    struct ofd_rec *next;
};

static struct ofd_rec *ofds;

static struct ofd_rec *ofd_rec_of(struct open_file *ofd)
{
    return (struct ofd_rec *)ofd;
}

/**
 * Allocate a fresh open-file description.
 *
 * Returns an OFD with refcount 1, both offsets 0, and cleared status flags,
 * or NULL
 * if the pool is exhausted.
 */
static struct open_file *ofd_alloc(void)
{
    struct ofd_rec *rec = kzalloc(sizeof(*rec));

    if (!rec) {
        return NULL;
    }
    rec->ofd.refcount = 1;
    wait_queue_init(&rec->wq);
    rec->next = ofds;
    ofds = rec;
    return &rec->ofd;
}

void fd_ofd_ref(struct open_file *ofd)
{
    if (ofd) {
        ofd->refcount++;
    }
}

static int ofd_idle(void *arg)
{
    return ((struct open_file *)arg)->busy == 0;
}

void fd_ofd_lock(struct open_file *ofd)
{
    while (ofd->busy) {
        wq_wait_event_nointr(&ofd_rec_of(ofd)->wq, ofd_idle, ofd);
    }
    ofd->busy = 1;
}

void fd_ofd_unlock(struct open_file *ofd)
{
    ofd->busy = 0;
    wq_wake_all(&ofd_rec_of(ofd)->wq);
}

/**
 * Whether any description of the file at (`mnt`, `node`) is inside an
 * operation. A write or a truncation holds its description for the length of
 * the call, so this is how an attachment asks whether one is in flight over
 * the file it is about to take.
 */
int fd_node_busy(struct vfs_mount *mnt, void *node)
{
    struct ofd_rec *rec;

    for (rec = ofds; rec; rec = rec->next) {
        if (rec->ofd.vmnt == mnt && rec->ofd.vnode == node && rec->ofd.busy) {
            return 1;
        }
    }
    return 0;
}

void fd_ofd_repoint(struct vfs_mount *mnt, void *from, void *to)
{
    struct ofd_rec *rec;

    for (rec = ofds; rec; rec = rec->next) {
        if (rec->ofd.vmnt != mnt) {
            continue;
        }
        if (rec->ofd.vnode == from) {
            rec->ofd.vnode = to;
        }
        if (rec->ofd.fhandle == from) {
            rec->ofd.fhandle = to;
        }
    }
}

/**
 * Drop one reference to an open-file description.
 *
 * A refcount of 0 marks the slot reusable by `ofd_alloc`. The last drop of
 * a description on a mounted file system is that file system's release,
 * the node's put, and the mount's reference, all exactly once, run after
 * the slot's fields are cleared so a slot retaken meanwhile is not touched.
 */
void fd_ofd_unref(struct open_file *ofd)
{
    if (ofd && ofd->refcount > 0) {
        ofd->refcount--;
        if (ofd->refcount == 0) {
            struct ofd_rec **link = &ofds;
            struct ofd_rec *rec = ofd_rec_of(ofd);

            if (ofd->vmnt) {
                struct vfs_mount *mnt = ofd->vmnt;
                void *node = ofd->vnode;
                void *handle = ofd->fhandle;

                ofd->vmnt = NULL;
                ofd->vnode = NULL;
                ofd->fhandle = NULL;
                vfs_desc_close(mnt, node, handle);
            }
            /* Off the list and back to the heap: nothing points at it now. */
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

/**
 * Clear every entry of a descriptor table.
 *
 * This does not unref any OFDs; it is for a fresh (zeroed) table only. A live
 * table is torn down descriptor by descriptor through `fd_free`, which releases
 * each OFD reference.
 */
void fd_table_init(fd_table_t *table)
{
    int i;

    if (!table) {
        return;
    }

    for (i = 0; i < MAX_FDS; i++) {
        table->entries[i].type   = FD_TYPE_NONE;
        table->entries[i].node   = NULL;
        table->entries[i].sock   = NULL;
        table->entries[i].ofd    = NULL;
        table->entries[i].ops    = NULL;
        table->entries[i].priv   = NULL;
        table->entries[i].flags  = 0;
    }
}

/**
 * Wire up the three standard descriptors on a fresh table.
 *
 * fd 0 is stdin (read-only); fd 1 and fd 2 are stdout and stderr (write-only).
 * Each is a console descriptor with its own open-file description.
 */
void fd_init_stdio(fd_table_t *table)
{
    int i;

    if (!table) {
        return;
    }

    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        table->entries[i].type  = FD_TYPE_CONSOLE;
        table->entries[i].node  = NULL;
        table->entries[i].sock  = NULL;
        table->entries[i].ofd   = ofd_alloc();
        table->entries[i].ops   = NULL;
        table->entries[i].priv  = NULL;
        table->entries[i].flags = (i == FD_STDIN) ? FD_FLAG_READ : FD_FLAG_WRITE;
    }
}

/**
 * Reserve the lowest free descriptor at or above `minfd`.
 *
 * The chosen slot gets a fresh OFD but stays FD_TYPE_NONE; the caller fills in
 * type, node or sock, and flags. Returns the descriptor number, or -1 if the
 * table has no free slot at or above `minfd` or the OFD pool is exhausted.
 */
int fd_alloc_from(fd_table_t *table, int minfd)
{
    int i;

    if (!table || minfd < 0 || minfd >= MAX_FDS) {
        return -1;
    }

    for (i = minfd; i < MAX_FDS; i++) {
        if (table->entries[i].type == FD_TYPE_NONE) {
            struct open_file *ofd = ofd_alloc();
            if (!ofd) {
                return -1;
            }
            table->entries[i].ofd  = ofd;
            table->entries[i].ops  = NULL;
            table->entries[i].priv = NULL;
            return i;
        }
    }

    return -1;
}

int fd_alloc(fd_table_t *table)
{
    return fd_alloc_from(table, 0);
}

/**
 * Release a descriptor and its OFD reference, clearing the slot.
 *
 * A slot with no backing and no reserved OFD is already free; freeing it
 * returns -1. The OFD reference is dropped even when the slot is still
 * typeless, so a slot that `fd_alloc_from` reserved but a caller bailed on
 * before assigning a type does not leak its OFD.
 */
int fd_free(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= MAX_FDS) {
        return -1;
    }

    if (table->entries[fd].type == FD_TYPE_NONE && table->entries[fd].ofd == NULL) {
        return -1;
    }

    fd_ofd_unref(table->entries[fd].ofd);
    table->entries[fd].type   = FD_TYPE_NONE;
    table->entries[fd].node   = NULL;
    table->entries[fd].sock   = NULL;
    table->entries[fd].ofd    = NULL;
    table->entries[fd].ops    = NULL;
    table->entries[fd].priv   = NULL;
    table->entries[fd].flags  = 0;

    return 0;
}

fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= MAX_FDS) {
        return NULL;
    }

    if (table->entries[fd].type == FD_TYPE_NONE) {
        return NULL;
    }

    return &table->entries[fd];
}

int fd_is_valid(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= MAX_FDS) {
        return 0;
    }

    return table->entries[fd].type != FD_TYPE_NONE;
}

int fd_can_alloc(const fd_table_t *table)
{
    int i;

    for (i = 0; i < MAX_FDS; i++) {
        if (table->entries[i].type == FD_TYPE_NONE) {
            return 1;
        }
    }
    return 0;
}
