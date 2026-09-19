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
 * Policy: the descriptors a process may hold at most, which is the width of
 * a select fd_set, so no descriptor is one select cannot name. A table
 * starts with FD_TABLE_FIRST entries and doubles as it fills, up to the
 * limit; the entries are in the heap and freed with the process.
 */
#define FD_TABLE_LIMIT 1024
#define FD_TABLE_FIRST 16

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
    FD_TYPE_NONE,        /* Slot is free */
    FD_TYPE_CONSOLE,     /* Console (stdin/stdout/stderr) */
    FD_TYPE_FILE,        /* Regular file from KRFFS */
    FD_TYPE_DIR,         /* Directory (for getdents64) */
    FD_TYPE_FRAMEBUFFER, /* VGA framebuffer device */
    FD_TYPE_MOUSE,       /* PS/2 mouse device */
    FD_TYPE_SOCKET,      /* Network socket */
    FD_TYPE_PIPE,        /* Pipe end (backed by file_ops + priv) */
    FD_TYPE_DEVICE,      /* devfs character device (backed by file_ops + priv) */
    FD_TYPE_PROC,        /* procfs file snapshot (backed by file_ops + priv) */
    FD_TYPE_SYNTH_DIR,   /* synthetic directory (/dev, /proc, ...); priv encodes it */
    FD_TYPE_PTM,         /* pty master (/dev/ptmx; file_ops + priv = struct pty) */
    FD_TYPE_PTS          /* pty slave (/dev/pts/N; file_ops + priv = struct pty) */
} fd_type_t;

/*
 * File descriptor flags (for open mode)
 */
#define FD_FLAG_READ    0x01
#define FD_FLAG_WRITE   0x02
#define FD_FLAG_CLOEXEC 0x08 /* close-on-exec (F_SETFD FD_CLOEXEC, O_CLOEXEC) */
/* O_APPEND/O_NONBLOCK are OFD status flags (OFD_* in struct open_file), not
 * per-descriptor flags; they are shared by dup/dup2/fork. */

/*
 * Open-file-description status flags. POSIX keeps these on the open-file
 * description (so dup/dup2/fork share them, and fcntl(F_SETFL) on one
 * descriptor is visible through its duplicates), unlike FD_CLOEXEC which is
 * per-descriptor. The values are the Linux i386 O_* bits so fcntl passes
 * them straight through.
 */
#define OFD_APPEND   02000 /* O_APPEND: every write goes to end-of-file */
#define OFD_NONBLOCK 04000 /* O_NONBLOCK: a would-block returns -EAGAIN */

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
    uint32_t refcount;     /* Number of descriptors sharing this OFD */
    uint64_t offset;       /* Shared read/write offset */
    uint64_t raw_offset;   /* Shared offset for the raw disk windows */
    uint32_t status_flags; /* Shared O_APPEND/O_NONBLOCK (OFD_* above) */
    /* A description on a mounted file system holds its mount, its node, and
     * the file system's per-open handle. The last unref releases all three
     * (fd.c calls vfs_desc_close), which is what makes the file system's
     * open and release counts match one to one with open(2) and the last
     * close. NULL vmnt marks every other backing kind. */
    struct vfs_mount *vmnt;
    void *vnode;
    void *fhandle;
    /* The description lock: read, write, lseek, getdents64 and ftruncate
     * hold the description for the length of the operation, so a sharer
     * cannot use the offset while another sleeps in a disk request with it.
     * The wait queue is beside the pool in fd.c. */
    int busy;
    /* A node of another type: the canonical path it was opened by, which
     * the `*at` calls resolve relative names against for a directory whose
     * type has no `path` operation and `/proc/PID/fd` reports, or "" when
     * the path did not fit, so that such a call answers ENAMETOOLONG. */
    char dirpath[64];
};

/*
 * File descriptor entry. The backing object (type/node/sock, or an ops
 * vtable for pipes/ttys/ptys) and its own reference (sockets are refcounted
 * per descriptor) live here; the seek offset and status flags are shared
 * through the open-file description ofd.
 */
typedef struct fd_entry {
    fd_type_t type;             /* Type of this fd (NONE, CONSOLE, FILE) */
    struct krffs_node *node;    /* Pointer to KRFFS node (for FILE type) */
    struct socket *sock;        /* Pointer to socket (for SOCKET type) */
    struct open_file *ofd;      /* Shared open-file description (offset+flags) */
    const struct file_ops *ops; /* Backing ops (pipes/ttys/...); NULL = enum dispatch */
    void *priv;                 /* Backing private object (e.g. a pipe end) */
    uint32_t flags;             /* Per-fd flags (READ, WRITE, CLOEXEC) */
} fd_entry_t;

/*
 * File descriptor table in each process's record: `cap` entries in the
 * heap, every one FD_TYPE_NONE unless open.
 */
typedef struct fd_table {
    fd_entry_t *entries;
    int cap;
    int limit; /* the process's RLIMIT_NOFILE, at most FD_TABLE_LIMIT */
} fd_table_t;

/** Close every descriptor's backing object before releasing its slot. */
void release_fd_table(fd_table_t *table);

/** Give a fresh table its first entries; 0, or -1 when the heap has none. */
int fd_table_init(fd_table_t *table);

/** Free the entries; the descriptors must have been released first. */
void fd_table_free(fd_table_t *table);

/** How many entries the table has now: the bound of a walk over it. */
int fd_table_cap(const fd_table_t *table);

/**
 * Grow the table so that `fd` is inside it, for dup2 onto a number past
 * the end; 0 when it is, -1 when `fd` is past the process's limit or the
 * heap has no room.
 */
int fd_table_reach(fd_table_t *table, int fd);

/**
 * Make `dst`, which has no entries, a copy of `src`'s entries, for fork;
 * the references the copies share are the caller's to add. 0 or -1.
 */
int fd_table_copy(fd_table_t *dst, const fd_table_t *src);

/** Wire fd 0/1/2 as console stdin/stdout/stderr; call after fd_table_init. */
void fd_init_stdio(fd_table_t *table);

/** Reserve the lowest free descriptor; fd (>= 0) on success, -1 if table
 * full. */
int fd_alloc(fd_table_t *table);

/** 1 when `fd_alloc` would find or make a slot, for a call that must know
 * before it creates or truncates anything; a heap with no room for the
 * growth is not foreseen here. */
int fd_can_alloc(const fd_table_t *table);

/** Reserve the lowest free descriptor >= minfd (the fcntl(F_DUPFD)
 * contract). */
int fd_alloc_from(fd_table_t *table, int minfd);

/** Release a descriptor and its OFD reference; 0 ok, -1 if fd invalid. */
int fd_free(fd_table_t *table, int fd);

/** The entry for fd, or NULL if fd is out of range or free. */
fd_entry_t *fd_get(fd_table_t *table, int fd);

/**
 * Add or drop a reference to an open-file description. dup/dup2/fork point a
 * new descriptor at an existing OFD and call fd_ofd_ref so the shared seek
 * offset outlives the original; fd_free calls fd_ofd_unref. The OFD frees
 * itself back to the pool when its last reference goes.
 */
void fd_ofd_ref(struct open_file *ofd);
void fd_ofd_unref(struct open_file *ofd);

/** The description lock (see `busy` above); the lock is uninterruptible and
 * short: one file operation. */
void fd_ofd_lock(struct open_file *ofd);
void fd_ofd_unlock(struct open_file *ofd);

/** Repoint every description field naming `from` at `to`, after a KRFFS
 * relocation moved a node. Called by vfs_repoint, which owns the sweep. */
void fd_ofd_repoint(struct vfs_mount *mnt, void *from, void *to);

/** Whether a description of (`mnt`, `node`) is inside an operation now. */
int fd_node_busy(struct vfs_mount *mnt, void *node);

/** 1 if fd is allocated, 0 otherwise. */
int fd_is_valid(fd_table_t *table, int fd);

#endif /* _FD_H_ */
