/*
 * fsize.c: the RLIMIT_FSIZE checks of a write, a truncation and an
 * allocation (`vfs_fsize_refused` and `vfs_newsize_ok` in vfs.h), apart from
 * vfs.c so that a host test of a caller can link them.
 */

#include "vfs.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/process.h"
#include "../proc/smp.h"

/** 0 when the caller's RLIMIT_FSIZE lets a file reach `end` bytes, else
 * -EFBIG with SIGXFSZ sent to the calling thread (vfs.h). */
int32_t vfs_fsize_refused(uint64_t end)
{
    unsigned long limit;
    struct k_siginfo info;
    int big = 0;

    if (!current_proc) {
        return 0;
    }
    limit = current_proc->rlim[RLIM_FSIZE].cur;
    if (limit == RLIM_INFINITY || end <= limit) {
        return 0;
    }
    k_memset(&info, 0, sizeof(info));
    info.code = SI_USER;
    info.pid = current_proc->tgid;
    info.uid = current_proc->ruid;
    if (!bkl_held()) {
        bkl_enter();
        big = 1;
    }
    signal_post_thread(current_proc, SIGXFSZ, &info);
    if (big) {
        bkl_exit();
    }
    return -EFBIG;
}

/** `vfs_fsize_refused` of `end` when it grows a file of `size` bytes, else
 * 0 (vfs.h). */
int32_t vfs_newsize_ok(uint64_t size, uint64_t end)
{
    return end > size ? vfs_fsize_refused(end) : 0;
}
