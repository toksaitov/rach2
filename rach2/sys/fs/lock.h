#ifndef _LOCK_H_
#define _LOCK_H_

/*
 * Advisory file locks, of the three kinds Linux has.
 *
 * A POSIX record lock (fcntl F_SETLK) covers a range of bytes and belongs to
 * a descriptor table, as Linux's belongs to a files_struct, so the threads
 * of a process and the processes that share a table (CLONE_FILES) are one
 * owner: a second lock of the owner over the range replaces the first, and
 * any close through the table of a descriptor of the file drops all the
 * table's record locks on the file. An open file description lock
 * (F_OFD_SETLK) is a record lock that belongs to the description instead. A
 * flock(2) lock covers the whole file, belongs to the description, and does
 * not interact with the record locks. A description's locks go when its
 * last reference does.
 *
 * Locks of two owners conflict when their ranges overlap and one of them is
 * a write lock. A request that conflicts fails with EAGAIN, or waits until
 * the conflict is gone; a wait for a record lock that would close a cycle of
 * waiting owners fails with EDEADLK. lock.Readme.md has the whole contract.
 */

#include "../lib/kint.h"
#include "fd.h"

/* The file a lock is on: its mount and a number that names the file while a
 * description of it is open. A file of KRFFS, FAT32 or exFAT has its node,
 * a KRFFS node being the file's place, which `lock_repoint` follows when a
 * write moves the file; any other file has its inode number at the open. A
 * descriptor that is not open on a file of a mounted type has the object it
 * is open on in `ino` and no mount. `shown` is the inode number /proc/locks
 * gives, the one the description recorded at the open, or 0 for no file; it
 * takes no part in matching keys. */
struct lock_key {
    const void *mnt;
    uint64_t ino;
    uint64_t shown;
};

/* The range sentinel of a lock that runs to the end of the file, however
 * far the file grows. */
#define LOCK_EOF 0xFFFFFFFFFFFFFFFFull

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* A record-lock request, and for LOCK_GET the answer: the conflicting
 * lock's type, range and the process that took it, -1 for a description's,
 * or F_UNLCK for none. `end` is the last byte, LOCK_EOF for the end of the
 * file, which is also how a range that ends at the largest offset,
 * 2^63 - 1, is given, since it is the same range. */
struct lock_req {
    int type;
    uint64_t start;
    uint64_t end;
    int32_t pid;
};

#define LOCK_GET  0
#define LOCK_SET  1
#define LOCK_WAIT 2

/* flock(2)'s operations, with Linux's values: a shared or an exclusive lock,
 * no wait, and the unlock. */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

/**
 * A record lock request `cmd` (LOCK_GET, LOCK_SET or LOCK_WAIT) on the file
 * `key`, for the calling thread's descriptor table, or with `ofd` non-NULL
 * for that open file description; a lock set records the calling process's
 * id for LOCK_GET to report. Returns 0 or -errno. A wait goes on with a copy
 * of `key`, which `lock_repoint` moves with the file's locks, so the
 * caller's own key is not the file's after a wait the file moved in.
 */
int32_t lock_record(const struct lock_key *key, struct open_file *ofd, int cmd,
                    struct lock_req *req);

/**
 * flock(2): `op` is LOCK_SH, LOCK_EX or LOCK_UN, with LOCK_NB for no wait,
 * by the description `ofd` of the file `key`. Returns 0 or -errno.
 */
int32_t lock_flock(const struct lock_key *key, struct open_file *ofd, int op);

/** Whether any lock exists: a close has nothing to drop when none does. */
int lock_any(void);

/* A lock as /proc/locks shows it: its kind, LOCK_ROW_POSIX, LOCK_ROW_OFD or
 * LOCK_ROW_FLOCK; its type, F_RDLCK or F_WRLCK; the process that took it, -1
 * for an open file description's record lock, as Linux shows one; its file;
 * and its range, `end` LOCK_EOF to the end of the file. */
struct lock_row {
    int kind;
    int type;
    int32_t pid;
    struct lock_key key;
    uint64_t start;
    uint64_t end;
};

#define LOCK_ROW_POSIX 0
#define LOCK_ROW_OFD   1
#define LOCK_ROW_FLOCK 2

/** The number of locks held. */
int lock_count(void);

/** Copy at most `max` of the locks held into `rows`, the newest first, as
 * Linux lists them, and return how many were copied. */
int lock_rows(struct lock_row *rows, int max);

/** Drop the record locks the calling thread's descriptor table has on `key`:
 * a descriptor of the file in that table was closed. */
void lock_close(const struct lock_key *key);

/** Drop the locks `ofd` owns: its last reference is gone. */
void lock_ofd_gone(const struct open_file *ofd);

/**
 * The file whose key is (`mnt`, `from`) is now (`mnt`, `to`): every lock on
 * it, of every kind and owner, and the key of every wait for one move with
 * it. A KRFFS file is keyed by the place of its node, and `vfs_repoint`
 * calls this when a write moves the node, so that locks taken before the
 * move meet those taken after it and a file made at the old place meets
 * none of them.
 */
void lock_repoint(const void *mnt, uint64_t from, uint64_t to);

#endif /* _LOCK_H_ */
