#ifndef _FD_H_
#define _FD_H_

/*
 * File Descriptor Table
 * Per-process file descriptor management for stdin/stdout/stderr and files.
 */

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * Maximum number of file descriptors per process
 */
#define MAX_FDS 16

/*
 * Standard file descriptor numbers
 */
#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

/*
 * File descriptor types
 */
typedef enum {
    FD_TYPE_NONE,         /* Slot is free */
    FD_TYPE_CONSOLE,      /* Console (stdin/stdout/stderr) */
    FD_TYPE_FILE,         /* Regular file from KRFFS */
    FD_TYPE_DIR,          /* Directory (for getdents64) */
    FD_TYPE_FRAMEBUFFER   /* VGA framebuffer device */
} fd_type_t;

/*
 * File descriptor flags (for open mode)
 */
#define FD_FLAG_READ   0x01
#define FD_FLAG_WRITE  0x02
#define FD_FLAG_APPEND 0x04

/*
 * Forward declaration for krffs_node_t
 */
struct krffs_node;

/*
 * File descriptor entry
 */
typedef struct fd_entry {
    fd_type_t type;              /* Type of this fd (NONE, CONSOLE, FILE) */
    struct krffs_node *node;     /* Pointer to KRFFS node (for FILE type) */
    uint32_t offset;             /* Current read/write offset */
    uint32_t flags;              /* Open flags (READ, WRITE, APPEND) */
} fd_entry_t;

/*
 * File descriptor table (array of MAX_FDS entries)
 * This is embedded in each process's PCB
 */
typedef struct fd_table {
    fd_entry_t entries[MAX_FDS];
} fd_table_t;

/*
 * Initialize a file descriptor table
 * Clears all entries to FD_TYPE_NONE
 */
void fd_table_init(fd_table_t *table);

/*
 * Initialize standard file descriptors (0, 1, 2) as console
 * Should be called after fd_table_init()
 */
void fd_init_stdio(fd_table_t *table);

/*
 * Allocate a new file descriptor in the table
 * Returns fd number (>= 0) on success, -1 if table full
 */
int fd_alloc(fd_table_t *table);

/*
 * Free a file descriptor
 * Returns 0 on success, -1 if fd invalid
 */
int fd_free(fd_table_t *table, int fd);

/*
 * Get a file descriptor entry
 * Returns pointer to entry, or NULL if fd invalid or free
 */
fd_entry_t *fd_get(fd_table_t *table, int fd);

/*
 * Check if a file descriptor is valid (allocated)
 * Returns 1 if valid, 0 if invalid
 */
int fd_is_valid(fd_table_t *table, int fd);

#endif /* _FD_H_ */
