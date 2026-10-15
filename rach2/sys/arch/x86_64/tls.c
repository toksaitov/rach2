/*
 * tls.c: the x86-64's thread pointer and arch_prctl, as tls.h describes
 * them.
 */

#include "tls.h"
#include "percpu.h"

#include "../../lib/kerrno.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"

/** x86-64's thread pointer is FS's base, IA32_FS_BASE. */
void arch_tls_set(uintptr_t base)
{
    msr_write(MSR_FS_BASE, base);
}

/** The processor's FS base, which a selector loaded into FS may have
 * changed since the record was written. */
uintptr_t arch_tls_get(void)
{
    return (uintptr_t)msr_read(MSR_FS_BASE);
}

/** Clone's TLS argument on x86-64 is the base itself, which must be a user
 * address, as ARCH_SET_FS takes one. */
long arch_clone_tls(unsigned long arg, uintptr_t *base_out)
{
    if (arg >= USER_ADDR_MAX) {
        return -EPERM;
    }
    *base_out = arg;
    return 0;
}

long sys_arch_prctl(isr_params_t *params)
{
    unsigned long code = SC_ARG(params, 0);
    unsigned long addr = SC_ARG(params, 1);
    uint64_t base;

    if (!current_proc) {
        return -ESRCH;
    }
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= USER_ADDR_MAX) {
            return -EPERM;
        }
        x86_64_fs_load(0);
        current_proc->tls_base = addr;
        msr_write(MSR_FS_BASE, addr);
        return 0;
    case ARCH_SET_GS:
        if (addr >= USER_ADDR_MAX) {
            return -EPERM;
        }
        x86_64_gs_load(0);
        current_proc->regs.gs_base = addr;
        /* While the kernel runs, the program's GS base is in this MSR. */
        msr_write(MSR_KERNEL_GS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
    case ARCH_GET_GS:
        base = msr_read(code == ARCH_GET_FS ? MSR_FS_BASE : MSR_KERNEL_GS_BASE);
        if (copy_to_user((void __user *)addr, &base, sizeof(base)) != 0) {
            return -EFAULT;
        }
        return 0;
    default:
        return -EINVAL;
    }
}
