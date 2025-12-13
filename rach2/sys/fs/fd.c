#include "fd.h"
#include "../lib/kstring.h"

void fd_table_init(fd_table_t *table)
{
    int i;

    if (!table) {
        return;
    }

    /* Clear all entries */
    for (i = 0; i < MAX_FDS; i++) {
        table->entries[i].type = FD_TYPE_NONE;
        table->entries[i].node = NULL;
        table->entries[i].offset = 0;
        table->entries[i].flags = 0;
    }
}

void fd_init_stdio(fd_table_t *table)
{
    if (!table) {
        return;
    }

    /* fd 0: stdin - console, read-only */
    table->entries[FD_STDIN].type = FD_TYPE_CONSOLE;
    table->entries[FD_STDIN].node = NULL;
    table->entries[FD_STDIN].offset = 0;
    table->entries[FD_STDIN].flags = FD_FLAG_READ;

    /* fd 1: stdout - console, write-only */
    table->entries[FD_STDOUT].type = FD_TYPE_CONSOLE;
    table->entries[FD_STDOUT].node = NULL;
    table->entries[FD_STDOUT].offset = 0;
    table->entries[FD_STDOUT].flags = FD_FLAG_WRITE;

    /* fd 2: stderr - console, write-only */
    table->entries[FD_STDERR].type = FD_TYPE_CONSOLE;
    table->entries[FD_STDERR].node = NULL;
    table->entries[FD_STDERR].offset = 0;
    table->entries[FD_STDERR].flags = FD_FLAG_WRITE;
}

int fd_alloc(fd_table_t *table)
{
    int i;

    if (!table) {
        return -1;
    }

    /* Find first free slot (skip 0, 1, 2 which are reserved for stdio) */
    for (i = 0; i < MAX_FDS; i++) {
        if (table->entries[i].type == FD_TYPE_NONE) {
            /* Mark as allocated but type not yet set */
            /* Caller should set type, node, flags as needed */
            return i;
        }
    }

    /* No free slots */
    return -1;
}

int fd_free(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= MAX_FDS) {
        return -1;
    }

    if (table->entries[fd].type == FD_TYPE_NONE) {
        /* Already free */
        return -1;
    }

    /* Clear the entry */
    table->entries[fd].type = FD_TYPE_NONE;
    table->entries[fd].node = NULL;
    table->entries[fd].offset = 0;
    table->entries[fd].flags = 0;

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
