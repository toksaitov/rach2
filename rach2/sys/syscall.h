#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "isr.h"

/*
 * Syscall vector number (int 0x80)
 */
#define SYSCALL_VECTOR 128

/*
 * Syscall numbers (Linux 32-bit compatible subset)
 */
#define SYS_EXIT     1
#define SYS_FORK     2
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_WAITPID  7
#define SYS_CREAT    8
#define SYS_UNLINK  10
#define SYS_EXECVE  11
#define SYS_CHDIR   12

/*
 * Size of the syscall dispatch table
 */
#define SYSCALL_TABLE_SIZE 64

/*
 * Initialize syscall handling by registering the syscall handler.
 */
void init_syscall(void);

#endif
