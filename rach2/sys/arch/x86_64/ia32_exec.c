/*
 * ia32_exec.c: the i386 forms of execve, execveat, clone and the two mmap
 * calls, for the compat table (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(compat_sys_execve)
IA32_STUB(compat_sys_ia32_mmap)
IA32_STUB(compat_sys_ia32_clone)
IA32_STUB(compat_sys_mmap_pgoff)
IA32_STUB(compat_sys_execveat)
