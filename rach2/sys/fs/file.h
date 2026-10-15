#ifndef _FILE_H_
#define _FILE_H_

/* The operations a descriptor's object provides to the system calls. */

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "fd.h"

struct wait_queue;

/*
 * The operations of a descriptor's object, Linux's `struct file_operations`.
 * An object such as a pipe end, a terminal, a device or an anonymous object
 * sets `fd_entry_t.ops` to its table and `priv` to itself, and the system
 * calls call through the table; a descriptor with no table (the console, a
 * file, a socket and the other types) is dispatched on `fd_entry_t.type`.
 *
 * Any operation may be NULL: a read or write then fails with EBADF, an ioctl
 * with ENOTTY, a poll falls back to the dispatch on the type, and a close
 * has nothing to release.
 */
struct file_ops {
    /* Read up to count bytes into the user buffer; bytes read (>=0) or
     * -errno. */
    int32_t (*read)(fd_entry_t *e, char __user *buf, uint32_t count);
    /* Write up to count bytes from the user buffer; bytes written or -errno. */
    int32_t (*write)(fd_entry_t *e, const char __user *buf, uint32_t count);
    /* For poll/select: return the subset of `events` ({POLLIN, POLLOUT}) ready
     * now plus any POLLHUP/POLLERR, and set *wq to the wait queue whose wake
     * signals a readiness change for this fd (NULL if always ready). It does
     * not sleep; the poll core does the waiting. */
    short (*poll)(fd_entry_t *e, short events, struct wait_queue **wq);
    /* Device/terminal control; request value and arg are the ioctl ABI. */
    int32_t (*ioctl)(fd_entry_t *e, uint32_t request, uintptr_t arg);
    /* Release the object when the last descriptor of its open file
     * description closes. */
    void (*close)(fd_entry_t *e);
};

#endif /* _FILE_H_ */
