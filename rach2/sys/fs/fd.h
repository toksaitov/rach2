#ifndef _FD_H_
#define _FD_H_

/*
 * The per-process file descriptor table: the small array of slots a process
 * indexes by descriptor number to reach whatever it has open. A slot names
 * what it points at (a console, a KRFFS file or directory, a socket, a pipe
 * or tty or pty, a devfs or procfs node) and has the per-descriptor
 * flags; the seek offset and the POSIX status flags live one level down in
 * the shared open-file description so dup, dup2, and fork can hand several
 * descriptors the same open. fd 0/1/2 are wired to the console as stdin,
 * stdout, and stderr.
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
    FD_TYPE_SOCKET,       /* Network socket */
    FD_TYPE_PIPE,         /* Pipe end (backed by file_ops + priv) */
    FD_TYPE_DEVICE,       /* devfs character device (backed by file_ops + priv) */
    FD_TYPE_PROC,         /* procfs file snapshot (backed by file_ops + priv) */
    FD_TYPE_SYNTH_DIR,    /* synthetic directory (/dev, /proc, ...); priv encodes it */
    FD_TYPE_PTM,          /* pty master (/dev/ptmx; file_ops + priv = struct pty) */
    FD_TYPE_PTS           /* pty slave (/dev/pts/N; file_ops + priv = struct pty) */
} fd_type_t;

/*
 * File descriptor flags (for open mode)
 */
#define FD_FLAG_READ    0x01
#define FD_FLAG_WRITE   0x02
#define FD_FLAG_CLOEXEC 0x08  /* close-on-exec (F_SETFD FD_CLOEXEC, O_CLOEXEC) */
/* O_APPEND/O_NONBLOCK are OFD status flags (OFD_* in struct open_file), not
 * per-descriptor flags; they are shared by dup/dup2/fork. */

/*
 * Open-file-description status flags. POSIX keeps these on the open-file
 * description (so dup/dup2/fork share them, and fcntl(F_SETFL) on one
 * descriptor is visible through its duplicates), unlike FD_CLOEXEC which is
 * per-descriptor. The values are the Linux i386 O_* bits so fcntl passes
 * them straight through.
 */
#define OFD_APPEND    02000     /* O_APPEND: every write goes to end-of-file */
#define OFD_NONBLOCK  04000     /* O_NONBLOCK: a would-block returns -EAGAIN */

/*
 * Forward declarations for the types a descriptor can point at.
 */
struct krffs_node;
struct socket;
struct file_ops;
struct vfs_mount;

/*
 * Open-file description (OFD): the shared, refcounted state behind one or more
 * descriptors, the seek offset plus the POSIX status flags. dup/dup2/fork copy
 * the fd's pointer to this and bump the refcount, so the duplicates share one
 * offset and one set of status flags (the POSIX open-file description
 * semantics) instead of each carrying its own. Allocated from a fixed pool (see
 * fd.c); refcount 0 means the slot is free, and the last referencing close
 * frees it.
 *
 * There are two offsets because there are two ranges. Everything this kernel
 * opens as a file is under 4 GiB, so `offset` is 32 bits and every path that
 * reads or writes one stays in 32-bit arithmetic. The raw disk windows are
 * not: a whole 250 GB drive needs a 64-bit offset to address its far end,
 * and widening the field every other subsystem uses to reach it would put
 * the whole file, procfs and terminal layer in reach of a change
 * that only the disk windows need. `raw_offset` belongs to those windows
 * alone; nothing else reads it, and it lives here, not in the
 * descriptor so `dup` and `fork` keep sharing one offset the way POSIX says
 * they must.
 */
struct open_file {
    uint32_t refcount;           /* Number of descriptors sharing this OFD */
    uint32_t offset;             /* Shared read/write offset */
    uint64_t raw_offset;         /* Shared offset for the raw disk windows */
    uint32_t status_flags;       /* Shared O_APPEND/O_NONBLOCK (OFD_* above) */
    /* A description on a mounted file system holds its mount, its node, and
     * the file system's per-open handle. The last unref releases all three
     * (fd.c calls vfs_desc_close), which is what makes the file system's
     * open and release counts match one to one with open(2) and the last
     * close. NULL vmnt marks every other backing kind. */
    struct vfs_mount *vmnt;
    void *vnode;
    void *fhandle;
};

/*
 * File descriptor entry. The backing object (type/node/sock, or an ops
 * vtable for pipes/ttys/ptys) and its own reference (sockets are refcounted
 * per descriptor) live here; the seek offset and status flags are shared
 * through the open-file description ofd.
 */
typedef struct fd_entry {
    fd_type_t type;              /* Type of this fd (NONE, CONSOLE, FILE) */
    struct krffs_node *node;     /* Pointer to KRFFS node (for FILE type) */
    struct socket *sock;         /* Pointer to socket (for SOCKET type) */
    struct open_file *ofd;       /* Shared open-file description (offset+flags) */
    const struct file_ops *ops;  /* Backing ops (pipes/ttys/...); NULL = enum dispatch */
    void *priv;                  /* Backing private object (e.g. a pipe end) */
    uint32_t flags;              /* Per-fd flags (READ, WRITE, CLOEXEC) */
} fd_entry_t;

/*
 * File descriptor table embedded in each process's PCB.
 */
typedef struct fd_table {
    fd_entry_t entries[MAX_FDS];
} fd_table_t;

/** Clear every entry of a fresh table to FD_TYPE_NONE. */
void fd_table_init(fd_table_t *table);

/** Wire fd 0/1/2 as console stdin/stdout/stderr; call after fd_table_init. */
void fd_init_stdio(fd_table_t *table);

/** Reserve the lowest free descriptor; fd (>= 0) on success, -1 if table
 * full. */
int fd_alloc(fd_table_t *table);

/** Reserve the lowest free descriptor >= minfd (the fcntl(F_DUPFD)
 * contract). */
int fd_alloc_from(fd_table_t *table, int minfd);

/** Release a descriptor and its OFD reference; 0 ok, -1 if fd invalid. */
int fd_free(fd_table_t *table, int fd);

/*
 * Return the entry for fd, or NULL if fd is out of range or free.
 */
fd_entry_t *fd_get(fd_table_t *table, int fd);

/*
 * Add or drop a reference to an open-file description. dup/dup2/fork point a
 * new descriptor at an existing OFD and call fd_ofd_ref so the shared seek
 * offset outlives the original; fd_free calls fd_ofd_unref. (The OFD frees
 * itself back to the pool when its last reference goes.)
 */
void fd_ofd_ref(struct open_file *ofd);
void fd_ofd_unref(struct open_file *ofd);

/*
 * Return 1 if fd is allocated, 0 otherwise.
 */
int fd_is_valid(fd_table_t *table, int fd);

#endif /* _FD_H_ */
