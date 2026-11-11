#ifndef _FD_H_
#define _FD_H_

/*
 * File descriptor tables and open file descriptions.
 *
 * A process indexes its table by descriptor number. An entry says what the
 * descriptor refers to (the console, a file or directory, a socket, a pipe,
 * a terminal, a device, an anonymous object) and has the flags of that
 * descriptor alone; the seek offset and the status flags are in the
 * open file description the entry points at, which dup, dup2 and fork share
 * between descriptors.
 */

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * A table starts with FD_TABLE_FIRST entries and doubles as it fills, up to
 * the caller's limit; the entries are one array in the heap, freed with the
 * table. FD_TABLE_LIMIT is the width of a select fd_set, the soft
 * RLIMIT_NOFILE every process starts with, so that a program that selects
 * names every descriptor it is given unless it raises the limit.
 */
#define FD_TABLE_LIMIT 1024
#define FD_TABLE_FIRST 16

/*
 * Policy: the hard RLIMIT_NOFILE a process starts with, which init's
 * children inherit, and the most root may set, `fd_nr_open`, which
 * fs.nr_open reads and root may set between FD_NR_OPEN_MIN and
 * FD_NR_OPEN_MAX, as Linux's sysctl_nr_open. A table of FD_NR_OPEN_MAX
 * entries takes nearly 1 GiB of heap on x86-64.
 */
#define FD_NOFILE_HARD 524288
#define FD_NR_OPEN     1048576
#define FD_NR_OPEN_MIN 64
#define FD_NR_OPEN_MAX 16777216

/* fs.nr_open: the most descriptors a process may be allowed, and the bound
 * of a table's growth whatever the caller's limit says. */
extern int fd_nr_open;

/* fs.file-max: the open file descriptions the machine may have, past which
 * a caller that is not privileged is given ENFILE; LONG_MAX by default, as
 * a Linux system with systemd sets it. */
extern unsigned long fd_file_max;

/** How many open file descriptions there are: the first figure of
 * fs.file-nr. */
unsigned long fd_files_open(void);

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
    FD_TYPE_ANON,        /* eventfd, epoll, timerfd, inotify, signalfd, message
                            queue or namespace file (priv = struct anon_obj) */
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
 * The status flags of an open file description, in `status_flags`. POSIX
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
struct vfs_name;

/*
 * An open file description (OFD): the state that the descriptors made from
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
    /* A FIFO reader's writer-open count for poll: equal to the pipe's
     * count until a writer has opened since a nonblocking read-only open
     * with no writer. Other FIFO opens start with a different count. */
    uint32_t fifo_write_opens;
    /* A description of a file of a mounted file system holds its mount, its
     * node and the file system's handle of the open. The last unref releases
     * all three through `vfs_desc_close`, so the file system sees one release
     * for each open. `vmnt` is NULL for every other kind of description. */
    struct vfs_mount *vmnt;
    /* The held mount of a synthetic node opened by path, for NO_XDEV when
     * /proc/PID/fd follows this description; NULL on anonymous objects. */
    struct vfs_mount *synth_mnt;
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
     * it was opened by, from the heap, or NULL when the heap had no room
     * for it: what the `*at` calls resolve a relative name against when the
     * directory's type has no `path` operation, answering ENAMETOOLONG for
     * NULL, and what `/proc/PID/fd` reports. The last unref frees it. */
    char *dirpath;
    /* The selected regular-file entry, shared by fork and descriptor exec.
     * Its parent and mount stay held until the description's last unref. */
    struct vfs_name *name;
    /* The count of descriptions made when this one was, which tells it
     * from a new description the heap gives the same address after its
     * last close; an epoll registration keeps both. */
    uint32_t serial;
    /* For a device opened through a device node of tmpfs or ext2, the only
     * types that make one, that node and its mount, held as an O_PATH
     * description holds its own: what an open of /proc/PID/fd/N checks and
     * opens again, as Linux follows the descriptor's path. The type's hold
     * keeps the node after an unlink. NULL for every other description.
     * The last unref gives back both. */
    struct vfs_mount *dev_mnt;
    void *dev_node;
    /* A write or executable reservation, taken before an open may sleep.
     * The resolver holds this node until the description takes ownership. */
    struct vfs_mount *access_mnt;
    void *access_node;
    int access; /* 0: none, 1: writer, 2: executable */
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
    /* For a description of a terminal, the console or a pty's slave, the
     * terminal's count of vhangups when it was opened; a later vhangup
     * leaves it hung up (`tty_desc_hung`). */
    uint32_t tty_hangups;
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
    struct open_file *ofd;         /* Shared open file description */
    const struct file_ops *ops;    /* Operations of the object; NULL: by `type` */
    void *priv;                    /* The object `ops` work on, such as a pipe end */
    const struct devfs_entry *dev; /* The device table row, for a DEVICE */
    uint32_t flags;                /* FD_FLAG_* */
} fd_entry_t;

/*
 * File descriptor table, which each process's record points to: `cap`
 * entries in the heap, every one FD_TYPE_NONE unless open. Threads and the
 * processes clone makes with CLONE_FILES share one table, `refs` counting
 * them. A table grows by moving its entries to a larger array, with no sleep
 * between, so a lookup by number always finds the entry where it is now. A
 * call of one sharer may sleep with the address of an entry while another
 * grows the table; the old array then stays on `retired`, unchanged, until
 * no sharer's call holds a descriptor (`fd_table_reap`), so such an address
 * reads what it read before. A call that writes an entry after a sleep, or
 * reads what another thread may have changed in it since, takes the entry
 * as it is now first (`fd_entry_now`). The table has no limit of its own below
 * `fd_nr_open`: each sharer is held to its own RLIMIT_NOFILE (`fd_limit`).
 * The table is the owner of its sharers' POSIX record locks (`lock.h`),
 * which its release drops as it closes the descriptors.
 */
struct fd_retired;

typedef struct fd_table {
    fd_entry_t *entries;
    int cap;
    int refs;
    /* No slot below it is free: where a search for the lowest free
     * descriptor begins, as Linux's `next_fd`. */
    int next_free;
    struct fd_retired *retired; /* Arrays a growth replaced, newest first */
} fd_table_t;

struct process;

/** Release every descriptor of `table`, its object and then its slot, when
 * the last process that shares the table lets it go; a second call finds
 * nothing to release (syscall_proc.c). May sleep. */
void release_fd_table(fd_table_t *table);

/** Give a new table its first FD_TABLE_FIRST entries, all free, and no
 * retired arrays, whatever `table` held; it frees no old entries and leaves
 * `refs` to the caller. 0, or -1 when `table` is NULL or the heap has no
 * room. */
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

/** Take another reference to `table` for the CLONE_FILES child `child`,
 * first giving `child`, and on the table's first share its owner, a map of
 * the descriptors its calls hold as wide as the table (`fd_held` in
 * `process.h`); 0, or -1 when the heap has no room for them. */
int fd_table_share(fd_table_t *table, struct process *child);

/** Grow the array of `table` to at least `cap` entries, whatever the
 * limits, for an exec's staged copy of a shared table that grew since it
 * was made; 0, or -ENOMEM. */
int fd_table_fit(fd_table_t *table, int cap);

/** Free the arrays a growth of `table` replaced, unless a call of a sharer
 * other than the caller holds a descriptor and may have the address of an
 * entry in one; the dispatcher calls it as each call ends. */
void fd_table_reap(fd_table_t *table);

/** How many entries the table has now: the bound of a walk over it. */
int fd_table_cap(const fd_table_t *table);

/**
 * One more than the highest descriptor the caller may be given in `table`:
 * the calling process's soft RLIMIT_NOFILE when the table is its own, which
 * a process that shares the table under CLONE_FILES keeps apart, as Linux
 * applies the caller's limit at each allocation; FD_TABLE_LIMIT for any
 * other table, which the kernel fills for nobody's call. Never more than
 * `fd_nr_open`.
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
 * passes it. `hangups` is the console's count of vhangups now, which each
 * description records (`tty_hangups`). Returns 0, or -1 with nothing left
 * open when the heap has no room.
 */
int fd_init_stdio(fd_table_t *table, uint32_t hangups);

/**
 * Reserve the lowest free descriptor, growing the table up to the caller's
 * limit. The slot gets a new description and stays FD_TYPE_NONE until the
 * caller sets its type and object; `fd_free` returns it. In a shared table
 * the slot is held by the call, as `fd_get` holds one. Returns the
 * descriptor; -EMFILE when no slot is free below the limit; -ENFILE when the
 * machine has `fd_file_max` descriptions and the caller is not privileged;
 * or -ENOMEM, with the slot left free, when the heap has no room for the
 * growth or the description. The calls that make a descriptor return what
 * they get, so that EMFILE means the limit and ENOMEM the heap, as on Linux.
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
 * Add or drop a reference to an open file description. The last unref drops
 * the description's record locks, reports its close to inotify, unpins an
 * O_PATH node, closes a description of a mounted file system and frees the
 * description; it may sleep. NULL is ignored.
 */
void fd_ofd_ref(struct open_file *ofd);
void fd_ofd_unref(struct open_file *ofd);

/**
 * A new open file description with one reference and no descriptor pointing
 * at it: for a call that changes a file by path with the rules of an open
 * one, truncate(2), and for the program's file an address space keeps
 * (`fs_exe_open`). NULL when the heap has no room; `fd_ofd_unref` frees it.
 */
struct open_file *fd_ofd_new(void);

struct vfs_ref;
/** Reserve `ref` for writing (1) or execution (2) in `ofd`, in place of
 * any reservation `ofd` held: 0, or -ETXTBSY for an opposite reservation on
 * the same file, found from a count of each file's reservations as Linux's
 * i_writecount, which leaves `ofd`'s own as it was. Needs no memory, since
 * the description brought the count's record when it was made, and does
 * not sleep; the caller holds the node, and transfers that hold to the
 * description or releases the description before the node. The big kernel
 * lock is required. */
int fd_ofd_access(struct open_file *ofd, const struct vfs_ref *ref, int access);

/** Give back the reservation `ofd` holds, if any. The big kernel lock is
 * required. */
void fd_ofd_access_drop(struct open_file *ofd);

/** Take or release the description lock (see `busy` above), and with it,
 * taken first and released last, the namespace lock of the description's
 * mount when its type serialises every use. The waits sleep and a signal
 * does not end them; the lock is held for one file operation. */
void fd_ofd_lock(struct open_file *ofd);
void fd_ofd_unlock(struct open_file *ofd);

/** Repoint the node of every description of `mnt` that names `from` at
 * `to`, with its reservation, and the handle too on a mounted KRFFS volume,
 * after a growing write moved a KRFFS node. Called by `vfs_repoint`, which
 * sweeps every holder. */
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
 * Whether `e` is one of `table`'s entries, in its array or in one a growth
 * replaced, and not a copy of one that a file mapping or a message in
 * flight keeps: a close of a descriptor of the table drops the table's
 * record locks, and the release of such a copy does not.
 */
int fd_in_table(const fd_table_t *table, const fd_entry_t *e);

/**
 * The entry `e` is now: the same slot in `table`'s array when `e` is the
 * address of an entry in an array a growth replaced, else `e` itself, which
 * may be a copy no table has. A call that took the address of an entry and
 * then slept, and so may have let another sharer grow the table, reads it
 * again through this before it writes the entry, reads what another thread
 * may have changed in it, or compares it by address.
 */
fd_entry_t *fd_entry_now(fd_table_t *table, fd_entry_t *e);

/** Repoint the `node` of every entry of `table` that names `from` at `to`,
 * in its array and in those a growth replaced, after a growing write moved
 * a KRFFS node (`vfs_repoint`). */
void fd_table_repoint(fd_table_t *table, const void *from, void *to);

#endif /* _FD_H_ */
