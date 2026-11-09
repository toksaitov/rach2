/*
 * ia32_misc.c: the rest of the i386 calls whose arguments differ: the 16-bit
 * ids, limits and usage, the old uname forms, futex with 32-bit times and the
 * robust list, affinity masks, keyctl and kexec_load, for the compat table
 * (ia32_systab.c).
 */

#include "ia32.h"
#include "ia32_abi.h"

#include "../arch.h"
#include "../../fs/krffs.h"
#include "../../fs/vfs.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/futex.h"
#include "../../proc/process.h"
#include "../../syscall.h"
#include "../../syscall_internal.h"

/*
 * Most calls here run the x86-64 handler (`ia32_native`) with the i386
 * arguments widened, and convert the structure it reads or writes in kernel
 * memory, as ia32_fs.c does. A handler that reads its structure in the
 * caller's layout itself, under `in_compat_syscall`, is run on the i386
 * frame as it is.
 */

/* A user pointer of an i386 call, zero-extended, and a conversion buffer's
 * address, as native arguments. */
#define UPTR(v) ((unsigned long)(uint32_t)(v))
#define KPTR(p) ((unsigned long)(uintptr_t)(p))

/* The calls that answer ENOSYS, since Rach2 has them on neither kernel
 * (ia32.h). */
IA32_STUB(compat_sys_lchown16)
IA32_STUB(compat_sys_oldumount)
IA32_STUB(compat_sys_setuid16)
IA32_STUB(compat_sys_getuid16)
IA32_STUB(compat_sys_nice)
IA32_STUB(compat_sys_times)
IA32_STUB(compat_sys_setgid16)
IA32_STUB(compat_sys_getgid16)
IA32_STUB(compat_sys_geteuid16)
IA32_STUB(compat_sys_getegid16)
IA32_STUB(compat_sys_olduname)
IA32_STUB(compat_sys_setreuid16)
IA32_STUB(compat_sys_setregid16)
IA32_STUB(compat_sys_setrlimit)
IA32_STUB(compat_sys_old_getrlimit)
IA32_STUB(compat_sys_getrusage)
IA32_STUB(compat_sys_getgroups16)
IA32_STUB(compat_sys_setgroups16)
IA32_STUB(compat_sys_fchown16)
IA32_STUB(compat_sys_uname)
IA32_STUB(compat_sys_wait4)
IA32_STUB(compat_sys_sysinfo)
IA32_STUB(compat_sys_setfsuid16)
IA32_STUB(compat_sys_setfsgid16)
IA32_STUB(compat_sys_sched_rr_get_interval_time32)
IA32_STUB(compat_sys_setresuid16)
IA32_STUB(compat_sys_getresuid16)
IA32_STUB(compat_sys_setresgid16)
IA32_STUB(compat_sys_getresgid16)
IA32_STUB(compat_sys_chown16)
IA32_STUB(compat_sys_getrlimit)
IA32_STUB(compat_sys_sched_setaffinity)
IA32_STUB(compat_sys_sched_getaffinity)
IA32_STUB(compat_sys_kexec_load)
IA32_STUB(compat_sys_waitid)
IA32_STUB(compat_sys_keyctl)
IA32_STUB(compat_sys_arch_prctl)

/* The command of a futex operation, its low seven bits. */
#define FUTEX_CMD_MASK 0x7Fu

/** Whether the futex command `cmd` takes a timeout, as Linux's
 * futex_cmd_has_timeout: FUTEX_WAIT, FUTEX_LOCK_PI, FUTEX_WAIT_BITSET,
 * FUTEX_WAIT_REQUEUE_PI and FUTEX_LOCK_PI2. */
static int futex_has_timeout(uint32_t cmd)
{
    return cmd == 0 || cmd == 6 || cmd == 9 || cmd == 11 || cmd == 13;
}

/**
 * futex (240): the x86-64 futex with the i386 timespec of an operation that
 * takes a timeout read into a native one first, as Linux's futex_time32
 * reads it; another operation's fourth argument is a value and passes as it
 * is. -EFAULT for a timeout that cannot be read.
 */
long compat_sys_futex_time32(isr_params_t *params)
{
    uint32_t op = (uint32_t)SC_ARG(params, 1);
    unsigned long a3 = UPTR(SC_ARG(params, 3));
    long ts[2];
    void *win = NULL;

    if (a3 != 0 && futex_has_timeout(op & FUTEX_CMD_MASK)) {
        if (ia32_get_timespec(a3, ts) != 0) {
            return -EFAULT;
        }
        win = ts;
        a3 = KPTR(ts);
    }
    return ia32_native(params, sys_futex, win, sizeof(ts), UPTR(SC_ARG(params, 0)), op,
                       (uint32_t)SC_ARG(params, 2), a3, UPTR(SC_ARG(params, 4)),
                       (uint32_t)SC_ARG(params, 5));
}
