#include "fd.h"
#include "../lib/kstring.h"

/*
 * The open-file-description pool. Each OFD carries one shared seek offset and a
 * refcount of how many descriptors point at it (dup/dup2/fork add references). A
 * slot with refcount 0 is free. The pool covers the worst case of every
 * descriptor in every process holding a distinct OFD: MAX_PROCS (16) * MAX_FDS,
 * since the number of live OFDs can never exceed the number of live descriptors.
 */
#define OFD_POOL_SIZE (16 * MAX_FDS)
static struct open_file ofd_pool[OFD_POOL_SIZE];

/*
 * Allocate a fresh open-file description (refcount 1, offset 0), or NULL if the
 * pool is exhausted.
 */
static struct open_file *ofd_alloc(void)
{
    int i;

    for (i = 0; i < OFD_POOL_SIZE; i++) {
        if (ofd_pool[i].refcount == 0) {
            ofd_pool[i].refcount = 1;
            ofd_pool[i].offset   = 0;
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

void fd_ofd_unref(struct open_file *ofd)
{
    /* Drop one reference; the slot frees itself when the last reference goes. */
    if (ofd && ofd->refcount > 0) {
        ofd->refcount--;
    }
}

void fd_table_init(fd_table_t *table)
{
    int i;

    if (!table) {
        return;
    }

    /* Clear all entries. Note: this does NOT unref any OFDs -- it is for a fresh
     * (zeroed) table only; a live table is torn down fd by fd via fd_free. */
    for (i = 0; i < MAX_FDS; i++) {
        table->entries[i].type   = FD_TYPE_NONE;
        table->entries[i].node   = NULL;
        table->entries[i].sock   = NULL;
        table->entries[i].ofd    = NULL;
        table->entries[i].flags  = 0;
    }
}

void fd_init_stdio(fd_table_t *table)
{
    int i;

    if (!table) {
        return;
    }

    /* fd 0: stdin (read-only); fd 1/2: stdout/stderr (write-only). Each gets its
     * own open-file description. */
    for (i = FD_STDIN; i <= FD_STDERR; i++) {
        table->entries[i].type  = FD_TYPE_CONSOLE;
        table->entries[i].node  = NULL;
        table->entries[i].sock  = NULL;
        table->entries[i].ofd   = ofd_alloc();
        table->entries[i].flags = (i == FD_STDIN) ? FD_FLAG_READ : FD_FLAG_WRITE;
    }
}

int fd_alloc_from(fd_table_t *table, int minfd)
{
    int i;

    if (!table || minfd < 0 || minfd >= MAX_FDS) {
        return -1;
    }

    /* Find the first free slot at or above minfd and give it a fresh OFD. The
     * caller then sets type, node/sock, and flags. */
    for (i = minfd; i < MAX_FDS; i++) {
        if (table->entries[i].type == FD_TYPE_NONE) {
            struct open_file *ofd = ofd_alloc();
            if (!ofd) {
                return -1;   /* OFD pool exhausted */
            }
            table->entries[i].ofd = ofd;
            return i;
        }
    }

    /* No free slot at or above minfd */
    return -1;
}

int fd_alloc(fd_table_t *table)
{
    return fd_alloc_from(table, 0);
}

int fd_free(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= MAX_FDS) {
        return -1;
    }

    if (table->entries[fd].type == FD_TYPE_NONE && table->entries[fd].ofd == NULL) {
        /* Already free (no backing and no reserved OFD). */
        return -1;
    }

    /* Drop this descriptor's reference to its shared open-file description, then
     * clear the slot. (Releasing an OFD even on a still-typeless reserved slot --
     * one fd_alloc_from set up but a caller never finished -- keeps the pool from
     * leaking on a bail-out before the type is assigned.) */
    fd_ofd_unref(table->entries[fd].ofd);
    table->entries[fd].type   = FD_TYPE_NONE;
    table->entries[fd].node   = NULL;
    table->entries[fd].sock   = NULL;
    table->entries[fd].ofd    = NULL;
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
