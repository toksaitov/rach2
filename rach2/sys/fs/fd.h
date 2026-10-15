#ifndef _FD_H_
#define _FD_H_

/*
 * File descriptor tables and open-file descriptions.
 *
 * A process indexes its table by descriptor number. An entry says what the
 * descriptor refers to (the console, a file or directory, a socket, a pipe,
 * a terminal, a device, an anonymous object) and has the flags of that
 * descriptor alone; the seek offset and the status flags are in the
 * open-file description the entry points at, which dup, dup2 and fork share
 * between descriptors.
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

#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

/* What a descriptor refers to, which selects how the calls reach it. */
typedef enum {
    FD_TYPE_NONE,        /* Slot is free, or reserved when `ofd` is set */
    FD_TYPE_CONSOLE,     /* The kernel console */
    FD_TYPE_FILE,        /* File of a mounted file system or the KRFFS root */
    FD_TYPE_DIR,         /* Directory, for getdents64 */
    FD_TYPE_FRAMEBUFFER, /* The framebuffer, opened by the name "fb" */
    FD_TYPE_MOUSE,       /* The mouse, PS/2 or USB, opened by the name "mouse" */
    FD_TYPE_SOCKET,      /* Network socket */
    FD_TYPE_PIPE,        /* Pipe end (backed by file_ops + priv) */
    FD_TYPE_DEVICE,      /* devfs character device (backed by file_ops + priv) */
    FD_TYPE_PROC,        /* procfs file snapshot (backed by file_ops + priv) */
    FD_TYPE_SYNTH_DIR,   /* synthetic directory (/dev, /proc, ...); priv encodes it */
    FD_TYPE_PTM,         /* pty master (/dev/ptmx; file_ops + priv = struct pty) */
    FD_TYPE_PTS,         /* pty slave (/dev/pts/N; file_ops + priv = struct pty) */
    FD_TYPE_ANON,        /* eventfd, epoll, inotify ... (file_ops + priv = struct anon_obj) */
    FD_TYPE_UNIX,        /* AF_UNIX socket (file_ops + priv = struct unix_sock) */
    FD_TYPE_SYNTH_PATH   /* O_PATH on a /dev or /proc leaf; priv = struct vpath */
} fd_type_t;

/* The flags of one descriptor, in `fd_entry_t.flags`. */
#define FD_FLAG_READ      0x01
#define FD_FLAG_WRITE     0x02
#define FD_FLAG_CLOEXEC   0x08 /* close-on-exec (F_SETFD FD_CLOEXEC, O_CLOEXEC) */
#define FD_FLAG_PATH      0x10 /* O_PATH: names a file, reads and writes nothing */
#define FD_FLAG_CLOSING   0x20 /* closed; no lookup finds it until the slot is free */
#define FD_FLAG_RELEASING 0x40 /* its release has begun, and may be asleep */

/*
 * The status flags of an open-file description, in `status_flags`. POSIX
 * keeps them on the description, so dup, dup2 and fork share them and
 * fcntl(F_SETFL) on one descriptor shows through its duplicates; FD_CLOEXEC
 * belongs to one descriptor. The values are Linux's O_* bits, the same on
 * i386 and x86-64, so fcntl passes them through.
 */
#define OFD_APPEND   02000 /* O_APPEND: every write goes to end-of-file */
#define OFD_NONBLOCK 04000 /* O_NONBLOCK: a would-block returns -EAGAIN */

struct krffs_node;
struct socket;
struct file_ops;
struct vfs_mount;

/*
 * An open-file description (OFD): the state that the descriptors made from
 * one open share, the seek offset and the status flags among it. dup, dup2
 * and fork point a new descriptor at the same description and add a
 * reference, so the duplicates share one offset, as POSIX requires.
 * Descriptions are in the heap, on a list of live ones in fd.c, and the last
 * reference frees one. A call that changes a file by path with the rules of
 * an open one, truncate(2), and an address space that keeps its program's
 * file, each hold one that no descriptor points at.
 *
 * The raw disk windows of devfs, whose position on a large disk is past
 * what any other device reaches, keep their position in `raw_offset`, which
 * no other kind of descriptor reads or writes; `offset` is the position of
 * every other kind.
 */
struct open_file {
    uint32_t refcount;     /* References: descriptors and the holders above */
    uint64_t offset;       /* Shared read/write offset */
    uint64_t raw_offset;   /* Shared offset of a raw disk window */
    uint32_t status_flags; /* Shared O_APPEND/O_NONBLOCK (OFD_* above) */
    /* A description of a file of a mounted file system holds its mount, its
     * node and the file system's handle of the open. The last unref releases
     * all three through `vfs_desc_close`, so the file system sees one release
     * for each open. `vmnt` is NULL for every other kind of description. */
    struct vfs_mount *vmnt;
    void *vnode;
    void *fhandle;
    /* With `vmnt`, the inode number the file had when it was opened, or for
     * a FIFO of the KRFFS root its node's address: the key a FIFO's pipe is
     * found by, and the file's locks on a type other than KRFFS, FAT32 and
     * exFAT, whose locks are found by `vnode` instead. */
    uint64_t ino;
    /* The description lock: read, write, lseek, getdents64 and ftruncate
     * hold the description for the length of the operation, so a sharer
     * cannot use the offset while another sleeps in a disk request with it;
     * pread64 and pwrite64 hold it on a regular file and leave the offset
     * alone. The lock's wait queue is in the description's record in fd.c. */
    int busy;
    /* For a file of a mount other than the KRFFS root, the canonical path
     * it was opened by, or "" when the path did not fit: what the `*at`
     * calls resolve a relative name against when the directory's type has
     * no `path` operation, answering ENAMETOOLONG for "", and what
     * `/proc/PID/fd` reports. */
    char dirpath[64];
    /* The count of descriptions made when this one was, which tells it
     * from a new description the heap gives the same address after its
     * last close; an epoll registration keeps both. */
    uint32_t serial;
    /* The token of the type's `pin` for an O_PATH description on a type
     * that keeps a node only while it is open, which the last unref gives
     * to `unpin`; NULL for every other description. */
    void *pin;
    /* What inotify reports of the description (`io/inotify.h`): the
     * OFD_WATCH_* bits an open sets, and, for an open made while a watch
     * existed, the inode number of the directory the file was opened in
     * and its name there, from the heap, so that the directory's watches
     * hear of its reads, writes and close. The last unref reports the close
     * and frees the name. */
    uint32_t watch_flags;
    uint64_t parent_ino;
    char *leaf;
};

/* An open(2) made the description, which reports its open and close, and
 * made it for writing; the file it is open on is a directory; the open
 * created the file, which the open's own report reads and clears. */
#define OFD_WATCH_OPEN  1u
#define OFD_WATCH_WRITE 2u
#define OFD_WATCH_DIR   4u
#define OFD_WATCH_MADE  8u

/*
 * A file descriptor: what it refers to and the descriptor's own flags. A
 * socket is counted once for each descriptor that has it; the offset and
 * status flags are in the shared description `ofd`.
 */
struct devfs_entry;

typedef struct fd_entry {
    fd_type_t type;
    struct krffs_node *node;       /* FILE or DIR of the KRFFS root, not "/" */
    struct socket *sock;           /* For a SOCKET */
    struct open_file *ofd;         /* Shared open-file description */
    const struct file_ops *ops;    /* Operations of the object; NULL: by `type` */
    void *priv;                    /* The object `ops` work on, such as a pipe end */
    const struct devfs_entry *dev; /* The device table row, for a DEVICE */
    uint32_t flags;                /* FD_FLAG_* */
} fd_entry_t;

/*
 * File descriptor table, which each process's record points to: `cap`
 * entries in the heap, every one FD_TYPE_NONE unless open. Threads and the
 * processes clone makes with CLONE_FILES share one table, `refs` counting
 * them. A shared table has all FD_TABLE_LIMIT entries, so an entry stays
 * where it is while a call of one sharer sleeps with it and another opens
 * files. The table has no limit of its own below FD_TABLE_LIMIT: each
 * sharer is held to its own RLIMIT_NOFILE (`fd_limit`). The table is the
 * owner of its sharers' POSIX record locks (`lock.h`), which its release
 * drops as it closes the descriptors.
 */
typedef struct fd_table {
    fd_entry_t *entries;
    int cap;
    int refs;
} fd_table_t;

/** Release every descriptor of `table`, its object and then its slot, when
 * the last process that shares the table lets it go; a second call finds
 * nothing to release (syscall_proc.c). May sleep. */
void release_fd_table(fd_table_t *table);

/** Give a new table its first FD_TABLE_FIRST entries, all free; it frees no
 * old entries. 0, or -1 when `table` is NULL or the heap has no room. */
int fd_table_init(fd_table_t *table);

/** Free the entries; the descriptors must have been released first. */
void fd_table_free(fd_table_t *table);

/** A table from the heap with its first entries and one reference; NULL
 * when the heap has no room. */
fd_table_t *fd_table_new(void);

/** A table from the heap with a copy of `src`'s entries and one reference,
 * for fork; the references the copies share are the caller's to add, and a
 * slot an unfinished open has reserved is free in the copy. NULL when the
 * heap has no room. */
fd_table_t *fd_table_dup(const fd_table_t *src);

/** Free a table `fd_table_new` or `fd_table_dup` made, entries and all;
 * the descriptors must have been released first. */
void fd_table_delete(fd_table_t *table);

/** Take another reference to `table` for a CLONE_FILES child, first growing
 * it to FD_TABLE_LIMIT entries if it has fewer; 0, or -1 when the heap has
 * no room for them. */
int fd_table_share(fd_table_t *table);

/** How many entries the table has now: the bound of a walk over it. */
int fd_table_cap(const fd_table_t *table);

/**
 * One more than the highest descriptor the caller may be given in `table`:
 * the calling process's soft RLIMIT_NOFILE when the table is its own, which
 * a process that shares the table under CLONE_FILES keeps apart, as Linux
 * applies the caller's limit at each allocation; FD_TABLE_LIMIT for any
 * other table, which the kernel fills for nobody's call.
 */
int fd_limit(const fd_table_t *table);

/**
 * Grow the table so that `fd` is inside it, for dup2 onto a number past
 * the end; 0 when it is, -EBADF when `fd` is negative or past the process's
 * limit, which is dup2's answer for such a destination, or -ENOMEM when the
 * heap has no room.
 */
int fd_table_reach(fd_table_t *table, int fd);

/**
 * Open fds 0, 1 and 2 of a table `fd_table_init` made on the console, each
 * for reading and writing with a description of its own, as Linux gives init
 * `/dev/console` opened O_RDWR on all three: a program may write through its
 * standard input, as tmux's server draws through the terminal its client
 * passes it. Returns 0, or -1 with nothing left open when the heap has no
 * room.
 */
int fd_init_stdio(fd_table_t *table);

/**
 * Reserve the lowest free descriptor, growing the table up to the caller's
 * limit. The slot gets a new description and stays FD_TYPE_NONE until the
 * caller sets its type and object; `fd_free` returns it. Returns the
 * descriptor; -EMFILE when no slot is free below the limit; or -ENOMEM, with
 * the slot left free, when the heap has no room for the growth or the
 * description. The calls that make a descriptor return what they get, so
 * that EMFILE means the limit and ENOMEM the heap, as on Linux.
 */
int fd_alloc(fd_table_t *table);

/** 1 when `fd_alloc` would find or make a slot, for a call that must know
 * before it creates or truncates anything; a heap with no room for the
 * growth is not foreseen here. */
int fd_can_alloc(const fd_table_t *table);

/** Reserve the lowest free descriptor >= minfd (the fcntl(F_DUPFD)
 * contract), with `fd_alloc`'s results. */
int fd_alloc_from(fd_table_t *table, int minfd);

/** Clear a descriptor's slot and drop its reference to its description,
 * also for a slot reserved and given no type. The object of the descriptor
 * is the caller's to release first. 0, or -1 when the slot is free or `fd`
 * is outside the table. Dropping the last reference closes a description of
 * a mounted file system, which may sleep. */
int fd_free(fd_table_t *table, int fd);

/** The entry for fd, or NULL if fd is out of range, free or closing. A
 * lookup in the calling thread's shared table marks the descriptor held by
 * the call until it ends. */
fd_entry_t *fd_get(fd_table_t *table, int fd);

/**
 * Add or drop a reference to an open-file description. The last unref drops
 * the description's record locks, reports its close to inotify, unpins an
 * O_PATH node, closes a description of a mounted file system and frees the
 * description; it may sleep. NULL is ignored.
 */
void fd_ofd_ref(struct open_file *ofd);
void fd_ofd_unref(struct open_file *ofd);

/**
 * A new open-file description with one reference and no descriptor pointing
 * at it: for a call that changes a file by path with the rules of an open
 * one, truncate(2), and for the program's file an address space keeps
 * (`fs_exe_open`). NULL when the heap has no room; `fd_ofd_unref` frees it.
 */
struct open_file *fd_ofd_new(void);

/** Take or release the description lock (see `busy` above), and with it,
 * taken first and released last, the namespace lock of the description's
 * mount when its type serialises every use. The waits sleep and a signal
 * does not end them; the lock is held for one file operation. */
void fd_ofd_lock(struct open_file *ofd);
void fd_ofd_unlock(struct open_file *ofd);

/** Repoint the node of every description of `mnt` that names `from` at
 * `to`, and the handle too on a mounted KRFFS volume, after a growing write
 * moved a KRFFS node. Called by `vfs_repoint`, which sweeps every holder. */
void fd_ofd_repoint(struct vfs_mount *mnt, const void *from, void *to);

/** Whether a description of (`mnt`, `node`) is inside an operation now, as
 * a write or a truncation is for the length of its call: what an attachment
 * of the file asks before it takes the file. */
int fd_node_busy(struct vfs_mount *mnt, void *node);

/**
 * Whether the description at `ofd`, made with `serial`, is open still: some
 * descriptor or other holder has a reference to it. Only the list of live
 * descriptions is read, so `ofd` may be the address of one freed since, or
 * of a later one at that address, whose serial is another.
 */
int fd_ofd_live(const struct open_file *ofd, uint32_t serial);

/** How many descriptions of (`mnt`, `node`) there are, `going` counted only
 * when it keeps a reference besides the one its release drops: the holders
 * of a KRFFS root node that no descriptor's `node` names, a FIFO's ends
 * among them, wherever their descriptors are. */
int fd_ofd_holders(const struct vfs_mount *mnt, const void *node, const struct open_file *going);

/** 1 if `fd` is open and not closing, 0 otherwise. */
int fd_is_valid(fd_table_t *table, int fd);

/**
 * Whether `e` is one of `table`'s entries, and not a copy of one that a file
 * mapping or a message in flight keeps: a close of a descriptor of the table
 * drops the table's record locks, and the release of such a copy does not.
 */
int fd_in_table(const fd_table_t *table, const fd_entry_t *e);

#endif /* _FD_H_ */
