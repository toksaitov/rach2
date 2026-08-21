#ifndef _FILE_H_
#define _FILE_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "fd.h"

struct wait_queue;

/*
 * Backing operations for a descriptor, the kernel's `struct file_operations`.
 *
 * A backing object installs a `file_ops` vtable on its descriptor and the
 * syscall layer calls through it, alongside the enum dispatch on fd_entry_t.type
 * used for the older types (CONSOLE, FILE, SOCKET, ...). Pipes, ttys, and ptys
 * all hang off this one uniform dispatch.
 *
 * Any op may be NULL: a NULL read/write means the object cannot be read/written
 * that way (the caller returns -EBADF); a NULL close needs no teardown.
 */
struct file_ops {
    /* Read up to count bytes into the user buffer; bytes read (>=0) or
     * -errno. */
    int32_t (*read)(fd_entry_t *e, char __user *buf, uint32_t count);
    /* Write up to count bytes from the user buffer; bytes written or -errno. */
    int32_t (*write)(fd_entry_t *e, const char __user *buf, uint32_t count);
    /* For poll/select: return the subset of `events` ({POLLIN, POLLOUT}) ready
     * now plus any POLLHUP/POLLERR, and set *wq to the wait queue whose wake
     * signals a readiness change for this fd (NULL if always ready). Parks
     * nothing itself; the poll core owns the blocking. */
    short (*poll)(fd_entry_t *e, short events, struct wait_queue **wq);
    /* Device/terminal control; request value and arg are the ioctl ABI. */
    int32_t (*ioctl)(fd_entry_t *e, uint32_t request, uint32_t arg);
    /* Release the backing object when its last referencing descriptor
     * closes. */
    void (*close)(fd_entry_t *e);
};

#endif /* _FILE_H_ */
