#ifndef _FILE_H_
#define _FILE_H_

/* The operations a descriptor's object provides to the system calls. */

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "fd.h"

struct wait_queue;
struct vma;

/*
 * The operations of a descriptor's object, Linux's `struct file_operations`.
 * An object such as a pipe end, a terminal, a device or an anonymous object
 * sets `fd_entry_t.ops` to its table and `priv` to itself, and the system
 * calls call through the table; a descriptor with no table (the console, a
 * file, a socket and the other types) is dispatched on `fd_entry_t.type`.
 *
 * Any operation may be NULL: a read or write then fails with EBADF, an ioctl
 * with ENOTTY, a poll falls back to the dispatch on the type, and a close
 * has nothing to release; an mmap fails with ENODEV. The seek calls do not
 * call `llseek` yet (sys/fs/Readme.md).
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
    /* Map the object into `vma`, a new region that no address space holds
     * yet, after mmap checked the descriptor's access: its start is 0, its
     * end the length, its kind VM_SHARED for a shared mapping and VM_FILE
     * for a private one, and its protection and page offset the call's.
     * Give the region its backing, with a reference on the object, which
     * munmap, exec and exit drop and fork copies; a device object does it
     * with `vm_device_map` (mm/vm.h). 0, or -ENODEV for an object that
     * cannot be mapped, -EINVAL for a range outside it or a private mapping
     * it refuses, -EACCES or -EPERM for a protection or a caller it
     * refuses, or -ENOMEM. It may sleep. */
    int32_t (*mmap)(fd_entry_t *e, struct vma *vma);
    /* Seek: the offset `whence` (0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END) and
     * `offset` give, from the description's offset for SEEK_CUR. Returns
     * the new offset, which the call stores in the description, or -errno:
     * -EINVAL for a negative result or an unknown `whence`, -ESPIPE for an
     * object with no position. */
    int64_t (*llseek)(fd_entry_t *e, int64_t offset, int whence);
};

#endif /* _FILE_H_ */
