/*
 * ia32_exec.c: the i386 forms of execve, execveat, clone and the two mmap
 * calls, for the compat table (ia32_systab.c), and the processor's AT_HWCAP
 * an i386 program's auxiliary vector has.
 */

#include "ia32.h"

#include "../x86/kcpu.h"
#include "../../proc/process.h"
#include "../../fs/fd.h"
#include "../../fs/vfs.h"
#include "../../fs/krffs.h"
#include "../../network/socket/socket.h"
#include "../../mm/usercopy.h"
#include "../../syscall_internal.h"

/**
 * execve(2) of an i386 call, 11: the path, argv[] and envp[], whose
 * pointers are 4 bytes, which the shared exec reads as an i386 call's
 * (`exec_vec_get` in syscall_proc.c), as Linux's `compat_sys_execve` does.
 */
long compat_sys_execve(isr_params_t *params)
{
    return sys_execve(params);
}

/** execveat(2) of an i386 call, 358, read as `compat_sys_execve` reads
 * execve's: the descriptor, the path, argv[], envp[] and the flags. */
long compat_sys_execveat(isr_params_t *params)
{
    return sys_execveat(params);
}

/**
 * clone(2) of an i386 call, 120, with i386's order of arguments: the flags,
 * the new stack, the parent tid address, the TLS argument and the child tid
 * address. The TLS argument of CLONE_SETTLS is a `struct user_desc`, whose
 * descriptor the child gets (`ksys_clone`).
 */
long compat_sys_ia32_clone(isr_params_t *params)
{
    return ksys_clone(params, (uint32_t)SC_ARG(params, 0), SC_ARG(params, 1), SC_ARG(params, 2),
                      SC_ARG(params, 3), SC_ARG(params, 4));
}

/* i386's old mmap takes its six arguments in a structure of 32-bit words. */
struct ia32_mmap_args {
    uint32_t addr;
    uint32_t len;
    uint32_t prot;
    uint32_t flags;
    uint32_t fd;
    uint32_t offset; /* bytes, a multiple of the page */
};

/**
 * The old mmap(2) of an i386 call, 90, whose one argument points at the six
 * of mmap in a `struct mmap_arg_struct32`, as Linux's `compat_sys_ia32_mmap`
 * reads them: an offset that is not a multiple of the page is -EINVAL, a
 * structure that cannot be read -EFAULT. The mapping goes below 4 GiB
 * (`mmap_place` in syscall_proc.c).
 */
long compat_sys_ia32_mmap(isr_params_t *params)
{
    struct ia32_mmap_args a;

    if (copy_from_user(&a, (const void __user *)SC_ARG(params, 0), sizeof(a)) != 0) {
        return -EFAULT;
    }
    if (a.offset & (PAGE_SIZE - 1u)) {
        return -EINVAL;
    }
    return ksys_mmap_pgoff(a.addr, a.len, a.prot, a.flags, (int)a.fd, a.offset >> PAGE_SHIFT);
}

/** mmap2(2) of an i386 call, 192: mmap's arguments with the offset in
 * pages, as i686's own mmap2 (`sys_mmap2`); the mapping goes below 4 GiB. */
long compat_sys_mmap_pgoff(isr_params_t *params)
{
    return sys_mmap2(params);
}

/** The AT_HWCAP of an i386 program, as Linux's ELF_HWCAP on i386: the
 * feature bits of CPUID leaf 1 in EDX. */
uint32_t ia32_hwcap(void)
{
    uint32_t edx = 0;

    kcpu_id(1, NULL, NULL, NULL, &edx);
    return edx;
}
