#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "isr.h"

/*
 * Syscall vector number (int 0x80)
 */
#define SYSCALL_VECTOR 128

/*
 * Initialize syscall handling by registering the syscall handler.
 */
void init_syscall(void);

#endif
