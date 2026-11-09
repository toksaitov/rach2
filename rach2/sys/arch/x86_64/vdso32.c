/*
 * vdso32.c: the 32-bit vDSO of i386 programs, built at boot beside the
 * 64-bit one (vdso.c) from the code of vdso32code.s.
 */

#include "ia32.h"

/** None is built yet. */
int vdso32_present(void)
{
    return 0;
}
