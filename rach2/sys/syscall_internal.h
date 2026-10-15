#ifndef _SYSCALL_INTERNAL_H_
#define _SYSCALL_INTERNAL_H_

/*
 * What the files of the system call layer (syscall.c, syscall_fs.c,
 * syscall_proc.c and syscall_net.c) share with each other and with the
 * subsystems that serve calls: the helpers one file defines and another
 * calls, the constants and types two of them read, and every handler, which
 * `arch_syscall_init` of each architecture puts in the table
 * (arch/i686/systab.c, arch/x86_64/systab.c). Included after the kernel
 * headers, whose types it names.
 */

extern volatile uint32_t fb_owner_pid;

/**
 * Release what the descriptor `e` holds before its slot is freed: first, for a
 * descriptor of the calling thread's table, the table's record locks on what it
 * is open on (`fd_close_locks`), then its objects (`fd_release_objects`). The
 * caller frees the slot with `fd_free`. The close, the dup2 replacement of an
 * open target, the execve CLOEXEC sweep and the release of a table by its last
 * sharer come here. Nothing when `e` is NULL. May sleep.
 */
void fd_release_backing(fd_entry_t *e);

/**
 * Take the lock the release of `e`, a descriptor or a copy of one, needs when
 * the release may free a node of the KRFFS root that lost its name: the root's
 * namespace lock, for a file of the root, or for an end of a FIFO of the root
 * or the reservation of an open of one. Returns the mount locked, which the
 * caller unlocks (`vfs_ns_unlock`) once the release is done, or NULL, with
 * nothing taken, for any other descriptor.
 *
 * May sleep; no caller holds the namespace lock of another mount or the lock of
 * a description on a mount.
 */
struct vfs_mount *fd_release_lock(const fd_entry_t *e);

/**
 * At the end of a system call: let go of the descriptors the call held in its
 * shared table, and release those another sharer closed meanwhile that no other
 * call holds (`fd_teardown`). May sleep.
 */
void fd_holds_release(void);

extern volatile process_t *stdin_waiting_proc;

long sys_exit(isr_params_t *params);

long sys_ftruncate(isr_params_t *params);

/* A call's argument `n` of the frame `p` as a signed value of the
 * register's width, and the largest such value: 32 bits on i686 and 64 on
 * x86-64, by the width of the frame's own registers, so a host test of the
 * i686 kernel reads them as the kernel does whatever `long` is there. */
#define SC_SIGNED(p, n) \
    (sizeof(SC_RET(p)) == 4 ? (long long)(int32_t)SC_ARG(p, n) : (long long)SC_ARG(p, n))
#define SC_SIGNED_MAX(p) (~(uint64_t)0 >> (65 - 8 * sizeof(SC_RET(p))))

/*
 * The cores of the calls whose arguments differ in shape between i386 and
 * a 64-bit architecture, Linux's ksys_ functions: each takes its offsets
 * and lengths whole. The shared sys_ handler of each is the one-register
 * form a 64-bit architecture maps; i386's, which split a 64-bit value into
 * two registers, are in arch/i686/sys_ia32.c.
 */

/**
 * Truncate or extend the open file `fd` to `length` bytes: the core of
 * ftruncate(2) and of i386's ftruncate64, the form musl calls there, whose
 * handlers give it the length whole; the file system answers a length past what
 * it can hold. Returns 0 or a negative errno, in Linux's order: -EINVAL for a
 * negative length, before the descriptor is looked at; -EBADF for a descriptor
 * that is not open or is an O_PATH one; -EINVAL for one open on anything but a
 * file of a mount, such as a directory, a pipe, a socket or a device, or not
 * open for writing; -EFBIG past RLIMIT_FSIZE, with SIGXFSZ sent
 * (`fsize_refused`); -EPERM for an immutable or append-only file
 * (`size_guarded`); -EBUSY for a file that backs a loop device; or the file
 * system's error. May sleep.
 */
long ksys_ftruncate(int fd, long long length);

/**
 * The core of truncate(2) and of i386's truncate64(2): set the length of the
 * file `upath` names, relative to the working directory, to `length` bytes.
 * Returns 0 or a negative errno: -EINVAL for a negative length, before the path
 * is looked at, as on Linux; -EFAULT for a NULL path; or the errors of the copy
 * into a path page (`path_from_user_dir`) and of `truncate_path`.
 */
long ksys_truncate(const char __user *upath, long long length);

/**
 * The core of pread64(2) in every form: `count` bytes of `fd` at `pos` into
 * `buf`, leaving the descriptor's own offset where it was; `pio_entry` has what
 * it refuses. Returns the bytes read, 0 at the end of the file and for a
 * `count` of 0, whose descriptor is still checked, or a negative errno.
 */
long ksys_pread64(int fd, char __user *buf, uint32_t count, long long pos);

/**
 * The core of pwrite64(2) in every form: pread64's, with the buffer written
 * from. Writes at the offset and leaves the descriptor's own where it was,
 * except that O_APPEND writes at the end of the file, as Linux's pwrite does;
 * the set-id bits and the loop device's file are write's rules (`file_write`).
 * A device is written as pread64 reads it. Returns the bytes written, 0 for a
 * `count` of 0, whose descriptor is still checked, or a negative errno.
 */
long ksys_pwrite64(int fd, const char __user *buf, uint32_t count, long long pos);

/**
 * The core of fadvise64 in every form: check one advice of posix_fadvise(3) and
 * ignore it; the offset does not matter to it. Returns 0, or fails as Linux's
 * does: -EBADF for a descriptor not open, -ESPIPE for a pipe or a FIFO, and
 * -EINVAL for a negative length or an advice outside POSIX_FADV_NORMAL (0) to
 * POSIX_FADV_NOREUSE (5).
 */
long ksys_fadvise64(int fd, long long len, uint32_t advice);

/**
 * The core of fallocate(2) in every form: give `fd` the range of `len` bytes at
 * `offset` as `mode` asks. Returns 0 or a negative errno.
 *
 * A regular file of a mounted file system goes to its type's operation
 * (vfs_fallocate), under the description's lock: tmpfs makes the range's pages
 * or punches a hole, as Linux's tmpfs does, and ext2 gives the holes in the
 * range zeroed blocks; any other type, and a block device, is -EOPNOTSUPP,
 * Linux's answer for a file without the operation, after which glibc's
 * posix_fallocate writes the range itself; it does not after -ENOSYS. A range
 * that grows the file past RLIMIT_FSIZE is -EFBIG with SIGXFSZ, as a write's
 * is. The checks before it are Linux's, in its order (ksys_fallocate,
 * vfs_fallocate): -EBADF for a descriptor that is not open, -EINVAL for a
 * negative offset or a length that is not positive, -EOPNOTSUPP for a mode it
 * does not take, -EBADF for a descriptor not open for writing, then by the
 * file's type -ESPIPE for a pipe, -EISDIR for a directory and -ENODEV for
 * anything but a regular file or a block device, and -EFBIG for a range past
 * the largest offset.
 */
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

/**
 * Remove the empty and "." components of the absolute path `path`, in place,
 * and return its length. `path` begins with '/'; the result has no trailing
 * slash unless it is "/". ".." stays in the path: the walk resolves it
 * physically, where a missing or unsearchable directory before it fails, so no
 * spelling of a path reaches past what the caller may see.
 */
int canonicalize_abs(char *path);

/**
 * The path of execve(2), copied into `pathbuf` of `size` bytes as
 * `copy_path_at` copies it for `AT_FDCWD`, for the program it runs, with `*end`
 * saying how it named a directory by its end and a last "." put back. Such a
 * path must reach a directory, which no program is, so the one walk that finds
 * the program answers for the end (`resolve_named`): a file so named is
 * -ENOTDIR and a directory the open's -EACCES, as on Linux. Returns the length,
 * or a negative errno.
 */
int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath, int *end);

/**
 * Make the absolute path `path`, which the kernel read from a file, canonical
 * in place, as `copy_path_cwd` makes execve's, with `*end` saying how it named
 * a directory by its end and a last "." put back: the interpreter a script
 * names, or an ELF image's PT_INTERP, whose one walk answers for the end as the
 * program's does. Returns the length.
 */
int canonicalize_named(char *path, int *end);

/**
 * `vfs_resolve` of `name` with the flags `walk`, for a call whose path named a
 * directory by its end when `dir` is not 0 (`path_names_dir`): such a path
 * follows a final symbolic link whatever `walk` says, and must reach a
 * directory, as Linux's walk requires, or the call is -ENOTDIR, with nothing
 * held and no lock kept. Returns 0 with `ref` held, and the final mount locked
 * under VFS_WALK_LOCK, or a negative errno.
 */
int32_t resolve_named(const char *name, int walk, int dir, struct vfs_ref *ref);

/**
 * Check, before an operation on it, that the KRFFS root node the descriptor
 * `entry` has is still a file the descriptor may use: 0, or -EBADF.
 */
int fd_node_check(fd_entry_t *entry);

/** Non-zero when the caller has appropriate privilege (`proc_privileged`). */
int cred_privileged(void);

/**
 * Drop the set-id bits a modification must not leave standing on the KRFFS root
 * node `node`: a write or a truncation by a caller without privilege, or a
 * change of owner or group by any caller. A directory keeps its bits, as on
 * Linux, and a NULL `node` is ignored.
 */
void cred_kill_setid(krffs_node_t *node);

/**
 * The rule of `cred_kill_setid` for the file `node` of the mount `mnt` of
 * another type, through the attributes the type reports and sets: a write or a
 * truncation by a caller without the privilege drops the set-id bits, so that a
 * program a group may modify cannot keep root's identity. A directory keeps its
 * bits, as on Linux. Nothing is done for a NULL `mnt` or `node` or a file whose
 * attributes cannot be read, and a failure to set them is ignored.
 */
void vfs_kill_setid(struct vfs_mount *mnt, void *node);

/* Access-check `want` bits, matching an octal permission digit. */
#define CRED_R 4

#define CRED_W 2

#define CRED_X 1

/**
 * Whether `mnt` is the KRFFS root, whose nodes take the paths of the system
 * call layer written for KRFFS; an ext2 root is entry zero of the mount table
 * as well and goes the way of every other type.
 */
int mnt_is_root(const struct vfs_mount *mnt);

/**
 * The permission check of the `want` bits over the node `r` of any type, from
 * the attributes its file system reports, by the real ids when `use_real` is
 * set: the nine-bit rule of `cred_access_bits`, as `cred_access_node` applies
 * it, with its execute rule for anything but a directory, which needs an
 * execute bit somewhere even for root. A write of an immutable file is -EPERM
 * for every caller, root too, as Linux's inode_permission answers. Returns 0,
 * -EACCES, -EPERM, or the error of reading the attributes.
 */
int32_t cred_access_ref(const struct vfs_ref *r, int want, int use_real);

/**
 * The permission check of the `want` bits over the KRFFS root node `node`, by
 * the real ids when `use_real` is set (`cred_access_bits`): 0, or -EACCES. The
 * root directory reads as 0755, the fixed identity stat reports (its stored
 * mode has the dirty-mount flag). Executing a file that is not a directory
 * needs an execute bit somewhere, even for root.
 */
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

/**
 * The inode-flag requests of ioctl(2), FS_IOC_GETFLAGS, FS_IOC_SETFLAGS and
 * FS_IOC_GETVERSION, on the descriptor `entry`, with `arg` the user address of
 * the int they read or write. `*handled` is set when the request was answered
 * here, and the return is then 0, -EFAULT, -EPERM or the type's error;
 * otherwise it is 0.
 *
 * Of the types here only ext2 keeps the flags and the generation, and only for
 * a file or a directory; any other descriptor leaves the request to the rest of
 * ioctl(2), which answers -ENOTTY, as Linux does for a file system without
 * them. The file's owner, or a privileged caller, sets the flags the file
 * system keeps; the immutable and append-only flags, which the VFS enforces,
 * only a privileged caller changes, as CAP_LINUX_IMMUTABLE does on Linux, and
 * another caller is -EPERM.
 */
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

/**
 * Whether `sock` is the query descriptor: the raw socket socket(2) makes of
 * AF_INET, SOCK_RAW and IPPROTO_RAW, which stores no protocol. A DHCP client
 * opens one only to ask the interface ioctls about the machine, and it sends
 * and receives nothing: bind, sendto, recvfrom, read and readv fail on it with
 * EOPNOTSUPP.
 */
int sock_is_query(const struct socket *sock);

long sys_read(isr_params_t *params);

long sys_pread64(isr_params_t *params);

long sys_pwrite64(isr_params_t *params);

long sys_preadv(isr_params_t *params);

long sys_pwritev(isr_params_t *params);

/**
 * Fill the `iovcnt` buffers of the user's iovec array at `iov_user` from one
 * datagram of the socket `entry` is open on, for readv(2) and recvmsg(2),
 * waiting for one unless the description has O_NONBLOCK. Returns the bytes
 * copied, or a negative errno from the wait: -EAGAIN when no datagram waits
 * under O_NONBLOCK, -EINTR, or the socket's pending error, which the call
 * takes.
 *
 * A datagram is a message, so it is taken once and copied out in pieces: the
 * buffers are filled in order until the datagram runs out, and what is left
 * when the buffers run out first is dropped, as a `read` too small for a
 * datagram drops it. One datagram is consumed either way, unless `peek` leaves
 * it waiting for the next receive, as MSG_PEEK asks, and one of no bytes
 * returns 0 without being the end of input. `src_ip` and `src_port`, when not
 * NULL, are given the datagram's sender, and `whole` its length, which is more
 * than the bytes copied when the buffers took less, for MSG_TRUNC.
 */
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

/**
 * The core of statfs(2) and statfs64(2): copy out to `out` the statfs of the
 * file system the user's path `upath` is on; 0 or a negative errno. The caller
 * has refused a NULL for either. The path must name something, so a path that
 * names nothing is the walk's error, -ENOENT among them, and one that names a
 * directory by its end must reach one (`resolve_named`). A node of /dev, /proc
 * or /sys answers with its mount's magic, a node of a mounted volume with the
 * volume's own statfs, and a node of the KRFFS boot root with KRFFS's.
 */
long ksys_statfs(const char __user *upath, struct user_statfs __user *out);

/**
 * The core of fstatfs(2) and fstatfs64(2): copy out to `out` the statfs of the
 * file system descriptor `fd` is open on; 0 or a negative errno, -EBADF for a
 * descriptor that is not open. The caller has refused a NULL `out`.
 *
 * The descriptor's kind decides which file system answers. A file, a
 * directory or a FIFO of a mounted volume answers with the volume's own
 * statfs, and one of the KRFFS boot root with KRFFS's. A /proc file answers
 * proc's, or sysfs's for a /sys file; a synthetic directory, and an O_PATH
 * descriptor on a synthetic leaf, answer with the magic of the mount the
 * node is in (`statfs_magic_for_kind`), as statfs of its path does. A
 * device, the console, a pty master or slave, the mouse and the framebuffer
 * answer devfs's, a device opened by a device node on a volume too. The rest
 * answer as Linux does: a pipe pipefs's, a socket sockfs's, a namespace file
 * nsfs's, a message queue mqueue's, and any other anonymous descriptor, an
 * eventfd or an epoll set among them, the anonymous inode file system's.
 */
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

/* The `struct timespec` of the caller's ABI, two `long`s, which utimensat,
 * the clocks, the sleeps and futex read and write. */
struct kernel_timespec {
    long tv_sec;
    long tv_nsec;
};

/**
 * The current real time, in seconds and nanoseconds since the epoch, as
 * `rtc_realtime` (clock.c) gives it: the second the machine's clock gave at
 * boot plus the time since that read, or the time since boot on a machine whose
 * clock could not be read, with the timer's place within the tick and any step
 * a setting of the clock made, and never earlier than the reading before it.
 */
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
int32_t munmap_shm(uintptr_t addr, const struct shmem *sh, size_t size);

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
long sys_time(isr_params_t *params);
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
