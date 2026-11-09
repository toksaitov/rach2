/*
 * ldt.c: modify_ldt, the local descriptor table of 32-bit code, for the compat
 * table (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(sys_modify_ldt)
