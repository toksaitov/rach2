#ifndef _SYSCALL_INTERNAL_H_
#define _SYSCALL_INTERNAL_H_

/*
 * What the four parts of the system call layer share: the helpers one part
 * defines and another calls, the constants and types two parts read, and
 * every handler, which syscall.c registers in its table. Included after the
 * kernel headers, whose types it names. The layer was one file until
 * milestone 53 divided it along its arms; the bodies did not change.
 */

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

long sys_exit(isr_params_t *params);

long sys_ftruncate(isr_params_t *params);

/*
 * The cores of the calls whose arguments differ in shape between i386 and
 * a 64-bit architecture, Linux's ksys_ functions: each takes its offsets
 * and lengths whole. The shared sys_ handler of each is the one-register
 * form a 64-bit architecture maps; i386's, which split a 64-bit value into
 * two registers, are in arch/i686/sys_ia32.c.
 */
/* A call's argument `n` of the frame `p` as a signed value of the
 * register's width, and the largest such value: 32 bits on i686 and 64 on
 * x86-64, by the width of the frame's own registers, so a host test of the
 * i686 kernel reads them as the kernel does whatever `long` is there. */
#define SC_SIGNED(p, n)                                                    \
    (sizeof(SC_RET(p)) == 4 ? (long long)(int32_t)SC_ARG(p, n) : (long long)SC_ARG(p, n))
#define SC_SIGNED_MAX(p) (~(uint64_t)0 >> (65 - 8 * sizeof(SC_RET(p))))

long ksys_ftruncate(int fd, uint64_t length);
long ksys_truncate(const char __user *upath, long long length);
long ksys_pread64(int fd, char __user *buf, uint32_t count, long long pos);
long ksys_pwrite64(int fd, const char __user *buf, uint32_t count, long long pos);
long ksys_fadvise64(int fd, long long len, uint32_t advice);
long ksys_fallocate(int fd, uint32_t mode, uint64_t offset, uint64_t len);
long ksys_mmap_pgoff(uintptr_t addr, uintptr_t length, uint32_t prot, uint32_t flags, int fd,
                     unsigned long pgoff);

long sys_write(isr_params_t *params);

/*
 * The user's structures that a call reads or writes whole, declared with the
 * C types of Linux's own declarations: a pointer as `uintptr_t`, a `size_t`
 * as `unsigned long`. The kernel is compiled for the processor's C ABI, as
 * a program is, so each structure has the program's layout on either
 * architecture: 8 bytes for an iovec on i686 and 16 on x86-64.
 */
struct kernel_iovec {
    uintptr_t iov_base;
    unsigned long iov_len;
};

/* Linux's `struct user_msghdr`, which sendmsg(2) and recvmsg(2) take. */
struct kernel_msghdr {
    uintptr_t msg_name;
    int msg_namelen;
    uintptr_t msg_iov;
    unsigned long msg_iovlen;
    uintptr_t msg_control;
    unsigned long msg_controllen;
    unsigned int msg_flags;
};

/* The header of one control message, and the alignment of what follows
 * it and of the next message: that of the `size_t` it begins with. */
struct kernel_cmsghdr {
    unsigned long cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

#define CMSG_ALIGN(n) (((n) + sizeof(unsigned long) - 1u) & ~(sizeof(unsigned long) - 1u))
#define CMSG_HDR      CMSG_ALIGN(sizeof(struct kernel_cmsghdr))

/* Format: the vectors readv, writev and sendmsg accept, the Linux IOV_MAX. */
#define IOV_MAX_LOCAL 1024

long sys_writev(isr_params_t *params);

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

long sys_mount(isr_params_t *params);

long sys_umount2(isr_params_t *params);

long sys_open(isr_params_t *params);

long sys_openat(isr_params_t *params);

long sys_close(isr_params_t *params);

long sys_fsync(isr_params_t *params);

long sys_sync(isr_params_t *params);

long sys_syncfs(isr_params_t *params);

long sys_readahead(isr_params_t *params);

int32_t fs_flags_ioctl(fd_entry_t *entry, uint32_t request, uintptr_t arg, int *handled);

long sys_fadvise64(isr_params_t *params);

long sys_setxattr(isr_params_t *params);
long sys_lsetxattr(isr_params_t *params);
long sys_fsetxattr(isr_params_t *params);
long sys_getxattr(isr_params_t *params);
long sys_lgetxattr(isr_params_t *params);
long sys_fgetxattr(isr_params_t *params);
long sys_listxattr(isr_params_t *params);
long sys_llistxattr(isr_params_t *params);
long sys_flistxattr(isr_params_t *params);
long sys_removexattr(isr_params_t *params);
long sys_lremovexattr(isr_params_t *params);
long sys_fremovexattr(isr_params_t *params);

int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence, uint64_t *out);

long sys_lseek(isr_params_t *params);

int sock_is_query(const struct socket *sock);

long sys_read(isr_params_t *params);

long sys_pread64(isr_params_t *params);

long sys_pwrite64(isr_params_t *params);

long sys_preadv(isr_params_t *params);

long sys_pwritev(isr_params_t *params);

int32_t readv_dgram(fd_entry_t *entry, uintptr_t iov_user, int iovcnt, uint32_t *src_ip,
                    uint16_t *src_port, int peek, int32_t *whole);

long sys_readv(isr_params_t *params);

long sys_dup(isr_params_t *params);

long sys_dup2(isr_params_t *params);

long sys_chdir(isr_params_t *params);

long sys_fchdir(isr_params_t *params);

long sys_chroot(isr_params_t *params);

long sys_getcwd(isr_params_t *params);

long sys_readlink(isr_params_t *params);

long sys_readlinkat(isr_params_t *params);

long sys_stat64(isr_params_t *params);

long sys_lstat64(isr_params_t *params);

struct user_statfs;

long ksys_statfs(const char __user *upath, struct user_statfs __user *out);

long ksys_fstatfs(int fd, struct user_statfs __user *out);

long sys_statfs(isr_params_t *params);

long sys_fstatfs(isr_params_t *params);

long sys_statfs64(isr_params_t *params);

long sys_fstatfs64(isr_params_t *params);

long sys_fstat64(isr_params_t *params);

long sys_fstatat64(isr_params_t *params);

long sys_statx(isr_params_t *params);

long sys_access(isr_params_t *params);

long sys_faccessat(isr_params_t *params);
long sys_inotify_add_watch(isr_params_t *params);

long sys_chmod(isr_params_t *params);

long sys_fchmodat(isr_params_t *params);

long sys_fchmod(isr_params_t *params);

long sys_chown(isr_params_t *params);

long sys_lchown(isr_params_t *params);

long sys_fchownat(isr_params_t *params);

long sys_fchown(isr_params_t *params);

long sys_truncate(isr_params_t *params);

long sys_symlink(isr_params_t *params);

long sys_symlinkat(isr_params_t *params);

long sys_link(isr_params_t *params);

long sys_linkat(isr_params_t *params);

long sys__llseek(isr_params_t *params);

long sys_fcntl64(isr_params_t *params);

long sys_flock(isr_params_t *params);

long sys_memfd_create(isr_params_t *params);

long sys_splice(isr_params_t *params);

long sys_tee(isr_params_t *params);

long sys_sendfile(isr_params_t *params);

long sys_fallocate(isr_params_t *params);

long sys_close_range(isr_params_t *params);

long sys_stat(isr_params_t *params);

long sys_lstat(isr_params_t *params);

long sys_execve(isr_params_t *params);

long sys_fork(isr_params_t *params);

long sys_vfork(isr_params_t *params);

long sys_exit_group(isr_params_t *params);

long sys_getcpu(isr_params_t *params);

long sys_rt_sigtimedwait(isr_params_t *params);

long sys_mlock(isr_params_t *params);

long sys_munlock(isr_params_t *params);

long sys_mlock2(isr_params_t *params);

long sys_mlockall(isr_params_t *params);

long sys_munlockall(isr_params_t *params);

long sys_mincore(isr_params_t *params);

long sys_personality(isr_params_t *params);

long sys_waitpid(isr_params_t *params);

long sys_wait4(isr_params_t *params);

long sys_waitid(isr_params_t *params);

long sys_getrusage(isr_params_t *params);

long sys_creat(isr_params_t *params);

long sys_unlink(isr_params_t *params);

long sys_unlinkat(isr_params_t *params);

long sys_mkdir(isr_params_t *params);

long sys_mkdirat(isr_params_t *params);

long sys_mknod(isr_params_t *params);

long sys_mknodat(isr_params_t *params);

long sys_rmdir(isr_params_t *params);

long sys_rename(isr_params_t *params);

long sys_renameat(isr_params_t *params);

long sys_renameat2(isr_params_t *params);

/*
 * Timespec structure for utimensat
 */
struct kernel_timespec {
    long tv_sec;
    long tv_nsec;
};

/* Global tick counter from kernel.c - incremented at 100 Hz */
extern volatile uint32_t kernel_ticks;

void get_current_time(int64_t *seconds, int64_t *nanoseconds);

long sys_utimensat(isr_params_t *params);

long sys_getdents64(isr_params_t *params);

long sys_getuid(isr_params_t *params);

long sys_geteuid(isr_params_t *params);

long sys_getgid(isr_params_t *params);

long sys_getegid(isr_params_t *params);

long sys_kill(isr_params_t *params);

long sys_tkill(isr_params_t *params);

long sys_tgkill(isr_params_t *params);

long sys_rt_sigqueueinfo(isr_params_t *params);

long sys_rt_tgsigqueueinfo(isr_params_t *params);

long sys_signalfd(isr_params_t *params);

long sys_signalfd4(isr_params_t *params);

long sys_pause(isr_params_t *params);

long sys_rt_sigsuspend(isr_params_t *params);

long sys_rt_sigaction(isr_params_t *params);

long sys_rt_sigprocmask(isr_params_t *params);

long sys_rt_sigreturn(isr_params_t *tf);

long sys_sigreturn(isr_params_t *tf);

int pgrp_in_session(int pgid, int sid);

struct tty *fd_tty(fd_entry_t *e);

int user_access_ok(uintptr_t addr, size_t len);

int user_buffer_ok(uintptr_t addr, size_t len, int write);

int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port, const uint8_t *buf,
                   uint16_t len);

/* The query-only raw descriptor a DHCP client asks the interface through. */
#define SOCK_RAW 3

long sys_socketcall(isr_params_t *params);

long sys_socket(isr_params_t *params);

long sys_socketpair(isr_params_t *params);

long sys_bind(isr_params_t *params);

long sys_connect(isr_params_t *params);

long sys_listen(isr_params_t *params);

long sys_accept4(isr_params_t *params);

long sys_getsockopt(isr_params_t *params);

long sys_setsockopt(isr_params_t *params);

long sys_getsockname(isr_params_t *params);

long sys_getpeername(isr_params_t *params);

long sys_sendto(isr_params_t *params);

long sys_sendmsg(isr_params_t *params);

long sys_recvfrom(isr_params_t *params);

long sys_recvmsg(isr_params_t *params);

long sys_shutdown(isr_params_t *params);

long sys_recvmmsg(isr_params_t *params);

long sys_sendmmsg(isr_params_t *params);

long sys_getpid(isr_params_t *params);

long sys_gettid(isr_params_t *params);

long sys_setsid(isr_params_t *params);

long sys_setpgid(isr_params_t *params);

long sys_getpgid(isr_params_t *params);

long sys_getpgrp(isr_params_t *params);

long sys_getsid(isr_params_t *params);

long sys_getppid(isr_params_t *params);

long sys_umask(isr_params_t *params);

/* The id setters: the V7 rule with POSIX saved IDs, over the PCB credentials.
 * Privilege is effective uid 0 for the uid and gid families alike. A privileged
 * caller sets anything; an unprivileged one only swaps among its own real,
 * effective, and saved ids, so a shed identity cannot be regained. The saved id
 * follows POSIX: a 2-argument call that sets the real id, or sets the effective
 * id away from the pre-call real id, also saves the new effective id. `KEEP`
 * ((id_t)-1) leaves a field unchanged. */
#define SC_ID_KEEP 0xFFFFFFFFu

long sys_setuid(isr_params_t *params);

long sys_setgid(isr_params_t *params);

long sys_setreuid(isr_params_t *params);

long sys_setregid(isr_params_t *params);

long sys_setresuid(isr_params_t *params);

long sys_setresgid(isr_params_t *params);

long sys_getresuid(isr_params_t *params);

long sys_getresgid(isr_params_t *params);

long sys_setgroups(isr_params_t *params);

long sys_getgroups(isr_params_t *params);

long sys_getrlimit(isr_params_t *params);

long sys_setrlimit(isr_params_t *params);

long sys_prlimit64(isr_params_t *params);

long sys_dup3(isr_params_t *params);

long sys_brk(isr_params_t *params);

long sys_mmap2(isr_params_t *params);

long sys_mmap(isr_params_t *params);

long sys_munmap(isr_params_t *params);

long sys_mprotect(isr_params_t *params);

long sys_mremap(isr_params_t *params);

long sys_madvise(isr_params_t *params);

long sys_msync(isr_params_t *params);

long sys_clone(isr_params_t *params);

long sys_clone3(isr_params_t *params);

long sys_sigaltstack(isr_params_t *params);

long sys_rt_sigpending(isr_params_t *params);

long sys_sigprocmask(isr_params_t *params);

long sys_getpriority(isr_params_t *params);

long sys_setpriority(isr_params_t *params);

long sys_ioprio_set(isr_params_t *params);

long sys_ioprio_get(isr_params_t *params);

long sys_sched_yield(isr_params_t *params);

long sys_sched_setscheduler(isr_params_t *params);

long sys_sched_setparam(isr_params_t *params);

long sys_sched_getscheduler(isr_params_t *params);

long sys_sched_getparam(isr_params_t *params);

long sys_sched_get_priority_max(isr_params_t *params);

long sys_sched_get_priority_min(isr_params_t *params);

long sys_sched_rr_get_interval(isr_params_t *params);

long sys_sched_getaffinity(isr_params_t *params);

long sys_sched_setaffinity(isr_params_t *params);

long sys_prctl(isr_params_t *params);

long sys_capget(isr_params_t *params);

long sys_capset(isr_params_t *params);

struct open_file;
struct shmem;

/* What a System V attachment may do besides read. */
#define SHM_MAP_WRITE 1u
#define SHM_MAP_EXEC  2u

/**
 * Map a System V segment, the tmpfs file `ofd` describes, `len` bytes
 * shared: where the kernel finds room for `place` 0, at `addr` refusing a
 * range in use with EEXIST for 1, and at `addr` replacing what it held for
 * 2. It may be read, and written and executed as `access` says; one that
 * may not be written keeps VM_PROT_NOWRITE, so mprotect cannot make it
 * writable. `path`, from the heap, is what /proc/PID/maps names it, and the
 * region takes it. Returns the address, or -errno as mmap does.
 */
long mmap_shm(struct open_file *ofd, uintptr_t addr, uintptr_t len, uint32_t access, int place,
              char *path);

/** Unmap the System V attachment at `addr`: the regions from it on that map
 * the pages `sh` from the segment's start, within `size` bytes. -EINVAL
 * when no such region begins at `addr`. */
int32_t munmap_shm(uintptr_t addr, const struct shmem *sh, uint32_t size);

/** How many attachments the pages `sh` have, each address space's regions
 * that map them from the start counted once however many threads share it. */
uint32_t shm_attaches(const struct shmem *sh);

/* The handlers syscall.c defines for the table (arch/<arch>/systab.c): the
 * clocks, timers and sleeps, the host names, the log, ioctl and the rest of
 * the calls that belong to no subsystem. */
long sys_adjtimex(isr_params_t *params);
long sys_alarm(isr_params_t *params);
long sys_clock_adjtime(isr_params_t *params);
long sys_clock_adjtime64(isr_params_t *params);
long sys_clock_getres(isr_params_t *params);
long sys_clock_getres_time64(isr_params_t *params);
long sys_clock_gettime(isr_params_t *params);
long sys_clock_gettime64(isr_params_t *params);
long sys_clock_nanosleep(isr_params_t *params);
long sys_clock_settime(isr_params_t *params);
long sys_clock_settime64(isr_params_t *params);
long sys_getitimer(isr_params_t *params);
long sys_getrandom(isr_params_t *params);
long sys_gettimeofday(isr_params_t *params);
long sys_ioctl(isr_params_t *params);
long sys_nanosleep(isr_params_t *params);
long sys_reboot(isr_params_t *params);
long sys_set_tid_address(isr_params_t *params);
long sys_setdomainname(isr_params_t *params);
long sys_sethostname(isr_params_t *params);
long sys_setitimer(isr_params_t *params);
long sys_settimeofday(isr_params_t *params);
long sys_stime(isr_params_t *params);
long sys_sysinfo(isr_params_t *params);
long sys_syslog(isr_params_t *params);
long sys_times(isr_params_t *params);
long sys_uname(isr_params_t *params);

#endif
