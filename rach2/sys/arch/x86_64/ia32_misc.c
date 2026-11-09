/*
 * ia32_misc.c: the rest of the i386 calls whose arguments differ: the 16-bit
 * ids, limits and usage, the old uname forms, futex with 32-bit times and the
 * robust list, affinity masks, seccomp's filter, keyctl and kexec_load, for the
 * compat table (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
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
IA32_STUB(compat_sys_futex_time32)
IA32_STUB(compat_sys_sched_setaffinity)
IA32_STUB(compat_sys_sched_getaffinity)
IA32_STUB(compat_sys_kexec_load)
IA32_STUB(compat_sys_waitid)
IA32_STUB(compat_sys_keyctl)
IA32_STUB(compat_sys_set_robust_list)
IA32_STUB(compat_sys_get_robust_list)
IA32_STUB(compat_sys_seccomp)
IA32_STUB(compat_sys_arch_prctl)
