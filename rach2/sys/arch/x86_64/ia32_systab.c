/*
 * ia32_systab.c: the compat table, from Linux 6.12's i386 numbers
 * (ia32_sysnr.h) to the handlers of the calls a program makes by `int $0x80` on
 * the x86-64 kernel, as ia32.h describes it.  Every row of Linux's
 * arch/x86/entry/syscalls/syscall_32.tbl has a line here, in number order: the
 * native handler where the arguments and structures are the same in both ABIs,
 * which on Linux is a row with no compat entry; a compat handler of ia32.h
 * where they differ, which may still answer ENOSYS; or NULL where Linux's row
 * has no entry, as for break or vm86, or Rach2's x86-64 kernel has no handler
 * for the call. Linux leaves 222, 223, 251, 285, 387 to 392 and 415 without a
 * row, and so does this table.
 */

#include "ia32.h"
#include "ia32_sysnr.h"
#include "tls.h"

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
#include "../../proc/kcmp.h"
#include "../../proc/rseq.h"
#include "../../proc/process_vm.h"
#include "../../mm/mempolicy.h"
#include "../../proc/pidfd.h"
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

/** The rows, one for each i386 number Linux has; `init_syscall` cleared
 * the table first, so a NULL row only says so. */
void ia32_syscall_init(syscall_fn_t *table)
{
    table[SYS_RESTART_SYSCALL] = NULL;
    table[SYS_EXIT] = sys_exit;
    table[SYS_FORK] = sys_fork;
    table[SYS_READ] = sys_read;
    table[SYS_WRITE] = sys_write;
    table[SYS_OPEN] = sys_open;
    table[SYS_CLOSE] = sys_close;
    table[SYS_WAITPID] = sys_waitpid;
    table[SYS_CREAT] = sys_creat;
    table[SYS_LINK] = sys_link;
    table[SYS_UNLINK] = sys_unlink;
    table[SYS_EXECVE] = compat_sys_execve;
    table[SYS_CHDIR] = sys_chdir;
    table[SYS_TIME] = compat_sys_time32;
    table[SYS_MKNOD] = sys_mknod;
    table[SYS_CHMOD] = sys_chmod;
    table[SYS_LCHOWN] = compat_sys_lchown16;
    table[SYS_BREAK] = NULL;
    table[SYS_OLDSTAT] = compat_sys_stat;
    table[SYS_LSEEK] = sys_lseek;
    table[SYS_GETPID] = sys_getpid;
    table[SYS_MOUNT] = sys_mount;
    table[SYS_UMOUNT] = compat_sys_oldumount;
    table[SYS_SETUID] = compat_sys_setuid16;
    table[SYS_GETUID] = compat_sys_getuid16;
    table[SYS_STIME] = sys_stime;
    table[SYS_PTRACE] = compat_sys_ptrace;
    table[SYS_ALARM] = sys_alarm;
    table[SYS_OLDFSTAT] = compat_sys_fstat;
    table[SYS_PAUSE] = sys_pause;
    table[SYS_UTIME] = compat_sys_utime32;
    table[SYS_STTY] = NULL;
    table[SYS_GTTY] = NULL;
    table[SYS_ACCESS] = sys_access;
    table[SYS_NICE] = compat_sys_nice;
    table[SYS_FTIME] = NULL;
    table[SYS_SYNC] = sys_sync;
    table[SYS_KILL] = sys_kill;
    table[SYS_RENAME] = sys_rename;
    table[SYS_MKDIR] = sys_mkdir;
    table[SYS_RMDIR] = sys_rmdir;
    table[SYS_DUP] = sys_dup;
    table[SYS_PIPE] = sys_pipe;
    table[SYS_TIMES] = compat_sys_times;
    table[SYS_PROF] = NULL;
    table[SYS_BRK] = sys_brk;
    table[SYS_SETGID] = compat_sys_setgid16;
    table[SYS_GETGID] = compat_sys_getgid16;
    table[SYS_SIGNAL] = compat_sys_signal;
    table[SYS_GETEUID] = compat_sys_geteuid16;
    table[SYS_GETEGID] = compat_sys_getegid16;
    table[SYS_ACCT] = NULL;
    table[SYS_UMOUNT2] = sys_umount2;
    table[SYS_LOCK] = NULL;
    table[SYS_IOCTL] = compat_sys_ioctl;
    table[SYS_FCNTL] = compat_sys_fcntl64;
    table[SYS_MPX] = NULL;
    table[SYS_SETPGID] = sys_setpgid;
    table[SYS_ULIMIT] = NULL;
    table[SYS_OLDOLDUNAME] = compat_sys_olduname;
    table[SYS_UMASK] = sys_umask;
    table[SYS_CHROOT] = sys_chroot;
    table[SYS_USTAT] = compat_sys_ustat;
    table[SYS_DUP2] = sys_dup2;
    table[SYS_GETPPID] = sys_getppid;
    table[SYS_GETPGRP] = sys_getpgrp;
    table[SYS_SETSID] = sys_setsid;
    table[SYS_SIGACTION] = compat_sys_sigaction;
    table[SYS_SGETMASK] = compat_sys_sgetmask;
    table[SYS_SSETMASK] = compat_sys_ssetmask;
    table[SYS_SETREUID] = compat_sys_setreuid16;
    table[SYS_SETREGID] = compat_sys_setregid16;
    table[SYS_SIGSUSPEND] = compat_sys_sigsuspend;
    table[SYS_SIGPENDING] = compat_sys_sigpending;
    table[SYS_SETHOSTNAME] = sys_sethostname;
    table[SYS_SETRLIMIT] = compat_sys_setrlimit;
    table[SYS_GETRLIMIT] = compat_sys_old_getrlimit;
    table[SYS_GETRUSAGE] = compat_sys_getrusage;
    table[SYS_GETTIMEOFDAY] = compat_sys_gettimeofday;
    table[SYS_SETTIMEOFDAY] = compat_sys_settimeofday;
    table[SYS_GETGROUPS] = compat_sys_getgroups16;
    table[SYS_SETGROUPS] = compat_sys_setgroups16;
    table[SYS_SELECT] = compat_sys_old_select;
    table[SYS_SYMLINK] = sys_symlink;
    table[SYS_OLDLSTAT] = compat_sys_lstat;
    table[SYS_READLINK] = sys_readlink;
    table[SYS_USELIB] = NULL;
    table[SYS_SWAPON] = NULL;
    table[SYS_REBOOT] = sys_reboot;
    table[SYS_READDIR] = compat_sys_old_readdir;
    table[SYS_MMAP] = compat_sys_ia32_mmap;
    table[SYS_MUNMAP] = sys_munmap;
    table[SYS_TRUNCATE] = sys_truncate;
    table[SYS_FTRUNCATE] = sys_ftruncate;
    table[SYS_FCHMOD] = sys_fchmod;
    table[SYS_FCHOWN] = compat_sys_fchown16;
    table[SYS_GETPRIORITY] = sys_getpriority;
    table[SYS_SETPRIORITY] = sys_setpriority;
    table[SYS_PROFIL] = NULL;
    table[SYS_STATFS] = compat_sys_statfs;
    table[SYS_FSTATFS] = compat_sys_fstatfs;
    table[SYS_IOPERM] = NULL;
    table[SYS_SOCKETCALL] = compat_sys_socketcall;
    table[SYS_SYSLOG] = sys_syslog;
    table[SYS_SETITIMER] = compat_sys_setitimer;
    table[SYS_GETITIMER] = compat_sys_getitimer;
    table[SYS_STAT] = compat_sys_newstat;
    table[SYS_LSTAT] = compat_sys_newlstat;
    table[SYS_FSTAT] = compat_sys_newfstat;
    table[SYS_OLDUNAME] = compat_sys_uname;
    table[SYS_IOPL] = NULL;
    table[SYS_VHANGUP] = NULL;
    table[SYS_IDLE] = NULL;
    table[SYS_VM86OLD] = NULL;
    table[SYS_WAIT4] = compat_sys_wait4;
    table[SYS_SWAPOFF] = NULL;
    table[SYS_SYSINFO] = compat_sys_sysinfo;
    table[SYS_IPC] = compat_sys_ipc;
    table[SYS_FSYNC] = sys_fsync;
    table[SYS_SIGRETURN] = compat_sys_sigreturn;
    table[SYS_CLONE] = compat_sys_ia32_clone;
    table[SYS_SETDOMAINNAME] = sys_setdomainname;
    table[SYS_UNAME] = sys_uname;
    table[SYS_MODIFY_LDT] = sys_modify_ldt;
    table[SYS_ADJTIMEX] = sys_adjtimex;
    table[SYS_MPROTECT] = sys_mprotect;
    table[SYS_SIGPROCMASK] = compat_sys_sigprocmask;
    table[SYS_CREATE_MODULE] = NULL;
    table[SYS_INIT_MODULE] = NULL;
    table[SYS_DELETE_MODULE] = NULL;
    table[SYS_GET_KERNEL_SYMS] = NULL;
    table[SYS_QUOTACTL] = NULL;
    table[SYS_GETPGID] = sys_getpgid;
    table[SYS_FCHDIR] = sys_fchdir;
    table[SYS_BDFLUSH] = NULL;
    table[SYS_SYSFS] = NULL;
    table[SYS_PERSONALITY] = sys_personality;
    table[SYS_AFS_SYSCALL] = NULL;
    table[SYS_SETFSUID] = compat_sys_setfsuid16;
    table[SYS_SETFSGID] = compat_sys_setfsgid16;
    table[SYS_LLSEEK] = sys__llseek;
    table[SYS_GETDENTS] = compat_sys_getdents;
    table[SYS_NEWSELECT] = compat_sys_select;
    table[SYS_FLOCK] = sys_flock;
    table[SYS_MSYNC] = sys_msync;
    table[SYS_READV] = sys_readv;
    table[SYS_WRITEV] = sys_writev;
    table[SYS_GETSID] = sys_getsid;
    table[SYS_FDATASYNC] = sys_fdatasync;
    table[SYS_SYSCTL] = NULL;
    table[SYS_MLOCK] = sys_mlock;
    table[SYS_MUNLOCK] = sys_munlock;
    table[SYS_MLOCKALL] = sys_mlockall;
    table[SYS_MUNLOCKALL] = sys_munlockall;
    table[SYS_SCHED_SETPARAM] = sys_sched_setparam;
    table[SYS_SCHED_GETPARAM] = sys_sched_getparam;
    table[SYS_SCHED_SETSCHEDULER] = sys_sched_setscheduler;
    table[SYS_SCHED_GETSCHEDULER] = sys_sched_getscheduler;
    table[SYS_SCHED_YIELD] = sys_sched_yield;
    table[SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max;
    table[SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min;
    table[SYS_SCHED_RR_GET_INTERVAL] = compat_sys_sched_rr_get_interval_time32;
    table[SYS_NANOSLEEP] = compat_sys_nanosleep_time32;
    table[SYS_MREMAP] = sys_mremap;
    table[SYS_SETRESUID] = compat_sys_setresuid16;
    table[SYS_GETRESUID] = compat_sys_getresuid16;
    table[SYS_VM86] = NULL;
    table[SYS_QUERY_MODULE] = NULL;
    table[SYS_POLL] = sys_poll;
    table[SYS_NFSSERVCTL] = NULL;
    table[SYS_SETRESGID] = compat_sys_setresgid16;
    table[SYS_GETRESGID] = compat_sys_getresgid16;
    table[SYS_PRCTL] = sys_prctl;
    table[SYS_RT_SIGRETURN] = compat_sys_rt_sigreturn;
    table[SYS_RT_SIGACTION] = compat_sys_rt_sigaction;
    table[SYS_RT_SIGPROCMASK] = compat_sys_rt_sigprocmask;
    table[SYS_RT_SIGPENDING] = compat_sys_rt_sigpending;
    table[SYS_RT_SIGTIMEDWAIT] = compat_sys_rt_sigtimedwait_time32;
    table[SYS_RT_SIGQUEUEINFO] = compat_sys_rt_sigqueueinfo;
    table[SYS_RT_SIGSUSPEND] = compat_sys_rt_sigsuspend;
    table[SYS_PREAD64] = compat_sys_ia32_pread64;
    table[SYS_PWRITE64] = compat_sys_ia32_pwrite64;
    table[SYS_CHOWN] = compat_sys_chown16;
    table[SYS_GETCWD] = sys_getcwd;
    table[SYS_CAPGET] = sys_capget;
    table[SYS_CAPSET] = sys_capset;
    table[SYS_SIGALTSTACK] = compat_sys_sigaltstack;
    table[SYS_SENDFILE] = sys_sendfile32;
    table[SYS_GETPMSG] = NULL;
    table[SYS_PUTPMSG] = NULL;
    table[SYS_VFORK] = sys_vfork;
    table[SYS_UGETRLIMIT] = compat_sys_getrlimit;
    table[SYS_MMAP2] = compat_sys_mmap_pgoff;
    table[SYS_TRUNCATE64] = compat_sys_ia32_truncate64;
    table[SYS_FTRUNCATE64] = compat_sys_ia32_ftruncate64;
    table[SYS_STAT64] = compat_sys_ia32_stat64;
    table[SYS_LSTAT64] = compat_sys_ia32_lstat64;
    table[SYS_FSTAT64] = compat_sys_ia32_fstat64;
    table[SYS_LCHOWN32] = sys_lchown;
    table[SYS_GETUID32] = sys_getuid;
    table[SYS_GETGID32] = sys_getgid;
    table[SYS_GETEUID32] = sys_geteuid;
    table[SYS_GETEGID32] = sys_getegid;
    table[SYS_SETREUID32] = sys_setreuid;
    table[SYS_SETREGID32] = sys_setregid;
    table[SYS_GETGROUPS32] = sys_getgroups;
    table[SYS_SETGROUPS32] = sys_setgroups;
    table[SYS_FCHOWN32] = sys_fchown;
    table[SYS_SETRESUID32] = sys_setresuid;
    table[SYS_GETRESUID32] = sys_getresuid;
    table[SYS_SETRESGID32] = sys_setresgid;
    table[SYS_GETRESGID32] = sys_getresgid;
    table[SYS_CHOWN32] = sys_chown;
    table[SYS_SETUID32] = sys_setuid;
    table[SYS_SETGID32] = sys_setgid;
    table[SYS_SETFSUID32] = NULL;
    table[SYS_SETFSGID32] = NULL;
    table[SYS_PIVOT_ROOT] = sys_pivot_root;
    table[SYS_MINCORE] = sys_mincore;
    table[SYS_MADVISE] = sys_madvise;
    table[SYS_GETDENTS64] = sys_getdents64;
    table[SYS_FCNTL64] = compat_sys_fcntl64;
    table[SYS_GETTID] = sys_gettid;
    table[SYS_READAHEAD] = compat_sys_ia32_readahead;
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
    table[SYS_TKILL] = sys_tkill;
    table[SYS_SENDFILE64] = sys_sendfile;
    table[SYS_FUTEX] = compat_sys_futex_time32;
    table[SYS_SCHED_SETAFFINITY] = compat_sys_sched_setaffinity;
    table[SYS_SCHED_GETAFFINITY] = compat_sys_sched_getaffinity;
    table[SYS_SET_THREAD_AREA] = sys_set_thread_area;
    table[SYS_GET_THREAD_AREA] = sys_get_thread_area;
    table[SYS_IO_SETUP] = compat_sys_io_setup;
    table[SYS_IO_DESTROY] = NULL;
    table[SYS_IO_GETEVENTS] = compat_sys_io_getevents_time32;
    table[SYS_IO_SUBMIT] = compat_sys_io_submit;
    table[SYS_IO_CANCEL] = NULL;
    table[SYS_FADVISE64] = compat_sys_ia32_fadvise64;
    table[SYS_EXIT_GROUP] = sys_exit_group;
    table[SYS_LOOKUP_DCOOKIE] = NULL;
    table[SYS_EPOLL_CREATE] = sys_epoll_create;
    table[SYS_EPOLL_CTL] = sys_epoll_ctl;
    table[SYS_EPOLL_WAIT] = sys_epoll_wait;
    table[SYS_REMAP_FILE_PAGES] = NULL;
    table[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    table[SYS_TIMER_CREATE] = compat_sys_timer_create;
    table[SYS_TIMER_SETTIME] = compat_sys_timer_settime32;
    table[SYS_TIMER_GETTIME] = compat_sys_timer_gettime32;
    table[SYS_TIMER_GETOVERRUN] = sys_timer_getoverrun;
    table[SYS_TIMER_DELETE] = sys_timer_delete;
    table[SYS_CLOCK_SETTIME] = compat_sys_clock_settime32;
    table[SYS_CLOCK_GETTIME] = compat_sys_clock_gettime32;
    table[SYS_CLOCK_GETRES] = compat_sys_clock_getres_time32;
    table[SYS_CLOCK_NANOSLEEP] = compat_sys_clock_nanosleep_time32;
    table[SYS_STATFS64] = compat_sys_statfs64;
    table[SYS_FSTATFS64] = compat_sys_fstatfs64;
    table[SYS_TGKILL] = sys_tgkill;
    table[SYS_UTIMES] = compat_sys_utimes_time32;
    table[SYS_FADVISE64_64] = compat_sys_ia32_fadvise64_64;
    table[SYS_VSERVER] = NULL;
    table[SYS_MBIND] = NULL;
    table[SYS_GET_MEMPOLICY] = sys_get_mempolicy;
    table[SYS_SET_MEMPOLICY] = NULL;
    table[SYS_MQ_OPEN] = compat_sys_mq_open;
    table[SYS_MQ_UNLINK] = sys_mq_unlink;
    table[SYS_MQ_TIMEDSEND] = compat_sys_mq_timedsend_time32;
    table[SYS_MQ_TIMEDRECEIVE] = compat_sys_mq_timedreceive_time32;
    table[SYS_MQ_NOTIFY] = compat_sys_mq_notify;
    table[SYS_MQ_GETSETATTR] = compat_sys_mq_getsetattr;
    table[SYS_KEXEC_LOAD] = compat_sys_kexec_load;
    table[SYS_WAITID] = compat_sys_waitid;
    table[SYS_ADD_KEY] = NULL;
    table[SYS_REQUEST_KEY] = NULL;
    table[SYS_KEYCTL] = compat_sys_keyctl;
    table[SYS_IOPRIO_SET] = sys_ioprio_set;
    table[SYS_IOPRIO_GET] = sys_ioprio_get;
    table[SYS_INOTIFY_INIT] = sys_inotify_init;
    table[SYS_INOTIFY_ADD_WATCH] = sys_inotify_add_watch;
    table[SYS_INOTIFY_RM_WATCH] = sys_inotify_rm_watch;
    table[SYS_MIGRATE_PAGES] = NULL;
    table[SYS_OPENAT] = sys_openat;
    table[SYS_MKDIRAT] = sys_mkdirat;
    table[SYS_MKNODAT] = sys_mknodat;
    table[SYS_FCHOWNAT] = sys_fchownat;
    table[SYS_FUTIMESAT] = compat_sys_futimesat_time32;
    table[SYS_FSTATAT64] = compat_sys_ia32_fstatat64;
    table[SYS_UNLINKAT] = sys_unlinkat;
    table[SYS_RENAMEAT] = sys_renameat;
    table[SYS_LINKAT] = sys_linkat;
    table[SYS_SYMLINKAT] = sys_symlinkat;
    table[SYS_READLINKAT] = sys_readlinkat;
    table[SYS_FCHMODAT] = sys_fchmodat;
    table[SYS_FACCESSAT] = sys_faccessat;
    table[SYS_PSELECT6] = compat_sys_pselect6_time32;
    table[SYS_PPOLL] = compat_sys_ppoll_time32;
    table[SYS_UNSHARE] = sys_unshare;
    table[SYS_SET_ROBUST_LIST] = compat_sys_set_robust_list;
    table[SYS_GET_ROBUST_LIST] = compat_sys_get_robust_list;
    table[SYS_SPLICE] = sys_splice;
    table[SYS_SYNC_FILE_RANGE] = compat_sys_ia32_sync_file_range;
    table[SYS_TEE] = sys_tee;
    table[SYS_VMSPLICE] = sys_vmsplice;
    table[SYS_MOVE_PAGES] = NULL;
    table[SYS_GETCPU] = sys_getcpu;
    table[SYS_EPOLL_PWAIT] = sys_epoll_pwait;
    table[SYS_UTIMENSAT] = compat_sys_utimensat_time32;
    table[SYS_SIGNALFD] = compat_sys_signalfd;
    table[SYS_TIMERFD_CREATE] = sys_timerfd_create;
    table[SYS_EVENTFD] = sys_eventfd;
    table[SYS_FALLOCATE] = compat_sys_ia32_fallocate;
    table[SYS_TIMERFD_SETTIME] = compat_sys_timerfd_settime32;
    table[SYS_TIMERFD_GETTIME] = compat_sys_timerfd_gettime32;
    table[SYS_SIGNALFD4] = compat_sys_signalfd4;
    table[SYS_EVENTFD2] = sys_eventfd2;
    table[SYS_EPOLL_CREATE1] = sys_epoll_create1;
    table[SYS_DUP3] = sys_dup3;
    table[SYS_PIPE2] = sys_pipe2;
    table[SYS_INOTIFY_INIT1] = sys_inotify_init1;
    table[SYS_PREADV] = compat_sys_preadv;
    table[SYS_PWRITEV] = compat_sys_pwritev;
    table[SYS_RT_TGSIGQUEUEINFO] = compat_sys_rt_tgsigqueueinfo;
    table[SYS_PERF_EVENT_OPEN] = NULL;
    table[SYS_RECVMMSG] = compat_sys_recvmmsg_time32;
    table[SYS_FANOTIFY_INIT] = NULL;
    table[SYS_FANOTIFY_MARK] = compat_sys_fanotify_mark;
    table[SYS_PRLIMIT64] = sys_prlimit64;
    table[SYS_NAME_TO_HANDLE_AT] = NULL;
    table[SYS_OPEN_BY_HANDLE_AT] = compat_sys_open_by_handle_at;
    table[SYS_CLOCK_ADJTIME] = sys_clock_adjtime;
    table[SYS_SYNCFS] = sys_syncfs;
    table[SYS_SENDMMSG] = compat_sys_sendmmsg;
    table[SYS_SETNS] = sys_setns;
    table[SYS_PROCESS_VM_READV] = sys_process_vm_readv;
    table[SYS_PROCESS_VM_WRITEV] = sys_process_vm_writev;
    table[SYS_KCMP] = sys_kcmp;
    table[SYS_FINIT_MODULE] = NULL;
    table[SYS_SCHED_SETATTR] = NULL;
    table[SYS_SCHED_GETATTR] = NULL;
    table[SYS_RENAMEAT2] = sys_renameat2;
    table[SYS_SECCOMP] = sys_seccomp;
    table[SYS_GETRANDOM] = sys_getrandom;
    table[SYS_MEMFD_CREATE] = sys_memfd_create;
    table[SYS_BPF] = NULL;
    table[SYS_EXECVEAT] = compat_sys_execveat;
    table[SYS_SOCKET] = sys_socket;
    table[SYS_SOCKETPAIR] = sys_socketpair;
    table[SYS_BIND] = sys_bind;
    table[SYS_CONNECT] = sys_connect;
    table[SYS_LISTEN] = sys_listen;
    table[SYS_ACCEPT4] = sys_accept4;
    table[SYS_GETSOCKOPT] = compat_sys_getsockopt;
    table[SYS_SETSOCKOPT] = compat_sys_setsockopt;
    table[SYS_GETSOCKNAME] = sys_getsockname;
    table[SYS_GETPEERNAME] = sys_getpeername;
    table[SYS_SENDTO] = sys_sendto;
    table[SYS_SENDMSG] = compat_sys_sendmsg;
    table[SYS_RECVFROM] = compat_sys_recvfrom;
    table[SYS_RECVMSG] = compat_sys_recvmsg;
    table[SYS_SHUTDOWN] = sys_shutdown;
    table[SYS_USERFAULTFD] = NULL;
    table[SYS_MEMBARRIER] = NULL;
    table[SYS_MLOCK2] = sys_mlock2;
    table[SYS_COPY_FILE_RANGE] = sys_copy_file_range;
    table[SYS_PREADV2] = compat_sys_preadv2;
    table[SYS_PWRITEV2] = compat_sys_pwritev2;
    table[SYS_PKEY_MPROTECT] = NULL;
    table[SYS_PKEY_ALLOC] = NULL;
    table[SYS_PKEY_FREE] = NULL;
    table[SYS_STATX] = sys_statx;
    table[SYS_ARCH_PRCTL] = compat_sys_arch_prctl;
    table[SYS_IO_PGETEVENTS] = compat_sys_io_pgetevents;
    table[SYS_RSEQ] = sys_rseq;
    table[SYS_SEMGET] = sys_semget;
    table[SYS_SEMCTL] = compat_sys_semctl;
    table[SYS_SHMGET] = sys_shmget;
    table[SYS_SHMCTL] = compat_sys_shmctl;
    table[SYS_SHMAT] = compat_sys_shmat;
    table[SYS_SHMDT] = sys_shmdt;
    table[SYS_MSGGET] = sys_msgget;
    table[SYS_MSGSND] = compat_sys_msgsnd;
    table[SYS_MSGRCV] = compat_sys_msgrcv;
    table[SYS_MSGCTL] = compat_sys_msgctl;
    table[SYS_CLOCK_GETTIME64] = sys_clock_gettime;
    table[SYS_CLOCK_SETTIME64] = compat_sys_clock_settime64;
    table[SYS_CLOCK_ADJTIME64] = sys_clock_adjtime64;
    table[SYS_CLOCK_GETRES_TIME64] = sys_clock_getres;
    table[SYS_CLOCK_NANOSLEEP_TIME64] = compat_sys_clock_nanosleep_time64;
    table[SYS_TIMER_GETTIME64] = sys_timer_gettime;
    table[SYS_TIMER_SETTIME64] = compat_sys_timer_settime64;
    table[SYS_TIMERFD_GETTIME64] = sys_timerfd_gettime;
    table[SYS_TIMERFD_SETTIME64] = compat_sys_timerfd_settime64;
    table[SYS_UTIMENSAT_TIME64] = sys_utimensat_time64;
    table[SYS_PSELECT6_TIME64] = compat_sys_pselect6_time64;
    table[SYS_PPOLL_TIME64] = compat_sys_ppoll_time64;
    table[SYS_IO_PGETEVENTS_TIME64] = compat_sys_io_pgetevents_time64;
    table[SYS_RECVMMSG_TIME64] = compat_sys_recvmmsg_time64;
    table[SYS_MQ_TIMEDSEND_TIME64] = sys_mq_timedsend;
    table[SYS_MQ_TIMEDRECEIVE_TIME64] = sys_mq_timedreceive;
    table[SYS_SEMTIMEDOP_TIME64] = sys_semtimedop_time64;
    table[SYS_RT_SIGTIMEDWAIT_TIME64] = compat_sys_rt_sigtimedwait_time64;
    table[SYS_FUTEX_TIME64] = sys_futex;
    table[SYS_SCHED_RR_GET_INTERVAL_TIME64] = sys_sched_rr_get_interval;
    table[SYS_PIDFD_SEND_SIGNAL] = compat_sys_pidfd_send_signal;
    table[SYS_IO_URING_SETUP] = NULL;
    table[SYS_IO_URING_ENTER] = NULL;
    table[SYS_IO_URING_REGISTER] = NULL;
    table[SYS_OPEN_TREE] = NULL;
    table[SYS_MOVE_MOUNT] = NULL;
    table[SYS_FSOPEN] = NULL;
    table[SYS_FSCONFIG] = NULL;
    table[SYS_FSMOUNT] = NULL;
    table[SYS_FSPICK] = NULL;
    table[SYS_PIDFD_OPEN] = sys_pidfd_open;
    table[SYS_CLONE3] = sys_clone3;
    table[SYS_CLOSE_RANGE] = sys_close_range;
    table[SYS_OPENAT2] = NULL;
    table[SYS_PIDFD_GETFD] = sys_pidfd_getfd;
    table[SYS_FACCESSAT2] = sys_faccessat2;
    table[SYS_PROCESS_MADVISE] = NULL;
    table[SYS_EPOLL_PWAIT2] = compat_sys_epoll_pwait2;
    table[SYS_MOUNT_SETATTR] = NULL;
    table[SYS_QUOTACTL_FD] = NULL;
    table[SYS_LANDLOCK_CREATE_RULESET] = NULL;
    table[SYS_LANDLOCK_ADD_RULE] = NULL;
    table[SYS_LANDLOCK_RESTRICT_SELF] = NULL;
    table[SYS_MEMFD_SECRET] = NULL;
    table[SYS_PROCESS_MRELEASE] = NULL;
    table[SYS_FUTEX_WAITV] = sys_futex_waitv;
    table[SYS_SET_MEMPOLICY_HOME_NODE] = NULL;
    table[SYS_CACHESTAT] = NULL;
    table[SYS_FCHMODAT2] = NULL;
    table[SYS_MAP_SHADOW_STACK] = NULL;
    table[SYS_FUTEX_WAKE] = NULL;
    table[SYS_FUTEX_WAIT] = NULL;
    table[SYS_FUTEX_REQUEUE] = NULL;
    table[SYS_STATMOUNT] = NULL;
    table[SYS_LISTMOUNT] = NULL;
    table[SYS_LSM_GET_SELF_ATTR] = NULL;
    table[SYS_LSM_SET_SELF_ATTR] = NULL;
    table[SYS_LSM_LIST_MODULES] = NULL;
    table[SYS_MSEAL] = NULL;
}
