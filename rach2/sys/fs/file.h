#ifndef _FILE_H_
#define _FILE_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "fd.h"

/*
 * Backing operations for a descriptor -- the kernel's `struct file_operations`.
 *
 * Part I dispatched read/write/close by switching on fd_entry_t.type (CONSOLE,
 * FILE, SOCKET, ...). That does not scale to the objects Part II adds (pipes,
 * ttys, ptys), so a backing can instead install a `file_ops` vtable on its
 * descriptor and the syscall layer calls through it. The existing enum-dispatch
 * stays for the Part I types; new objects register their own ops, which is how
 * pipes (Phase 13), poll (Phase 14), ttys (Phase 16), and ptys (Phase 19) all
 * hang off one uniform dispatch.
 *
 * Any op may be NULL: read/write NULL means "this object cannot be read/written
 * that way" (the caller returns -EBADF); a NULL close needs no teardown.
 */
struct file_ops {
    /* Read up to count bytes into the user buffer; bytes read (>=0) or -errno. */
    int32_t (*read)(fd_entry_t *e, char __user *buf, uint32_t count);
    /* Write up to count bytes from the user buffer; bytes written or -errno. */
    int32_t (*write)(fd_entry_t *e, const char __user *buf, uint32_t count);
    /* Return the subset of `events` ({POLLIN, POLLOUT, POLLHUP}) ready now, for
     * poll/select (Phase 14). Parks nothing itself; poll owns the blocking. */
    short (*poll)(fd_entry_t *e, short events);
    /* Device/terminal control; request value and arg are the ioctl ABI. */
    int32_t (*ioctl)(fd_entry_t *e, uint32_t request, uint32_t arg);
    /* Release the backing object when its last referencing descriptor closes. */
    void (*close)(fd_entry_t *e);
};

#endif /* _FILE_H_ */
