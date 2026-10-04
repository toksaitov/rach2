#ifndef _X86_64_TLS_H_
#define _X86_64_TLS_H_

#include "../arch.h"

/*
 * Thread-local storage on x86-64: the thread pointer is FS's base, which a
 * program sets with arch_prctl(ARCH_SET_FS) or gets from clone's
 * CLONE_SETTLS, and GS's base is the program's to use as it likes, through
 * ARCH_SET_GS. Both live in the process record (`tls_base` and
 * `regs.gs_base`), read back from their MSRs when the thread switches away
 * and loaded when it switches in (context.c): a selector the program loads
 * into FS or GS changes the base too, to the descriptor's 0.
 */

/* arch_prctl's codes, as Linux numbers them. */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/**
 * arch_prctl(2), Linux x86-64's 158: RDI the code, RSI the base to set or the
 * user address to store it at. A base must be a user address, below
 * USER_ADDR_MAX, as Linux requires: -EPERM otherwise. -EFAULT for an address
 * to store at that cannot be written, -EINVAL for any other code.
 */
long sys_arch_prctl(isr_params_t *params);

#endif /* _X86_64_TLS_H_ */
