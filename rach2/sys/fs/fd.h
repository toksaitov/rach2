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
#include "../lib/spinlock.h"
#include "../lib/rcu.h"

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
#define FD_NR_OPEN_MIN ((int)sizeof(long) * 8) /* BITS_PER_LONG */
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

/**
 * Write one decimal creator pid per live description into `buf`'s `cap`
 * bytes, and return the whole text's length, including bytes that did not
 * fit. NULL and 0 size the text. The pid is the allocating thread group's
 * id in the initial PID namespace, 0 before a current process exists; it
 * stays with the description through transfers and after the creator's
 * exit, until its final reference goes. The walk may sleep; it holds the
 * description list's spinlock for a bounded batch of records at a time, so
 * a description live through the whole walk is written once, one made
 * during it is not, and one that ends during it may be. -ENOMEM when the
 * walk's cursor cannot be made.
 */
int32_t fd_files_creators(char *buf, uint32_t cap);

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
#define OFD_EXCL     0200     /* O_EXCL: kept only by a pidfd of a thread, PIDFD_THREAD */
#define OFD_APPEND   02000    /* O_APPEND: every write goes to end-of-file */
#define OFD_NONBLOCK 04000    /* O_NONBLOCK: a would-block returns -EAGAIN */
#define OFD_DSYNC    010000   /* O_DSYNC: a write ends with an fdatasync */
#define OFD_SYNC     04010000 /* O_SYNC, O_DSYNC's bit with __O_SYNC: an fsync */
#define OFD_DIRECT   040000   /* O_DIRECT: reads and writes go around the cache */

/* The status flags an open with Linux's open flags `f` gives the
 * description it makes, before the description is published, whatever the
 * kind of file: O_APPEND and O_NONBLOCK, and O_SYNC's two bits when __O_SYNC
 * is among them, alone or with O_DSYNC, as Linux's build_open_flags adds
 * O_DSYNC to __O_SYNC (fs/open.c:1296) and keeps them in f_flags for
 * F_GETFL; else O_DSYNC's bit. An O_PATH open keeps none of them. */
#define OFD_OPEN_STATUS(f)                           \
    (((uint32_t)(f) & (OFD_APPEND | OFD_NONBLOCK)) | \
     (((uint32_t)(f) & (OFD_SYNC & ~OFD_DSYNC)) ? OFD_SYNC : ((uint32_t)(f) & OFD_DSYNC)))

struct krffs_node;
struct socket;
struct file_ops;
struct vfs_mount;
struct vfs_name;

/*
 * The readahead state of a description, Linux's `struct file_ra_state`
 * (include/linux/fs.h), which the file page cache keeps for read(2) and
 * its kin under the description's lock (filecache.c). The last window read
 * ahead is the `size` pages from `start`, and the reader's arrival at the
 * first of its last `async_size` pages, which carries the cache's mark,
 * starts the next window without waiting for it. Zero at the open but for
 * `prev_pos`.
 */
/* The pages of the default readahead window: Linux's VM_READAHEAD_PAGES,
 * 128 KiB (include/linux/pagemap.h:1375), which `bdi_init` gives every
 * device (mm/backing-dev.c:1037). */
#define FILE_RA_PAGES 32u

struct file_ra_state {
    uint32_t start;      /* the first page of the last window */
    uint32_t size;       /* its pages */
    uint32_t async_size; /* the pages at its end that start the next one */
    /* The most pages a window takes: 0 for FILE_RA_PAGES, and twice that
     * after POSIX_FADV_SEQUENTIAL. */
    uint32_t ra_pages;
    /* The byte after the last one a read took, all ones at the open, as
     * Linux's `file_ra_state_init` sets it (mm/readahead.c:138-142). */
    uint64_t prev_pos;
    /* POSIX_FADV_RANDOM, Linux's FMODE_RANDOM: a miss reads what the read
     * asks for and no more. */
    uint32_t random;
};

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
struct epitem;

struct open_file {
    /* References: descriptors and the holders above. Changed only by an
     * atomic step (`fd_ofd_ref`, `fd_ofd_unref`), since descriptors of one
     * description in different tables close without a common lock. */
    uint32_t refcount;
    uint64_t offset;     /* Shared read/write offset */
    uint64_t raw_offset; /* Shared offset of a raw disk window */
    /* The readahead of reads through the file page cache. */
    struct file_ra_state ra;
    uint32_t status_flags; /* Shared O_APPEND, O_NONBLOCK, O_SYNC (OFD_* above) */
    /* A FIFO reader's writer-open count for poll: equal to the pipe's
     * count until a writer has opened since a nonblocking read-only open
     * with no writer. Other FIFO opens start with a different count. */
    uint32_t fifo_write_opens;
    /* A description of a file of a mounted file system holds its mount, its
     * node and the file system's handle of the open. The last unref releases
     * all three through `vfs_desc_close`, so the file system sees one release
     * for each open. `vmnt` is NULL for every other kind of description. */
    struct vfs_mount *vmnt;
    /* With `vmnt`, the description's cursors into the write-back errors of
     * its file system, which fsync and fdatasync report from (`wb_seen`)
     * and syncfs from (`sb_seen`), Linux's `f_wb_err` and `f_sb_err`:
     * sampled at the open (`vfs_wb_err_sample`) and moved only under the
     * lock of the errors (vfs.c). */
    uint32_t wb_seen;
    uint32_t sb_seen;
    /* With `vmnt`, the description's cursor into its file's own error
     * sequence (`vfs_ino_err_set`), which fsync, fdatasync and MS_SYNC
     * report from, Linux's `f_wb_err` into the mapping's `wb_err`: sampled
     * at the open once `ino` is set (`vfs_ino_err_sample`) and moved only
     * under the lock of the errors (vfs.c). */
    uint32_t ino_seen;
    /* The held mount of a synthetic node opened by path, for NO_XDEV when
     * /proc/PID/fd follows this description; NULL on anonymous objects. */
    struct vfs_mount *synth_mnt;
    void *synth_node; /* the held node on synth_mnt */
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
     * alone. The lock's wait queue is in the description's record in fd.c,
     * and the lock is taken by an atomic exchange (`fd_ofd_lock`). */
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
    /* The epoll registrations made of this description, under epoll.c's
     * lock of them, which the release of its last descriptor removes
     * (`epoll_release_file`), as Linux's `file->f_ep`. */
    struct epitem *epolls;
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
     * leaves it hung up (`tty_desc_hung`), unless `tty_console`. */
    uint32_t tty_hangups;
    /* The description is the console's own, one of /dev/console or of the
     * first process's descriptors 0, 1 and 2, which no hangup reaches, as
     * Linux's hangup passes over a file of /dev/console. */
    uint32_t tty_console;
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
    /* 1 from the reservation of the slot until `fd_install` publishes what
     * its caller wrote, and again while `fd_free` lets the description go:
     * no lookup finds the slot and no copy of the table keeps it. */
    uint32_t pending;
    /* FD_MODE_* below, or 0 until the first call that asks (`fd_mode`,
     * syscall_fs.c). */
    uint32_t mode;
} fd_entry_t;

/** The read and write rights granted by the low two open flag bits. */
static __inline__ unsigned fd_open_rights(unsigned acc)
{
    return ((acc == 0 || acc == 2) ? FD_FLAG_READ : 0u) |
           ((acc == 1 || acc == 2) ? FD_FLAG_WRITE : 0u);
}

/** The access mode reported by F_GETFL and fdinfo. A file opened with
 * mode 3 permits neither reads nor writes. O_PATH reports mode zero. */
static __inline__ unsigned fd_access_mode(const fd_entry_t *e)
{
    unsigned rw = e->flags & (FD_FLAG_READ | FD_FLAG_WRITE);

    if (rw == FD_FLAG_WRITE)
        return 1;
    if (rw == (FD_FLAG_READ | FD_FLAG_WRITE))
        return 2;
    return !rw && e->type == FD_TYPE_FILE && !(e->flags & FD_FLAG_PATH) ? 3 : 0;
}

/*
 * What a read or a write of a descriptor needs to know of it, in
 * `fd_entry_t.mode`, as Linux keeps it in `f_mode`: decided once from what
 * the open set, the descriptor's type, object, device row and mounts, which
 * nothing changes while the descriptor is open, so that each call tests bits
 * in place of classifying the descriptor again. A copy of the entry, by
 * dup, fork or a view of pread64, keeps the bits, since it names the same
 * object.
 */
#define FD_MODE_SET   0x01u /* the bits below are set */
#define FD_MODE_PREAD 0x02u /* pread64 and pwrite64 take it: not a stream */
#define FD_MODE_POS                                                             \
    0x04u                        /* read and write start at an offset whose end \
                                    is checked: FMODE_PREAD but no directory */
#define FD_MODE_RAW_POS    0x08u /* the offset is a raw disk window's `raw_offset` */
#define FD_MODE_UNSIGNED   0x10u /* offsets are unsigned (FMODE_UNSIGNED_OFFSET) */
#define FD_MODE_POS_HELD   0x20u /* an ops-backed transfer holds the description */
#define FD_MODE_READ_FREE  0x40u /* read and pread64 run without the big lock */
#define FD_MODE_WRITE_FREE 0x80u /* write and pwrite64 run without the big lock */

/* The most chunks a table has (fd.c), enough for FD_NR_OPEN_MAX. */
#define FD_CHUNKS 22

/*
 * File descriptor table, which each process's record points to: `cap`
 * entries in chunks from the heap, every one FD_TYPE_NONE unless open.
 * Threads and the processes clone makes with CLONE_FILES share one table,
 * `refs` counting them and `sharers` listing them. A table grows by adding
 * a chunk as large as all before it, and an entry never moves, so its
 * address (`fd_slot`) is its slot's for the table's life: a call may sleep
 * with it while another sharer grows the table, and finds the slot as it is
 * now. The table has no limit of its own below `fd_nr_open`: each sharer is
 * held to its own RLIMIT_NOFILE (`fd_limit`). The table is the owner of its
 * sharers' POSIX record locks (`lock.h`), which its release drops as it
 * closes the descriptors.
 *
 * `lock` guards the entries, `cap`, `next_free`, the sharer list and the
 * replacement of every sharer's map of held descriptors (`fd_held` in
 * process.h), as Linux's `files->file_lock` (include/linux/fdtable.h:51).
 * It is a spinlock, taken with interrupts off and held for no sleep, below
 * the big lock and the sleeping locks and above the scheduler's locks and
 * the heap's, which a growth under it takes. Every reservation,
 * publication, change of a descriptor's flags, release of a slot, dup2 and
 * growth takes it, and another process's table is read only under it
 * (`fd_peek`). A lookup in the caller's own table takes it only when it
 * finds the slot changing: a table only the caller has is read as it is,
 * as Linux's `__fget_light`, and a shared one is read with the hold set in
 * between two looks at the slot, as Linux's `__fget_files_rcu` takes the
 * file's reference between two reads of the array (`fd_get`). A slot that
 * a call has looked up or reserved is held by the call, so that no other
 * sharer releases what it names until the call ends, which also lets go
 * of the holds without the lock (`fd_holds_claim`).
 *
 * A descriptor that dup2 replaces while calls of other sharers hold it
 * stays where those calls read it until the last of them ends, as Linux's
 * calls keep the file `fdget` gave them (fs/file.c:1205-1248), and its
 * number leads to a spare entry from the heap meanwhile (`moved`): the
 * number's entry is the spare while `moved` names one, and the slot's own
 * entry otherwise. A spare stays until it is free and the slot's own entry
 * no longer kept, when the number leads back to the slot. The entries kept
 * for calls are on `gone`, and every spare, kept or not, on `spares`.
 */
struct process;
struct fd_spare;
struct fd_gone;

typedef struct fd_table {
    fd_entry_t *chunks[FD_CHUNKS]; /* NULL past the last */
    /* Per chunk, NULL or an array of its size from the heap whose element
     * is NULL or the spare a slot's number leads to; written under the
     * lock and read with none in a table only the caller has. */
    fd_entry_t **moved[FD_CHUNKS];
    struct fd_spare *spares;
    struct fd_gone *gone;
    int cap;
    int refs;
    /* No slot below it is free: where a search for the lowest free
     * descriptor begins, as Linux's `next_fd`. */
    int next_free;
    struct spinlock lock;
    /* The processes whose `fds` is this table, through their
     * `fd_sharer_next`; a table the kernel fills for nobody has none. */
    struct process *sharers;
} fd_table_t;

/*
 * A sharer's map of the descriptors its call holds in its shared table
 * (process.h's `fd_held`), one bit each, Linux's file references of a
 * call's fdget: the thread sets its bits as it looks a descriptor up and
 * clears them as the call ends, without the table's lock, and a close or a
 * dup2 by another sharer reads them under the lock (`fd_held_elsewhere`),
 * where a dup2 also clears the bit of a hold it moves to the entry it
 * replaced (`gone_make`); every change of a word is one locked
 * compare-and-exchange. A map never shrinks; a growth of the table past it
 * replaces it under the lock with a wider copy, marking it stale first, and
 * frees it after a grace period, since its thread may still be changing a
 * bit in it (fd.c).
 */
struct fd_hold_map {
    struct rcu_head rcu;           /* first: the record `call_rcu` frees */
    volatile unsigned int stale;   /* 1 once a growth has copied it */
    int words;                     /* the words of `bits` */
    volatile unsigned int bits[1]; /* `words` words, from the heap */
};

/** Release every descriptor of `table`, its object and then its slot, when
 * the last process that shares the table lets it go; a second call finds
 * nothing to release (syscall_proc.c). May sleep. */
void release_fd_table(fd_table_t *table);

/** Give a new table its first FD_TABLE_FIRST entries, all free, no
 * sharers and a free lock, whatever `table` held; it frees no old entries
 * and leaves `refs` to the caller. 0, or -1 when `table` is NULL or the heap
 * has no room. */
int fd_table_init(fd_table_t *table);

/** Free the entries; the descriptors must have been released first. */
void fd_table_free(fd_table_t *table);

/** A table from the heap with its first entries, one reference and no
 * sharer, which `fd_table_join` adds; NULL when the heap has no room. */
fd_table_t *fd_table_new(void);

/** A table from the heap with a copy of `src`'s entries, taken under its
 * lock, one reference and no sharer: storage as large as `src` for an exec
 * that may unshare it (`fd_table_take`), whose entries name descriptions
 * without references. A slot an unfinished open has reserved is free in the
 * copy. NULL when the heap has no room. */
fd_table_t *fd_table_dup(fd_table_t *src);

/** `fd_table_dup` for fork: the copy also takes a reference to each
 * description it names, and drops each slot of `src` that is closing or
 * not yet published, as Linux's `dup_fd` drops a descriptor not yet
 * installed. Sockets' references are the caller's to add. */
fd_table_t *fd_table_copy(fd_table_t *src);

/** Make `dst`, storage from `fd_table_dup`, a copy of `src` as it is now,
 * as `fd_table_copy` makes one, growing it under `src`'s lock if `src` grew
 * since; 0, or -ENOMEM. */
int fd_table_take(fd_table_t *dst, fd_table_t *src);

/** Free a table `fd_table_new` or `fd_table_dup` made, entries and all;
 * the descriptors must have been released first. */
void fd_table_delete(fd_table_t *table);

/** Make `table` the table of `p`, which shares no other, and list `p` among
 * its sharers; the reference is the caller's, the one a new table has. */
void fd_table_join(fd_table_t *table, struct process *p);

/** Give up `p`'s reference to its table: 1 when it is the last, which the
 * caller then releases and deletes, `p` still its sharer; 0 when other
 * sharers keep it, with `p` taken off its list and its `fds` cleared, and
 * when `p` has no table. */
int fd_table_put(struct process *p);

/** Take another reference to `table` for the CLONE_FILES child `child`,
 * make it `child`'s table and list `child` among its sharers, first giving
 * `child`, and on the table's first share its owner, a map of the
 * descriptors its calls hold as wide as the table (`fd_held` in
 * `process.h`); 0, or -1 when the heap has no room for them. */
int fd_table_share(fd_table_t *table, struct process *child);

/** Grow `table` to at least `cap` entries, whatever the limits; 0, or
 * -ENOMEM. */
int fd_table_fit(fd_table_t *table, int cap);

/** How many entries the table has now: the bound of a walk over it. */
int fd_table_cap(const fd_table_t *table);

/** The entry of descriptor `fd`, 0 <= `fd` < `fd_table_cap`, as the
 * calling thread's call sees it: the entry dup2 replaced while the call
 * held it, until the call looks the descriptor up again, and otherwise
 * the entry the table has now (`fd_table_entry`). Its address stays the
 * same while the caller's call holds it. Reading what it holds, unless the
 * caller's call holds the slot or the table is the caller's own and
 * unshared, takes the lock. */
fd_entry_t *fd_slot(const fd_table_t *table, int fd);

/** The entry `table` has now for descriptor `fd`, 0 <= `fd` <
 * `fd_table_cap`: the slot's own, or the spare its number leads to after a
 * dup2 replaced a held descriptor. For a walk over the table under its
 * lock, or a table only the caller has. */
fd_entry_t *fd_table_entry(const fd_table_t *table, int fd);

/** The `i`th entry of `table` that dup2 replaced and the calls that held
 * it keep, or NULL past the last, for a walk under the table's lock that
 * must meet every entry that holds an object. */
const fd_entry_t *fd_gone_entry(const fd_table_t *table, int i);

/** Take and give back `table`'s lock, for a walk over its slots; the walk
 * does not sleep. */
uint32_t fd_table_lock(fd_table_t *table);
void fd_table_unlock(fd_table_t *table, uint32_t flags);

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
 * description records (`tty_hangups`), though none is hung up
 * (`tty_console`). Returns 0, or -1 with nothing left
 * open when the heap has no room.
 */
int fd_init_stdio(fd_table_t *table, uint32_t hangups);

/**
 * Reserve the lowest free descriptor, growing the table up to the caller's
 * limit. The slot gets a new description and stays hidden from lookups
 * until the caller sets its type and object and publishes it with
 * `fd_install`; `fd_free` returns it. In a shared table the slot is held by
 * the call, as `fd_get` holds one. Returns the
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
int fd_can_alloc(fd_table_t *table);

/** Reserve the lowest free descriptor >= minfd (the fcntl(F_DUPFD)
 * contract), with `fd_alloc`'s results. */
int fd_alloc_from(fd_table_t *table, int minfd);

/** Publish slot `fd` of `table`, which the caller reserved and has filled
 * with its type and object: from here lookups find it. Linux's
 * `fd_install` (fs/file.c:595). */
void fd_install(fd_table_t *table, int fd);

/** Clear a descriptor's slot and drop its reference to its description,
 * also for a slot reserved and given no type. The object of the descriptor
 * is the caller's to release first. 0, or -1 when the slot is free or `fd`
 * is outside the table. Dropping the last reference closes a description of
 * a mounted file system, which may sleep. */
int fd_free(fd_table_t *table, int fd);

/** Clear `e`, the entry of descriptor `fd` that the caller marked
 * releasing and has released, whose reference to its description the
 * caller has already given up (`fd_ofd_unref_unless_last`): the slot of
 * `fd` when `e` is still its entry, else the entry dup2 replaced, which is
 * cleared or freed and the slot's number left as it is. */
void fd_free_after(fd_table_t *table, int fd, fd_entry_t *e);

/**
 * The entry for fd, or NULL if fd is out of range, free, not yet published
 * or closing: looked up with no lock in the calling thread's own table,
 * and under the lock in another's, when the slot is changing, when its
 * number leads to a spare dup2 made, or while the call keeps an entry dup2
 * replaced. A lookup in the calling thread's shared table
 * marks the descriptor held by the call until it ends, as Linux's
 * `__fget_files` (fs/file.c:1006) takes a reference: a close by another
 * sharer then leaves the slot and what it names in place until the call
 * ends (`fd_holds_claim`), and a dup2 by one leaves the entry where it is
 * for the call and gives the number another (`fd_dup_swap`), so the
 * entry may be read through the address for the length of the call, sleeps
 * included, and only its flags change meanwhile, under the lock
 * (`fd_set_flags`). The entry is not copied, since a copy would not be one
 * of the table's (`fd_in_table`).
 */
fd_entry_t *fd_get(fd_table_t *table, int fd);

/** `fd_peek`'s `ref`: a reference to the description. */
#define FD_PEEK_REF 1

/** A copy of the entry for `fd` in `table`, another process's or the
 * caller's own, into `*out`, taken under the lock, with a reference to its
 * description when `ref` is FD_PEEK_REF (0 for none), which the caller
 * gives back as a descriptor in flight is released; 0, or -EBADF when
 * `fd_get` would find nothing. */
int fd_peek(fd_table_t *table, int fd, fd_entry_t *out, int ref);

/** Whether `e`, an entry of a table whose lock the caller holds, is open:
 * published and not closing, as `fd_get` and `fd_peek` find it. Its
 * description and objects stay while the lock is held. */
int fd_entry_open(const fd_entry_t *e);

/** The lowest open descriptor (`fd_entry_open`) of `table` at or above
 * `from`, or -1 for none; the table's lock is taken. */
int fd_next_open(fd_table_t *table, int from);

/** Change the flags of `e`, a slot of `table`: clear `clear`, then set
 * `set`, under the lock, so that no close's mark is lost. */
void fd_set_flags(fd_table_t *table, fd_entry_t *e, uint32_t clear, uint32_t set);

/** Whether a call of a thread other than the caller that shares `table`
 * holds descriptor `fd`: has looked it up since the call began. The caller
 * holds the table's lock. */
int fd_held_elsewhere(fd_table_t *table, int fd);

/**
 * Begin the close of descriptor `fd` of `table` under its lock: -EBADF when
 * it is not open; 0 when a call of another sharer holds it, with the entry
 * marked closing, which the last call to hold it releases as it ends
 * (`fd_holds_claim`); 1 when the caller is to release it now, with the
 * entry marked closing and releasing. `*e` gets the entry claimed, for 0
 * and 1.
 */
int fd_close_claim(fd_table_t *table, int fd, fd_entry_t **e);

/**
 * Put `nw`, a whole entry the caller made for dup2 with its references
 * taken, in slot `fd` of `table` in one step under its lock, as Linux's
 * do_dup2 swaps the file in the slot, so that a lookup by another sharer
 * finds the old entry or the new one and never an empty slot. 0 when it is
 * in: in the free entry the number had, or in a spare that takes the place
 * of an entry that was open or closing, which the calls that hold it, the
 * release under way or the caller keep until they end (`fd_gone_claim`).
 * `*e` is then the replaced entry when it was open, whose record locks the
 * caller drops and which the caller's call keeps until it ends or it
 * releases it, or NULL. -EBUSY when an open has reserved the slot and not
 * yet published it, as Linux's dup2 answers; -ENOMEM when the heap has no
 * room for the spare; -EBADF when `fd` is outside the table. On an error
 * nothing changed and `nw` is still the caller's.
 */
int fd_dup_swap(fd_table_t *table, int fd, const fd_entry_t *nw, fd_entry_t **e);

/**
 * Let go of the calling thread's holds from descriptor `from` up, and
 * return the first of them that is closing, held by no other sharer and not
 * yet being released, marked releasing for the caller to release, with
 * `*e` its entry, or -1 once none is left, when the thread holds nothing
 * more. The dispatcher calls it from 0, and again past each descriptor it
 * releases, as each call ends (`fd_holds_release`). The lock is taken only
 * for a descriptor that a close marked meanwhile or whose number leads to
 * a spare, and when a growth replaced the map.
 */
int fd_holds_claim(int from, fd_entry_t **e);

/**
 * Let go of the calling thread's holds on entries dup2 replaced (`fd_gone`
 * in process.h), and return the descriptor of the first that no other call
 * keeps and whose release has not begun, marked releasing for the caller
 * to release and clear (`fd_free_after`), with `*table` its table and `*e`
 * the entry; or -1 once the thread keeps none. Called as each call ends,
 * after `fd_holds_claim` (`fd_holds_release`).
 */
int fd_gone_claim(fd_table_t **table, fd_entry_t **e);

/**
 * The descriptor of an entry dup2 replaced in `table` whose release has not
 * begun, marked releasing, with `*e` the entry, for the release of a table
 * whose sharers have all gone, whatever calls the entry was kept for; or
 * -1 when none is left.
 */
int fd_gone_take(fd_table_t *table, fd_entry_t **e);

/**
 * Add or drop a reference to an open file description, each in one atomic
 * step. The last unref drops the description's record locks, reports its
 * close to inotify, unpins an O_PATH node, closes a description of a
 * mounted file system and frees the description, under the big lock when
 * any of that has work to do; it may sleep. NULL is ignored.
 */
void fd_ofd_ref(struct open_file *ofd);
void fd_ofd_unref(struct open_file *ofd);

/**
 * Clear the bits `clear` of the status flags of `ofd` (OFD_APPEND,
 * OFD_NONBLOCK) and set `set`, in one atomic step, so that F_SETFL, which
 * runs without the big lock, and FIONBIO and mq_setattr lose none of each
 * other's changes, as Linux changes `f_flags` under the file's `f_lock`
 * (fs/fcntl.c:82-84).
 */
void fd_ofd_status_set(struct open_file *ofd, uint32_t clear, uint32_t set);

/** Drop a reference to `ofd` and return 1 when it is not the last; return
 * 0, dropping nothing, when it is, so that its holder may release what the
 * description's last descriptor releases before `fd_ofd_unref` ends it. One
 * atomic step decides, so of two descriptors of one description closed at
 * once in two tables exactly one is the last. */
int fd_ofd_unref_unless_last(struct open_file *ofd);

/**
 * A new open file description with one reference and no descriptor pointing
 * at it: for a call that changes a file by path with the rules of an open
 * one, truncate(2), and for the program's file an address space keeps
 * (`fs_exe_open`). NULL when the heap has no room; `fd_ofd_unref` frees it.
 */
struct open_file *fd_ofd_new(void);

/**
 * Whether the current thread acts with the credentials that opened `ofd`,
 * as Linux 6.12's linkat with AT_EMPTY_PATH asks that the file's `f_cred`
 * be the caller's: the same credential identity (`cred_id`), which a
 * thread made with CLONE_THREAD shares and which another process, an exec
 * and every change of ids, groups or capabilities make anew. 1 or 0.
 */
int fd_ofd_opener_is_current(struct open_file *ofd);

struct vfs_ref;
/** Reserve `ref` for writing (1) or execution (2) in `ofd`, in place of
 * any reservation `ofd` held: 0, -EROFS for writing through a mount that
 * is read-only, or that a remount is making so (`readonly_pending`), or
 * -ETXTBSY for an opposite reservation on the same file, found from a
 * count of each file's reservations as Linux's i_writecount, which leaves
 * `ofd`'s own as it was. Needs no memory, since the description brought the
 * count's record when it was made, and does
 * not sleep; the caller holds the node, and transfers that hold to the
 * description or releases the description before the node. The big kernel
 * lock is required. */
int fd_ofd_access(struct open_file *ofd, const struct vfs_ref *ref, int access);

/** Begin a remount of `mnt` read-only: -EBUSY while a description holds a
 * write reservation through it, one only a mapping holds among them, or
 * another such remount of it is under way; else 0, and new reservations
 * for writing through it are refused with EROFS (`readonly_pending`) until
 * `fd_mount_readonly_end`. A bind, and a base with `alone`, for a remount
 * of that mount's own flag (MS_BIND), count the descriptions opened
 * through it and refuse writers through it alone; a base without counts
 * those opened through it or any bind of it and refuses writers through
 * them all, as Linux counts a mount's writers or every mount of the
 * superblock's. Takes the lock of the descriptions; nothing here sleeps. */
int fd_mount_readonly_begin(struct vfs_mount *mnt, int alone);

/** End the refusal `fd_mount_readonly_begin` began on `mnt`, once its
 * flags say whether it is read-only. */
void fd_mount_readonly_end(struct vfs_mount *mnt);

/** Give back the reservation `ofd` holds, if any. The big kernel lock is
 * required. */
void fd_ofd_access_drop(struct open_file *ofd);

/** Take or release the description lock (see `busy` above), and with
 * it, taken after it and released before it, the namespace lock of the
 * description's mount when its type serialises every use. The waits
 * sleep and a signal does not end them; the lock is held for one file
 * operation. */
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

/** How many descriptions of (`mnt`, `node`) there are, `going` counted only
 * when it keeps a reference besides the one its release drops: the holders
 * of a KRFFS root node that no descriptor's `node` names, a FIFO's ends
 * among them, wherever their descriptors are. */
int fd_ofd_holders(const struct vfs_mount *mnt, const void *node, const struct open_file *going);

/** 1 if `fd` is open and not closing, 0 otherwise. */
int fd_is_valid(fd_table_t *table, int fd);

/**
 * Whether `e` is one of `table`'s entries, a slot's own or a spare, and not
 * a copy of one that a file mapping or a message in flight keeps: a close
 * of a descriptor of the table drops the table's record locks, and the
 * release of such a copy does not.
 */
int fd_in_table(const fd_table_t *table, const fd_entry_t *e);

/** Repoint the `node` of every entry of `table` that names `from` at `to`,
 * after a growing write moved a KRFFS node (`vfs_repoint`). */
void fd_table_repoint(fd_table_t *table, const void *from, void *to);

#endif /* _FD_H_ */
