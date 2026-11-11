/*
 * sys_ia32.c: the i386 forms of the calls that take a 64-bit offset or
 * length, which i386 passes in two registers, low word first, as Linux's
 * arch/x86/kernel/sys_ia32.c has them. Each puts the halves together and
 * calls the shared core (syscall_internal.h); a 64-bit architecture maps the
 * shared handler, which takes the value in one register. Also futex_time64,
 * whose timeout has 64-bit fields where the native futex's has 32-bit ones.
 */

#include "sys_ia32.h"

#include "../arch.h"
#include "../../fs/fd.h"
#include "../../fs/krffs.h"
#include "../../fs/vfs.h"
#include "../../lib/kerrno.h"
#include "../../mm/usercopy.h"
#include "../../network/socket/socket.h"
#include "../../proc/futex.h"
#include "../../proc/process.h"
#include "../../syscall_internal.h"

/* The 64-bit value i386 passes as the registers `low` and `high`. */
static uint64_t ia32_join(unsigned long low, unsigned long high)
{
    return (uint64_t)low | ((uint64_t)high << 32);
}

/** truncate64(2): the path, then the length's low and high words. Musl's
 * and GNU libc's truncate are this call on i386. */
long sys_ia32_truncate64(isr_params_t *params)
{
    return ksys_truncate((const char __user *)SC_ARG(params, 0),
                         (long long)ia32_join(SC_ARG(params, 1), SC_ARG(params, 2)));
}

/** ftruncate64(2): the descriptor, then the length's low and high words. */
long sys_ia32_ftruncate64(isr_params_t *params)
{
    return ksys_ftruncate((int)SC_ARG(params, 0),
                          (long long)ia32_join(SC_ARG(params, 1), SC_ARG(params, 2)));
}

/** pread64(2): the descriptor, the buffer, the count, then the offset's low
 * and high words. */
long sys_ia32_pread64(isr_params_t *params)
{
    return ksys_pread64((int)SC_ARG(params, 0), (char __user *)SC_ARG(params, 1), SC_ARG(params, 2),
                        (long long)ia32_join(SC_ARG(params, 3), SC_ARG(params, 4)));
}

/** pwrite64(2): pread64's arguments, the buffer written from. */
long sys_ia32_pwrite64(isr_params_t *params)
{
    return ksys_pwrite64((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1),
                         SC_ARG(params, 2),
                         (long long)ia32_join(SC_ARG(params, 3), SC_ARG(params, 4)));
}

/** sync_file_range(2): the descriptor, the offset and the length in two
 * arguments each, low word first, then the flags. preadv, pwritev, preadv2
 * and pwritev2 split their position the same way, which the shared handlers
 * put together themselves (`pos_from_hilo`). */
long sys_ia32_sync_file_range(isr_params_t *params)
{
    return ksys_sync_file_range(
        (int)SC_ARG(params, 0), (int64_t)ia32_join(SC_ARG(params, 1), SC_ARG(params, 2)),
        (int64_t)ia32_join(SC_ARG(params, 3), SC_ARG(params, 4)), (uint32_t)SC_ARG(params, 5));
}

/** fallocate(2): the descriptor, the mode, then the offset and the length
 * in two arguments each, low word first. */
long sys_ia32_fallocate(isr_params_t *params)
{
    return ksys_fallocate((int)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1),
                          ia32_join(SC_ARG(params, 2), SC_ARG(params, 3)),
                          ia32_join(SC_ARG(params, 4), SC_ARG(params, 5)));
}

/** fadvise64(2): the descriptor, the offset in two arguments low word
 * first, a 32-bit length, which is never negative, and the advice. */
long sys_ia32_fadvise64(isr_params_t *params)
{
    return ksys_fadvise64((int)SC_ARG(params, 0), (long long)(uint32_t)SC_ARG(params, 3),
                          (uint32_t)SC_ARG(params, 4));
}

/** fadvise64_64(2), which musl calls: the descriptor, the offset and the
 * length each in two arguments low word first, and the advice. */
long sys_ia32_fadvise64_64(isr_params_t *params)
{
    return ksys_fadvise64((int)SC_ARG(params, 0),
                          (long long)ia32_join(SC_ARG(params, 3), SC_ARG(params, 4)),
                          (uint32_t)SC_ARG(params, 5));
}

/* The bits of a futex operation that name the command. */
#define FUTEX_CMD_MASK 0x7Fu

/**
 * Return whether the futex command `cmd` takes a timeout as its fourth
 * argument, as Linux's futex_cmd_has_timeout (kernel/futex/futex.h): WAIT,
 * LOCK_PI, WAIT_BITSET, WAIT_REQUEUE_PI and LOCK_PI2.
 */
static int futex_has_timeout(uint32_t cmd)
{
    return cmd == 0 || cmd == 6 || cmd == 9 || cmd == 11 || cmd == 13;
}

/**
 * futex_time64 (422): futex, the timeout of an operation that takes one
 * read from its 64-bit fields into the native timespec first, as Linux's
 * get_timespec64 reads it on a 32-bit kernel (kernel/time/time.c), where
 * the store into the native `long` drops the high word of the nanoseconds.
 * A count of seconds past what a `long` holds is held at its limit, a wait
 * of some 68 years. Another operation's fourth argument is a value and
 * passes as it is. Musl's and GNU libc's timed futex waits on i386 are this
 * call. -EFAULT for a timeout that cannot be read; otherwise futex's answer.
 */
long sys_ia32_futex_time64(isr_params_t *params)
{
    uint32_t op = (uint32_t)SC_ARG(params, 1);
    struct kernel_timespec ts;
    isr_params_t p;
    int64_t t[2];
    long rc;

    if (SC_ARG(params, 3) == 0 || !futex_has_timeout(op & FUTEX_CMD_MASK)) {
        return sys_futex(params);
    }
    if (copy_from_user(t, (const void __user *)SC_ARG(params, 3), sizeof(t)) != 0) {
        return -EFAULT;
    }
    ts.tv_sec = t[0] > 0x7FFFFFFFLL    ? 0x7FFFFFFFL
                : t[0] < -0x7FFFFFFFLL ? -0x7FFFFFFFL
                                       : (long)t[0];
    ts.tv_nsec = (long)(uint32_t)t[1];
    p = *params;
    p.esi = (uint32_t)(uintptr_t)&ts;
    user_window_open(&ts, sizeof(ts));
    rc = sys_futex(&p);
    user_window_close();
    return rc;
}
