#include "fd.h"

#include "vfs.h"
#include "../lib/kstring.h"

/*
 * The open-file-description pool. Each OFD has one shared seek offset
 * and a refcount of how many descriptors point at it (dup/dup2/fork add
 * references). A slot with refcount 0 is free. The pool is sized for the
 * worst case of every descriptor in every process holding a distinct OFD,
 * MAX_PROCS (16) * MAX_FDS, since the number of live OFDs can never exceed
 * the number of live descriptors.
 */
#define OFD_POOL_SIZE (16 * MAX_FDS)
static struct open_file ofd_pool[OFD_POOL_SIZE];

/**
 * Allocate a fresh open-file description.
 *
 * Returns an OFD with refcount 1, both offsets 0, and cleared status flags,
 * or NULL
 * if the pool is exhausted.
 */
static struct open_file *ofd_alloc(void)
{
    int i;

    for (i = 0; i < OFD_POOL_SIZE; i++) {
        if (ofd_pool[i].refcount == 0) {
            ofd_pool[i].refcount     = 1;
            ofd_pool[i].offset       = 0;
            ofd_pool[i].raw_offset   = 0;
            ofd_pool[i].status_flags = 0;
            ofd_pool[i].vmnt         = NULL;
            ofd_pool[i].vnode        = NULL;
            ofd_pool[i].fhandle      = NULL;
            return &ofd_pool[i];
        }
    }
    return NULL;
}

void fd_ofd_ref(struct open_file *ofd)
{
    if (ofd) {
        ofd->refcount++;
    }
}

/**
 * Drop one reference to an open-file description.
 *
 * The pool slot frees itself when the last reference goes: a refcount of 0
 * marks the slot reusable by `ofd_alloc`.
 */
void fd_ofd_unref(struct open_file *ofd)
{
    if (ofd && ofd->refcount > 0) {
        ofd->refcount--;
        if (ofd->refcount == 0 && ofd->vmnt) {
            /* The description's last close: the file system's release, the
             * node's put, and the mount's reference, all exactly once. */
            vfs_desc_close(ofd->vmnt, ofd->vnode, ofd->fhandle);
            ofd->vmnt = NULL;
            ofd->vnode = NULL;
            ofd->fhandle = NULL;
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
