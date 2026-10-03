/*
 * sys_ia32.c: the i386 forms of the calls that take a 64-bit offset or
 * length, which i386 passes in two registers, low word first, as Linux's
 * arch/x86/kernel/sys_ia32.c has them. Each puts the halves together and
 * calls the shared core (syscall_internal.h); a 64-bit architecture maps the
 * shared handler, which takes the value in one register.
 */

#include "sys_ia32.h"

#include "../arch.h"
#include "../../fs/fd.h"
#include "../../fs/krffs.h"
#include "../../fs/vfs.h"
#include "../../mm/usercopy.h"
#include "../../network/socket/socket.h"
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
    return ksys_ftruncate((int)SC_ARG(params, 0), ia32_join(SC_ARG(params, 1), SC_ARG(params, 2)));
}

/** pread64(2): the descriptor, the buffer, the count, then the offset's low
 * and high words. */
long sys_ia32_pread64(isr_params_t *params)
{
    return ksys_pread64((int)SC_ARG(params, 0), (char __user *)SC_ARG(params, 1),
                        USER_LEN32(SC_ARG(params, 2)),
                        (long long)ia32_join(SC_ARG(params, 3), SC_ARG(params, 4)));
}

/** pwrite64(2): pread64's arguments, the buffer written from. */
long sys_ia32_pwrite64(isr_params_t *params)
{
    return ksys_pwrite64((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1),
                         USER_LEN32(SC_ARG(params, 2)),
                         (long long)ia32_join(SC_ARG(params, 3), SC_ARG(params, 4)));
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
