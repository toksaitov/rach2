/*
 * ia32_signal.c: the i386 forms of the signal calls, with their 32-bit
 * sigaction, siginfo, masks and stacks, and of ptrace, for the compat table
 * (ia32_systab.c).
 *
 * The calls whose structures have one layout in both ABIs go to their
 * native handlers, which read their arguments through SC_ARG and so take
 * the i386 registers: the 64-bit masks of the rt_ calls, sigprocmask's
 * 32-bit one, signalfd's mask and its 128-byte records. The native
 * handlers read and write a siginfo, and ptrace its words and register
 * sets, in the caller's layout themselves (`siginfo_to_caller`,
 * proc/ptrace.c, ptrace.c), and the frames are sigframe32.c's. The rest
 * translate here, around the cores of syscall_internal.h, as Linux's
 * compat calls of the same names do.
 */

#include "ia32.h"

#include "../../syscall.h"
#include "../../fs/fd.h"
#include "../../fs/krffs.h"
#include "../../lib/kerrno.h"
#include "../../lib/ksignal.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"
#include "../../proc/ptrace.h"
#include "../../syscall_internal.h"

/* Linux's `compat_old_sigaction` of sigaction (67), 16 bytes, and
 * `compat_sigaction` of rt_sigaction, 20, with 32-bit pointers. */
struct old_sigaction32 {
    uint32_t handler;
    uint32_t mask; /* signals 1 to 32 */
    uint32_t flags;
    uint32_t restorer;
};

struct sigaction32 {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint32_t mask[2];
};

/* Linux's `compat_stack_t`, 12 bytes. */
struct stack32 {
    uint32_t ss_sp;
    int32_t ss_flags;
    uint32_t ss_size;
};

/* i386's `struct old_timespec32`. */
struct timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/** ptrace: the shared handler, which gives an i386 tracer i386's words,
 * siginfo and registers (proc/ptrace.c, ptrace.c). */
long compat_sys_ptrace(isr_params_t *params)
{
    return sys_ptrace(params);
}

/**
 * signal(2), i386's 48: the handler in ECX for the signal in EBX, with
 * SA_RESETHAND and SA_NODEFER and an empty mask, as Linux's `sys_signal`
 * installs it. Returns the old handler, or -EINVAL from `signal_action`.
 */
long compat_sys_signal(isr_params_t *params)
{
    struct k_sigaction act, old;
    long rc;

    k_memset(&act, 0, sizeof(act));
    act.handler = (uint32_t)SC_ARG(params, 1);
    act.flags = SA_RESETHAND | SA_NODEFER;
    rc = signal_action((int)SC_ARG(params, 0), &act, &old);
    return rc != 0 ? rc : (long)(uint32_t)old.handler;
}

/**
 * sigaction(2), i386's 67: signal, new `compat_old_sigaction` or NULL and
 * old or NULL. The new action's mask is signals 1 to 32 and the rest clear;
 * the old action's mask word is signals 1 to 32 of the stored one. The old
 * action is written after the new one is stored, as Linux writes it.
 * -EFAULT, or -EINVAL from `signal_action`.
 */
long compat_sys_sigaction(isr_params_t *params)
{
    uintptr_t uact = SC_ARG(params, 1), uold = SC_ARG(params, 2);
    struct old_sigaction32 sa32;
    struct k_sigaction act, old;
    long rc;

    if (uact) {
        if (copy_from_user(&sa32, (const void __user *)uact, sizeof(sa32)) != 0) {
            return -EFAULT;
        }
        k_memset(&act, 0, sizeof(act));
        act.handler = sa32.handler;
        act.flags = sa32.flags;
        act.restorer = sa32.restorer;
        act.mask[0] = sa32.mask;
    }
    rc = signal_action((int)SC_ARG(params, 0), uact ? &act : NULL, uold ? &old : NULL);
    if (rc == 0 && uold) {
        sa32.handler = (uint32_t)old.handler;
        sa32.mask = old.mask[0];
        sa32.flags = (uint32_t)old.flags;
        sa32.restorer = (uint32_t)old.restorer;
        if (copy_to_user((void __user *)uold, &sa32, sizeof(sa32)) != 0) {
            return -EFAULT;
        }
    }
    return rc;
}

/**
 * rt_sigaction(2), i386's 174: signal, new `compat_sigaction` or NULL, old
 * or NULL and the set's size, which must be 8, as `sys_rt_sigaction` takes
 * them. -EINVAL for another size or from `signal_action`, -EFAULT.
 */
long compat_sys_rt_sigaction(isr_params_t *params)
{
    uintptr_t uact = SC_ARG(params, 1), uold = SC_ARG(params, 2);
    struct sigaction32 sa32;
    struct k_sigaction act, old;
    long rc;

    if (SC_ARG(params, 3) != 8u) {
        return -EINVAL;
    }
    if (uact) {
        if (copy_from_user(&sa32, (const void __user *)uact, sizeof(sa32)) != 0) {
            return -EFAULT;
        }
        k_memset(&act, 0, sizeof(act));
        act.handler = sa32.handler;
        act.flags = sa32.flags;
        act.restorer = sa32.restorer;
        act.mask[0] = sa32.mask[0];
        act.mask[1] = sa32.mask[1];
    }
    rc = signal_action((int)SC_ARG(params, 0), uact ? &act : NULL, uold ? &old : NULL);
    if (rc == 0 && uold) {
        sa32.handler = (uint32_t)old.handler;
        sa32.flags = (uint32_t)old.flags;
        sa32.restorer = (uint32_t)old.restorer;
        sa32.mask[0] = old.mask[0];
        sa32.mask[1] = old.mask[1];
        if (copy_to_user((void __user *)uold, &sa32, sizeof(sa32)) != 0) {
            return -EFAULT;
        }
    }
    return rc;
}

/** sgetmask(2), i386's 68: signals 1 to 32 of the blocked mask. */
long compat_sys_sgetmask(isr_params_t *params)
{
    (void)params;
    return (long)(uint32_t)current_proc->sig_blocked;
}

/**
 * ssetmask(2), i386's 69: the int in EBX becomes the blocked mask, and the
 * old signals 1 to 32 are returned. Linux widens the int to its 64-bit
 * mask word with its sign, so a mask that blocks signal 32 blocks 33 to 64
 * as well, and so does this.
 */
long compat_sys_ssetmask(isr_params_t *params)
{
    uint32_t old = (uint32_t)current_proc->sig_blocked;

    signal_set_blocked((uint64_t)(int64_t)(int32_t)SC_ARG(params, 0));
    return (long)old;
}

/** sigsuspend(2), i386's 72, Linux's three-argument form: the mask in EDX,
 * signals 1 to 32 with the rest clear, then as rt_sigsuspend. */
long compat_sys_sigsuspend(isr_params_t *params)
{
    return signal_suspend((uint32_t)SC_ARG(params, 2));
}

/** sigpending(2), i386's 73: signals 1 to 32 of the pending blocked set, as
 * a 32-bit word at EBX. -EFAULT. */
long compat_sys_sigpending(isr_params_t *params)
{
    uint32_t set = (uint32_t)signal_pending_blocked();

    return copy_to_user((void __user *)SC_ARG(params, 0), &set, sizeof(set)) == 0 ? 0 : -EFAULT;
}

/** sigreturn(2), i386's 119: the return from a plain i386 frame, which
 * `sigframe_restore` takes as one since the call came by `int $0x80`. */
long compat_sys_sigreturn(isr_params_t *params)
{
    return sys_sigreturn(params);
}

/** sigprocmask(2), i386's 126: the native handler, whose sets are 32-bit
 * words on both ABIs. */
long compat_sys_sigprocmask(isr_params_t *params)
{
    return sys_sigprocmask(params);
}

/** rt_sigreturn(2), i386's 173: the return from an i386 rt frame, as
 * `compat_sys_sigreturn`. */
long compat_sys_rt_sigreturn(isr_params_t *params)
{
    return sys_rt_sigreturn(params);
}

/** rt_sigprocmask(2), i386's 175: the native handler, whose 64-bit set the
 * two words of `compat_sigset_t` are. */
long compat_sys_rt_sigprocmask(isr_params_t *params)
{
    return sys_rt_sigprocmask(params);
}

/** rt_sigpending(2), i386's 176: the native handler. */
long compat_sys_rt_sigpending(isr_params_t *params)
{
    return sys_rt_sigpending(params);
}

/** Read i386's 32-bit `struct timespec` at `u` into `ts`, sign-extended:
 * 0 or -EFAULT. */
static int32_t timespec32_read(uintptr_t u, struct kernel_timespec *ts)
{
    struct timespec32 t32;

    if (copy_from_user(&t32, (const void __user *)u, sizeof(t32)) != 0) {
        return -EFAULT;
    }
    ts->tv_sec = t32.tv_sec;
    ts->tv_nsec = t32.tv_nsec;
    return 0;
}

/** Read the 64-bit `struct __kernel_timespec` an i386 caller passes at `u`
 * into `ts`, with the upper half of `tv_nsec` dropped, as Linux's
 * `get_timespec64` drops that padding for a compat call: 0 or -EFAULT. */
static int32_t timespec64_read(uintptr_t u, struct kernel_timespec *ts)
{
    if (copy_from_user(ts, (const void __user *)u, sizeof(*ts)) != 0) {
        return -EFAULT;
    }
    ts->tv_nsec = (long)(uint32_t)ts->tv_nsec;
    return 0;
}

/** rt_sigtimedwait(2), i386's 177, with a 32-bit timeout. */
long compat_sys_rt_sigtimedwait_time32(isr_params_t *params)
{
    return signal_timedwait(SC_ARG(params, 3), SC_ARG(params, 0), SC_ARG(params, 1),
                            SC_ARG(params, 2), timespec32_read);
}

/** rt_sigqueueinfo(2), i386's 178: the native handler, which reads the
 * i386 siginfo (`siginfo_from_caller`). */
long compat_sys_rt_sigqueueinfo(isr_params_t *params)
{
    return sys_rt_sigqueueinfo(params);
}

/** rt_sigsuspend(2), i386's 179: the native handler. */
long compat_sys_rt_sigsuspend(isr_params_t *params)
{
    return sys_rt_sigsuspend(params);
}

/**
 * sigaltstack(2), i386's 186: new `compat_stack_t` or NULL and old or NULL,
 * as `signal_altstack` takes them; the old one is written only when the
 * call succeeds, as Linux's `compat_sigaltstack` writes it. -EFAULT.
 */
long compat_sys_sigaltstack(isr_params_t *params)
{
    uintptr_t uss = SC_ARG(params, 0), uoss = SC_ARG(params, 1);
    struct stack32 s32;
    struct k_altstack in, old;
    long rc;

    if (uss) {
        if (copy_from_user(&s32, (const void __user *)uss, sizeof(s32)) != 0) {
            return -EFAULT;
        }
        in.sp = s32.ss_sp;
        in.flags = s32.ss_flags;
        in.size = s32.ss_size;
    }
    rc = signal_altstack(uss ? &in : NULL, &old, FRAME_SP(params));
    if (rc == 0 && uoss) {
        s32.ss_sp = (uint32_t)old.sp;
        s32.ss_flags = old.flags;
        s32.ss_size = (uint32_t)old.size;
        if (copy_to_user((void __user *)uoss, &s32, sizeof(s32)) != 0) {
            return -EFAULT;
        }
    }
    return rc;
}

/** signalfd(2), i386's 321: the native handler, whose mask and records have
 * one layout in both ABIs. */
long compat_sys_signalfd(isr_params_t *params)
{
    return sys_signalfd(params);
}

/** signalfd4(2), i386's 327: the native handler. */
long compat_sys_signalfd4(isr_params_t *params)
{
    return sys_signalfd4(params);
}

/** rt_tgsigqueueinfo(2), i386's 335: the native handler. */
long compat_sys_rt_tgsigqueueinfo(isr_params_t *params)
{
    return sys_rt_tgsigqueueinfo(params);
}

/** rt_sigtimedwait_time64(2), i386's 421, with a 64-bit timeout. */
long compat_sys_rt_sigtimedwait_time64(isr_params_t *params)
{
    return signal_timedwait(SC_ARG(params, 3), SC_ARG(params, 0), SC_ARG(params, 1),
                            SC_ARG(params, 2), timespec64_read);
}

/** pidfd_send_signal(2), i386's 424: the native handler. */
long compat_sys_pidfd_send_signal(isr_params_t *params)
{
    return sys_pidfd_send_signal(params);
}
