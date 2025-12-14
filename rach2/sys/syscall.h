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
#define SYS_RENAME  38
#define SYS_DUP2    63
#define SYS_GETCWD  183
#define SYS_STAT64  195

/*
 * Size of the syscall dispatch table
 * Must be large enough to hold highest syscall number + 1
 */
#define SYSCALL_TABLE_SIZE 196

/*
 * Initialize syscall handling by registering the syscall handler.
 */
void init_syscall(void);

#endif
