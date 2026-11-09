/*
 * ia32_ioctl.c: the compat ioctl path, which passes a command whose argument
 * has one layout in both ABIs and translates or refuses the others, for the
 * compat table (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(compat_sys_ioctl)
