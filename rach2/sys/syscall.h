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
#define SYS_READLINK   85
#define SYS_READLINKAT 305
#define SYS_ACCESS     33
#define SYS_FACCESSAT  307
#define SYS_WAITPID  7
#define SYS_CREAT    8
#define SYS_UNLINK  10
#define SYS_EXECVE  11
#define SYS_CHDIR   12
#define SYS_LSEEK   19
#define SYS_MKDIR   39
#define SYS_RMDIR   40
#define SYS_GETPID   20
#define SYS_TIMES     43
#define SYS_UMASK     60
#define SYS_GETPPID   64
#define SYS_SYSINFO  116
#define SYS_VFORK    190
#define SYS_PAUSE     29
#define SYS_SETPGID   57
#define SYS_GETPGRP   65
#define SYS_SETSID    66
#define SYS_GETPGID  132
#define SYS_GETSID   147
#define SYS_RT_SIGSUSPEND 179
#define SYS_RENAME    38
#define SYS_BRK       45
#define SYS_MUNMAP    91
#define SYS_WAIT4    114
#define SYS_MMAP2    192
#define SYS_SOCKETCALL 102
#define SYS_IOCTL     54
#define SYS_DUP       41
#define SYS_DUP2      63
#define SYS_PIPE      42
#define SYS_PIPE2     331
#define SYS_SELECT    82   /* old i386 select: 5 args behind one pointer */
#define SYS_NEWSELECT 142  /* _newselect: the normal 5-arg form */
#define SYS_POLL      168
#define SYS_PSELECT6  308
#define SYS_PPOLL     309
#define SYS_GETTIMEOFDAY 78
#define SYS_STAT     106
#define SYS_LSTAT    107
#define SYS_UNAME    122
#define SYS_READV    145
#define SYS_WRITEV   146
#define SYS_GETCWD   183
#define SYS_LLSEEK   140
#define SYS_FSTAT64  197
#define SYS_FCNTL64  221
#define SYS_OPENAT   295
#define SYS_FSTATAT64 300
#define SYS_STAT64   195
#define SYS_LSTAT64  196
#define SYS_GETDENTS64 220
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP 252
#define SYS_SET_TID_ADDRESS 258
#define SYS_UTIMENSAT 320
#define SYS_NANOSLEEP 162
#define SYS_GETUID32 199
#define SYS_GETGID32 200
#define SYS_GETEUID32 201
#define SYS_GETEGID32 202
#define SYS_CLOCK_GETTIME 265
#define SYS_GETRANDOM 355
#define SYS_KILL 37
#define SYS_SIGRETURN 119
#define SYS_RT_SIGRETURN 173
#define SYS_RT_SIGACTION 174
#define SYS_RT_SIGPROCMASK 175
#define SYS_TKILL 238
#define SYS_TGKILL 270

/* Auth/identity, resource, and tail syscalls a login server touches. */
#define SYS_SETHOSTNAME 74
#define SYS_GETRLIMIT   76
#define SYS_UGETRLIMIT  191
#define SYS_GETGROUPS32 205
#define SYS_SETGROUPS32 206
#define SYS_GETRESUID32 209
#define SYS_GETRESGID32 211
#define SYS_SETUID32    213
#define SYS_SETGID32    214
#define SYS_DUP3        330
#define SYS_PRLIMIT64   340

/* Durability: flush the disk-backed page cache (a no-op when KRFFS is the RAM
   module). fsync/fdatasync flush per the open fd; sync flushes everything. */
#define SYS_SYNC        36
#define SYS_FSYNC       118
#define SYS_FDATASYNC   148

/* Stop the machine, after the same flush sync(2) does. Root only. */
#define SYS_REBOOT      88
/* KRFFS has no hard links; link/linkat return -EPERM (dropbear falls back). */
#define SYS_LINK        9
#define SYS_LINKAT      303
/* The 2-/3-arg privilege drop and restore a login server uses (seteuid).
 * musl remaps the whole legacy 16-bit id family to the *32 forms on i386
 * (src/internal/syscall.h), so only the *32 numbers are wired; the 16-bit
 * ABI (narrower ids, 0xFFFF keep-sentinel) is left to ENOSYS. */
#define SYS_SETREUID32  203
#define SYS_SETREGID32  204
#define SYS_SETRESUID32 208
#define SYS_SETRESGID32 210
/* chmod/chown the session pty (dropbear) and arbitrary KRFFS paths (busybox).
 * Both legacy 16-bit-id and *32 chown numbers, since musl issues the former. */
#define SYS_CHMOD       15
#define SYS_FCHMOD      94
#define SYS_FCHMODAT    306
#define SYS_CHOWN       182
#define SYS_CHOWN32     212
#define SYS_LCHOWN      16
#define SYS_LCHOWN32    198
#define SYS_FCHOWN      95
#define SYS_FCHOWN32    207
#define SYS_FCHOWNAT    298

/*
 * Size of the syscall dispatch table
 * Must be large enough to hold highest syscall number + 1
 */
#define SYSCALL_TABLE_SIZE 356

/**
 * Register the syscall handler and populate the dispatch table.
 */
void init_syscall(void);

/** Restore text mode if `pid` owns the framebuffer. */
void fb_release_if_owner(uint32_t pid);


/* Whether any open descriptor holds this KRFFS node. The debug shell asks before
   removing or renaming, since it goes below the syscall layer that owns the
   rule that a held node is orphaned rather than freed. */
struct krffs_node;
int fs_node_is_held(const struct krffs_node *node);
void fs_node_moved(const struct krffs_node *from, struct krffs_node *to);

#endif
