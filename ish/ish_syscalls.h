/*
 * The system calls ish makes, as `ish_*` wrappers that need no C library.
 */

#ifndef ISH_SYSCALLS_H
#define ISH_SYSCALLS_H

/*
 * Each wrapper makes one system call of the Linux ABI and returns what the
 * kernel returns: the result, or a negated error number on failure, since
 * there is no `errno`. The trap instruction, the registers and the call
 * numbers of each architecture are in `ish_syscalls.i386.c` and
 * `ish_syscalls.x86_64.c`.
 *
 * Unless `ISH_USE_STDLIB` is defined, the C library's names below are macros
 * for the wrappers, so the shell calls `read`, `open` and the rest without a
 * C library.
 */
#ifndef ISH_USE_STDLIB
#define read    ish_read
#define chdir   ish_chdir
#define exit    ish_exit
#define stat    ish_stat
#define open    ish_open
#define creat   ish_creat
#define dup2    ish_dup2
#define close   ish_close
#define fork    ish_fork
#define execve  ish_execve
#define waitpid ish_waitpid
#define write   ish_write
#endif

/** read(2): the bytes read, 0 at end of file, or a negated error number. */
long ish_read(int file_descriptor, void *buffer, unsigned long buffer_size);
/** chdir(2): 0, or a negated error number. */
int ish_chdir(const char *path);
/** exit(2): end the process with the low eight bits of `status`. */
void ish_exit(int status);
/**
 * stat(2) into `stat_result`, which must hold the architecture's
 * `struct stat`: 0, or a negated error number.
 */
int ish_stat(const char *path, void *stat_result);
/** open(2) with no mode: the new descriptor, or a negated error number. */
int ish_open(const char *path, int flags);
/** creat(2): the new descriptor, or a negated error number. */
int ish_creat(const char *path, unsigned int mode);
/** dup2(2): the new descriptor, or a negated error number. */
int ish_dup2(int old_file_descriptor, int new_file_descriptor);
/** close(2): 0, or a negated error number. */
int ish_close(int file_descriptor);
/**
 * fork(2): the child's process ID in the parent, 0 in the child, or a
 * negated error number.
 */
int ish_fork(void);
/** execve(2): returns only on failure, with a negated error number. */
int ish_execve(const char *path, char *const arguments[], char *const environment[]);
/**
 * waitpid(2): the process ID of the child waited for, or a negated error
 * number, -4 (`EINTR`) when a signal ended the wait.
 */
int ish_waitpid(int pid, int *status, int options);
/** write(2): the bytes written, or a negated error number. */
long ish_write(int file_descriptor, const void *buffer, unsigned long buffer_size);

#endif
