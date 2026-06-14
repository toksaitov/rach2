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
    FD_TYPE_FRAMEBUFFER,  /* VGA framebuffer device */
    FD_TYPE_MOUSE,        /* PS/2 mouse device */
    FD_TYPE_SOCKET        /* Network socket */
} fd_type_t;

/*
 * File descriptor flags (for open mode)
 */
#define FD_FLAG_READ    0x01
#define FD_FLAG_WRITE   0x02
#define FD_FLAG_APPEND  0x04
#define FD_FLAG_CLOEXEC 0x08  /* close-on-exec (F_SETFD FD_CLOEXEC, O_CLOEXEC) */

/*
 * Forward declarations for the types a descriptor can point at.
 */
struct krffs_node;
struct socket;

/*
 * Open-file description (OFD): the shared, refcounted seek position behind one or
 * more descriptors. dup/dup2/fork copy the fd's pointer to this and bump the
 * refcount, so the duplicates share one seek offset (the POSIX open-file
 * description semantics) instead of each carrying its own. It is freed back to a
 * static pool when the last referencing descriptor closes. Allocated from a fixed
 * pool (see fd.c); refcount 0 means the slot is free.
 */
struct open_file {
    uint32_t refcount;           /* Number of descriptors sharing this offset */
    uint32_t offset;             /* Shared read/write offset */
};

/*
 * File descriptor entry. The backing object (type/node/sock) and its own
 * reference (sockets are refcounted per descriptor) live here; only the seek
 * offset is shared through the open-file description ofd.
 */
typedef struct fd_entry {
    fd_type_t type;              /* Type of this fd (NONE, CONSOLE, FILE) */
    struct krffs_node *node;     /* Pointer to KRFFS node (for FILE type) */
    struct socket *sock;         /* Pointer to socket (for SOCKET type) */
    struct open_file *ofd;       /* Shared open-file description (the offset) */
    uint32_t flags;              /* Open flags (READ, WRITE, APPEND, CLOEXEC) */
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
 * Allocate the lowest free descriptor with number >= minfd.
 * This is the fcntl(F_DUPFD) contract. Returns the fd (>= 0) on success, -1 if
 * minfd is out of range or no free slot exists at or above it.
 */
int fd_alloc_from(fd_table_t *table, int minfd);

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
 * Add / drop a reference to an open-file description. dup/dup2/fork point a new
 * descriptor at an existing OFD and call fd_ofd_ref so the shared seek offset
 * outlives the original; fd_free calls fd_ofd_unref. (The OFD frees itself back
 * to the pool when its last reference goes.)
 */
void fd_ofd_ref(struct open_file *ofd);
void fd_ofd_unref(struct open_file *ofd);

/*
 * Check if a file descriptor is valid (allocated)
 * Returns 1 if valid, 0 if invalid
 */
int fd_is_valid(fd_table_t *table, int fd);

#endif /* _FD_H_ */
