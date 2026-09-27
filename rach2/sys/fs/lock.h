#ifndef _LOCK_H_
#define _LOCK_H_

#include "../lib/kint.h"
#include "fd.h"

/*
 * Advisory file locks, of the three kinds Linux has.
 *
 * A POSIX record lock (fcntl F_SETLK) covers a range of bytes and belongs to
 * a process: a second lock of the same process over the range replaces the
 * first, and any close by the process of a descriptor of the file drops all
 * its record locks on the file. An open file description lock (F_OFD_SETLK)
 * is a record lock that belongs to the description instead. A flock(2) lock
 * covers the whole file, belongs to the description, and does not interact
 * with the record locks. A description's locks go when its last reference
 * does.
 *
 * Locks of two owners conflict when their ranges overlap and one of them is
 * a write lock. A request that conflicts fails with EAGAIN, or waits until
 * the conflict is gone; a wait for a record lock that would close a cycle of
 * waiting processes fails with EDEADLK. lock.Readme.md has the whole
 * contract.
 */

/* The file a lock is on: its mount and a number that names the file while a
 * description of it is open. A KRFFS file's number is the place of its node,
 * which `lock_repoint` follows when a write moves the file; any other file's
 * is its inode number at the open. A descriptor that is not open on a file
 * of a mounted type has the object it is open on in `ino` and no mount. */
struct lock_key {
    const void *mnt;
    uint64_t ino;
};

/* The range sentinel of a lock that runs to the end of the file, however
 * far the file grows. */
#define LOCK_EOF 0xFFFFFFFFFFFFFFFFull

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* A record-lock request, and for LOCK_GET the answer: the conflicting
 * lock's type, range and owner, or F_UNLCK for none. `end` is the last byte,
 * LOCK_EOF for the end of the file, which is also how a range that ends at
 * the largest offset, 2^63 - 1, is given, since it is the same range. */
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
 * `key`, for the calling process, or with `ofd` non-NULL for that open file
 * description. Returns 0 or -errno. A wait goes on with a copy of `key`,
 * which `lock_repoint` moves with the file's locks, so the caller's own key
 * is not the file's after a wait the file moved in.
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

/** Drop the record locks the calling process has on `key`: it closed a
 * descriptor of the file. */
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
