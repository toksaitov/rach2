/*
 * ia32_signal.c: the i386 forms of the signal calls, with their 32-bit
 * sigaction, siginfo, masks and frames, and of ptrace, for the compat table
 * (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(compat_sys_ptrace)
IA32_STUB(compat_sys_signal)
IA32_STUB(compat_sys_sigaction)
IA32_STUB(compat_sys_sgetmask)
IA32_STUB(compat_sys_ssetmask)
IA32_STUB(compat_sys_sigsuspend)
IA32_STUB(compat_sys_sigpending)
IA32_STUB(compat_sys_sigreturn)
IA32_STUB(compat_sys_sigprocmask)
IA32_STUB(compat_sys_rt_sigreturn)
IA32_STUB(compat_sys_rt_sigaction)
IA32_STUB(compat_sys_rt_sigprocmask)
IA32_STUB(compat_sys_rt_sigpending)
IA32_STUB(compat_sys_rt_sigtimedwait_time32)
IA32_STUB(compat_sys_rt_sigqueueinfo)
IA32_STUB(compat_sys_rt_sigsuspend)
IA32_STUB(compat_sys_sigaltstack)
IA32_STUB(compat_sys_signalfd)
IA32_STUB(compat_sys_signalfd4)
IA32_STUB(compat_sys_rt_tgsigqueueinfo)
IA32_STUB(compat_sys_rt_sigtimedwait_time64)
IA32_STUB(compat_sys_pidfd_send_signal)
