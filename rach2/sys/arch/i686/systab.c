/*
 * systab.c: the i686's system call table, from Linux i386's numbers
 * (sysnr.h) to the shared handlers, and its entry, `int $0x80`.
 */

#include "sysnr.h"
#include "sys_ia32.h"

#include "../arch.h"
#include "../../../kernel.h"
#include "../../mm/kheap.h"
#include "../../klog.h"
#include "../../syscall.h"
#include "../../power.h"
#include "../../serial.h"
#include "../../panic.h"
#include "../../kio.h"
#include "../../kbd.h"
#include "../../input.h"
#include "../../mouse.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/elf.h"
#include "../../proc/uimage.h"
#include "../../proc/futex.h"
#include "../../proc/tgroup.h"
#include "../../proc/ptrace.h"
#include "../../proc/seccomp.h"
#include "../../proc/ns.h"
#include "../../lib/krandom.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../lib/ksignal.h"
#include "../../fs/fd.h"
#include "../../fs/file.h"
#include "../../ipc/pipe.h"
#include "../../io/poll.h"
#include "../../usb/usb.h"
#include "../../tty/tty.h"
#include "../../tty/pty.h"
#include "../../rtc.h"
#include "../../block/loop.h"
#include "../../fs/krffs.h"
#include "../../fs/bcache.h"
#include "../../fs/devfs.h"
#include "../../fs/synthfs.h"
#include "../../fs/vfs.h"
#include "../../block/usbdisk.h"
#include "../../fs/procfs.h"
#include "../../mm/paging.h"
#include "../../mm/pressure.h"
#include "../../mm/pagecache.h"
#include "../../mm/heap.h"
#include "../../mm/usercopy.h"
#include "../../mm/vm.h"
#include "../../lib/kuser.h"
#include "../../lib/kstring.h"
#include "../../display/fb.h"
#include "../../speaker.h"
#include "../../audio/audio.h"
#include "../../network/socket/socket.h"
#include "../../network/socket/packet.h"
#include "../../network/socket/raw.h"
#include "../../network/ethernet/ethernet.h"
#include "../../network/netif.h"
#include "../../network/driver/netdev.h"
#include "../../network/udp/udp.h"
#include "../../network/tcp/tcp.h"
#include "../../network/ip/ip.h"
#include "../../network/ip/icmp.h"
#include "../../network/net.h"
#include "../../network/netconf.h"
#include "../../cmdline.h"
#include "../../io/eventfd.h"
#include "../../io/inotify.h"
#include "../../io/epoll.h"
#include "../../io/timerfd.h"
#include "../../ipc/sysv.h"
#include "../../ipc/mqueue.h"
#include "../../syscall_internal.h"

void arch_syscall_init(syscall_fn_t *table, void (*dispatch)(isr_params_t *params))
{
    table[SYS_EXIT] = sys_exit;
    table[SYS_FORK] = sys_fork;
    table[SYS_READ] = sys_read;
    table[SYS_WRITE] = sys_write;
    table[SYS_PREAD64] = sys_ia32_pread64;
    table[SYS_PWRITE64] = sys_ia32_pwrite64;
    table[SYS_OPEN] = sys_open;
    table[SYS_CLOSE] = sys_close;
    table[SYS_WAITPID] = sys_waitpid;
    table[SYS_WAIT4] = sys_wait4;
    table[SYS_WAITID] = sys_waitid;
    table[SYS_GETRUSAGE] = sys_getrusage;
    table[SYS_CREAT] = sys_creat;
    table[SYS_UNLINK] = sys_unlink;
    table[SYS_UNLINKAT] = sys_unlinkat;
    table[SYS_MKDIR] = sys_mkdir;
    table[SYS_MKDIRAT] = sys_mkdirat;
    table[SYS_MKNOD] = sys_mknod;
    table[SYS_MKNODAT] = sys_mknodat;
    table[SYS_RMDIR] = sys_rmdir;
    table[SYS_EXECVE] = sys_execve;
    table[SYS_CHDIR] = sys_chdir;
    table[SYS_FCHDIR] = sys_fchdir;
    table[SYS_CHROOT] = sys_chroot;
    table[SYS_BRK] = sys_brk;
    table[SYS_MMAP2] = sys_mmap2;
    table[SYS_MUNMAP] = sys_munmap;
    table[SYS_MPROTECT] = sys_mprotect;
    table[SYS_CLONE] = sys_clone;
    table[SYS_FUTEX] = sys_futex;
    table[SYS_SIGALTSTACK] = sys_sigaltstack;
    table[SYS_RT_SIGPENDING] = sys_rt_sigpending;
    table[SYS_SIGPROCMASK] = sys_sigprocmask;
    table[SYS_LSEEK] = sys_lseek;
    table[SYS_RENAME] = sys_rename;
    table[SYS_RENAMEAT] = sys_renameat;
    table[SYS_RENAMEAT2] = sys_renameat2;
    table[SYS_IOCTL] = sys_ioctl;
    table[SYS_DUP] = sys_dup;
    table[SYS_DUP2] = sys_dup2;
    table[SYS_PIPE] = sys_pipe;
    table[SYS_PIPE2] = sys_pipe2;
    table[SYS_POLL] = sys_poll;
    table[SYS_PPOLL] = sys_ppoll;
    table[SYS_SELECT] = sys_select;
    table[SYS_NEWSELECT] = sys__newselect;
    table[SYS_PSELECT6] = sys_pselect6;
    table[SYS_GETTIMEOFDAY] = sys_gettimeofday;
    table[SYS_TIME] = sys_time;
    table[SYS_SETTIMEOFDAY] = sys_settimeofday;
    table[SYS_STIME] = sys_stime;
    table[SYS_STAT] = sys_stat;
    table[SYS_LSTAT] = sys_lstat;
    table[SYS_GETCWD] = sys_getcwd;
    table[SYS_STAT64] = sys_stat64;
    table[SYS_LSTAT64] = sys_lstat64;
    table[SYS_FSTAT64] = sys_fstat64;
    table[SYS_STATFS64] = sys_statfs64;
    table[SYS_FSTATFS64] = sys_fstatfs64;
    table[SYS_FSTATAT64] = sys_fstatat64;
    table[SYS_STATX] = sys_statx;
    table[SYS_OPENAT] = sys_openat;
    table[SYS_READLINK] = sys_readlink;
    table[SYS_READLINKAT] = sys_readlinkat;
    table[SYS_ACCESS] = sys_access;
    table[SYS_FACCESSAT] = sys_faccessat;
    table[SYS_LLSEEK] = sys__llseek;
    table[SYS_FCNTL64] = sys_fcntl64;
    table[SYS_FLOCK] = sys_flock;
    table[SYS_MEMFD_CREATE] = sys_memfd_create;
    table[SYS_SENDFILE] = sys_sendfile;
    table[SYS_SENDFILE64] = sys_sendfile;
    table[SYS_SPLICE] = sys_splice;
    table[SYS_TEE] = sys_tee;
    table[SYS_CLOSE_RANGE] = sys_close_range;
    table[SYS_CLONE3] = sys_clone3;
    table[SYS_GETDENTS64] = sys_getdents64;
    table[SYS_UTIMENSAT] = sys_utimensat;
    table[SYS_CLOCK_GETTIME] = sys_clock_gettime;
    table[SYS_CLOCK_SETTIME] = sys_clock_settime;
    table[SYS_ADJTIMEX] = sys_adjtimex;
    table[SYS_CLOCK_ADJTIME] = sys_clock_adjtime;
    table[SYS_CLOCK_ADJTIME64] = sys_clock_adjtime64;
    table[SYS_CLOCK_GETRES] = sys_clock_getres;
    table[SYS_CLOCK_NANOSLEEP] = sys_clock_nanosleep;
    table[SYS_NANOSLEEP] = sys_nanosleep;
    table[SYS_SETITIMER] = sys_setitimer;
    table[SYS_GETITIMER] = sys_getitimer;
    table[SYS_ALARM] = sys_alarm;
    table[SYS_GETRANDOM] = sys_getrandom;
    table[SYS_SECCOMP] = sys_seccomp;
    table[SYS_UNSHARE] = sys_unshare;
    table[SYS_SETNS] = sys_setns;
    table[SYS_KILL] = sys_kill;
    table[SYS_TKILL] = sys_tkill;
    table[SYS_TGKILL] = sys_tgkill;
    table[SYS_RT_SIGQUEUEINFO] = sys_rt_sigqueueinfo;
    table[SYS_RT_TGSIGQUEUEINFO] = sys_rt_tgsigqueueinfo;
    table[SYS_SIGNALFD] = sys_signalfd;
    table[SYS_SIGNALFD4] = sys_signalfd4;
    table[SYS_RT_SIGACTION] = sys_rt_sigaction;
    table[SYS_RT_SIGPROCMASK] = sys_rt_sigprocmask;
    table[SYS_RT_SIGRETURN] = sys_rt_sigreturn;
    table[SYS_SIGRETURN] = sys_sigreturn;
    table[SYS_GETUID32] = sys_getuid;
    table[SYS_GETGID32] = sys_getgid;
    table[SYS_GETEUID32] = sys_geteuid;
    table[SYS_GETEGID32] = sys_getegid;
    table[SYS_GETRESUID32] = sys_getresuid;
    table[SYS_GETRESGID32] = sys_getresgid;
    table[SYS_GETGROUPS32] = sys_getgroups;
    table[SYS_SOCKETCALL] = sys_socketcall;
    table[SYS_SET_THREAD_AREA] = sys_set_thread_area;
    table[SYS_GETPID] = sys_getpid;
    table[SYS_GETTID] = sys_gettid;
    table[SYS_GETPRIORITY] = sys_getpriority;
    table[SYS_SETPRIORITY] = sys_setpriority;
    table[SYS_IOPRIO_SET] = sys_ioprio_set;
    table[SYS_IOPRIO_GET] = sys_ioprio_get;
    table[SYS_IPC] = sys_ipc;
    table[SYS_SEMGET] = sys_semget;
    table[SYS_SEMCTL] = sys_semctl;
    table[SYS_SHMGET] = sys_shmget;
    table[SYS_SHMCTL] = sys_shmctl;
    table[SYS_SHMAT] = sys_shmat;
    table[SYS_SHMDT] = sys_shmdt;
    table[SYS_MSGGET] = sys_msgget;
    table[SYS_MSGSND] = sys_msgsnd;
    table[SYS_MSGRCV] = sys_msgrcv;
    table[SYS_MSGCTL] = sys_msgctl;
    table[SYS_SEMTIMEDOP_TIME64] = sys_semtimedop_time64;
    table[SYS_MQ_OPEN] = sys_mq_open;
    table[SYS_MQ_UNLINK] = sys_mq_unlink;
    table[SYS_MQ_TIMEDSEND] = sys_mq_timedsend;
    table[SYS_MQ_TIMEDRECEIVE] = sys_mq_timedreceive;
    table[SYS_MQ_NOTIFY] = sys_mq_notify;
    table[SYS_MQ_GETSETATTR] = sys_mq_getsetattr;
    table[SYS_MQ_TIMEDSEND_TIME64] = sys_mq_timedsend_time64;
    table[SYS_MQ_TIMEDRECEIVE_TIME64] = sys_mq_timedreceive_time64;
    table[SYS_SCHED_SETPARAM] = sys_sched_setparam;
    table[SYS_SCHED_GETPARAM] = sys_sched_getparam;
    table[SYS_SCHED_SETSCHEDULER] = sys_sched_setscheduler;
    table[SYS_SCHED_GETSCHEDULER] = sys_sched_getscheduler;
    table[SYS_SCHED_YIELD] = sys_sched_yield;
    table[SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max;
    table[SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min;
    table[SYS_SCHED_RR_GET_INTERVAL] = sys_sched_rr_get_interval;
    table[SYS_SCHED_SETAFFINITY] = sys_sched_setaffinity;
    table[SYS_SCHED_GETAFFINITY] = sys_sched_getaffinity;
    table[SYS_PRCTL] = sys_prctl;
    table[SYS_CAPGET] = sys_capget;
    table[SYS_CAPSET] = sys_capset;
    table[SYS_SETSID] = sys_setsid;
    table[SYS_SETPGID] = sys_setpgid;
    table[SYS_GETPGID] = sys_getpgid;
    table[SYS_GETPGRP] = sys_getpgrp;
    table[SYS_GETSID] = sys_getsid;
    table[SYS_PAUSE] = sys_pause;
    table[SYS_RT_SIGSUSPEND] = sys_rt_sigsuspend;
    table[SYS_GETPPID] = sys_getppid;
    table[SYS_UMASK] = sys_umask;
    table[SYS_TIMES] = sys_times;
    table[SYS_SYSINFO] = sys_sysinfo;
    table[SYS_VFORK] = sys_vfork;
    table[SYS_READV] = sys_readv;
    table[SYS_WRITEV] = sys_writev;
    table[SYS_UNAME] = sys_uname;
    table[SYS_SYSLOG] = sys_syslog;
    table[SYS_SETHOSTNAME] = sys_sethostname;
    table[SYS_SETDOMAINNAME] = sys_setdomainname;
    table[SYS_SETUID32] = sys_setuid;
    table[SYS_SETGID32] = sys_setgid;
    table[SYS_SETGROUPS32] = sys_setgroups;
    table[SYS_GETRLIMIT] = sys_getrlimit;
    table[SYS_SETRLIMIT] = sys_setrlimit;
    table[SYS_UGETRLIMIT] = sys_getrlimit;
    table[SYS_PRLIMIT64] = sys_prlimit64;
    table[SYS_DUP3] = sys_dup3;
    table[SYS_SYNC] = sys_sync;
    table[SYS_SYNCFS] = sys_syncfs;
    table[SYS_FALLOCATE] = sys_ia32_fallocate;
    table[SYS_READAHEAD] = sys_readahead;
    table[SYS_FADVISE64] = sys_ia32_fadvise64;
    table[SYS_FADVISE64_64] = sys_ia32_fadvise64_64;
    table[SYS_SETXATTR] = sys_setxattr;
    table[SYS_LSETXATTR] = sys_lsetxattr;
    table[SYS_FSETXATTR] = sys_fsetxattr;
    table[SYS_GETXATTR] = sys_getxattr;
    table[SYS_LGETXATTR] = sys_lgetxattr;
    table[SYS_FGETXATTR] = sys_fgetxattr;
    table[SYS_LISTXATTR] = sys_listxattr;
    table[SYS_LLISTXATTR] = sys_llistxattr;
    table[SYS_FLISTXATTR] = sys_flistxattr;
    table[SYS_REMOVEXATTR] = sys_removexattr;
    table[SYS_LREMOVEXATTR] = sys_lremovexattr;
    table[SYS_FREMOVEXATTR] = sys_fremovexattr;
    table[SYS_REBOOT] = sys_reboot;
    table[SYS_FSYNC] = sys_fsync;
    table[SYS_TRUNCATE] = sys_truncate;
    table[SYS_TRUNCATE64] = sys_ia32_truncate64;
    table[SYS_FTRUNCATE] = sys_ftruncate;
    table[SYS_FTRUNCATE64] = sys_ia32_ftruncate64;
    table[SYS_MOUNT] = sys_mount;
    table[SYS_UMOUNT2] = sys_umount2;
    table[SYS_FDATASYNC] = sys_fsync;
    table[SYS_LINK] = sys_link;
    table[SYS_LINKAT] = sys_linkat;
    table[SYS_SYMLINK] = sys_symlink;
    table[SYS_SYMLINKAT] = sys_symlinkat;
    /* No 16-bit set-id numbers: their ids and their "keep" differ (sysnr.h). */
    table[SYS_SETREUID32] = sys_setreuid;
    table[SYS_SETREGID32] = sys_setregid;
    table[SYS_SETRESUID32] = sys_setresuid;
    table[SYS_SETRESGID32] = sys_setresgid;
    table[SYS_CHMOD] = sys_chmod;
    table[SYS_FCHMODAT] = sys_fchmodat;
    table[SYS_FCHMOD] = sys_fchmod;
    table[SYS_CHOWN] = sys_chown;
    table[SYS_CHOWN32] = sys_chown;
    table[SYS_LCHOWN] = sys_lchown;
    table[SYS_LCHOWN32] = sys_lchown;
    table[SYS_FCHOWNAT] = sys_fchownat;
    table[SYS_FCHOWN] = sys_fchown;
    table[SYS_FCHOWN32] = sys_fchown;
    table[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    table[SYS_PTRACE] = sys_ptrace;
    table[SYS_PERSONALITY] = sys_personality;
    table[SYS_GETCPU] = sys_getcpu;
    table[SYS_RT_SIGTIMEDWAIT] = sys_rt_sigtimedwait;
    table[SYS_MLOCK] = sys_mlock;
    table[SYS_MUNLOCK] = sys_munlock;
    table[SYS_MLOCKALL] = sys_mlockall;
    table[SYS_MUNLOCKALL] = sys_munlockall;
    table[SYS_MLOCK2] = sys_mlock2;
    table[SYS_MINCORE] = sys_mincore;
    table[SYS_CLOCK_GETTIME64] = sys_clock_gettime64;
    table[SYS_CLOCK_SETTIME64] = sys_clock_settime64;
    table[SYS_CLOCK_GETRES_TIME64] = sys_clock_getres_time64;
    table[SYS_SET_ROBUST_LIST] = sys_set_robust_list;
    table[SYS_GET_ROBUST_LIST] = sys_get_robust_list;
    table[SYS_EXIT_GROUP] = sys_exit_group;
    table[SYS_SOCKET] = sys_socket;
    table[SYS_SOCKETPAIR] = sys_socketpair;
    table[SYS_BIND] = sys_bind;
    table[SYS_CONNECT] = sys_connect;
    table[SYS_LISTEN] = sys_listen;
    table[SYS_ACCEPT4] = sys_accept4;
    table[SYS_GETSOCKOPT] = sys_getsockopt;
    table[SYS_SETSOCKOPT] = sys_setsockopt;
    table[SYS_GETSOCKNAME] = sys_getsockname;
    table[SYS_GETPEERNAME] = sys_getpeername;
    table[SYS_SENDTO] = sys_sendto;
    table[SYS_SENDMSG] = sys_sendmsg;
    table[SYS_RECVFROM] = sys_recvfrom;
    table[SYS_RECVMSG] = sys_recvmsg;
    table[SYS_SHUTDOWN] = sys_shutdown;
    table[SYS_RECVMMSG] = sys_recvmmsg;
    table[SYS_SENDMMSG] = sys_sendmmsg;
    table[SYS_MSYNC] = sys_msync;
    table[SYS_MREMAP] = sys_mremap;
    table[SYS_MADVISE] = sys_madvise;
    table[SYS_EVENTFD] = sys_eventfd;
    table[SYS_EVENTFD2] = sys_eventfd2;
    table[SYS_INOTIFY_INIT] = sys_inotify_init;
    table[SYS_INOTIFY_INIT1] = sys_inotify_init1;
    table[SYS_INOTIFY_ADD_WATCH] = sys_inotify_add_watch;
    table[SYS_INOTIFY_RM_WATCH] = sys_inotify_rm_watch;
    table[SYS_EPOLL_CREATE] = sys_epoll_create;
    table[SYS_EPOLL_CREATE1] = sys_epoll_create1;
    table[SYS_EPOLL_CTL] = sys_epoll_ctl;
    table[SYS_EPOLL_WAIT] = sys_epoll_wait;
    table[SYS_EPOLL_PWAIT] = sys_epoll_pwait;
    table[SYS_EPOLL_PWAIT2] = sys_epoll_pwait2;
    table[SYS_TIMERFD_CREATE] = sys_timerfd_create;
    table[SYS_TIMERFD_SETTIME] = sys_timerfd_settime;
    table[SYS_TIMERFD_GETTIME] = sys_timerfd_gettime;
    table[SYS_TIMERFD_SETTIME64] = sys_timerfd_settime64;
    table[SYS_TIMERFD_GETTIME64] = sys_timerfd_gettime64;
    table[SYS_TIMER_CREATE] = sys_timer_create;
    table[SYS_TIMER_SETTIME] = sys_timer_settime;
    table[SYS_TIMER_GETTIME] = sys_timer_gettime;
    table[SYS_TIMER_GETOVERRUN] = sys_timer_getoverrun;
    table[SYS_TIMER_DELETE] = sys_timer_delete;
    table[SYS_TIMER_SETTIME64] = sys_timer_settime64;
    table[SYS_TIMER_GETTIME64] = sys_timer_gettime64;

    register_isr_handler(SYSCALL_VECTOR, dispatch, "syscall");
}
