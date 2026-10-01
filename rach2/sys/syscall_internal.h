#ifndef _SYSCALL_INTERNAL_H_
#define _SYSCALL_INTERNAL_H_

/*
 * What the four parts of the system call layer share: the helpers one part
 * defines and another calls, the constants and types two parts read, and
 * every handler, which syscall.c registers in its table. Included after the
 * kernel headers, whose types it names. The layer was one file until
 * milestone 53 divided it along its arms; the bodies did not change.
 */

/* The highest user page, the top page of a new image's stack. execve lays
   the strings down from the page's end (`proc/uimage.c`), taking as many
   pages below it as they need. */
#define USER_STACK_BASE 0xBFFFF000

extern volatile uint32_t fb_owner_pid;

void fd_release_backing(fd_entry_t *e);

/** The root's namespace lock, taken for the release of `e` when it may free
 * a nameless node of the KRFFS root, or NULL (syscall_fs.c). A holder in a
 * list of its own takes it before it leaves the list. May sleep. */
struct vfs_mount *fd_release_lock(const fd_entry_t *e);

/** At the end of a system call: let go of the descriptors the call held in
 * its shared table, and release those another sharer closed meanwhile that
 * no other call holds. May sleep. */
void fd_holds_release(void);

void init_kstacks(void);

extern volatile process_t *stdin_waiting_proc;

int32_t sys_exit(isr_params_t *params);

int32_t sys_ftruncate(isr_params_t *params);

int32_t sys_ftruncate64(isr_params_t *params);

int32_t sys_write(isr_params_t *params);

/*
 * Gather write (writev(2)). musl's buffered stdio (printf) emits its output
 * through this call. EBX = fd, ECX = struct iovec[], EDX = iovcnt. Returns the
 * total bytes written across the iovecs, or a negative errno.
 */
struct kernel_iovec {
    uint32_t iov_base;
    uint32_t iov_len;
};

/* Format: the vectors readv, writev and sendmsg accept, the Linux IOV_MAX. */
#define IOV_MAX_LOCAL 1024

int32_t sys_writev(isr_params_t *params);

int canonicalize_abs(char *path);

int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath, int *end);

int canonicalize_named(char *path, int *end);

int32_t resolve_named(const char *name, int walk, int dir, struct vfs_ref *ref);

int fd_node_check(fd_entry_t *entry);

int cred_privileged(void);

void cred_kill_setid(krffs_node_t *node);

void vfs_kill_setid(struct vfs_mount *mnt, void *node);

/* Access-check `want` bits, matching an octal permission digit. */
#define CRED_R 4

#define CRED_W 2

#define CRED_X 1

int mnt_is_root(const struct vfs_mount *mnt);

int32_t cred_access_ref(const struct vfs_ref *r, int want, int use_real);

int32_t cred_access_node(const krffs_node_t *node, int want, int use_real);

int32_t sys_mount(isr_params_t *params);

int32_t sys_umount2(isr_params_t *params);

int32_t sys_open(isr_params_t *params);

int32_t sys_openat(isr_params_t *params);

int32_t sys_close(isr_params_t *params);

int32_t sys_fsync(isr_params_t *params);

int32_t sys_sync(isr_params_t *params);

int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence, uint64_t *out);

int32_t sys_lseek(isr_params_t *params);

int sock_is_query(const struct socket *sock);

int32_t sys_read(isr_params_t *params);

int32_t sys_pread64(isr_params_t *params);

int32_t sys_pwrite64(isr_params_t *params);

int32_t readv_dgram(fd_entry_t *entry, uint32_t iov_user, int iovcnt, uint32_t *src_ip,
                    uint16_t *src_port, int peek);

int32_t sys_readv(isr_params_t *params);

int32_t sys_dup(isr_params_t *params);

int32_t sys_dup2(isr_params_t *params);

int32_t sys_chdir(isr_params_t *params);

int32_t sys_fchdir(isr_params_t *params);

int32_t sys_chroot(isr_params_t *params);

int32_t sys_getcwd(isr_params_t *params);

int32_t sys_readlink(isr_params_t *params);

int32_t sys_readlinkat(isr_params_t *params);

int32_t sys_stat64(isr_params_t *params);

int32_t sys_lstat64(isr_params_t *params);

int32_t sys_statfs64(isr_params_t *params);

int32_t sys_fstatfs64(isr_params_t *params);

int32_t sys_fstat64(isr_params_t *params);

int32_t sys_fstatat64(isr_params_t *params);

int32_t sys_statx(isr_params_t *params);

int32_t sys_access(isr_params_t *params);

int32_t sys_faccessat(isr_params_t *params);
int32_t sys_inotify_add_watch(isr_params_t *params);

int32_t sys_chmod(isr_params_t *params);

int32_t sys_fchmodat(isr_params_t *params);

int32_t sys_fchmod(isr_params_t *params);

int32_t sys_chown(isr_params_t *params);

int32_t sys_lchown(isr_params_t *params);

int32_t sys_fchownat(isr_params_t *params);

int32_t sys_fchown(isr_params_t *params);

int32_t sys_truncate(isr_params_t *params);

int32_t sys_truncate64(isr_params_t *params);

int32_t sys_symlink(isr_params_t *params);

int32_t sys_symlinkat(isr_params_t *params);

int32_t sys_link(isr_params_t *params);

int32_t sys_linkat(isr_params_t *params);

int32_t sys__llseek(isr_params_t *params);

int32_t sys_fcntl64(isr_params_t *params);

int32_t sys_flock(isr_params_t *params);

int32_t sys_memfd_create(isr_params_t *params);

int32_t sys_splice(isr_params_t *params);

int32_t sys_tee(isr_params_t *params);

int32_t sys_sendfile(isr_params_t *params);

int32_t sys_close_range(isr_params_t *params);

int32_t sys_stat(isr_params_t *params);

int32_t sys_lstat(isr_params_t *params);

int32_t sys_execve(isr_params_t *params);

int32_t sys_fork(isr_params_t *params);

int32_t sys_vfork(isr_params_t *params);

int32_t sys_exit_group(isr_params_t *params);

int32_t sys_getcpu(isr_params_t *params);

int32_t sys_rt_sigtimedwait(isr_params_t *params);

int32_t sys_mlock(isr_params_t *params);

int32_t sys_munlock(isr_params_t *params);

int32_t sys_mlock2(isr_params_t *params);

int32_t sys_mlockall(isr_params_t *params);

int32_t sys_munlockall(isr_params_t *params);

int32_t sys_mincore(isr_params_t *params);

int32_t sys_personality(isr_params_t *params);

int32_t sys_waitpid(isr_params_t *params);

int32_t sys_wait4(isr_params_t *params);

int32_t sys_waitid(isr_params_t *params);

int32_t sys_getrusage(isr_params_t *params);

int32_t sys_creat(isr_params_t *params);

int32_t sys_unlink(isr_params_t *params);

int32_t sys_unlinkat(isr_params_t *params);

int32_t sys_mkdir(isr_params_t *params);

int32_t sys_mkdirat(isr_params_t *params);

int32_t sys_mknod(isr_params_t *params);

int32_t sys_mknodat(isr_params_t *params);

int32_t sys_rmdir(isr_params_t *params);

int32_t sys_rename(isr_params_t *params);

int32_t sys_renameat(isr_params_t *params);

int32_t sys_renameat2(isr_params_t *params);

/*
 * Timespec structure for utimensat
 */
struct kernel_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/* Global tick counter from kernel.c - incremented at 100 Hz */
extern volatile uint32_t kernel_ticks;

void get_current_time(int64_t *seconds, int64_t *nanoseconds);

int32_t sys_utimensat(isr_params_t *params);

int32_t sys_getdents64(isr_params_t *params);

int32_t sys_getuid(isr_params_t *params);

int32_t sys_geteuid(isr_params_t *params);

int32_t sys_getgid(isr_params_t *params);

int32_t sys_getegid(isr_params_t *params);

int32_t sys_kill(isr_params_t *params);

int32_t sys_tkill(isr_params_t *params);

int32_t sys_tgkill(isr_params_t *params);

int32_t sys_pause(isr_params_t *params);

int32_t sys_rt_sigsuspend(isr_params_t *params);

int32_t sys_rt_sigaction(isr_params_t *params);

int32_t sys_rt_sigprocmask(isr_params_t *params);

int32_t sys_rt_sigreturn(isr_params_t *tf);

int32_t sys_sigreturn(isr_params_t *tf);

int pgrp_in_session(int pgid, int sid);

struct tty *fd_tty(fd_entry_t *e);

int user_access_ok(uint32_t addr, uint32_t len);

int user_buffer_ok(uint32_t addr, uint32_t len, int write);

int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port, const uint8_t *buf,
                   uint16_t len);

/* The query-only raw descriptor a DHCP client asks the interface through. */
#define SOCK_RAW 3

int32_t sys_socketcall(isr_params_t *params);

int32_t sys_socket(isr_params_t *params);

int32_t sys_socketpair(isr_params_t *params);

int32_t sys_bind(isr_params_t *params);

int32_t sys_connect(isr_params_t *params);

int32_t sys_listen(isr_params_t *params);

int32_t sys_accept4(isr_params_t *params);

int32_t sys_getsockopt(isr_params_t *params);

int32_t sys_setsockopt(isr_params_t *params);

int32_t sys_getsockname(isr_params_t *params);

int32_t sys_getpeername(isr_params_t *params);

int32_t sys_sendto(isr_params_t *params);

int32_t sys_sendmsg(isr_params_t *params);

int32_t sys_recvfrom(isr_params_t *params);

int32_t sys_recvmsg(isr_params_t *params);

int32_t sys_shutdown(isr_params_t *params);

int32_t sys_recvmmsg(isr_params_t *params);

int32_t sys_sendmmsg(isr_params_t *params);

int32_t sys_getpid(isr_params_t *params);

int32_t sys_gettid(isr_params_t *params);

int32_t sys_setsid(isr_params_t *params);

int32_t sys_setpgid(isr_params_t *params);

int32_t sys_getpgid(isr_params_t *params);

int32_t sys_getpgrp(isr_params_t *params);

int32_t sys_getsid(isr_params_t *params);

int32_t sys_getppid(isr_params_t *params);

int32_t sys_umask(isr_params_t *params);

/* The id setters: the V7 rule with POSIX saved IDs, over the PCB credentials.
 * Privilege is effective uid 0 for the uid and gid families alike. A privileged
 * caller sets anything; an unprivileged one only swaps among its own real,
 * effective, and saved ids, so a shed identity cannot be regained. The saved id
 * follows POSIX: a 2-argument call that sets the real id, or sets the effective
 * id away from the pre-call real id, also saves the new effective id. `KEEP`
 * ((id_t)-1) leaves a field unchanged. */
#define SC_ID_KEEP 0xFFFFFFFFu

int32_t sys_setuid(isr_params_t *params);

int32_t sys_setgid(isr_params_t *params);

int32_t sys_setreuid(isr_params_t *params);

int32_t sys_setregid(isr_params_t *params);

int32_t sys_setresuid(isr_params_t *params);

int32_t sys_setresgid(isr_params_t *params);

int32_t sys_getresuid(isr_params_t *params);

int32_t sys_getresgid(isr_params_t *params);

int32_t sys_setgroups(isr_params_t *params);

int32_t sys_getgroups(isr_params_t *params);

int32_t sys_getrlimit(isr_params_t *params);

int32_t sys_setrlimit(isr_params_t *params);

int32_t sys_prlimit64(isr_params_t *params);

int32_t sys_dup3(isr_params_t *params);

int32_t sys_brk(isr_params_t *params);

int32_t sys_mmap2(isr_params_t *params);

int32_t sys_munmap(isr_params_t *params);

int32_t sys_mprotect(isr_params_t *params);

int32_t sys_mremap(isr_params_t *params);

int32_t sys_madvise(isr_params_t *params);

int32_t sys_msync(isr_params_t *params);

int32_t sys_clone(isr_params_t *params);

int32_t sys_sigaltstack(isr_params_t *params);

int32_t sys_rt_sigpending(isr_params_t *params);

int32_t sys_sigprocmask(isr_params_t *params);

int32_t sys_getpriority(isr_params_t *params);

int32_t sys_setpriority(isr_params_t *params);

int32_t sys_sched_yield(isr_params_t *params);

int32_t sys_sched_setscheduler(isr_params_t *params);

int32_t sys_sched_setparam(isr_params_t *params);

int32_t sys_sched_getscheduler(isr_params_t *params);

int32_t sys_sched_getparam(isr_params_t *params);

int32_t sys_sched_get_priority_max(isr_params_t *params);

int32_t sys_sched_get_priority_min(isr_params_t *params);

int32_t sys_sched_rr_get_interval(isr_params_t *params);

int32_t sys_sched_getaffinity(isr_params_t *params);

int32_t sys_sched_setaffinity(isr_params_t *params);

int32_t sys_prctl(isr_params_t *params);

int32_t sys_capget(isr_params_t *params);

int32_t sys_capset(isr_params_t *params);

#endif
