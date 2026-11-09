#ifndef _X86_64_IA32_H_
#define _X86_64_IA32_H_

/*
 * The i386 ABI under the x86-64 kernel, the compat layer of a Linux with
 * its 32-bit emulation. A program, 32-bit code or 64-bit, makes an i386
 * system call by `int $0x80` (idt.c), whose frame SC_COMPAT marks; the
 * dispatcher takes the number from EAX and the arguments from EBX, ECX,
 * EDX, ESI, EDI and EBP, zero-extended (cpu.h), and looks the number up in
 * the compat table, which `ia32_syscall_init` fills (ia32_systab.c) with
 * one row for each of Linux 6.12's i386 numbers (ia32_sysnr.h).
 *
 * A row points at the native handler where the call's arguments and
 * structures have one layout in both ABIs, at a compat handler where they
 * differ, or is empty where Linux has no such call or Rach2 has no handler.
 * The compat handlers are below, by the file that owns them; those
 * IA32_STUB defines answer ENOSYS until their translation is written, and
 * mancheck reads the stubs so that `rach2/syscalls.md` lists only the rows
 * that run a call.
 */

#include "../arch.h"

#include "../../lib/kerrno.h"

/* A compat handler not yet written, which answers ENOSYS: one line
 * `IA32_STUB(name)` in its owner's file, with no semicolon. */
#define IA32_STUB(name)             \
    long name(isr_params_t *params) \
    {                               \
        (void)params;               \
        return -ENOSYS;             \
    }

/* The compat handlers, by their owner's file. */

/* ia32_exec.c */
long compat_sys_execve(isr_params_t *params);
long compat_sys_ia32_mmap(isr_params_t *params);
long compat_sys_ia32_clone(isr_params_t *params);
long compat_sys_mmap_pgoff(isr_params_t *params);
long compat_sys_execveat(isr_params_t *params);

/* ldt.c */
long sys_modify_ldt(isr_params_t *params);

/* ia32_signal.c */
long compat_sys_ptrace(isr_params_t *params);
long compat_sys_signal(isr_params_t *params);
long compat_sys_sigaction(isr_params_t *params);
long compat_sys_sgetmask(isr_params_t *params);
long compat_sys_ssetmask(isr_params_t *params);
long compat_sys_sigsuspend(isr_params_t *params);
long compat_sys_sigpending(isr_params_t *params);
long compat_sys_sigreturn(isr_params_t *params);
long compat_sys_sigprocmask(isr_params_t *params);
long compat_sys_rt_sigreturn(isr_params_t *params);
long compat_sys_rt_sigaction(isr_params_t *params);
long compat_sys_rt_sigprocmask(isr_params_t *params);
long compat_sys_rt_sigpending(isr_params_t *params);
long compat_sys_rt_sigtimedwait_time32(isr_params_t *params);
long compat_sys_rt_sigqueueinfo(isr_params_t *params);
long compat_sys_rt_sigsuspend(isr_params_t *params);
long compat_sys_sigaltstack(isr_params_t *params);
long compat_sys_signalfd(isr_params_t *params);
long compat_sys_signalfd4(isr_params_t *params);
long compat_sys_rt_tgsigqueueinfo(isr_params_t *params);
long compat_sys_rt_sigtimedwait_time64(isr_params_t *params);
long compat_sys_pidfd_send_signal(isr_params_t *params);

/* ia32_fs.c */
long compat_sys_stat(isr_params_t *params);
long compat_sys_fstat(isr_params_t *params);
long compat_sys_fcntl64(isr_params_t *params);
long compat_sys_ustat(isr_params_t *params);
long compat_sys_old_select(isr_params_t *params);
long compat_sys_lstat(isr_params_t *params);
long compat_sys_old_readdir(isr_params_t *params);
long compat_sys_statfs(isr_params_t *params);
long compat_sys_fstatfs(isr_params_t *params);
long compat_sys_newstat(isr_params_t *params);
long compat_sys_newlstat(isr_params_t *params);
long compat_sys_newfstat(isr_params_t *params);
long compat_sys_getdents(isr_params_t *params);
long compat_sys_select(isr_params_t *params);
long compat_sys_ia32_pread64(isr_params_t *params);
long compat_sys_ia32_pwrite64(isr_params_t *params);
long compat_sys_ia32_truncate64(isr_params_t *params);
long compat_sys_ia32_ftruncate64(isr_params_t *params);
long compat_sys_ia32_stat64(isr_params_t *params);
long compat_sys_ia32_lstat64(isr_params_t *params);
long compat_sys_ia32_fstat64(isr_params_t *params);
long compat_sys_ia32_readahead(isr_params_t *params);
long compat_sys_io_setup(isr_params_t *params);
long compat_sys_io_getevents_time32(isr_params_t *params);
long compat_sys_io_submit(isr_params_t *params);
long compat_sys_ia32_fadvise64(isr_params_t *params);
long compat_sys_statfs64(isr_params_t *params);
long compat_sys_fstatfs64(isr_params_t *params);
long compat_sys_ia32_fadvise64_64(isr_params_t *params);
long compat_sys_ia32_fstatat64(isr_params_t *params);
long compat_sys_pselect6_time32(isr_params_t *params);
long compat_sys_ppoll_time32(isr_params_t *params);
long compat_sys_ia32_sync_file_range(isr_params_t *params);
long compat_sys_ia32_fallocate(isr_params_t *params);
long compat_sys_preadv(isr_params_t *params);
long compat_sys_pwritev(isr_params_t *params);
long compat_sys_fanotify_mark(isr_params_t *params);
long compat_sys_open_by_handle_at(isr_params_t *params);
long compat_sys_preadv2(isr_params_t *params);
long compat_sys_pwritev2(isr_params_t *params);
long compat_sys_io_pgetevents(isr_params_t *params);
long compat_sys_pselect6_time64(isr_params_t *params);
long compat_sys_ppoll_time64(isr_params_t *params);
long compat_sys_io_pgetevents_time64(isr_params_t *params);
long compat_sys_epoll_pwait2(isr_params_t *params);

/* ia32_time.c */
long compat_sys_time32(isr_params_t *params);
long compat_sys_utime32(isr_params_t *params);
long compat_sys_gettimeofday(isr_params_t *params);
long compat_sys_settimeofday(isr_params_t *params);
long compat_sys_setitimer(isr_params_t *params);
long compat_sys_getitimer(isr_params_t *params);
long compat_sys_nanosleep_time32(isr_params_t *params);
long compat_sys_timer_create(isr_params_t *params);
long compat_sys_timer_settime32(isr_params_t *params);
long compat_sys_timer_gettime32(isr_params_t *params);
long compat_sys_clock_settime32(isr_params_t *params);
long compat_sys_clock_gettime32(isr_params_t *params);
long compat_sys_clock_getres_time32(isr_params_t *params);
long compat_sys_clock_nanosleep_time32(isr_params_t *params);
long compat_sys_utimes_time32(isr_params_t *params);
long compat_sys_futimesat_time32(isr_params_t *params);
long compat_sys_utimensat_time32(isr_params_t *params);
long compat_sys_timerfd_settime32(isr_params_t *params);
long compat_sys_timerfd_gettime32(isr_params_t *params);
long compat_sys_clock_settime64(isr_params_t *params);
long compat_sys_clock_nanosleep_time64(isr_params_t *params);
long compat_sys_timer_settime64(isr_params_t *params);
long compat_sys_timerfd_settime64(isr_params_t *params);

/* ia32_ioctl.c */
long compat_sys_ioctl(isr_params_t *params);

/* ia32_net.c */
long compat_sys_socketcall(isr_params_t *params);
long compat_sys_recvmmsg_time32(isr_params_t *params);
long compat_sys_sendmmsg(isr_params_t *params);
long compat_sys_getsockopt(isr_params_t *params);
long compat_sys_setsockopt(isr_params_t *params);
long compat_sys_sendmsg(isr_params_t *params);
long compat_sys_recvfrom(isr_params_t *params);
long compat_sys_recvmsg(isr_params_t *params);
long compat_sys_recvmmsg_time64(isr_params_t *params);

/* ia32_ipc.c */
long compat_sys_ipc(isr_params_t *params);
long compat_sys_mq_open(isr_params_t *params);
long compat_sys_mq_timedsend_time32(isr_params_t *params);
long compat_sys_mq_timedreceive_time32(isr_params_t *params);
long compat_sys_mq_notify(isr_params_t *params);
long compat_sys_mq_getsetattr(isr_params_t *params);
long compat_sys_semctl(isr_params_t *params);
long compat_sys_shmctl(isr_params_t *params);
long compat_sys_shmat(isr_params_t *params);
long compat_sys_msgsnd(isr_params_t *params);
long compat_sys_msgrcv(isr_params_t *params);
long compat_sys_msgctl(isr_params_t *params);

/* ia32_misc.c */
long compat_sys_lchown16(isr_params_t *params);
long compat_sys_oldumount(isr_params_t *params);
long compat_sys_setuid16(isr_params_t *params);
long compat_sys_getuid16(isr_params_t *params);
long compat_sys_nice(isr_params_t *params);
long compat_sys_times(isr_params_t *params);
long compat_sys_setgid16(isr_params_t *params);
long compat_sys_getgid16(isr_params_t *params);
long compat_sys_geteuid16(isr_params_t *params);
long compat_sys_getegid16(isr_params_t *params);
long compat_sys_olduname(isr_params_t *params);
long compat_sys_setreuid16(isr_params_t *params);
long compat_sys_setregid16(isr_params_t *params);
long compat_sys_setrlimit(isr_params_t *params);
long compat_sys_old_getrlimit(isr_params_t *params);
long compat_sys_getrusage(isr_params_t *params);
long compat_sys_getgroups16(isr_params_t *params);
long compat_sys_setgroups16(isr_params_t *params);
long compat_sys_fchown16(isr_params_t *params);
long compat_sys_uname(isr_params_t *params);
long compat_sys_wait4(isr_params_t *params);
long compat_sys_sysinfo(isr_params_t *params);
long compat_sys_setfsuid16(isr_params_t *params);
long compat_sys_setfsgid16(isr_params_t *params);
long compat_sys_sched_rr_get_interval_time32(isr_params_t *params);
long compat_sys_setresuid16(isr_params_t *params);
long compat_sys_getresuid16(isr_params_t *params);
long compat_sys_setresgid16(isr_params_t *params);
long compat_sys_getresgid16(isr_params_t *params);
long compat_sys_chown16(isr_params_t *params);
long compat_sys_getrlimit(isr_params_t *params);
long compat_sys_futex_time32(isr_params_t *params);
long compat_sys_sched_setaffinity(isr_params_t *params);
long compat_sys_sched_getaffinity(isr_params_t *params);
long compat_sys_kexec_load(isr_params_t *params);
long compat_sys_waitid(isr_params_t *params);
long compat_sys_keyctl(isr_params_t *params);
long compat_sys_set_robust_list(isr_params_t *params);
long compat_sys_get_robust_list(isr_params_t *params);
long compat_sys_arch_prctl(isr_params_t *params);
/* The parts of the i386 ABI that are not rows of the table. */

/**
 * Run the x86-64 handler `fn` for the i386 call of `params`, with the
 * arguments `a0` to `a5` as whole 64-bit registers in a copy of the frame
 * that is not marked SC_COMPAT, and with the `winlen` bytes of kernel memory
 * at `win`, when it is not NULL, open to its user-copy checks for the call
 * (`user_window_open`), so that a structure converted there may be passed by
 * its address. Returns the handler's result (ia32_fs.c).
 */
long ia32_native(const isr_params_t *params, syscall_fn_t fn, void *win, uint32_t winlen,
                 unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                 unsigned long a4, unsigned long a5);

/** Read the i386 timespec, or the 64-bit one of the _time64 calls with the
 * high word of its nanoseconds dropped, at `u` into the seconds and
 * nanoseconds `ts`; 0 or -EFAULT (ia32_fs.c). */
int ia32_get_timespec(uintptr_t u, long ts[2]);
int ia32_get_timespec64(uintptr_t u, long ts[2]);

/** The ioctl translations of the DRM core and of the GPU drivers, for the
 * compat ioctl path: each answers the call `cmd` on `fd` with `arg`, or
 * -ENOTTY where it has no translation (ia32_ioctl_drm.c, ia32_ioctl_gpu.c). */
long ia32_ioctl_drm(unsigned int fd, unsigned int cmd, unsigned long arg);
long ia32_ioctl_gpu(unsigned int fd, unsigned int cmd, unsigned long arg);

/** Build the signal frame of a handler whose action an i386 call installed
 * (SA_IA32_ABI), as `sigframe_setup` builds a 64-bit one: the rt frame with
 * SA_SIGINFO, else the plain one. Returns 0, or -1, with `tf` unchanged, and
 * the thread takes SIGSEGV (sigframe32.c). */
struct k_sigaction;
struct k_siginfo;
int ia32_sigframe_setup(isr_params_t *tf, int signo, const struct k_sigaction *sa,
                        const struct k_siginfo *info, uint64_t oldmask);

/** The address of the 32-bit vDSO's __kernel_rt_sigreturn when `rt` is
 * set, else of __kernel_sigreturn, in the calling thread's address space,
 * the restorer an i386 signal frame returns to without SA_RESTORER, as
 * Linux's `ia32_setup_frame` finds it; 0 where the image is not an i386
 * program's or has no vDSO (vdso32.c). */
uintptr_t ia32_vdso_sigreturn(int rt);

/** Restore the state an i386 frame saved, for the sigreturn (without `rt`)
 * or rt_sigreturn an i386 call makes, as `sigframe_restore` describes it:
 * 0, or -1 for a frame that cannot be read or would load a faulting state
 * (sigframe32.c). */
int ia32_sigframe_restore(isr_params_t *tf, int rt, uint64_t *mask);

/** The most bytes `ia32_sigframe_setup` puts below the top of an
 * alternate stack (sigframe32.c). */
uintptr_t ia32_sigframe_bytes(void);

/** Whether `sel` names a segment an `iretq` to user mode may load into SS
 * (sigframe.c). */
int sigframe_ss_valid(unsigned int sel);

#endif /* _X86_64_IA32_H_ */
