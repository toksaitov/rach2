#ifndef _I686_SYSNR_H_
#define _I686_SYSNR_H_

/*
 * The numbers of the system calls the i686 kernel has: Linux i386's, which a
 * program built for i386 Linux makes with `int $0x80`. systab.c maps each to
 * its handler.
 */

/* The vector of `int $0x80`. */
#define SYSCALL_VECTOR 128

#define SYS_EXIT          1
#define SYS_FORK          2
#define SYS_READ          3
#define SYS_WRITE         4
#define SYS_OPEN          5
#define SYS_CLOSE         6
#define SYS_READLINK      85
#define SYS_READLINKAT    305
#define SYS_ACCESS        33
#define SYS_FACCESSAT     307
#define SYS_WAITPID       7
#define SYS_CREAT         8
#define SYS_UNLINK        10
#define SYS_EXECVE        11
#define SYS_CHDIR         12
#define SYS_FCHDIR        133
#define SYS_CHROOT        61
#define SYS_LSEEK         19
#define SYS_MOUNT         21
#define SYS_UMOUNT2       52
#define SYS_TRUNCATE      92
#define SYS_TRUNCATE64    193
#define SYS_FTRUNCATE     93
#define SYS_FTRUNCATE64   194
#define SYS_MKDIR         39
#define SYS_RMDIR         40
#define SYS_MKDIRAT       296
#define SYS_MKNOD         14
#define SYS_MKNODAT       297
#define SYS_STATX         383
#define SYS_FLOCK         143
#define SYS_MEMFD_CREATE  356
#define SYS_SENDFILE      187
#define SYS_SENDFILE64    239
#define SYS_SPLICE        313
#define SYS_TEE           315
#define SYS_CLOSE_RANGE   436
#define SYS_CLONE3        435
#define SYS_UNLINKAT      301
#define SYS_GETPID        20
#define SYS_TIMES         43
#define SYS_UMASK         60
#define SYS_GETPPID       64
#define SYS_SYSINFO       116
#define SYS_VFORK         190
#define SYS_PAUSE         29
#define SYS_ALARM         27
#define SYS_SYSLOG        103
#define SYS_SETITIMER     104
#define SYS_GETITIMER     105
#define SYS_SETPGID       57
#define SYS_GETPGRP       65
#define SYS_SETSID        66
#define SYS_GETPGID       132
#define SYS_GETSID        147
#define SYS_RT_SIGSUSPEND 179
#define SYS_RENAME        38
#define SYS_RENAMEAT      302
#define SYS_RENAMEAT2     353
#define SYS_BRK           45
#define SYS_MUNMAP        91
#define SYS_WAIT4         114
#define SYS_GETPRIORITY   96
#define SYS_SETPRIORITY   97
#define SYS_IOPRIO_SET    289
#define SYS_IOPRIO_GET    290

/* System V IPC (ipc/sysv.c): the one call and the direct calls. */
#define SYS_IPC               117
#define SYS_SEMGET            393
#define SYS_SEMCTL            394
#define SYS_SHMGET            395
#define SYS_SHMCTL            396
#define SYS_SHMAT             397
#define SYS_SHMDT             398
#define SYS_MSGGET            399
#define SYS_MSGSND            400
#define SYS_MSGRCV            401
#define SYS_MSGCTL            402
#define SYS_SEMTIMEDOP_TIME64 420

/* POSIX message queues (ipc/mqueue.c). */
#define SYS_MQ_OPEN                277
#define SYS_MQ_UNLINK              278
#define SYS_MQ_TIMEDSEND           279
#define SYS_MQ_TIMEDRECEIVE        280
#define SYS_MQ_NOTIFY              281
#define SYS_MQ_GETSETATTR          282
#define SYS_MQ_TIMEDSEND_TIME64    418
#define SYS_MQ_TIMEDRECEIVE_TIME64 419
#define SYS_MMAP2                  192
#define SYS_MPROTECT               125
#define SYS_CLONE                  120
#define SYS_FUTEX                  240
#define SYS_SIGALTSTACK            186
#define SYS_RT_SIGPENDING          176
#define SYS_SIGPROCMASK            126
#define SYS_SOCKETCALL             102
#define SYS_IOCTL                  54
#define SYS_DUP                    41
#define SYS_DUP2                   63
#define SYS_PIPE                   42
#define SYS_PIPE2                  331
#define SYS_SELECT                 82  /* old i386 select: 5 args behind one pointer */
#define SYS_NEWSELECT              142 /* _newselect: the normal 5-arg form */
#define SYS_POLL                   168
#define SYS_PRCTL                  172
#define SYS_CAPGET                 184
#define SYS_CAPSET                 185
#define SYS_PSELECT6               308
#define SYS_PPOLL                  309
#define SYS_GETRUSAGE              77
#define SYS_GETTIMEOFDAY           78
#define SYS_TIME                   13
#define SYS_SETTIMEOFDAY           79
#define SYS_STIME                  25
#define SYS_STAT                   106
#define SYS_LSTAT                  107
#define SYS_UNAME                  122
#define SYS_READV                  145
#define SYS_WRITEV                 146
#define SYS_PREAD64                180
#define SYS_PWRITE64               181
#define SYS_GETCWD                 183
#define SYS_LLSEEK                 140
#define SYS_FSTAT64                197
#define SYS_STATFS64               268
#define SYS_FSTATFS64              269
#define SYS_FCNTL64                221
#define SYS_GETTID                 224
#define SYS_OPENAT                 295
#define SYS_FSTATAT64              300
#define SYS_STAT64                 195
#define SYS_LSTAT64                196
#define SYS_GETDENTS64             220
#define SYS_SET_THREAD_AREA        243
#define SYS_EXIT_GROUP             252
#define SYS_PTRACE                 26
#define SYS_PERSONALITY            136
#define SYS_GETCPU                 318
#define SYS_MLOCK                  150
#define SYS_MUNLOCK                151
#define SYS_MLOCKALL               152
#define SYS_MUNLOCKALL             153
#define SYS_RT_SIGTIMEDWAIT        177
#define SYS_MINCORE                218
#define SYS_MLOCK2                 376
#define SYS_SET_TID_ADDRESS        258
#define SYS_SET_ROBUST_LIST        311
#define SYS_GET_ROBUST_LIST        312
#define SYS_WAITID                 284
#define SYS_UTIMENSAT              320
#define SYS_NANOSLEEP              162
#define SYS_GETUID32               199
#define SYS_GETGID32               200
#define SYS_GETEUID32              201
#define SYS_GETEGID32              202
#define SYS_CLOCK_SETTIME          264
#define SYS_CLOCK_GETTIME          265
#define SYS_CLOCK_GETRES           266
#define SYS_CLOCK_NANOSLEEP        267
#define SYS_GETRANDOM              355
#define SYS_SECCOMP                354
#define SYS_UNSHARE                310
#define SYS_SETNS                  346
#define SYS_KILL                   37
#define SYS_SIGRETURN              119
#define SYS_RT_SIGRETURN           173
#define SYS_RT_SIGACTION           174
#define SYS_RT_SIGPROCMASK         175
#define SYS_TKILL                  238
#define SYS_TGKILL                 270
#define SYS_RT_SIGQUEUEINFO        178
#define SYS_RT_TGSIGQUEUEINFO      335
#define SYS_SIGNALFD               321
#define SYS_SIGNALFD4              327

/* The scheduling calls, whose names are longer than the column above. */
#define SYS_SCHED_SETPARAM         154
#define SYS_SCHED_GETPARAM         155
#define SYS_SCHED_SETSCHEDULER     156
#define SYS_SCHED_GETSCHEDULER     157
#define SYS_SCHED_YIELD            158
#define SYS_SCHED_GET_PRIORITY_MAX 159
#define SYS_SCHED_GET_PRIORITY_MIN 160
#define SYS_SCHED_RR_GET_INTERVAL  161
#define SYS_SCHED_SETAFFINITY      241
#define SYS_SCHED_GETAFFINITY      242

/* The clock calls with 64-bit times, whose names are longer than the column
 * above. */
#define SYS_CLOCK_GETTIME64     403
#define SYS_CLOCK_SETTIME64     404
#define SYS_CLOCK_ADJTIME64     405
#define SYS_CLOCK_ADJTIME       343
#define SYS_ADJTIMEX            124
#define SYS_CLOCK_GETRES_TIME64 406

/* Names, limits and identities. */
#define SYS_SETHOSTNAME   74
#define SYS_SETDOMAINNAME 121
#define SYS_SETRLIMIT     75
#define SYS_GETRLIMIT     76
#define SYS_UGETRLIMIT    191
#define SYS_GETGROUPS32   205
#define SYS_SETGROUPS32   206
#define SYS_GETRESUID32   209
#define SYS_GETRESGID32   211
#define SYS_SETUID32      213
#define SYS_SETGID32      214
#define SYS_DUP3          330
#define SYS_PRLIMIT64     340

/* Write-back and space: fsync and fdatasync write back one open file, sync
 * and syncfs every mounted file system (syscall_fs.c). */
#define SYS_SYNC      36
#define SYS_FSYNC     118
#define SYS_FDATASYNC 148
#define SYS_SYNCFS    344
#define SYS_FALLOCATE 324

/* Advice about coming reads, which the page cache accepts and ignores. */
#define SYS_READAHEAD    225
#define SYS_FADVISE64    250
#define SYS_FADVISE64_64 272

/* Extended attributes, which no file system here stores. */
#define SYS_SETXATTR     226
#define SYS_LSETXATTR    227
#define SYS_FSETXATTR    228
#define SYS_GETXATTR     229
#define SYS_LGETXATTR    230
#define SYS_FGETXATTR    231
#define SYS_LISTXATTR    232
#define SYS_LLISTXATTR   233
#define SYS_FLISTXATTR   234
#define SYS_REMOVEXATTR  235
#define SYS_LREMOVEXATTR 236
#define SYS_FREMOVEXATTR 237

/* Stop the machine, after the same flush sync(2) does. Root only. */
#define SYS_REBOOT      88
/* Hard and symbolic links, which ext2 and tmpfs have; on a file system
 * without them the calls are -EPERM. */
#define SYS_LINK        9
#define SYS_LINKAT      303
#define SYS_SYMLINK     83
#define SYS_SYMLINKAT   304
/* setreuid, setregid, setresuid and setresgid by their 32-bit numbers,
 * which musl calls on i386 (src/internal/syscall.h); the 16-bit forms,
 * whose ids are narrower and whose "keep" is 0xFFFF, are not in the table. */
#define SYS_SETREUID32  203
#define SYS_SETREGID32  204
#define SYS_SETRESUID32 208
#define SYS_SETRESGID32 210
/* The mode and owner calls; the 16-bit chown numbers take 32-bit ids here,
 * as their *32 forms do. */
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

/* mremap, madvise and msync (syscall_proc.c). */
#define SYS_MSYNC   144
#define SYS_MREMAP  163
#define SYS_MADVISE 219

/* The direct socket calls (syscall_net.c), socketcall's sub-calls by
 * number. */
#define SYS_SOCKET      359
#define SYS_SOCKETPAIR  360
#define SYS_BIND        361
#define SYS_CONNECT     362
#define SYS_LISTEN      363
#define SYS_ACCEPT4     364
#define SYS_GETSOCKOPT  365
#define SYS_SETSOCKOPT  366
#define SYS_GETSOCKNAME 367
#define SYS_GETPEERNAME 368
#define SYS_SENDTO      369
#define SYS_SENDMSG     370
#define SYS_RECVFROM    371
#define SYS_RECVMSG     372
#define SYS_SHUTDOWN    373
#define SYS_RECVMMSG    337
#define SYS_SENDMMSG    345

/* eventfd, epoll and timerfd (io/eventfd.c, io/epoll.c, io/timerfd.c). */
#define SYS_EPOLL_CREATE      254
#define SYS_EPOLL_CTL         255
#define SYS_EPOLL_WAIT        256
#define SYS_EPOLL_PWAIT       319
#define SYS_TIMERFD_CREATE    322
#define SYS_EVENTFD           323
#define SYS_TIMERFD_SETTIME   325
#define SYS_TIMERFD_GETTIME   326
#define SYS_EVENTFD2          328
#define SYS_EPOLL_CREATE1     329
#define SYS_TIMERFD_GETTIME64 410
#define SYS_TIMERFD_SETTIME64 411
#define SYS_EPOLL_PWAIT2      441

/* POSIX timers (io/timerfd.c, which has the timers both kinds share). */
#define SYS_TIMER_CREATE     259
#define SYS_TIMER_SETTIME    260
#define SYS_TIMER_GETTIME    261
#define SYS_TIMER_GETOVERRUN 262
#define SYS_TIMER_DELETE     263
#define SYS_TIMER_GETTIME64  408
#define SYS_TIMER_SETTIME64  409

/* inotify (io/inotify.c; the add is in syscall_fs.c, which resolves paths). */
#define SYS_INOTIFY_INIT      291
#define SYS_INOTIFY_ADD_WATCH 292
#define SYS_INOTIFY_RM_WATCH  293
#define SYS_INOTIFY_INIT1     332

#endif /* _I686_SYSNR_H_ */
