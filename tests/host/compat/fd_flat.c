/* The descriptor table's slot lookup and publication for host tests that keep
 * a table's entries in one array, `chunks[0]`, and stub the rest of fs/fd.c. */
#include "fs/fd.h"
#include "fs/file.h"

/** These flat-table models have no global description list. */
int32_t fd_files_creators(char *buf, uint32_t cap)
{
    (void)buf;
    (void)cap;
    return 0;
}

/** The entry of `fd`, which the test keeps below the array's length. */
fd_entry_t *fd_slot(const fd_table_t *table, int fd)
{
    return &table->chunks[0][fd];
}

/** The entry the table has for `fd`: a flat table has no spares, so the
 * slot's own. */
fd_entry_t *fd_table_entry(const fd_table_t *table, int fd)
{
    return &table->chunks[0][fd];
}

/** Publish the slot the test's `fd_alloc` reserved. */
void fd_install(fd_table_t *table, int fd)
{
    table->chunks[0][fd].pending = 0;
}

/** The table's lock: the test has one thread, so nothing is taken. */
uint32_t fd_table_lock(fd_table_t *table)
{
    (void)table;
    return 0;
}

/** Release what `fd_table_lock` took. */
void fd_table_unlock(fd_table_t *table, uint32_t flags)
{
    (void)table;
    (void)flags;
}

/** Whether `e` is open: published and not closing. */
int fd_entry_open(const fd_entry_t *e)
{
    return e->type != FD_TYPE_NONE && !e->pending && !(e->flags & FD_FLAG_CLOSING);
}

/** A copy of descriptor `fd`, below `cap`, without a reference; 0 or -EBADF. */
int fd_peek(fd_table_t *table, int fd, fd_entry_t *out, int ref)
{
    (void)ref;
    if (!table || fd < 0 || fd >= table->cap || !fd_entry_open(fd_slot(table, fd))) {
        return -9;
    }
    *out = *fd_slot(table, fd);
    return 0;
}

/** The big lock for a call on `e`: the test has one thread, so nothing is
 * taken, and nothing is given back. */
int file_ops_lock(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

/** An ioctl uses its object's locks even with a retained device inode. */
int file_ioctl_unlocked(const fd_entry_t *e)
{
    return e && e->ops && (e->ops->flags & FILE_OPS_UNLOCKED);
}

/** Give back what `file_ops_lock` took: nothing. */
void file_ops_unlock(int taken)
{
    (void)taken;
}
