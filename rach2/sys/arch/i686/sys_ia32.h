#ifndef _I686_SYS_IA32_H_
#define _I686_SYS_IA32_H_

#include "cpu.h"

/*
 * The i386 forms of the calls that take a 64-bit offset or length in two
 * registers (sys_ia32.c), which i686's table maps where a 64-bit
 * architecture maps the shared handler.
 */
long sys_ia32_truncate64(isr_params_t *params);
long sys_ia32_ftruncate64(isr_params_t *params);
long sys_ia32_pread64(isr_params_t *params);
long sys_ia32_pwrite64(isr_params_t *params);
long sys_ia32_fallocate(isr_params_t *params);
long sys_ia32_fadvise64(isr_params_t *params);
long sys_ia32_fadvise64_64(isr_params_t *params);

#endif /* _I686_SYS_IA32_H_ */
