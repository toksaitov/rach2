#ifndef ISH_SYSCALLS_H
#define ISH_SYSCALLS_H

/**
 * The system call surface ish uses, declared as thin `ish_*` wrappers. The
 * i386 Linux ABI (the `int $0x80` mechanism, the register bindings, and each
 * syscall number) lives on the definitions in `ish_syscalls.i386.c`.
 *
 * Unless `ISH_USE_STDLIB` is defined, the bare libc names below are macros that
 * route to the wrappers, so the freestanding shell calls `read`, `open`, and so
 * on without a libc.
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

long ish_read(int file_descriptor, void *buffer, unsigned long buffer_size);
int ish_chdir(const char *path);
void ish_exit(int status);
int ish_stat(const char *path, void *stat_result);
int ish_open(const char *path, int flags);
int ish_creat(const char *path, unsigned int mode);
int ish_dup2(int old_file_descriptor, int new_file_descriptor);
int ish_close(int file_descriptor);
int ish_fork(void);
int ish_execve(const char *path, char *const arguments[], char *const environment[]);
int ish_waitpid(int pid, int *status, int options);
long ish_write(int file_descriptor, const void *buffer, unsigned long buffer_size);

#endif
