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
 * waiting processes fails with EDEADLK. See lock.Readme.md.
 */

/* The file a lock is on: its mount and its inode number. */
struct lock_key {
    const void *mnt;
    uint32_t ino;
};

/* The range sentinel of a lock that runs to the end of the file, however
 * far the file grows. */
#define LOCK_EOF 0xFFFFFFFFFFFFFFFFull

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* A record-lock request, and for LOCK_GET the answer: the conflicting
 * lock's type, range and owner, or F_UNLCK for none. `end` is the last byte,
 * LOCK_EOF for the end of the file. */
struct lock_req {
    int type;
    uint64_t start;
    uint64_t end;
    int32_t pid;
};

#define LOCK_GET  0
#define LOCK_SET  1
#define LOCK_WAIT 2

/**
 * A record lock request `cmd` (LOCK_GET, LOCK_SET or LOCK_WAIT) on the file
 * `key`, for the calling process, or with `ofd` non-NULL for that open file
 * description. Returns 0 or -errno.
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

#endif /* _LOCK_H_ */
