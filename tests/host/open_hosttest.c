/**
 * open_hosttest.c: the open, close, dup and path calls of
 * rach2/sys/syscall_fs.c, with the descriptor tables of rach2/sys/fs/fd.c,
 * the loop devices of rach2/sys/block/loop.c and the file mappings of
 * rach2/sys/mm/filemap.c, on a model mounted type and a model KRFFS root.
 *
 * host-deps: tests/host/compat/vfs_name_none.c tests/host/compat/devtree_none.c
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/park_host.c
 * host-deps: rach2/sys/fs/nodelock.c rach2/sys/mm/notify.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/bkl_held.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c tests/host/compat/fsize_none.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The rows check:
 *
 * - a device ioctl admitted without the big lock despite its retained inode,
 *   with the last release and an unmarked ioctl still locked;
 * - an open that truncates a file of a mounted type, and the cleanup of an
 *   open that fails;
 * - the close of a descriptor whose release sleeps, by its own thread or at
 *   the end of another thread's call, and a dup2 whose source is closed
 *   while the replacement of its target sleeps;
 * - the errors of an open, a dup and a dup2 that get no descriptor;
 * - an unlink and a rename on the KRFFS root of a file an open is
 *   installing or a program runs from;
 * - the release of a program's file that lost its name, under the root's
 *   lock;
 * - a mkdir of a name another process makes while the call waits for the
 *   parent's lock;
 * - the locks of an orphan's last description, dropped before the orphan is
 *   freed;
 * - an rmdir on the KRFFS root of a directory a description or a working
 *   directory has, or an open publishes a descriptor on while the call
 *   waits;
 * - a chdir that pins its directory until the working directory records it;
 * - a chmod, a chown, the stat calls, access and utimensat of a file a write
 *   moves;
 * - an open that truncates while another thread sharing its table takes the
 *   table's last free slot;
 * - a loop device's handle when its node moves, and its attachment found by
 *   its node alone;
 * - the key a file's locks are found by on each kind of mount;
 * - a seek of a /proc file's or /dev/bootlog's description while a read or
 *   a write of it sleeps, and the log's seeks past the offsets they can
 *   return;
 * - getdents64 past 4 GiB of a directory whose cursor is a byte, which asks
 *   the type's `readdir` for a failed mount's or a removed directory's
 *   error, and the seeks of such a directory up to its volume's largest
 *   file offset and past it;
 * - an open of /proc/PID/fd/N for a device opened through a device node of
 *   a mounted type while another thread closes descriptor N, the device's
 *   last;
 * - a followed /proc/PID/fd/N stat of a pipe, and one that fails, while
 *   the owner closes descriptor N, its description's last;
 * - the release of the last holder of a file of the KRFFS root that lost
 *   its name, by each path a descriptor or a file mapping ends on, while
 *   another process's allocation on the root runs in the reclaim's sleep;
 * - a read of a disk file whose pages a shared mapping holds in memory
 *   when no temporary mapping is free for one of them;
 * - a read of an ext2 file without the big lock while another thread, as
 *   the read's first page waits for the disk, maps the file shared and
 *   writable and stores into its second page, through the file system and
 *   through the file page cache (`resident_race`, which `--race-only` runs
 *   alone);
 * - a read without the big lock whose end a sharer's seek put past 2^63-1,
 *   which holds no big lock after, and an open without the big lock
 *   that installs descriptor 1 and is not made again under it;
 * - on a model type that sets `file_locked`, with the node locks of
 *   rach2/sys/fs/nodelock.c, the order of the locks a create, an unlink,
 *   an rmdir and a rename in one directory and between two take, and a
 *   mount against an rmdir of the directory it covers;
 * - on the same model, fs.protected_hardlinks on a hard link to another
 *   user's file, and fs.protected_regular and fs.protected_fifos on an
 *   O_CREAT open of an existing file in a sticky directory, at each level,
 *   as Linux 6.12's may_linkat and may_create_in_sticky judge them.
 *
 * The static declarations before the includes make the unused exports of
 * the three sources private, so the compiler drops them and the hardware
 * calls they make need no stub. `node` and `handle` are distinct, so a
 * check can tell a node from an open handle of it. A call of another
 * process that must wait for a description or for the namespace lock that
 * a sleeping call holds parks in its wait with its frames kept
 * (`compat/park_host.c`), and the row resumes it there once the holder has
 * let go and woken its queue.
 */
#include "isr.h"
#include "lib/kuser.h"
#include "fs/fd.h"
#include "fs/vfs.h"
#include "fs/krffs.h"
#include "network/socket/socket.h"
static int fs_node_is_held(const krffs_node_t *node);
static struct open_file *fs_exe_open(const struct vfs_ref *ref);
static struct vfs_mount *fs_exe_lock(const struct open_file *ofd);
static void fs_exe_close(struct open_file *ofd, struct vfs_mount *locked);
static void fs_node_moved(const krffs_node_t *from, krffs_node_t *to);
static void fd_release_backing(fd_entry_t *e);
static void fd_holds_release(void);
static long sys_ftruncate(isr_params_t *params);
static long ksys_ftruncate(int fd, long long length);
static long ksys_truncate(const char __user *upath, long long length);
static long ksys_pread64(int fd, char __user *buf, size_t count, long long pos);
static long ksys_pwrite64(int fd, const char __user *buf, size_t count, long long pos);
static long ksys_fadvise64(int fd, long long offset, long long len, uint32_t advice);
static long ksys_readahead(int fd, uint64_t offset, uint64_t count);
static long ksys_fallocate(int fd, uint32_t mode, uint64_t offset, uint64_t len);
static long sys_write(isr_params_t *params);
static long sys_writev(isr_params_t *params);
static int canonicalize_abs(char *path);
static int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath, int *end);
static int canonicalize_named(char *path, int *end);
static int32_t resolve_named(const char *name, int walk, int dir, struct vfs_ref *ref);
static int fd_node_check(fd_entry_t *entry);
static int cred_privileged(void);
static void cred_kill_setid(krffs_node_t *node);
static void vfs_kill_setid(struct vfs_mount *mnt, void *node);
static int mnt_is_root(const struct vfs_mount *mnt);
static int32_t cred_access_ref(const struct vfs_ref *r, int want, int use_real);
static int32_t cred_access_node(const krffs_node_t *node, int want, int use_real);
static long sys_pivot_root(isr_params_t *params);
static long sys_faccessat2(isr_params_t *params);
static long sys_splice(isr_params_t *params);
static long sys_tee(isr_params_t *params);
static long sys_vmsplice(isr_params_t *params);
static long sys_sendfile(isr_params_t *params);
static long sys_sendfile32(isr_params_t *params);
static long sys_copy_file_range(isr_params_t *params);
static long sys_mount(isr_params_t *params);
static long sys_umount2(isr_params_t *params);
static long sys_open(isr_params_t *params);
static long sys_openat(isr_params_t *params);
static long sys_openat2(isr_params_t *params);
static long sys_close(isr_params_t *params);
static long sys_fsync(isr_params_t *params);
static long sys_fdatasync(isr_params_t *params);
static long sys_sync(isr_params_t *params);
static long sys_syncfs(isr_params_t *params);
static long sys_lseek(isr_params_t *params);
static int sock_is_query(const struct socket *sock);
static long sys_read(isr_params_t *params);
static long sys_pread64(isr_params_t *params);
static long sys_pwrite64(isr_params_t *params);
static long sys_preadv(isr_params_t *params);
static long sys_pwritev(isr_params_t *params);
struct kernel_iovec;
static int32_t readv_dgram(fd_entry_t *entry, const struct kernel_iovec *iov, int iovcnt,
                           net_addr_t *src_ip, uint16_t *src_port, int peek, int32_t *whole);
static long sys_readv(isr_params_t *params);
static long sys_dup(isr_params_t *params);
static long sys_dup2(isr_params_t *params);
static long sys_chdir(isr_params_t *params);
static long sys_fchdir(isr_params_t *params);
static long sys_chroot(isr_params_t *params);
static long sys_getcwd(isr_params_t *params);
static long sys_readlink(isr_params_t *params);
static long sys_readlinkat(isr_params_t *params);
static long sys_stat64(isr_params_t *params);
static long sys_lstat64(isr_params_t *params);
struct user_statfs;
static long ksys_statfs(const char __user *upath, struct user_statfs __user *out);
static long ksys_fstatfs(int fd, struct user_statfs __user *out);
static long sys_statfs(isr_params_t *params);
static long sys_fstatfs(isr_params_t *params);
static long sys_statfs64(isr_params_t *params);
static long sys_fstatfs64(isr_params_t *params);
static int ksys_ustat_counts(uint32_t dev, uint64_t *bfree, uint64_t *ffree);
static long sys_ustat(isr_params_t *params);
static long sys_sysfs(isr_params_t *params);
static long sys_fstat64(isr_params_t *params);
static long sys_fstatat64(isr_params_t *params);
static long sys_access(isr_params_t *params);
static long sys_faccessat(isr_params_t *params);
static long sys_chmod(isr_params_t *params);
static long sys_fchmodat(isr_params_t *params);
static long sys_fchmod(isr_params_t *params);
static long sys_chown(isr_params_t *params);
static long sys_lchown(isr_params_t *params);
static long sys_fchownat(isr_params_t *params);
static long sys_fchown(isr_params_t *params);
static long sys_truncate(isr_params_t *params);
static long sys_symlink(isr_params_t *params);
static long sys_symlinkat(isr_params_t *params);
static long sys_link(isr_params_t *params);
static long sys_linkat(isr_params_t *params);
static long sys__llseek(isr_params_t *params);
static long sys_fcntl64(isr_params_t *params);
static long sys_stat(isr_params_t *params);
static long sys_lstat(isr_params_t *params);
static long sys_creat(isr_params_t *params);
static long sys_unlink(isr_params_t *params);
static long sys_mkdir(isr_params_t *params);
static long sys_mkdirat(isr_params_t *params);
static long sys_rmdir(isr_params_t *params);
static long sys_unlinkat(isr_params_t *params);
static long sys_rename(isr_params_t *params);
static long sys_renameat(isr_params_t *params);
static long sys_renameat2(isr_params_t *params);
static void get_current_time(int64_t *seconds, int64_t *nanoseconds);
static long sys_utimensat(isr_params_t *params);
static long sys_utimensat_time64(isr_params_t *params);
static long ksys_getdents32(int fd, uintptr_t buf, uint32_t count, int old);
static long ksys_sync_file_range(int fd, int64_t off, int64_t len, uint32_t flags);
static long sys_sync_file_range(isr_params_t *params);
static long sys_preadv2(isr_params_t *params);
static long sys_pwritev2(isr_params_t *params);
static long sys_getdents64(isr_params_t *params);
static long sys_dup3(isr_params_t *params);
static long sys_statx(isr_params_t *params);
static long sys_memfd_create(isr_params_t *params);
static long sys_memfd_secret(isr_params_t *params);
static long sys_cachestat(isr_params_t *params);
static long sys_flock(isr_params_t *params);
static long sys_close_range(isr_params_t *params);
static int32_t fs_core_file(int signo, unsigned long limit);
static long sys_mknod(isr_params_t *params);
static long sys_mknodat(isr_params_t *params);
static long sys_name_to_handle_at(isr_params_t *params);
static long sys_open_by_handle_at(isr_params_t *params);
static int32_t fd_stat_ids(int pid, int fd, uint32_t *mnt_id, uint64_t *ino);
static int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino);
static void fs_socket_unbind(struct vfs_ref *ref);
static int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino);
static int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off);
static int loop_attach(int index, const char *path, int writable);
static int loop_detach(int index);
static int loop_backs(struct vfs_mount *mnt, void *node);
static void loop_repoint(struct vfs_mount *mnt, const void *from, void *to);
static void loop_device_gone(block_device_t *device);
static int loop_line(int index, char *buffer, uint32_t cap);
static int loop_control_write(const char *buffer, uint32_t count);
static struct vfs_mount *fd_release_lock(const fd_entry_t *e);
struct vm_file;
struct shmem;
static struct vm_file *filemap_new(const fd_entry_t *e);
static struct vm_file *filemap_shared(const fd_entry_t *e, struct shmem *sh);
static struct vm_file *filemap_anon_shared(void);
static void filemap_shmem_cut(struct shmem *sh, uint32_t first);
static int filemap_holds(const void *node);
#include "syscall_fs.c"

/** No row advises a read: nothing to queue. */
void filecache_willneed(struct open_file *ofd, uint64_t offset, uint64_t len)
{
    (void)ofd;
    (void)offset;
    (void)len;
}

/** No row advises a drop: nothing to drop. */
void filecache_dontneed(struct open_file *ofd, uint64_t offset, uint64_t len)
{
    (void)ofd;
    (void)offset;
    (void)len;
}
#include "block/loop.c"
#include "mm/filemap.c"

/** No file of the rows is on tmpfs: not reached. */
struct shmem *tmpfs_shmem(void *node)
{
    (void)node;
    return NULL;
}

/** No file of the rows is on tmpfs: not reached. */
uint32_t tmpfs_seals(void *node)
{
    (void)node;
    return 0;
}
#include "ns_first.h"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern void exit(int status);

int park_run(void (*call)(void));
int park_resume(void);
int park_active(void);
void park(void);

static process_t proc, other, third;
static fd_table_t files;
/* The hold maps of the three threads, as wide as the model's tables grow. */
static uint32_t proc_held[FD_TABLE_LIMIT / 32], other_held[FD_TABLE_LIMIT / 32],
    third_held[FD_TABLE_LIMIT / 32];

/** Give the three threads their hold maps, which a reset of a record
 * clears. */
static void give_maps(void)
{
    proc.fd_held = proc_held;
    proc.fd_held_words = FD_TABLE_LIMIT / 32;
    other.fd_held = other_held;
    other.fd_held_words = FD_TABLE_LIMIT / 32;
    third.fd_held = third_held;
    third.fd_held_words = FD_TABLE_LIMIT / 32;
}

/** Mark `fd` held by a call of `t`, as `fd_get` marks it. */
static void hold_fd(process_t *t, int fd)
{
    t->fd_held[fd / 32] |= 1u << (fd % 32);
    if (t->fd_holding <= fd / 32) {
        t->fd_holding = fd / 32 + 1;
    }
}
struct percpu cpu_table[NR_CPUS] = {{.current = &proc}};
process_t *proc_list;
volatile uint32_t fb_owner_pid;
krffs_file_system_t *krffs_fs;
/* The protections of /proc/sys/fs, at the values rach2/sys/fs/vfs.c gives
 * them. */
int vfs_protected_hardlinks = 1;
int vfs_protected_symlinks = 1;
int vfs_protected_fifos = 1;
int vfs_protected_regular = 2;
static int root_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **found);
static const struct vfs_ops root_ops = {.lookup = root_lookup};
const struct vfs_type vfs_krffs_type = {"krffs", 0, &root_ops, 1, 0};
/* A mounted KRFFS volume's type and the two FAT types, which the lock key
 * row names. */
const struct vfs_type vfs_krffs_volume_type = {"krffs", 0, 0, 1, 0};
const struct vfs_type vfs_vfat_type = {"vfat", 0, 0, 0, 0};
const struct vfs_type vfs_exfat_type = {"exfat", 0, 0, 0, 0};
/* ext2, the type whose files a shared mapping keeps pages of, for the race
 * row, unserialised as it is. */
const struct vfs_type vfs_ext2_type = {"ext2", 0, 0, 0, 0};
/* tmpfs's type, which no mount of the rows has. */
const struct vfs_type vfs_tmpfs_type = {"tmpfs", 0, 0, 0, 0};
static const struct vfs_type mounted_type = {0};
/* A mounted type whose uses are serialised under its namespace lock, as
 * KRFFS volumes and ext2 are, for the attribute rows. */
static const struct vfs_type locked_type = {"locked", 0, 0, 1, 0};
/* A serialised mounted type with the root's lookup, for the mkdir rows. */
static const struct vfs_type mkdir_type = {"mkdir", 0, &root_ops, 1, 0};
/* A serialised mounted type that keeps a node only while it is open and
 * pins one for a holder that opens nothing, as a KRFFS volume does, for the
 * chdir row. */
static int dir_pin(struct vfs_mount *mnt, void *at, void **token);
static void dir_unpin(struct vfs_mount *mnt, void *token);
static const struct vfs_ops pinning_ops = {.pin = dir_pin, .unpin = dir_unpin};
static const struct vfs_type pinning_type = {"pinning", 0, &pinning_ops, 1, 0};
static struct vfs_mount mount;
static int node, handle, moved;
static int failures, allocations, opens, releases, refs, leaves, truncates;
static int open_error, truncate_error, getattr_error, dropped, attrs, relocate;
/* The descriptor a release is under way for, and what happens while the
 * file system's release of its description sleeps. */
static int releasing = -1;
static void (*in_release)(void);
/* The descriptor an open under way has reserved. */
static int installing = -1;
/* Whether a record lock exists, and the drops of one process's locks. */
static int locking, lock_closes;
static process_t *lock_closer;
/* The KRFFS root of the unlink and rename rows: its directory, the file an
 * open is installing, the file a rename moves over it, and the frees and
 * keeps of that file. */
static krffs_node_t root_dir, victim, source;
static krffs_file_system_t root_fs;
static int removals, keeps;
/* The frees of that file made with the root's namespace lock held. */
static int removals_locked;
/* The node of the root an address space keeps as its program's file. */
static const krffs_node_t *program;
/* The directory of the mkdir and rmdir rows, named once a mkdir has made
 * it, the working directories and umask of their calling process, and those
 * of another process. */
static krffs_node_t made_dir;
static struct fs_struct row_fs, other_fs;
static int mkdir_row;
/* The description whose last close an orphan row makes, and the last
 * description whose locks went. */
static const struct open_file *orphan_ofd, *ofd_gone;
/* The call that holds the root's namespace lock when a row's call first
 * takes it, which runs before the row's call has the lock, the depth of
 * the lock, the number of times it was taken from a depth of 0, and that
 * number at the row's first lookup in the root. */
static void (*waiting)(void);
static int ns_depth, ns_takes, lookup_take;
/* Set in the rows of chmod, chown and utimensat on a mounted type. */
static int attr_row;
/* The path pages the rows' calls have taken and not given back. */
static char path_pages[4][VFS_PATH_MAX];
static int path_pages_out[4];
/* The queue a call that waits for a description parked on, and the wakes of
 * that queue since. */
static wait_queue_t *parked_on;
static int parked_wakes;
/* The process that holds the namespace lock, and the queue a call of another
 * process sleeps on while it waits for the lock. */
static process_t *ns_holder;
static wait_queue_t ns_queue;
/* The call another process makes while a reclaim of a node of the root
 * sleeps, whether the reclaim is under way, whether that call parked, and
 * the extents it took, all of them and those while the reclaim was under
 * way. */
static void (*in_reclaim)(void);
static int reclaiming, reclaim_parked, extents_taken, extents_in_reclaim;

/* The model of a type that sets `file_locked`, defined before `main`. */
static struct vfs_mount fl_mount;
/* The device row: a serialised mounted type, as tmpfs is, whose node
 * `dev_inode` is a character device, the holds of that node and the
 * operations entered on its mount that are not given back yet, the closes
 * of the device, and what runs while the device's open sleeps. */
static const struct vfs_ops dev_vfs_ops = {0};
static const struct vfs_type dev_type = {"devnode", 0, &dev_vfs_ops, 1, 0};
static struct vfs_mount dev_mount;
static int dev_inode, dev_holds, dev_enters, dev_closes;
static void (*in_dev_open)(void);
static int fl_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **found);
static int fl_getattr(struct vfs_mount *mnt, void *at, struct vfs_stat *st);
static int fl_resolve(const char *path, int flags, struct vfs_ref *out, char *leaf,
                      uint32_t leafcap);
static struct vfs_mount *fl_leaf_mount(void *at, const char *name);
static int fl_make(void *dir, const char *name, int isdir);
static int fl_remove(void *dir, const char *name, int isdir);
static int fl_move(void *olddir, const char *oldname, void *newdir, const char *newname);
static void fl_note(const char *token);
static int fl_holds(void *at);

static uint32_t interrupt_flags = 0x200u;

/** Save and clear the model's interrupt-enable bit. */
uint32_t kirq_save(void)
{
    uint32_t flags = interrupt_flags;

    interrupt_flags = 0;
    return flags;
}

/** Restore the model's saved flags. */
void kirq_restore(uint32_t flags)
{
    interrupt_flags = flags;
}

/** Whether the model allows interrupts. */
int kirq_enabled(void)
{
    return (interrupt_flags & 0x200u) != 0;
}

/** Enable interrupts in the model. */
void arch_irq_enable(void)
{
    interrupt_flags |= 0x200u;
}

/** Whether the modeled thread is in a system call. */
int proc_in_syscall(void)
{
    return current_proc && current_proc->in_syscall;
}

/** Model devices have no partition view; return their own flags. */
uint32_t block_flags(block_device_t *device)
{
    return device->flags;
}

/** Record every failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("open-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Run the call queued to run while a row's call waits: a caller that
 * holds the mount's lock, or a thread the scheduler runs in a sleep. */
static void run_waiting(void)
{
    void (*run)(void) = waiting;

    waiting = 0;
    if (run) {
        run();
    }
}

/** A user string, which in the model is the caller's own memory: its length,
 * or -ENAMETOOLONG when it does not fit `n` bytes with its terminator. */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

/** Copy to user memory, which in the model is the caller's own. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    k_memcpy(dst, src, n);
    return 0;
}

/** Copy from user memory, which in the model is the caller's own. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    k_memcpy(dst, src, n);
    return 0;
}

/** Whether the call is an i386 one on the x86-64 kernel, which no call of
 * the model is. */
int in_compat_syscall(void)
{
    return 0;
}

/** Whether a buffer is user memory, which in the model every buffer is. */
int user_buffer_ok(uintptr_t addr, size_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

/** Whether a range ends in user space, which in the model every range does. */
int user_span_ok(uintptr_t addr, size_t len)
{
    (void)addr;
    (void)len;
    return 1;
}

/** Take a path page from the model's four; every one must come back. */
char *path_buf_get(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (!path_pages_out[i]) {
            path_pages_out[i] = 1;
            return path_pages[i];
        }
    }
    check(0, "a call takes four path pages at most");
    return 0;
}

/** Give back a path page `path_buf_get` took. */
void path_buf_put(char *buf)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (buf == path_pages[i]) {
            path_pages_out[i] = 0;
        }
    }
}

/** No row's path is relative. */
const char *proc_cwd(const struct process *p)
{
    (void)p;
    return "/";
}

/** The mount the model has, at the index a stat reports as its device. */
int vfs_mount_slots(void)
{
    return 1;
}

/** The model's one mount, at index 0. */
struct vfs_mount *vfs_mount_at(int idx)
{
    return idx == 0 ? &mount : 0;
}

/** The slot of `m` in the mount table: 0 for the model's one mount. */
int vfs_mount_index(const struct vfs_mount *m)
{
    return m == &mount ? 0 : -1;
}

/** The open model's descriptors never refer to exported dma-bufs. */
int is_dma_buf_file(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

/** No row opens a message queue. */
int mq_fd_ref(const struct fd_entry *e, struct vfs_ref *r)
{
    (void)e;
    (void)r;
    return 0;
}

/** No row opens a /proc/PID/mem. */
int procfs_is_mem(const struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** The device a stat reports for mount `i`: its index past the fixed ids,
 * as procfs gives a mount of no node of /dev. */
uint32_t procfs_mount_dev(int i, const struct vfs_mount *m)
{
    (void)m;
    return 0x10u + (uint32_t)i;
}

/** The clock, which no row reads. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 0;
    *nanoseconds = 0;
}

/** The boot time, which no row reads. */
int64_t rtc_boot_epoch(void)
{
    return 0;
}

/** No row opens a FIFO, a device node or a terminal, or checks a node of
 * the KRFFS root by its path. */
int32_t fifo_attach(struct fd_entry *e, const void *mnt, uint64_t ino, int flags)
{
    (void)e;
    (void)mnt;
    (void)ino;
    (void)flags;
    check(0, "unexpected FIFO open");
    return -ENXIO;
}

/** The device row's /dev name, "dri/card0"; any other row fails. */
const char *devfs_name_of(int block, uint32_t rdev)
{
    (void)block;
    (void)rdev;
    check(in_dev_open != 0, "unexpected device node open");
    return "dri/card0";
}

/** The device row's open of its device, which sleeps, running what the row
 * has happen meanwhile, and then finds the device gone: -ENXIO. Any other
 * row fails. */
int devfs_open(const char *name, int flags)
{
    void (*meanwhile)(void) = in_dev_open;

    (void)flags;
    check(meanwhile && __builtin_strcmp(name, "dri/card0") == 0, "unexpected device open");
    in_dev_open = 0;
    if (meanwhile) {
        meanwhile();
    }
    return -ENXIO;
}

/** Fail the row: no row looks up a terminal. */
struct pty *pty_get(int index)
{
    (void)index;
    check(0, "unexpected terminal lookup");
    return 0;
}

/** Fail the row: no row opens a terminal. */
int pty_open_slave(int index, int flags)
{
    (void)index;
    (void)flags;
    check(0, "unexpected terminal open");
    return -ENXIO;
}

/** No row creates a file on a mounted type: the one that could, an open
 * whose fault takes its descriptor after its walk, fails before it would. */
int vfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    (void)mnt;
    (void)dir;
    (void)name;
    (void)mode;
    (void)node;
    check(0, "unexpected create");
    return -EROFS;
}

/** Fail the row: no row opens the framebuffer by name. */
int32_t fb_take(struct fd_entry *e)
{
    (void)e;
    check(0, "unexpected framebuffer open");
    return -EBUSY;
}

/** Fail the row: no row seeks the framebuffer. */
uint32_t fb_size(void)
{
    check(0, "unexpected framebuffer seek");
    return 0;
}

/* The framebuffer's operations, which no row reaches. */
const struct file_ops fb_file_ops;

/** Fail the row: no row opens the mouse. */
bool mouse_is_available(void)
{
    check(0, "unexpected mouse open");
    return false;
}

/** Fail the row: no row opens the mouse. */
int input_mouse_present(void)
{
    check(0, "unexpected mouse open");
    return 0;
}

/** Fail the row: no row opens the mouse. */
void mouse_clear_packets(void)
{
    check(0, "unexpected mouse open");
}

/** Fail the row: no row reads the path of a node of the root. */
int krffs_node_path(krffs_node_t *n, char *buf, uint32_t size)
{
    (void)n;
    (void)buf;
    (void)size;
    check(0, "unexpected root node path");
    return -1;
}

/** Fail the row: no row resolves a path of the root by name. */
krffs_node_t *krffs_resolve(const char *path)
{
    (void)path;
    check(0, "unexpected root path resolution");
    return 0;
}

/** A KRFFS node's inode number, which no row reads. */
uint64_t krffs_node_ino(const krffs_node_t *node)
{
    (void)node;
    return 0;
}

/** A KRFFS node's type bits, which no row reads: its nodes are a mounted
 * type's. */
uint32_t krffs_mode_type(uint16_t mode)
{
    return mode & 0170000u;
}

/** Whether the row has a record lock somewhere, which is when a close
 * drops the closer's. */
int lock_any(void)
{
    return locking;
}

/** Record the end of a description's locks, for the orphan rows. */
void lock_ofd_gone(const struct open_file *ofd)
{
    ofd_gone = ofd;
}

/* The allocation that fails: 0 the next one, n the one after n more, and
 * -1 none. */
static int fail_after = -1;

/** Count host allocations, and fail the one `fail_after` names. */
void *kzalloc(size_t size)
{
    void *p;

    if (fail_after == 0) {
        return NULL;
    }
    if (fail_after > 0) {
        fail_after--;
    }
    p = calloc(1, size);
    if (p) {
        allocations++;
    }
    return p;
}

/** Count released allocations. */
void kfree(void *p)
{
    if (p) {
        allocations--;
        free(p);
    }
}

/** Identify a mounted type rather than the memory root. */
struct vfs_mount *vfs_root_mount(void)
{
    return &mount;
}

/* The resident-read row (`resident_unmapped`): set while it runs, the two
 * pages a shared mapping of its disk file holds in memory, the page
 * `kmap_temp` finds no free slot for, and the references its reads hold. */
static int resident_row;
static uint8_t resident_pages[2][PAGE_SIZE];
/* The unlocked open's row (`unlocked_open_one`): set while it runs, when the
 * name cache finishes the walk to `node` on a type that reads without the
 * big lock. */
static int unlocked_row;
static const struct vfs_type unlocked_type = {.name = "unlocked", .reads_unlocked = 1};
/* The big lock's model (`compat/bkl_held.c`): whether a row stands for a
 * caller without it, and the code's takes since the row cleared them. */
extern int bkl_host_free;
extern int bkl_host_takes;
static uintptr_t resident_fail;
static int resident_refs;
/* The race row (`resident_race`): set while it runs, an ext2 mount of a
 * file of two pages; whether the file page cache's model serves the read;
 * set once the other thread has mapped the file and stored into its second
 * page in memory, `resident_pages[1]`; and the disk's reads. */
static int race_row;
static struct vfs_mount race_mount;
static int race_cache;
static int race_mapped;
static int race_reads;
static void race_map(void);
static int mapped_write_row, mapped_write_ready;
static uint8_t mapped_disk[2 * PAGE_SIZE];

/** Report an accessible regular file with set-id bits, or in the mkdir rows
 * the directory a name is made in. A read the mount's lock does not cover
 * may sleep for the disk, and the call queued for the row, a write that
 * moves the file, runs then. */
int vfs_getattr(struct vfs_mount *mnt, void *at, struct vfs_stat *st)
{
    if (unlocked_row) {
        check(mnt == &mount && at == &node, "an unlocked open checks the file its walk found");
        k_memset(st, 0, sizeof(*st));
        st->mode = 0100644;
        st->ino = 78;
        return 0;
    }
    if (race_row) {
        check(mnt == &race_mount && at == &node, "a racing read sizes the file it reads");
        k_memset(st, 0, sizeof(*st));
        st->mode = 0100644;
        st->ino = 77;
        st->size = sizeof(resident_pages);
        return 0;
    }
    if (resident_row) {
        check(mnt == &mount && at == &node, "a resident read sizes the file it reads");
        k_memset(st, 0, sizeof(*st));
        st->mode = 0100644;
        st->size = sizeof(resident_pages);
        return 0;
    }
    if (mnt == &fl_mount) {
        return fl_getattr(mnt, at, st);
    }
    if (mnt == &dev_mount) {
        check(at == &dev_inode, "a device's reopen checks the node it was opened through");
        k_memset(st, 0, sizeof(*st));
        st->mode = 020666;
        st->rdev = 0xE200;
        return 0;
    }
    if (ns_depth == 0) {
        run_waiting();
    }
    k_memset(st, 0, sizeof(*st));
    if (mount.type == &pinning_type) {
        check(mnt == &mount && at == &node, "a chdir checks the directory its walk found");
        st->mode = 040755;
        return 0;
    }
    if (mkdir_row) {
        check(mnt == &mount && at == &root_dir,
              "a mkdir checks the directory it makes the name in");
        st->mode = 040755;
        return 0;
    }
    check(mnt == &mount && at == (relocate ? &moved : &node), "attributes use node identity");
    st->mode = 0106777;
    return getattr_error;
}

/** The mkdir rows have a root. */
int vfs_root_mounted(void)
{
    return 1;
}

/** Require the node the file is at and, except in the attribute rows,
 * set-id removal after successful truncation. */
int vfs_setattr(struct vfs_mount *mnt, void *at, int what, const struct vfs_stat *st)
{
    check(mnt == &mount && at == (relocate ? &moved : &node), "attributes use node identity");
    check(attr_row || (what == VFS_SET_MODE && !(st->mode & 06000)),
          "successful truncation clears set-id bits");
    attrs++;
    return 0;
}

/** The model has no extended attributes; keep the VFS default answers. */
int vfs_xattr(struct vfs_mount *mnt, void *node, struct vfs_xattr *arg)
{
    (void)mnt;
    (void)node;
    return arg->op == VFS_XATTR_LIST ? 0 : -EOPNOTSUPP;
}

/** Return an open identity distinct from the node. */
int vfs_open_node(struct vfs_mount *mnt, void *at, int flags, void **out)
{
    (void)flags;
    check(mnt == &mount && at == &node, "open uses node identity");
    check(installing < 0 || (fd_get(proc.fds, installing) == NULL &&
                             fd_slot(proc.fds, installing)->type == FD_TYPE_NONE &&
                             fd_slot(proc.fds, installing)->ofd),
          "an open's descriptor is a reservation while the file system opens it");
    if (open_error) {
        return open_error;
    }
    *out = &handle;
    opens++;
    return 0;
}

/** fallocate, which no row calls: EOPNOTSUPP. */
int vfs_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset, uint64_t len)
{
    (void)mnt;
    (void)handle;
    (void)mode;
    (void)offset;
    (void)len;
    return -EOPNOTSUPP;
}

/** The signal of a write past RLIMIT_FSIZE, which no row reaches. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)signo;
    (void)info;
}

/** The file page cache, which has no page in the rows. */
int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    return 0;
}

/** No change counted. */
uint32_t filecache_epoch(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** Keep no file, so that read(2) goes to the file system, but in the race
 * row with `race_cache`: then read the file a page at a time, each from
 * the disk (`vfs_read`), and before each page after the first end the read
 * when `cut` asks, as the cache does. */
int filecache_file_read(struct open_file *ofd, char __user *buf, uint32_t count, uint64_t offset,
                        int32_t *got, int (*cut)(const struct open_file *ofd))
{
    uint32_t done = 0;
    int32_t n = 0;

    if (!race_row || !race_cache) {
        return 0;
    }
    if (offset >= sizeof(resident_pages)) {
        *got = 0;
        return 1;
    }
    if (count > sizeof(resident_pages) - offset) {
        count = (uint32_t)(sizeof(resident_pages) - offset);
    }
    while (done < count) {
        uint32_t in = (uint32_t)((offset + done) % PAGE_SIZE);
        uint32_t want = PAGE_SIZE - in < count - done ? PAGE_SIZE - in : count - done;

        if (done > 0 && cut && cut(ofd)) {
            break;
        }
        n = vfs_read(ofd->vmnt, ofd->fhandle, (char __force *)buf + done, want, offset + done);
        if (n <= 0) {
            break;
        }
        done += (uint32_t)n;
    }
    *got = done > 0 || n > 0 ? (int32_t)done : n;
    return 1;
}

/** Keep no page. */
void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    (void)epoch;
}

/** Lend no page. */
uint32_t filecache_lend(const struct fd_entry *e, uint32_t first, uint32_t count, uintptr_t *phys)
{
    (void)e;
    (void)first;
    (void)count;
    (void)phys;
    return 0;
}

/** Keep no frame. */
void filecache_keep(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    (void)epoch;
}

/** Keep no file's pages. */
int filecache_keeps(const struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** Nothing is cached to forget. */
void filecache_forget_ino(const struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
}

/** Require a live open handle before truncation. */
int vfs_truncate(struct vfs_mount *mnt, void *at, uint64_t size)
{
    check(mnt == &mount && at == &handle && opens == releases + 1 && size == 0,
          "mounted truncation uses a live open handle");
    check(installing < 0 || (fd_get(proc.fds, installing) == NULL &&
                             fd_slot(proc.fds, installing)->type == FD_TYPE_NONE),
          "an open's descriptor is not published while it truncates");
    fd_ofd_repoint(mnt, &node, &moved);
    relocate = 1;
    truncates++;
    return truncate_error;
}

/** Count nothing: no walk here runs scoped. */
void vfs_tree_changed(void)
{}

/** Release a successfully opened handle, never more than were opened. */
void vfs_release(struct vfs_mount *mnt, void *at)
{
    check(mnt == &mount && at == &handle && opens > releases, "release balances successful open");
    releases++;
}

/** Record reference cleanup on failed open. */
void vfs_ref_release(struct vfs_ref *ref)
{
    if (ref->mnt == &fl_mount) {
        return;
    }
    if (ref->mnt == &dev_mount) {
        dev_holds--;
        dev_enters--;
        return;
    }
    check(ref->mnt == &mount, "failed open releases resolution");
    dropped++;
}

/** Record successful transfer into a description. */
void vfs_desc_open(struct vfs_mount *mnt)
{
    (void)mnt;
    refs++;
}

/** Start a description's cursors into the write-back errors at none,
 * which no row reports. */
void vfs_wb_err_sample(struct vfs_mount *mnt, uint32_t *file, uint32_t *fs)
{
    (void)mnt;
    *file = 0;
    *fs = 0;
}

/** Record completion of the resolution's mount operation. */
void vfs_leave(struct vfs_mount *mnt)
{
    if (mnt == &dev_mount) {
        dev_enters--;
        return;
    }
    leaves++;
}

/** Close the handle when the description is freed, which is where a release
 * sleeps: run what the row has happen meanwhile. The orphan rows'
 * descriptions open nothing on the root, and have nothing to close. */
void vfs_desc_close(struct vfs_mount *mnt, void *at, void *opened)
{
    if (mnt == &dev_mount) {
        check(at == &dev_inode && !opened, "a device's description keeps its node only");
        dev_holds--;
        refs--;
        return;
    }
    if (orphan_ofd) {
        return;
    }
    check(at == (relocate ? &moved : &node), "close retains current node identity");
    if (in_release) {
        void (*meanwhile)(void) = in_release;

        in_release = 0;
        meanwhile();
    }
    vfs_release(mnt, opened);
    refs--;
}

/** No row's file is held by a message in flight. */
int unix_inflight_holds(const void *node)
{
    (void)node;
    return 0;
}

/** Map a page of the resident-read row, the address being the page's, or
 * none for `resident_fail`; no other row reads, shares or cuts the pages of
 * a mapping. */
void *kmap_temp(uintptr_t phys)
{
    if (resident_row || race_mapped) {
        return phys == resident_fail ? NULL : (void *)phys;
    }
    check(0, "unexpected read of a mapping's page");
    return NULL;
}

/** Unmap nothing: `kmap_temp` maps nothing. */
void kunmap_temp(void *va)
{
    (void)va;
}

/* inotify, which no row watches, and the heap an open's name is kept in
 * for it. */
int inotify_watches;

/** Report no event: nothing is watched. */
void inotify_desc(struct open_file *ofd, uint32_t mask)
{
    (void)ofd;
    (void)mask;
}

/** No row's file system is watched. */
int inotify_mount_watched(const struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

/** No descriptor is an inotify instance. */
int inotify_is(int fd)
{
    (void)fd;
    return 0;
}

/** Add no watch: EINVAL. */
int32_t inotify_watch(int fd, struct vfs_mount *mnt, uint64_t ino, int isdir, uint32_t mask)
{
    (void)fd;
    (void)mnt;
    (void)ino;
    (void)isdir;
    (void)mask;
    return -EINVAL;
}

/** Allocate through `kzalloc`, which counts and can fail. */
void *kmalloc(size_t size)
{
    return kzalloc(size);
}

/** The pages of a shared mapping of a disk file in memory: the
 * resident-read row's two, and none in any other row, whose mappings are
 * private. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    if (resident_row && pgoff < 2) {
        return (uintptr_t)resident_pages[pgoff];
    }
    if (race_mapped && pgoff == 1) {
        return (uintptr_t)resident_pages[1];
    }
    return 0;
}

/** Whether the table has a page's leaf: never, since no row adds a page to
 * a disk file's shared mapping (`shmem_adopt`). */
int shmem_has_leaf(const struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    (void)pgoff;
    return 0;
}

/** Fail the row: no page joins a shared mapping. */
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    (void)sh;
    (void)pgoff;
    (void)phys;
    check(0, "unexpected page of a disk file's shared mapping");
    return 0;
}

/** Fail the row: no shared pages are truncated. */
int shmem_truncate(struct shmem *sh, uint64_t size)
{
    (void)sh;
    (void)size;
    check(0, "unexpected truncation of shared pages");
    return 0;
}

/** Fail the row: no frame is taken for a shared page. */
uintptr_t alloc_user_frame(void)
{
    check(0, "unexpected frame for a shared page");
    return 0;
}

/** Count a reference to a page of the resident-read row; fail any other
 * row, where no shared page gains one. */
void frame_ref(uintptr_t phys)
{
    (void)phys;
    check(resident_row || race_mapped, "unexpected reference to a shared page");
    resident_refs++;
}

/** Drop a reference to a page of the resident-read row; fail any other
 * row, where no shared page is freed. */
void free_frame(uintptr_t phys)
{
    (void)phys;
    check(resident_row || race_mapped, "unexpected free of a shared page");
    resident_refs--;
}

/** Fail the row: no row holds a mapping. */
void vm_file_get(struct vm_file *f)
{
    (void)f;
    check(0, "unexpected hold of a mapping");
}

/** Fail the row: no row holds a mapping. */
int vm_file_tryget(struct vm_file *f)
{
    (void)f;
    check(0, "unexpected hold of a mapping");
    return 0;
}

/** Fail the row: no row puts a mapping. */
void vm_file_put(struct vm_file *f)
{
    (void)f;
    check(0, "unexpected put of a mapping");
}

/** Read the race row's disk, whose first page is all 'a' and second all
 * 'b'; a read of the first page sleeps for the disk, and the other thread
 * maps the file and stores into its second page then (`race_map`). Fail
 * any other row: none reads a mapped file. */
int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset)
{
    uint32_t i;

    (void)handle;
    if (mapped_write_row) {
        check(offset + count <= sizeof(mapped_disk), "mapped write reads within its file");
        k_memcpy(buf, mapped_disk + (uint32_t)offset, count);
        return (int32_t)count;
    }
    if (!race_row) {
        check(0, "unexpected read of a mapped file");
        return -EIO;
    }
    check(mnt == &race_mount, "a racing read reads its own file");
    race_reads++;
    if (offset >= sizeof(resident_pages)) {
        return 0;
    }
    if (count > sizeof(resident_pages) - offset) {
        count = (uint32_t)(sizeof(resident_pages) - offset);
    }
    if (offset < PAGE_SIZE && !race_mapped) {
        race_map();
    }
    for (i = 0; i < count; i++) {
        ((char *)buf)[i] = offset + i < PAGE_SIZE ? 'a' : 'b';
    }
    return (int32_t)count;
}

/** Fail the row: no row writes a mapped file. */
int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset)
{
    (void)mnt;
    (void)handle;
    if (mapped_write_row) {
        check(ns_depth > 0, "a descriptor or core write holds the file lock");
        if (!mapped_write_ready) {
            return 0;
        }
        check(offset + count <= sizeof(mapped_disk), "the write fits the model disk");
        k_memcpy(mapped_disk + (uint32_t)offset, buf, count);
        return (int32_t)count;
    }
    check(0, "unexpected write of a mapped file");
    return -EIO;
}

/** A first write's buffer is absent until the row's fault makes a mapping. */
int vm_resident(uintptr_t start, size_t len, int write)
{
    (void)start;
    (void)len;
    (void)write;
    return !mapped_write_row || mapped_write_ready;
}

/** Fault the buffer while another thread maps and stores into the file. */
int vm_prefault_range(uintptr_t start, size_t len, int write)
{
    (void)start;
    (void)len;
    (void)write;
    if (mapped_write_row) {
        race_map();
        mapped_write_ready = 1;
    }
    return 1;
}

/** No row queues a real mapping's last reference. */
void vm_file_reap(void)
{}

/** No row queues a real mapping's zero publication. */
uint64_t vm_file_reap_start(void)
{
    return 0;
}

/** No row queues a real mapping's last reference. */
void vm_file_reap_until(uint64_t gen)
{
    (void)gen;
}

/** A quota with room, since no row maps a shared page. */
int shmem_quota_full(const struct shmem *sh)
{
    (void)sh;
    return 0;
}

/** Fail the row: no shared page is mapped. */
uintptr_t shmem_map_page(struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    (void)pgoff;
    check(0, "unexpected shared page");
    return 0;
}

/** Fail the row: no shared pages are released. */
void shmem_free(struct shmem *sh)
{
    (void)sh;
    check(0, "unexpected release of shared pages");
}

/** Fail the row: no shared pages are released. */
void shmem_free_yielding(struct shmem *sh)
{
    (void)sh;
    check(0, "unexpected release of shared pages");
}

/** Fail the row: no row unmaps a range. */
void unmap_user_range(page_dir_t *dir, uintptr_t start, uintptr_t end)
{
    (void)dir;
    (void)start;
    (void)end;
    check(0, "unexpected unmap");
}

/** An address space keeps `program`, a node of the root, as its program's
 * file. */
int mm_exe_holds(const struct vfs_mount *mnt, const void *n)
{
    return n != NULL && n == program && mnt == &mount;
}

/** Free an orphan of the KRFFS root, which only the orphan rows do, once the
 * closing description's locks are gone. The reclaim marks the extent free
 * and may then sleep reading a neighbouring header, and the call a row arms
 * for that runs meanwhile. */
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    (void)fs;
    check(orphan_ofd != 0 && node == &victim, "unexpected root node removal");
    check(ofd_gone == orphan_ofd,
          "a description's locks go before the orphan they are on is freed");
    removals++;
    if (ns_depth > 0) {
        removals_locked++;
    }
    if (in_reclaim) {
        void (*meanwhile)(void) = in_reclaim;
        process_t *self = current_proc;

        in_reclaim = 0;
        reclaiming = 1;
        reclaim_parked = !park_run(meanwhile);
        current_proc = self;
        reclaiming = 0;
    }
}

/** Fail the row: no row has a socket. */
void socket_ref(struct socket *sock)
{
    (void)sock;
    check(0, "unexpected socket reference");
}

/** Fail the row: no row has a socket. */
void socket_unref(struct socket *sock)
{
    (void)sock;
    check(0, "unexpected socket release");
}

/** Count the drops of a process's record locks, and whose they were. */
void lock_close(const struct lock_key *key)
{
    (void)key;
    lock_closes++;
    lock_closer = current_proc;
}

/** The clock, which no row reads. */
uint64_t tick_count(void)
{
    return 0;
}

/** The age of a shared mapping's written stores, which no row reads. */
void vfs_writeback_aged(const struct vfs_mount *mnt, uint64_t born)
{
    (void)mnt;
    (void)born;
}

/** The fsync of a file whose shared mapping is synced, which no row makes. */
int vfs_fsync(struct vfs_mount *mnt, void *handle, uint32_t *seen)
{
    (void)mnt;
    (void)handle;
    (void)seen;
    return 0;
}

/** The fdatasync of a file, which no row makes. */
int vfs_fdatasync(struct vfs_mount *mnt, void *handle, uint32_t *seen)
{
    (void)mnt;
    (void)handle;
    (void)seen;
    return 0;
}

/** The kernel log, which a failed write-back of a mapping writes to. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Initialize an unused wait queue. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/* The model's /dev/bootlog: the device descriptor with these operations, and
 * the bytes its ring holds. */
static const struct file_ops log_ops;
static uint32_t log_len;

/** The type of a device descriptor, which fallocate reads and no row has. */
uint32_t devfs_fd_mode(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** The device calls the dispatch of a transfer and a seek make: the one
 * device a row has is the log, and no row seeks another. */
int devfs_fd_is_raw(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** Fail the row: no row seeks a raw device window. */
int devfs_fd_raw_size(struct fd_entry *e, uint64_t *size)
{
    (void)e;
    (void)size;
    check(0, "unexpected raw window seek");
    return 0;
}

/** Whether `e` is the model's /dev/bootlog, with the ring's length in
 *  `size` when it is and `size` is not null. */
int devfs_fd_log_size(struct fd_entry *e, uint32_t *size)
{
    if (!e || e->type != FD_TYPE_DEVICE || e->ops != &log_ops) {
        return 0;
    }
    if (size) {
        *size = log_len;
    }
    return 1;
}

/** No row stats a name of /dev; `synth_stat`, which the check of a new
 * name reaches, links against it. */
int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino)
{
    (void)name;
    (void)mode;
    (void)rdev;
    (void)ino;
    check(0, "unexpected stat of a /dev name");
    return -ENOENT;
}

/* No row stats a file of /proc; `synth_stat` links against these. */
#define NO_PROC_INO(name)                            \
    uint32_t name(void)                              \
    {                                                \
        check(0, "unexpected stat of a /proc file"); \
        return 0;                                    \
    }
NO_PROC_INO(procfs_ino_audio)
NO_PROC_INO(procfs_ino_cmdline)
NO_PROC_INO(procfs_ino_console)
NO_PROC_INO(procfs_ino_dir)
NO_PROC_INO(procfs_ino_disk)
NO_PROC_INO(procfs_ino_filesystems)
NO_PROC_INO(procfs_ino_interrupts)
NO_PROC_INO(procfs_ino_loop)
NO_PROC_INO(procfs_ino_meminfo)
NO_PROC_INO(procfs_ino_cpuinfo)
NO_PROC_INO(procfs_ino_loadavg)
NO_PROC_INO(procfs_ino_diskstats)
NO_PROC_INO(procfs_ino_version)
NO_PROC_INO(procfs_ino_partitions)
NO_PROC_INO(procfs_ino_vmstat)
NO_PROC_INO(procfs_ino_slabinfo)
NO_PROC_INO(procfs_ino_swaps)
NO_PROC_INO(procfs_ino_locks)
NO_PROC_INO(procfs_ino_netstatdir)
NO_PROC_INO(procfs_ino_sysvipcdir)
NO_PROC_INO(procfs_ino_acpidir)
NO_PROC_INO(procfs_ino_acpicall)
NO_PROC_INO(procfs_ino_fsdir)
NO_PROC_INO(procfs_ino_lockwait)

/** No kind is a system-wide file of /proc here, so the opens and stats
 * that ask fall through to ENOENT. */
uint32_t procfs_ino_global(int kind)
{
    (void)kind;
    return 0;
}

/** Fail the row: no row stats a file of /proc. */
uint32_t procfs_ino_sysvipc(int origin)
{
    (void)origin;
    check(0, "unexpected stat of a /proc file");
    return 0;
}

/** No entry of /proc/sys is writable. */
int synth_sys_writable(int entry)
{
    (void)entry;
    return 0;
}

/** Remount nothing: EINVAL. */
int vfs_remount(struct vfs_mount *m, uint32_t flags, const char *options)
{
    (void)m;
    (void)flags;
    (void)options;
    return -EINVAL;
}

/** Fail the row: no row stats an entry of /proc/sys. */
uint32_t procfs_ino_sys(int entry)
{
    (void)entry;
    check(0, "unexpected stat of a /proc/sys entry");
    return 0;
}

NO_PROC_INO(procfs_ino_mounts)
NO_PROC_INO(procfs_ino_netarp)
NO_PROC_INO(procfs_ino_netdev)
NO_PROC_INO(procfs_ino_netdir)
NO_PROC_INO(procfs_ino_netroute)
NO_PROC_INO(procfs_ino_nettcp)
/** Fail the row: no row stats a file of /proc. */
uint32_t procfs_ino_net(int origin)
{
    (void)origin;
    check(0, "unexpected stat of a /proc file");
    return 0;
}
/** Fail the row: no row stats a file of /sys. */
uint32_t sysfs_ino(int node, int key, int file)
{
    (void)node;
    (void)key;
    (void)file;
    check(0, "unexpected stat of a /sys file");
    return 0;
}
/** Fail the row: no row stats a file of /sys. */
int sysfs_attr_writable(int node, int attr)
{
    (void)node;
    (void)attr;
    check(0, "unexpected stat of a /sys file");
    return 0;
}
/** No device of /sys exists. */
int sysfs_device_exists(int node, int key)
{
    (void)node;
    (void)key;
    return 0;
}
/** No row names a device: -ENOENT. */
int devfs_owner(const char *name, uint32_t *uid, uint32_t *gid)
{
    (void)name;
    (void)uid;
    (void)gid;
    return -ENOENT;
}

/** No row changes a device: -ENOENT. */
int32_t devfs_setattr(const char *name, uint32_t mode, uint32_t uid, uint32_t gid)
{
    (void)name;
    (void)mode;
    (void)uid;
    (void)gid;
    return -ENOENT;
}

/** The name of a device descriptor, which no row has: the empty name. */
const char *devfs_fd_name(struct fd_entry *e)
{
    (void)e;
    return "";
}

/** The owner of a device descriptor, which no row has: root. */
void devfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid)
{
    (void)e;
    *uid = 0;
    *gid = 0;
}

/** Every mount's id is 1, as the first slot's. */
uint32_t procfs_mount_id(const struct vfs_mount *m)
{
    (void)m;
    return 1;
}

/* The followed stat whose descriptor its owner closes as it runs
 * (`proc_fd_stat_closed`): the descriptor, -1 outside the row; the heap's
 * allocations once the close returned; the closes of the model pipe. */
static int stat_closed_fd = -1;
static int stat_closed_allocations, stat_pipe_closes;
static pipe_t stat_pipe;

/** Close `stat_closed_fd` once, as its owner would on another processor
 * while a followed stat reads it, and record the allocations left. */
static void stat_owner_closes(void)
{
    int fd = stat_closed_fd;

    stat_closed_fd = -1;
    check(close_fd(fd) == 0, "the owner closes the descriptor a followed stat reads");
    stat_closed_allocations = allocations;
}

/** Count a release of the model pipe of the followed stat's row. */
static void stat_pipe_close(fd_entry_t *e)
{
    check(e->priv == &stat_pipe, "the model pipe's release names it");
    stat_pipe_closes++;
}

static const struct file_ops stat_pipe_ops = {0, 0, 0, 0, stat_pipe_close, NULL, NULL, 0};

/** No synthetic file system is mounted: 0. A followed stat of a pipe asks
 * this after it reads the pipe, and its owner's close comes then. */
uint32_t procfs_synth_mount_id(uint64_t dev, const struct vfs_mount *hint)
{
    (void)dev;
    (void)hint;
    if (stat_closed_fd >= 0) {
        stat_owner_closes();
    }
    return 0;
}

/** Every attribute of a fixed /sys directory is there. */
int sysfs_attr_present(int node, int attr)
{
    (void)node;
    (void)attr;
    return 1;
}

/** No attribute of /sys is private. */
int sysfs_attr_private(int node, int attr)
{
    (void)node;
    (void)attr;
    return 0;
}
/** No device of /sys has a key: -1. */
int sysfs_device_key(int node, int n)
{
    (void)node;
    (void)n;
    return -1;
}
/** Fail the row: no row stats a driver's entry of /sys. */
uint32_t sysfs_node_mode(int handle)
{
    (void)handle;
    check(0, "unexpected stat of a /sys file");
    return 0;
}
/** No row's mount is debugfs. */
const struct vfs_type vfs_debugfs_type = {"debugfs", 0, 0, 0, 1};

/** Fail the row: no row opens a file of debugfs. */
int32_t debugfs_bind(void *node, fd_entry_t *e, int flags)
{
    (void)node;
    (void)e;
    (void)flags;
    check(0, "unexpected open of a debugfs file");
    return -EIO;
}

/** No PCI function of /sys exists, so none is a display. */
int sysfs_pci_is_vga(int key)
{
    (void)key;
    return 0;
}

/** No PCI function of /sys exists, so none has a BAR. */
uint64_t sysfs_pci_bar_size(int key, int bar)
{
    (void)key;
    (void)bar;
    return 0;
}

/** No PCI function of /sys exists, so it has none of its files. */
int sysfs_pci_attr_present(int key, int attr)
{
    (void)key;
    (void)attr;
    return 0;
}

/** No descriptor is of /proc/sys. */
int procfs_fd_is_sys(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** The path of an entry of /sys, which no row has: empty. */
void synth_sys_path(int node, int key, char *out, int cap)
{
    (void)node;
    (void)key;
    if (cap > 0) {
        out[0] = '\0';
    }
}
NO_PROC_INO(procfs_ino_statsys)
NO_PROC_INO(procfs_ino_uptime)
NO_PROC_INO(procfs_ino_usb)

/** The log is the model's one device, and its read moves the offset. */
int devfs_fd_moves_offset(struct fd_entry *e)
{
    return devfs_fd_log_size(e, NULL);
}

/** Fail the row: the log is the one device a row seeks. */
int devfs_lseek_zero(struct fd_entry *e)
{
    (void)e;
    check(0, "unexpected device seek");
    return 0;
}

/** No row opens /dev/mem: 0. */
int devfs_fd_is_mem(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** Fail the row: no row seeks a raw device window. */
int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence, uint64_t *out)
{
    (void)cur;
    (void)size;
    (void)delta;
    (void)whence;
    (void)out;
    check(0, "unexpected raw window seek");
    return -EINVAL;
}

/* Nonzero while `dir_wide_rows` runs, the answer its model `readdir` gives
 * and the cursor it was asked at. */
static int dir_rows, dir_answer;
static uint32_t dir_asked;

/** The listing of `dir_wide_rows`, which records the cursor it is asked at
 * and gives that row's answer; no other row lists a directory. */
int vfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    (void)mnt;
    (void)handle;
    (void)ent;
    if (dir_rows) {
        dir_asked = *cursor;
        return dir_answer;
    }
    check(0, "unexpected directory listing");
    return -EIO;
}

/** No row sleeps in epoll_wait, so a close finds no poller to let go of:
 * 1, as for a quiet one. */
int poll_let_go(process_t *t)
{
    (void)t;
    return 1;
}

/** No other processor claims the model's big lock. */
void bkl_yield(void)
{}

/** One processor: nothing to wait for. */
void arch_relax(void)
{}

/** No row sleeps in epoll_wait, so nothing is woken for a closed hold. */
void wq_wake_proc(process_t *p)
{
    (void)p;
}

/** Count a wake of the queue a parked call waits on. */
void wq_wake_all(wait_queue_t *wq)
{
    if (parked_on && wq == parked_on) {
        parked_wakes++;
    }
}

/**
 * A description wait: it ends when `cond` holds, and a call of another
 * process that a row runs in a sleep parks until the row resumes it, then
 * tests `cond` again. Any other wait is one that nothing can end, which
 * ends the run.
 */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    while (!cond(arg)) {
        if (!park_active()) {
            printf("open-hosttest: fail: a description wait that nothing can end\n");
            exit(1);
        }
        parked_on = wq;
        parked_wakes = 0;
        park();
    }
    return 0;
}

/**
 * Take the namespace lock, first running the call that holds it, which the
 * caller waits for, unless the caller holds the lock already. A call of
 * another process that a row runs in a sleep of the lock's holder parks
 * until the row resumes it, then tries again; any other wait for a lock
 * another process holds is one that nothing can end, which ends the run.
 */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    (void)mnt;
    if (ns_depth == 0 || ns_holder != current_proc) {
        run_waiting();
    }
    while (ns_depth > 0 && ns_holder != current_proc) {
        if (!park_active()) {
            printf("open-hosttest: fail: a namespace lock wait that nothing can end\n");
            exit(1);
        }
        parked_on = &ns_queue;
        parked_wakes = 0;
        park();
    }
    if (ns_depth++ == 0) {
        ns_takes++;
        ns_holder = current_proc;
    }
}

/** Require balanced namespace locking; the last release wakes a call that
 * waits for the lock. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    check(ns_depth > 0, "balanced namespace lock");
    if (ns_depth > 0 && --ns_depth == 0) {
        ns_holder = 0;
        wq_wake_all(&ns_queue);
    }
}

/** Find `victim`, `source` or `made_dir` by its name in the root directory. */
static int root_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **found)
{
    krffs_node_t *nodes[3];
    int i;

    nodes[0] = &victim;
    nodes[1] = &source;
    nodes[2] = &made_dir;
    check(mnt == &mount && dir == &root_dir, "a lookup in the root directory");
    check(ns_depth > 0, "a lookup in the root runs under the root's lock");
    if (lookup_take == 0) {
        lookup_take = ns_takes;
    }
    check(ns_takes == lookup_take,
          "the lookups, the count and the change run under one hold of the root's lock");
    for (i = 0; i < 3; i++) {
        if (nodes[i]->name[0] != '\0' &&
            __builtin_strcmp((const char *)nodes[i]->name, name) == 0) {
            *found = nodes[i];
            return 0;
        }
    }
    return -ENOENT;
}

/** Resolve a path of the root to the root directory and its last name,
 * taking the root's lock for a locked walk of a serialised type, as the
 * resolver does. A walk to the file itself finds the attribute rows' file,
 * under the same lock, as the resolver takes it before each lookup. */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t leafcap)
{
    const char *name = path;
    const char *p;

    (void)start;
    check(ns_depth == 0, "a walk starts with no namespace lock held");
    if (__builtin_strncmp(path, "/fl/", 4) == 0) {
        return fl_resolve(path, flags, out, leaf, leafcap);
    }
    if (!(flags & VFS_WALK_PARENT)) {
        if ((flags & VFS_WALK_LOCK) && mount.type->ns_serialised) {
            vfs_ns_lock(&mount);
        }
        out->mnt = &mount;
        out->node = relocate ? &moved : &node;
        return 0;
    }
    for (p = path; *p; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    check((flags & VFS_WALK_PARENT) && k_strlen(name) < leafcap, "a walk to a parent");
    k_strncpy(leaf, name, leafcap);
    out->mnt = &mount;
    out->node = &root_dir;
    if ((flags & VFS_WALK_LOCK) && mount.type->ns_serialised) {
        vfs_ns_lock(&mount);
    }
    return 0;
}

/** The name cache's walk: in the unlocked open's row, `node` on `mount`
 * with no name recorded; otherwise a walk it cannot finish. */
int vfs_resolve_cached(const char *path, int flags, struct vfs_ref *out, struct vfs_name **entry)
{
    (void)path;
    (void)flags;
    *entry = NULL;
    if (!unlocked_row) {
        return VFS_CACHED_MISS;
    }
    out->mnt = &mount;
    out->node = &node;
    return 0;
}

/** The working directory's path, which no unlocked row asks for, since the
 * unlocked open's row names an absolute path: "/". */
void proc_cwd_read(const struct process *p, char *out)
{
    (void)p;
    out[0] = '/';
    out[1] = '\0';
}

/** The walk with its path recorded, which the model does not record. */
int vfs_resolve_real(const char *path, int flags, struct vfs_ref *out, char *real)
{
    if (real) {
        real[0] = '\0';
    }
    return vfs_resolve(0, path, flags, out, 0, 0);
}

static void fl_dir_attrs(const struct vfs_ref *ref, struct vfs_stat *dir);

/** This resolver model has no captured entry; its original walk is
 * unchanged. The directory a file of /fl is in gives its attributes, and
 * any other path none. */
int vfs_resolve_entry(const char *path, int flags, struct vfs_ref *out, char *real,
                      struct vfs_name **entry, struct vfs_stat *dir)
{
    int rc;

    if (entry) {
        *entry = NULL;
    }
    rc = vfs_resolve_real(path, flags, out, real);
    if (rc == 0 && dir) {
        fl_dir_attrs(out, dir);
    }
    return rc;
}

/** Give back the lock a locked walk took. */
void vfs_walk_unlock(struct vfs_mount *mnt)
{
    if (mnt->type->ns_serialised) {
        vfs_ns_unlock(mnt);
    }
}

/** The root rows name no synthetic node and no mount point. */
const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** No name is a mount point. */
struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node, const char *name)
{
    if (mnt == &fl_mount) {
        return fl_leaf_mount(node, name);
    }
    return 0;
}

/** No row's directory is a process's root. */
int vfs_leaf_root(struct vfs_mount *mnt, void *node, const char *name)
{
    (void)mnt;
    (void)node;
    (void)name;
    return 0;
}

/* The pins the chdir row's directory has, and the pins it had while the
 * chdir slept between its walk and its record and when the working
 * directory recorded it; and what the working directory recorded. */
static int dir_pins, pins_asleep, pins_at_record;
static struct vfs_mount *cwd_mnt_recorded;
static void *cwd_node_recorded;

/** Pin the chdir row's directory, which the walk that found it must still
 * have locked, as a KRFFS volume pins a node for an O_PATH description. */
static int dir_pin(struct vfs_mount *mnt, void *at, void **token)
{
    check(mnt == &mount && at == &node && ns_depth > 0,
          "a chdir pins the node under the lock of the walk that found it");
    dir_pins++;
    *token = &dir_pins;
    return 0;
}

/** Let go of a pin `dir_pin` made. */
static void dir_unpin(struct vfs_mount *mnt, void *token)
{
    check(mnt == &mount && token == &dir_pins && dir_pins > 0, "an unpin matches a pin");
    dir_pins--;
}

/** Close the descriptor of a /proc directory that the followed stat's row
 * closes as the stat runs; fail the row for any other lookup. */
static void resolve_closes(const struct vpath *vp)
{
    if (stat_closed_fd >= 0 && vp->kind == VNODE_PROC_FD_DIR) {
        stat_owner_closes();
    } else {
        check(0, "unexpected /proc lookup");
    }
}

/** -1, the process gone: only the followed stat's row has a directory of
 * /proc (`resolve_closes`). The body stays a call and a constant, which the
 * compiler propagates, so the lookups that would follow a found process,
 * which no row reaches, and the procfs calls they make need no stub. */
int procfs_resolve_id(const struct vpath *vp)
{
    resolve_closes(vp);
    return -1;
}

/** Fail the row: no row's path is of /proc. */
int procfs_resolve_pid(int pid)
{
    (void)pid;
    check(0, "unexpected /proc lookup");
    return 0;
}

/** Fail the row: no row's path is of /proc. */
int procfs_may_read(int pid)
{
    (void)pid;
    check(0, "unexpected /proc lookup");
    return 0;
}

/** Fail the row: no row's path is of /proc. */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    (void)pid;
    (void)fd;
    (void)buf;
    (void)cap;
    check(0, "unexpected /proc lookup");
    return -ENOENT;
}

/** Fail the row: no row's path is of /proc. */
uint32_t procfs_ino_pid(int pid)
{
    (void)pid;
    check(0, "unexpected /proc lookup");
    return 0;
}

/** Fail the row: no row's path is of /proc. */
int procfs_pid_owner(int pid, uint32_t mode, uint32_t *uid, uint32_t *gid)
{
    (void)pid;
    (void)mode;
    (void)uid;
    (void)gid;
    check(0, "unexpected /proc lookup");
    return -ENOENT;
}

/** Find the model process for a followed /proc/PID/fd link. */
process_t *get_process(uint32_t pid)
{
    return pid == proc.pid ? &proc : NULL;
}

/** Fail the row: no row looks up a process of /proc. */
int procfs_oom_owner(int pid, uint32_t *uid, uint32_t *gid)
{
    (void)pid;
    (void)uid;
    (void)gid;
    check(0, "unexpected /proc lookup");
    return -ENOENT;
}

/** No descriptor is of /proc, so a lookup has no owner to set. */
void procfs_lookup_owner(const struct vpath *vp)
{
    (void)vp;
}

/** No descriptor is of /proc, so an open keeps no owner. */
void procfs_owner_keep(struct synth_owner *o, const struct vpath *vp)
{
    (void)o;
    (void)vp;
}

/** No descriptor is of /proc, so a close has no kept owner to drop. */
void procfs_owner_drop(struct synth_owner *o)
{
    (void)o;
}

/** No descriptor is of /proc: -1. */
int procfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid)
{
    (void)e;
    (void)uid;
    (void)gid;
    return -1;
}

/** No descriptor is a writable file of /proc. */
int procfs_fd_writable(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No descriptor has an inode number of /proc. */
uint32_t procfs_fd_ino(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No descriptor has a device number. */
uint32_t devfs_rdev(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No descriptor has an inode number of /dev. */
uint32_t devfs_fd_ino(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No descriptor is a terminal: -1. */
int pty_slave_index(struct fd_entry *e)
{
    (void)e;
    return -1;
}

/** No descriptor is a socket with an inode number. */
uint32_t unix_inode(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

/** No row's path names a link of /proc: every one is VNODE_KRFFS. */
int synth_classify(const char *path, struct vpath *vp)
{
    (void)path;
    vp->kind = VNODE_KRFFS;
    return vp->kind;
}

/** Fail the row: no row reaches a directory of /dev's table. Returns 0. */
int devfs_subdir_present(int row)
{
    (void)row;
    check(0, "unexpected directory of /dev");
    return 0;
}

/** Fail the row: no row reaches /dev's table of directories. Returns NULL. */
const struct synth_dev_dir *synth_dev_dir_at(int n)
{
    (void)n;
    check(0, "unexpected directory of /dev");
    return NULL;
}

/** No row names a directory of /dev's table: -1. */
int synth_dev_dir_find(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

/** Fail the row: no row names a synthetic directory. */
void synth_dir_path(int kind, int pid, int tid, char *out, int cap)
{
    (void)kind;
    (void)pid;
    (void)tid;
    (void)out;
    (void)cap;
    check(0, "unexpected synthetic directory path");
}

/** The working directory's path, which no row reads. */
int proc_cwd_set(struct process *p, const char *path)
{
    (void)p;
    (void)path;
    return 0;
}

/** Record the working directory, and the pins its node has then. */
void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *at)
{
    (void)p;
    cwd_mnt_recorded = mnt;
    cwd_node_recorded = at;
    pins_at_record = dir_pins;
}

/** No row's type names its nodes, and no row's process has a root of its
 * own. */
int vfs_node_path(struct vfs_mount *mnt, void *at, char *out, uint32_t cap)
{
    (void)mnt;
    (void)at;
    (void)out;
    (void)cap;
    check(0, "unexpected node path");
    return -ENOENT;
}

/** Count an operation on the device row's mount; fail any other row, which
 * enters no process's root. */
int vfs_enter(struct vfs_mount *mnt)
{
    if (mnt == &dev_mount) {
        dev_enters++;
        return 0;
    }
    check(0, "unexpected entry to a process's root");
    return -EBUSY;
}

/** Count a hold of the device row's node; fail any other row, which holds no
 * process's root. */
void vfs_ref_hold(const struct vfs_ref *r)
{
    if (r->mnt == &dev_mount) {
        check(r->node == &dev_inode, "a hold of the device's node");
        dev_holds++;
        return;
    }
    check(0, "unexpected hold of a process's root");
}

/** Whether a change to the root runs in the hold of the root's lock that
 * its lookups ran in, so that the holder count between them did too. */
static void changed_in_lookup_hold(void)
{
    check(ns_depth > 0 && ns_takes == lookup_take,
          "the lookups, the count and the change run under one hold of the root's lock");
}

/** Make `name` in the root directory. A name that is there fails, as it
 * does in the KRFFS core, with the errno its adapter gives every such
 * failure: ENOSPC on the root and EIO on a volume. */
int vfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    (void)mode;
    if (mnt == &fl_mount) {
        check(fl_holds(dir), "a mkdir holds its parent's lock");
        fl_note("mkdir");
        return fl_make(dir, name, 1);
    }
    check(mnt == &mount && dir == &root_dir, "the mkdir is in the root directory");
    changed_in_lookup_hold();
    if (made_dir.name[0] != '\0' && __builtin_strcmp((const char *)made_dir.name, name) == 0) {
        return mnt->type == &vfs_krffs_type ? -ENOSPC : -EIO;
    }
    k_strncpy((char *)made_dir.name, name, sizeof(made_dir.name));
    made_dir.type = KRFFS_RESERVED_NODE;
    made_dir.mode = KRFFS_S_IFDIR | 0755;
    return 0;
}

/** Remove the directory "made", as the root does once the checks of the
 * syscall layer have passed. */
int vfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    if (mnt == &fl_mount) {
        check(fl_holds(dir), "an rmdir holds its parent's lock");
        return fl_remove(dir, name, 1);
    }
    check(mnt == &mount && dir == &root_dir && __builtin_strcmp(name, "made") == 0,
          "the rmdir removes the name it was given");
    changed_in_lookup_hold();
    made_dir.name[0] = '\0';
    removals++;
    return 0;
}

/** Free the file an unlink takes the name of, as the root does. */
int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name)
{
    if (mnt == &fl_mount) {
        check(fl_holds(dir), "an unlink holds its parent's lock");
        return fl_remove(dir, name, 0);
    }
    check(mnt == &mount && dir == &root_dir && __builtin_strcmp(name, "kept") == 0,
          "the unlink removes the name it was given");
    changed_in_lookup_hold();
    removals++;
    return 0;
}

/** Rename in the root directory, freeing a destination that still has its
 * name, as the root does. */
int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
               const char *newname)
{
    if (mnt == &fl_mount) {
        check(fl_holds(olddir) && fl_holds(newdir), "a rename holds both directories' locks");
        return fl_move(olddir, oldname, newdir, newname);
    }
    check(mnt == &mount && olddir == &root_dir && newdir == &root_dir &&
              __builtin_strcmp(oldname, "src") == 0 && __builtin_strcmp(newname, "kept") == 0,
          "the rename moves the names it was given");
    changed_in_lookup_hold();
    if (victim.name[0] != '\0') {
        removals++;
    }
    return 0;
}

/** No row stamps a node of the KRFFS root: the attribute rows' file is on
 * a mounted type. */
void krffs_stamp(krffs_node_t *n, int what)
{
    (void)n;
    (void)what;
    check(0, "unexpected root node stamp");
}

/** Take the name off a node a descriptor holds, keeping the node. */
void krffs_unlink_held(krffs_node_t *n)
{
    changed_in_lookup_hold();
    n->name[0] = '\0';
    keeps++;
}

/** Take the name off a node a holder keeps, keeping the node. */
void krffs_orphan_node(krffs_node_t *n)
{
    changed_in_lookup_hold();
    n->name[0] = '\0';
    keeps++;
}

/** Check successful installation and both failure paths. */
static void truncation(void)
{
    int mode, rc;
    struct vfs_ref ref;

    for (mode = 0; mode < 4; mode++) {
        proc.fds = &files;
        proc.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
        check(fd_table_init(proc.fds) == 0, "initialize descriptor table");
        proc.euid = 1;
        opens = releases = refs = leaves = dropped = truncates = attrs = relocate = 0;
        open_error = mode == 1 ? -ENOMEM : 0;
        truncate_error = mode == 2 ? -EIO : 0;
        ref.mnt = &mount;
        ref.node = &node;
        check(open_mounted_node(&ref, 2 | O_TRUNC) == 0 && truncates == 0,
              "mounted node checks do not truncate before open");
        installing = fd_alloc(proc.fds);
        rc = open_install(&ref, installing, "target", 2 | (mode == 3 ? 0 : O_TRUNC));
        check(rc < 0 || (rc == installing && fd_get(proc.fds, rc) == fd_slot(proc.fds, rc)),
              "a finished open publishes the descriptor it reserved");
        installing = -1;
        if (mode == 0 || mode == 3) {
            check(rc >= 0 && refs == 1 && leaves == 1 && dropped == 0,
                  "successful open transfers reference");
            check(truncates == (mode == 0) && attrs == (mode == 0),
                  "only O_TRUNC truncates and clears set-id bits");
            if (rc >= 0) {
                fd_free(proc.fds, rc);
            }
        } else {
            check(rc == (mode == 1 ? -ENOMEM : -EIO) && refs == 0 && leaves == (mode == 2) &&
                      dropped == (mode == 1),
                  "failed open returns error and releases resolution");
            check(allocations == 1 && attrs == 0,
                  "failed open frees description without changing mode");
        }
        check(opens == releases && refs == 0, "every open handle released");
        fd_table_free(proc.fds);
        check(allocations == 0, "all descriptor storage released");
    }
}

/** A fresh table of one thread for a row, with the counters at zero. */
static void row_begin(void)
{
    proc.fds = &files;
    check(fd_table_init(proc.fds) == 0, "initialize descriptor table");
    files.refs = 1;
    proc.euid = 1;
    opens = releases = refs = leaves = dropped = truncates = attrs = relocate = 0;
    open_error = truncate_error = getattr_error = 0;
    locking = 1;
    lock_closes = 0;
    lock_closer = 0;
}

/** Free what the row left open and require every allocation back. */
static void row_end(void)
{
    int fd;

    for (fd = 0; fd < fd_table_cap(proc.fds); fd++) {
        if (fd_slot(proc.fds, fd)->type != FD_TYPE_NONE || fd_slot(proc.fds, fd)->ofd) {
            fd_free(proc.fds, fd);
        }
    }
    check(opens == releases && refs == 0, "every open handle released");
    fd_table_free(proc.fds);
    check(allocations == 0, "all descriptor storage released");
    check(!path_pages_out[0] && !path_pages_out[1] && !path_pages_out[2] && !path_pages_out[3],
          "every path page given back");
    k_memset(&other, 0, sizeof(other));
    k_memset(&third, 0, sizeof(third));
    k_memset(proc_held, 0, sizeof(proc_held));
    k_memset(other_held, 0, sizeof(other_held));
    k_memset(third_held, 0, sizeof(third_held));
    give_maps();
    proc.fd_holding = 0;
    proc.next = 0;
    proc_list = 0;
    current_proc = &proc;
    releasing = -1;
    in_release = 0;
    locking = 0;
}

/** A row on the KRFFS root, whose file `victim` is named "kept" and the file
 * `source` "src", with the calling process privileged. */
static void root_row_begin(void)
{
    row_begin();
    proc.euid = 0;
    proc_list = &proc;
    krffs_fs = &root_fs;
    root_fs.node = &root_dir;
    mount.type = &vfs_krffs_type;
    k_memset(&victim, 0, sizeof(victim));
    k_memset(&source, 0, sizeof(source));
    victim.type = source.type = KRFFS_RESERVED_NODE;
    victim.mode = source.mode = 0100644;
    k_strncpy((char *)victim.name, "kept", sizeof(victim.name));
    k_strncpy((char *)source.name, "src", sizeof(source.name));
    removals = keeps = 0;
    ns_takes = lookup_take = 0;
}

/** End a root row with the root's lock given back. */
static void root_row_end(void)
{
    check(ns_depth == 0 && !waiting, "the root's namespace lock is given back");
    waiting = 0;
    ns_depth = 0;
    krffs_fs = 0;
    mount.type = &mounted_type;
    row_end();
}

/** The open a root row's call waits for: it has walked to "kept" holding
 * the root's lock, and publishes its descriptor before it lets the lock go. */
static void open_publishes(void)
{
    int fd = fd_alloc(proc.fds);

    check(fd >= 0, "the waiting open reserves a descriptor");
    if (fd >= 0) {
        fd_slot(proc.fds, fd)->node = &victim;
        fd_slot(proc.fds, fd)->type = FD_TYPE_FILE;
    }
}

/**
 * An unlink on the KRFFS root of a file an open is installing, the open
 * holding the root's lock in its walk: the unlink counts the file's holders
 * only once it has that lock, so it counts the open's descriptor and keeps
 * the file, nameless, for it.
 */
static void unlink_installing(void)
{
    root_row_begin();
    waiting = open_publishes;
    check(do_unlink("/kept") == 0, "an unlink of a file an open is installing");
    check(keeps == 1 && removals == 0, "the unlink keeps the file for the open's descriptor");
    root_row_end();
}

/**
 * A rename on the KRFFS root over a file an open is installing: the rename
 * counts the replaced file's holders under the root's lock, so it takes the
 * file's name and leaves the node to the open's descriptor.
 */
static void rename_over_installing(void)
{
    root_row_begin();
    waiting = open_publishes;
    check(do_rename("/src", "/kept", 0, 0) == 0, "a rename over a file an open is installing");
    check(keeps == 1 && removals == 0,
          "the rename keeps the file it replaces for the open's descriptor");
    root_row_end();
}

/**
 * An unlink, and a rename over the file, of a file of the KRFFS root that an
 * address space keeps as its program: the program is one of the file's
 * holders, so each takes the name and leaves the node, as for a descriptor.
 */
static void program_kept(void)
{
    root_row_begin();
    program = &victim;
    check(do_unlink("/kept") == 0 && keeps == 1 && removals == 0,
          "an unlink keeps the file a program runs from");
    root_row_end();
    root_row_begin();
    program = &victim;
    check(do_rename("/src", "/kept", 0, 0) == 0 && keeps == 1 && removals == 0,
          "a rename over the file a program runs from keeps it");
    program = NULL;
    root_row_end();
}

static int orphan_desc(int fifo);

/* The descriptor of the program's file the second case of `program_reclaim`
 * closes, and the frees of the file there were when its close ended. */
static int program_fd = -1;
static int program_close_removals = -1;

/** The close of the program's file's descriptor by a process that waited
 * for the root's lock while the release of the program's file held it. */
static void program_desc_closes(void)
{
    orphan_ofd = fd_slot(proc.fds, program_fd)->ofd;
    check(close_fd(program_fd) == 0, "the close of the program file's descriptor");
    program_close_removals = removals;
}

/**
 * The release, by the last address space that kept it, of a program's file of
 * the KRFFS root that lost its name and that nothing else holds: it takes the
 * root's lock (`fs_exe_lock`), frees the node with the lock held, as an
 * unlink does, since the reclaim may sleep in the page cache and no
 * allocation may take the extent meanwhile, and gives the lock back. With a
 * descriptor of the file open, whose close waits for the lock while the
 * release holds it, the release counts the descriptor in the hold it took
 * before its address space left the holders, and leaves the file to the
 * close, which frees it once.
 */
static void program_reclaim(void)
{
    struct open_file *ofd;
    struct vfs_mount *locked;
    int kept;

    for (kept = 0; kept < 2; kept++) {
        root_row_begin();
        victim.name[0] = '\0';
        ofd = fd_ofd_new();
        check(ofd != NULL, "a description of the program's file");
        if (ofd) {
            program_fd = kept ? orphan_desc(0) : -1;
            program_close_removals = -1;
            ofd->vmnt = &mount;
            ofd->vnode = &victim;
            orphan_ofd = ofd;
            ofd_gone = 0;
            removals_locked = 0;
            locked = fs_exe_lock(ofd);
            check(locked == &mount && ns_depth == 1,
                  "the release of a program's file takes the lock");
            if (kept) {
                waiting = program_desc_closes;
            }
            fs_exe_close(ofd, locked);
            if (kept) {
                check(program_close_removals == -1 && removals == 0,
                      "a program's release counts the file's descriptor in the hold it took "
                      "before it left the holders");
                run_waiting();
                check(program_close_removals == 1,
                      "and the close of the descriptor frees the file");
            } else {
                check(removals == 1 && removals_locked == 1,
                      "and frees the nameless file under it");
            }
            check(ns_depth == 0, "and gives the lock back");
            orphan_ofd = 0;
        }
        program_fd = -1;
        root_row_end();
    }
}

/** The mkdir a row's call waits for: another process has made the
 * directory "made" while it held the lock the call waits for. */
static void other_makes_it(void)
{
    k_strncpy((char *)made_dir.name, "made", sizeof(made_dir.name));
    made_dir.type = KRFFS_RESERVED_NODE;
    made_dir.mode = KRFFS_S_IFDIR | 0755;
}

/** Begin a mkdir row on the root, or with `mounted` on a serialised mounted
 * type, with no directory made and a process whose umask is 027. */
static void mkdir_row_begin(int mounted)
{
    root_row_begin();
    if (mounted) {
        mount.type = &mkdir_type;
    }
    k_memset(&made_dir, 0, sizeof(made_dir));
    k_memset(&row_fs, 0, sizeof(row_fs));
    row_fs.umask = 027;
    proc.fs = &row_fs;
    proc.egid = 5;
    mkdir_row = 1;
}

/** End a mkdir row. */
static void mkdir_row_end(void)
{
    mkdir_row = 0;
    proc.fs = 0;
    proc.egid = 0;
    root_row_end();
}

/**
 * A mkdir of a name another process makes while this one waits for the
 * namespace lock, on the KRFFS root and on a serialised mounted type. The
 * lookup runs under the lock with the creation, so the name is found and
 * the answer is EEXIST, where the file system's own check of the name would
 * give the errno it gives a full volume on the root (ENOSPC) and a failing
 * one on a KRFFS volume (EIO). With no other process, the directory is
 * made, and on the root stamped with its maker's group and the mode less
 * the umask.
 */
static void mkdir_race(void)
{
    int mounted;

    for (mounted = 0; mounted < 2; mounted++) {
        mkdir_row_begin(mounted);
        waiting = other_makes_it;
        check(do_mkdir("/made", 0777) == -EEXIST,
              "a mkdir of a name another process made while it waited is EEXIST");
        mkdir_row_end();

        mkdir_row_begin(mounted);
        check(do_mkdir("/made", 0777) == 0 && made_dir.name[0] != '\0',
              "a mkdir with nobody before it makes the directory");
        check(mounted || (made_dir.gid == 5 && (made_dir.mode & 07777) == 0750),
              "the directory a mkdir makes on the root has its maker's group and mode");
        mkdir_row_end();
    }
}

/**
 * The last close of a file of the KRFFS root that lost its name while open,
 * and of the last end of such a FIFO: the node is freed, and the locks of
 * the closing description go before it is, since a lock of the root is
 * keyed by its node's place and a node made there later must meet none of
 * them.
 */
static void orphan_locks(void)
{
    int fifo;

    for (fifo = 0; fifo < 2; fifo++) {
        fd_entry_t *e;
        int fd;

        root_row_begin();
        fd = fd_alloc(proc.fds);
        check(fd >= 0, "a descriptor of the orphan");
        if (fd < 0) {
            root_row_end();
            continue;
        }
        e = fd_slot(proc.fds, fd);
        victim.name[0] = '\0';
        if (fifo) {
            victim.mode = KRFFS_S_IFIFO | 0600;
            e->type = FD_TYPE_PIPE;
        } else {
            e->type = FD_TYPE_FILE;
            e->node = &victim;
        }
        e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
        e->ofd->vmnt = &mount;
        e->ofd->vnode = &victim;
        orphan_ofd = e->ofd;
        ofd_gone = 0;
        check(close_fd(fd) == 0 && removals == 1, "the last close of an orphan frees it");
        orphan_ofd = 0;
        root_row_end();
    }
}

/** A descriptor of this process on the directory "made" of the KRFFS root,
 * opened for reading or, with `path`, with O_PATH, as an open installs one;
 * its number. */
static int made_desc(int path)
{
    int fd = fd_alloc(proc.fds);
    fd_entry_t *e;

    check(fd >= 0, "a descriptor on the directory");
    if (fd < 0) {
        return fd;
    }
    e = fd_slot(proc.fds, fd);
    e->ofd->vmnt = &mount;
    e->ofd->vnode = &made_dir;
    e->node = &made_dir;
    e->flags = path ? FD_FLAG_PATH : FD_FLAG_READ;
    e->type = FD_TYPE_DIR;
    return fd;
}

/** Close a descriptor `made_desc` made, whose description opened nothing
 * on the root and so has nothing for the file system to close. */
static void made_desc_close(int fd)
{
    if (fd >= 0) {
        fd_slot(proc.fds, fd)->ofd->vmnt = 0;
        fd_free(proc.fds, fd);
    }
}

/* The descriptor the open an rmdir row's call waits for publishes. */
static int published = -1;

/** The open an rmdir row's call waits for: it has walked to the directory
 * "made" holding the root's lock, and publishes an O_PATH descriptor on it
 * before it lets the lock go. */
static void open_dir_publishes(void)
{
    published = made_desc(1);
}

/** Begin an rmdir row on the root, whose directory "made" nothing uses. */
static void rmdir_row_begin(void)
{
    root_row_begin();
    k_memset(&made_dir, 0, sizeof(made_dir));
    k_strncpy((char *)made_dir.name, "made", sizeof(made_dir.name));
    made_dir.type = KRFFS_RESERVED_NODE;
    made_dir.mode = KRFFS_S_IFDIR | 0755;
    k_memset(&row_fs, 0, sizeof(row_fs));
    k_memset(&other_fs, 0, sizeof(other_fs));
}

/** End an rmdir row. */
static void rmdir_row_end(void)
{
    proc.fs = 0;
    root_row_end();
}

/**
 * An rmdir on the KRFFS root of a directory in use is EBUSY, and the
 * directory stays: a descriptor has it, opened for reading or with O_PATH;
 * a description in flight has it with no descriptor; it is the working
 * directory of the caller or of another process; or an open publishes a
 * descriptor on it while the rmdir waits for the root's lock, which the
 * rmdir sees only because it looks under that lock. A descriptor of the
 * root finds its directory by the node's place, and a directory freed under
 * one would leave it on whatever node the allocator put there next. A
 * directory nothing uses is removed.
 */
static void rmdir_in_use(void)
{
    struct open_file *flight;
    int path;
    int other_cwd;
    int fd;

    rmdir_row_begin();
    check(do_rmdir("/made") == 0 && removals == 1,
          "an rmdir of a directory nothing uses removes it");
    rmdir_row_end();

    for (path = 0; path < 2; path++) {
        rmdir_row_begin();
        fd = made_desc(path);
        check(do_rmdir("/made") == -EBUSY && removals == 0,
              path ? "an rmdir of a directory an O_PATH descriptor has is EBUSY"
                   : "an rmdir of a directory a descriptor has open is EBUSY");
        made_desc_close(fd);
        rmdir_row_end();
    }

    rmdir_row_begin();
    flight = fd_ofd_new();
    check(flight != 0, "a description in flight");
    if (flight) {
        flight->vmnt = &mount;
        flight->vnode = &made_dir;
        check(do_rmdir("/made") == -EBUSY && removals == 0,
              "an rmdir of a directory a description in flight has is EBUSY");
        flight->vmnt = 0;
        fd_ofd_unref(flight);
    }
    rmdir_row_end();

    for (other_cwd = 0; other_cwd < 2; other_cwd++) {
        struct fs_struct *fs = other_cwd ? &other_fs : &row_fs;

        rmdir_row_begin();
        fs->cwd_mnt = &mount;
        fs->cwd_node = &made_dir;
        if (other_cwd) {
            other.fs = fs;
            proc.next = &other;
        } else {
            proc.fs = fs;
        }
        check(do_rmdir("/made") == -EBUSY && removals == 0,
              other_cwd ? "an rmdir of another process's working directory is EBUSY"
                        : "an rmdir of the caller's working directory is EBUSY");
        rmdir_row_end();
    }

    rmdir_row_begin();
    waiting = open_dir_publishes;
    check(do_rmdir("/made") == -EBUSY && removals == 0,
          "an rmdir of a directory an open publishes a descriptor on while it waits is EBUSY");
    made_desc_close(published);
    published = -1;
    rmdir_row_end();
}

/** The call that runs while the chdir row's chdir sleeps for the disk
 * between its walk and its record, where an rmdir would look for the pin. */
static void chdir_sleeps(void)
{
    pins_asleep = dir_pins;
}

/** The call that holds the lock the chdir row's walk takes last; the
 * chdir's next sleep comes after its walk. */
static void chdir_walks(void)
{
    waiting = chdir_sleeps;
}

/**
 * A chdir into a directory of a type that keeps a node only while it is
 * open, as a KRFFS volume does. The walk's reference holds nothing there,
 * and the checks before the record may sleep for the disk, so the chdir
 * pins the node under the lock its walk ends with and lets the pin go once
 * the working directory records the node: an rmdir while the chdir sleeps
 * finds the pin, and one after it finds the working directory.
 */
static void chdir_pins(void)
{
    char path[] = "/dir";

    row_begin();
    mount.type = &pinning_type;
    k_memset(&row_fs, 0, sizeof(row_fs));
    proc.fs = &row_fs;
    dir_pins = pins_asleep = pins_at_record = 0;
    cwd_mnt_recorded = NULL;
    cwd_node_recorded = NULL;
    waiting = chdir_walks;
    check(do_chdir(path) == 0, "a chdir into a directory of a pinning type");
    check(pins_asleep == 1, "the directory is pinned while the chdir sleeps before its record");
    check(cwd_mnt_recorded == &mount && cwd_node_recorded == &node && pins_at_record == 1,
          "the directory is pinned when the working directory records it");
    check(dir_pins == 0 && ns_depth == 0 && !waiting, "the chdir lets its pin and the lock go");
    waiting = 0;
    proc.fs = 0;
    mount.type = &mounted_type;
    row_end();
}

/** The write a row's call waits for: it holds the mount's lock and moves
 * the file, as a growing write on a mounted KRFFS volume does, and repoints
 * the descriptions open on it. */
static void write_moves(void)
{
    fd_ofd_repoint(&mount, &node, &moved);
    relocate = 1;
}

/**
 * A chmod and a chown by path of a file on a mounted type whose uses are
 * serialised, which a write holding the mount's lock moves: each must read
 * and change the node the file is at once it has the lock, since its walk
 * takes that lock and keeps it through the store.
 */
static void attrs_after_move(void)
{
    int round;

    for (round = 0; round < 2; round++) {
        row_begin();
        proc.euid = 0;
        mount.type = &locked_type;
        attr_row = 1;
        waiting = write_moves;
        check((round == 0 ? do_chmod("f", 0600, 0, 1) : do_chown("f", 5, (uint32_t)-1, 1, 0)) == 0,
              "a chmod or chown of a file a write moves");
        check(attrs == 1 && ns_depth == 0 && !waiting,
              "the change is made once, with the mount's lock given back");
        attr_row = 0;
        waiting = 0;
        ns_depth = 0;
        mount.type = &mounted_type;
        row_end();
    }
}

/**
 * A stat64, an old stat, an fstatat and an access by path of a file on a
 * mounted type whose uses are serialised, which a write holding the mount's
 * lock moves: each must read the node the file is at once it has the lock,
 * since its walk takes that lock and keeps it until the node is read.
 * Without it the read sleeps outside the lock, and the write moves the file
 * from under it.
 */
static void stats_after_move(void)
{
    struct user_stat wide;
    struct kstat filled;
    struct linux_stat old;
    int round;
    int rc;

    for (round = 0; round < 4; round++) {
        row_begin();
        proc.euid = 0;
        mount.type = &locked_type;
        waiting = write_moves;
        if (round == 0) {
            rc = do_stat64("/f", &wide, 1);
        } else if (round == 1) {
            rc = do_old_stat("/f", &old, 1);
        } else if (round == 2) {
            rc = fstatat_path("/f", 1, &filled);
        } else {
            rc = do_access("f", R_OK, 0, 0);
        }
        check(rc == 0, "a stat or an access of a file a write moves");
        check(ns_depth == 0 && !waiting, "the node is read with the mount's lock, then given back");
        waiting = 0;
        ns_depth = 0;
        mount.type = &mounted_type;
        row_end();
    }
}

/** A descriptor on the model file, open for reading and writing. */
static int model_open(void)
{
    struct vfs_ref ref;

    ref.mnt = &mount;
    ref.node = &node;
    int fd = fd_alloc(proc.fds);

    return fd < 0 ? fd : open_install(&ref, fd, "target", 2);
}

/**
 * A followed /proc/PID/fd link returns a mounted file's getattr error and
 * releases its temporary description reference on success and failure.
 * Both file and directory descriptors use this path.
 */
static void proc_fd_stat_errors(void)
{
    int directory;

    for (directory = 0; directory < 2; directory++) {
        struct kstat st;
        struct open_file *ofd;
        int fd;

        row_begin();
        fd = model_open();
        check(fd >= 0, "open a descriptor for a followed stat");
        if (fd >= 0) {
            fd_slot(proc.fds, fd)->type = directory ? FD_TYPE_DIR : FD_TYPE_FILE;
            ofd = fd_slot(proc.fds, fd)->ofd;
            getattr_error = -EIO;
            check(synth_fd_stat((int)proc.pid, fd, &st) == -EIO,
                  "a followed descriptor stat returns the getattr error");
            check(ofd->refcount == 1 && releases == 0,
                  "a failed followed stat releases only its temporary reference");
            getattr_error = 0;
            check(synth_fd_stat((int)proc.pid, fd, &st) == 0 && st.st_mode == 0106777,
                  "a successful followed stat reports the file's attributes");
            check(ofd->refcount == 1 && releases == 0,
                  "a successful followed stat releases only its temporary reference");
        }
        row_end();
    }
}

/**
 * A followed /proc/PID/fd link's stat of a pipe, and its stat of an O_PATH
 * descriptor of a /proc/PID/fd directory whose process is gone, which fails
 * with -ENOENT, hold the description while the owner closes the descriptor
 * as the stat runs, since the last close of a pipe frees what it names
 * without the big lock, as Linux's proc_fd_getattr holds the file
 * (fs/proc/fd.c:182 in 6.12): the close frees nothing, and the stat's one
 * drop of its hold ends the description and the pipe or the path once.
 */
static void proc_fd_stat_closed(void)
{
    int failing;

    for (failing = 0; failing < 2; failing++) {
        struct kstat st;
        struct synth_path *sp;
        fd_entry_t *e;
        int fd, before;
        int32_t rc;

        row_begin();
        fd = fd_alloc(proc.fds);
        sp = failing ? kzalloc(sizeof(*sp)) : NULL;
        check(fd >= 0 && (sp || !failing), "a descriptor for a followed stat that a close meets");
        if (fd < 0 || (failing && !sp)) {
            kfree(sp);
            row_end();
            continue;
        }
        e = fd_slot(proc.fds, fd);
        if (failing) {
            sp->vp.kind = VNODE_PROC_FD_DIR;
            e->type = FD_TYPE_SYNTH_PATH;
            e->flags = FD_FLAG_PATH;
            e->priv = sp;
        } else {
            stat_pipe.inode = 77;
            e->type = FD_TYPE_PIPE;
            e->flags = FD_FLAG_READ;
            e->ops = &stat_pipe_ops;
            e->priv = &stat_pipe;
        }
        fd_install(proc.fds, fd);
        stat_pipe_closes = 0;
        stat_closed_fd = fd;
        before = allocations;
        rc = synth_fd_stat((int)proc.pid, fd, &st);
        check(failing ? rc == -ENOENT : rc == 0 && st.st_mode == 010666 && st.st_ino == 77,
              failing ? "a followed stat of a gone process's fd directory is ENOENT"
                      : "a followed stat reports the pipe");
        check(stat_closed_fd < 0 && stat_closed_allocations == before,
              "a close while a followed stat runs frees nothing the stat holds");
        check(allocations < before && fd_slot(proc.fds, fd)->type == FD_TYPE_NONE,
              "the stat's drop of its hold ends the closed description");
        check(stat_pipe_closes == !failing, "the pipe is released once, after the stat");
        stat_closed_fd = -1;
        row_end();
    }
}

/**
 * A utimensat by path and one by descriptor of a file on a mounted type
 * whose uses are serialised, which a write holding the mount's lock moves
 * while the call waits for that lock: the path's walk takes the lock, and
 * the descriptor's node is read once the lock is held, so each sets the
 * times of the node the file is at.
 */
static void times_after_move(void)
{
    int round;
    int rc;

    for (round = 0; round < 2; round++) {
        row_begin();
        proc.euid = 0;
        mount.type = &locked_type;
        attr_row = 1;
        if (round == 0) {
            waiting = write_moves;
            rc = utimes_path("/f", 0, 0, NULL);
        } else {
            int fd = model_open();

            waiting = write_moves;
            rc = fd < 0 ? fd : utimes_fd(fd, NULL, 0);
        }
        check(rc == 0, "a utimensat of a file a write moves");
        check(attrs == 1 && ns_depth == 0 && !waiting,
              "the times are set once, with the mount's lock given back");
        attr_row = 0;
        waiting = 0;
        ns_depth = 0;
        mount.type = &mounted_type;
        row_end();
    }
}

/* The descriptor the other thread of `open_last_slot` was given, or the
 * error `fd_alloc` gave it. */
static int other_slot;

/** Another thread that shares the table takes a descriptor while the open
 * waits for the mount's lock in its walk. */
static void other_takes_slot(void)
{
    current_proc = &other;
    other_slot = fd_alloc(other.fds);
    current_proc = &proc;
}

/**
 * An open that truncates, in a table with one free slot, while another
 * thread sharing the table takes a descriptor as the open's walk waits
 * for the mount's lock: the open reserved the slot before its walk, so the
 * other thread finds the table full, and the open ends with that slot and
 * the file truncated.
 */
static void open_last_slot(void)
{
    int fd;

    row_begin();
    proc.euid = 0;
    proc.rlim[RLIM_NOFILE].cur = 4;
    other.rlim[RLIM_NOFILE].cur = 4;
    files.refs = 2;
    other.fds = &files;
    for (fd = 0; fd < 3; fd++) {
        check(fd_alloc(proc.fds) == fd, "the table is full but for its last slot");
        fd_slot(proc.fds, fd)->type = FD_TYPE_FILE;
    }
    mount.type = &locked_type;
    installing = 3;
    other_slot = -2;
    waiting = other_takes_slot;
    fd = do_open("f", 2 | O_TRUNC, 0);
    check(other_slot == -EMFILE && fd == 3 && truncates == 1,
          "an open that truncates keeps the slot it reserved before its walk");
    check(ns_depth == 0 && !waiting, "the open gives the mount's lock back");
    installing = -1;
    waiting = 0;
    ns_depth = 0;
    mount.type = &mounted_type;
    proc.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    row_end();
}

/** While a close's release sleeps, its descriptor is closed to a lookup and
 * to a dup2 onto it. */
static void release_closed(void)
{
    check(fd_get(proc.fds, releasing) == NULL &&
              (fd_slot(proc.fds, releasing)->flags & FD_FLAG_RELEASING),
          "a descriptor being released is closed to lookups");
    check(dup_clear(releasing) == -EBUSY, "dup2 onto a descriptor being released is busy");
}

/** Close a descriptor whose release sleeps in the file system's release. */
static void close_sleeping(void)
{
    int fd;

    row_begin();
    fd = model_open();
    check(fd >= 0, "open a descriptor to close");
    if (fd >= 0) {
        releasing = fd;
        in_release = release_closed;
        check(close_fd(fd) == 0 && in_release == 0 && refs == 0, "close releases the description");
        check(lock_closes == 1 && lock_closer == &proc, "the close drops the closer's locks once");
        check(fd_slot(proc.fds, fd)->type == FD_TYPE_NONE && !fd_slot(proc.fds, fd)->ofd,
              "the slot is free after the release");
    }
    row_end();
}

/** The call of a third thread that held `releasing` ends while the release
 * the second one began sleeps: the release is left to the second. */
static void third_call_ends(void)
{
    process_t *self = current_proc;

    current_proc = &third;
    hold_fd(&third, releasing);
    fd_holds_release();
    current_proc = self;
    check(fd_slot(proc.fds, releasing)->type == FD_TYPE_FILE && fd_slot(proc.fds, releasing)->ofd,
          "a release under way is not begun again");
}

/** A close leaves a descriptor another thread's call holds to that call, and
 * its release happens once, when the call ends. */
static void deferred_release(void)
{
    int fd;

    row_begin();
    files.refs = 3;
    proc_list = &proc;
    proc.next = &other;
    other.next = &third;
    other.fds = &files;
    third.fds = &files;
    fd = model_open();
    check(fd >= 0, "open a descriptor two calls hold");
    if (fd >= 0) {
        hold_fd(&other, fd);
        check(close_fd(fd) == 0 && (fd_slot(proc.fds, fd)->flags & FD_FLAG_CLOSING) && refs == 1,
              "a close of a descriptor another call holds leaves it to that call");
        check(lock_closes == 1 && lock_closer == &proc,
              "that close drops the closer's record locks at once");
        fd_holds_release();
        check(fd_slot(proc.fds, fd)->type == FD_TYPE_FILE, "the closer's call end leaves it");
        current_proc = &other;
        releasing = fd;
        in_release = third_call_ends;
        fd_holds_release();
        current_proc = &proc;
        check(fd_slot(proc.fds, fd)->type == FD_TYPE_NONE && refs == 0 && in_release == 0,
              "the holder's call end releases it");
        check(lock_closes == 1, "that release drops no process's record locks");
    }
    row_end();
}

/* The source of the dup2 in `dup2_source_closed`. */
static int dup_source = -1;

/** Another thread closes the dup2's source while the replacement sleeps. */
static void other_closes_source(void)
{
    current_proc = &other;
    check(close_fd(dup_source) == 0 && (fd_slot(proc.fds, dup_source)->flags & FD_FLAG_CLOSING),
          "a close of the source the dup2 holds leaves it to the dup2's call");
    current_proc = &proc;
}

/** A dup2 whose source is closed while the replacement of its target sleeps
 * makes an open descriptor, which outlives the source's release. */
static void dup2_source_closed(void)
{
    isr_params_t params;
    int target;

    row_begin();
    files.refs = 2;
    proc_list = &proc;
    proc.next = &other;
    other.fds = &files;
    dup_source = model_open();
    target = model_open();
    check(dup_source >= 0 && target >= 0, "open a source and a target");
    if (dup_source >= 0 && target >= 0) {
        k_memset(&params, 0, sizeof(params));
        params.ebx = (uint32_t)dup_source;
        params.ecx = (uint32_t)target;
        in_release = other_closes_source;
        check(sys_dup2(&params) == target && in_release == 0, "dup2 replaces its target");
        check(fd_get(proc.fds, target) == fd_slot(proc.fds, target) &&
                  !(fd_slot(proc.fds, target)->flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)),
              "the copy is an open descriptor");
        fd_holds_release();
        current_proc = &other;
        fd_holds_release();
        current_proc = &proc;
        check(fd_slot(proc.fds, dup_source)->type == FD_TYPE_NONE &&
                  fd_slot(proc.fds, target)->type == FD_TYPE_FILE &&
                  fd_slot(proc.fds, target)->ofd->refcount == 1 && refs == 1,
              "the source's release leaves the copy its description");
    }
    dup_source = -1;
    row_end();
}

/**
 * An open, a dup and a dup2 that get no descriptor return what the
 * allocator answered, as Linux's do: ENOMEM when the heap has no room for
 * the description or the table's growth, and EMFILE at the descriptor
 * limit. The open reaches no file system either way.
 */
static void descriptor_errors(void)
{
    isr_params_t params;
    int fd, rc;

    row_begin();
    proc.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    fail_after = 0;
    rc = do_open("target", 2, 0);
    fail_after = -1;
    check(rc == -ENOMEM && opens == 0, "an open the heap has no room for is ENOMEM");
    proc.rlim[RLIM_NOFILE].cur = 0;
    rc = do_open("target", 2, 0);
    proc.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    check(rc == -EMFILE && opens == 0, "an open at the descriptor limit is EMFILE");
    fd = model_open();
    check(fd >= 0, "open a descriptor to duplicate");
    if (fd >= 0) {
        k_memset(&params, 0, sizeof(params));
        params.ebx = (uint32_t)fd;
        fail_after = 0;
        rc = sys_dup(&params);
        fail_after = -1;
        check(rc == -ENOMEM, "a dup the heap has no room for is ENOMEM");
        params.ecx = FD_TABLE_FIRST;
        fail_after = 0;
        rc = sys_dup2(&params);
        fail_after = -1;
        check(rc == -ENOMEM && fd_table_cap(proc.fds) == FD_TABLE_FIRST,
              "a dup2 past the end of a table the heap cannot grow is ENOMEM");
    }
    row_end();
}

/** Check node repointing without changing a mounted open identity, and
 * that an attachment is found by its node alone. */
static void loop_handles(void)
{
    loops[0].mnt = &mount;
    loops[0].base = &mount;
    loops[0].attached = 1;
    loops[0].node = &node;
    loops[0].handle = &handle;
    loop_repoint(&mount, &node, &moved);
    check(loops[0].node == &moved && loops[0].handle == &handle,
          "mounted loop handle stays stable when node moves");
    check(loop_backs(&mount, &moved) && !loop_backs(&mount, &node) && !loop_backs(&mount, &handle),
          "a loop attachment is found by the node where its file is, never by a handle");
    mount.type = &vfs_krffs_type;
    loops[0].handle = &node;
    loops[0].node = &node;
    loop_repoint(&mount, &node, &moved);
    check(loops[0].node == &moved && loops[0].handle == &moved,
          "memory-root loop handle follows node");
}

/**
 * The key a file's locks are found by. A KRFFS file, on the boot root or on
 * a mounted volume, and a FAT32 or exFAT file are keyed by the node their
 * description has now, which a move repoints and a rename keeps, and not by
 * the inode number recorded at the open, which a rename or a move leaves
 * behind and two KRFFS names with one hash share; a file of another type
 * is keyed by that inode number, which stays with the file.
 */
static void lock_keys(void)
{
    struct vfs_mount root_mnt, volume_mnt, vfat_mnt, exfat_mnt, other_mnt;
    struct open_file desc;
    struct lock_key key;
    fd_entry_t e;

    k_memset(&root_mnt, 0, sizeof(root_mnt));
    k_memset(&volume_mnt, 0, sizeof(volume_mnt));
    k_memset(&vfat_mnt, 0, sizeof(vfat_mnt));
    k_memset(&exfat_mnt, 0, sizeof(exfat_mnt));
    k_memset(&other_mnt, 0, sizeof(other_mnt));
    k_memset(&desc, 0, sizeof(desc));
    k_memset(&e, 0, sizeof(e));
    root_mnt.type = &vfs_krffs_type;
    volume_mnt.type = &vfs_krffs_volume_type;
    vfat_mnt.type = &vfs_vfat_type;
    exfat_mnt.type = &vfs_exfat_type;
    other_mnt.type = &mounted_type;
    e.ofd = &desc;
    desc.vnode = &moved;
    desc.ino = 7;
    desc.vmnt = &root_mnt;
    fd_lock_key(&e, &key);
    check(key.mnt == &root_mnt && key.ino == (uint64_t)(uintptr_t)&moved,
          "a file of the KRFFS root is keyed by its node");
    desc.vmnt = &volume_mnt;
    fd_lock_key(&e, &key);
    check(key.mnt == &volume_mnt && key.ino == (uint64_t)(uintptr_t)&moved,
          "a file of a KRFFS volume is keyed by its node");
    desc.vmnt = &vfat_mnt;
    fd_lock_key(&e, &key);
    check(key.mnt == &vfat_mnt && key.ino == (uint64_t)(uintptr_t)&moved,
          "a file of a FAT32 volume is keyed by its node, which a rename keeps");
    desc.vmnt = &exfat_mnt;
    fd_lock_key(&e, &key);
    check(key.mnt == &exfat_mnt && key.ino == (uint64_t)(uintptr_t)&moved,
          "a file of an exFAT volume is keyed by its node, which a rename keeps");
    desc.vmnt = &other_mnt;
    fd_lock_key(&e, &key);
    check(key.mnt == &other_mnt && key.ino == 7,
          "a file of another type is keyed by its inode number at the open");
}

/* The /proc and log rows: the table of the process that shares the
 * description, the seek it makes, the value that seek returned, where
 * _llseek wrote it, and whether the seek parked. */
static fd_table_t sharer_files;
static void (*sharer_call)(void);
static int32_t sharer_seek;
static long long sharer_result;
static int sharer_parked;

/** The seek of the process that shares the description: SEEK_CUR, 16 bytes
 * on, through its descriptor 0. */
static void sharer_seeks(void)
{
    isr_params_t params;

    k_memset(&params, 0, sizeof(params));
    params.ecx = 16;
    params.edx = 1;
    current_proc = &other;
    sharer_seek = sys_lseek(&params);
}

/** The 64-bit seek of the process that shares the description, as `_llseek`
 * makes it on the log: SEEK_CUR, 16 bytes on, the new offset written to
 * `sharer_result` and kept in `sharer_seek`. */
static void sharer_llseeks(void)
{
    current_proc = &other;
    sharer_seek =
        llseek_bootlog(fd_slot(&sharer_files, 0), 16, 1, (long long __user *)&sharer_result);
    if (sharer_seek == 0) {
        sharer_seek = (int32_t)sharer_result;
    }
}

/** Bring in a page for a transfer: of the process, for a /proc/PID/mem
 * transfer, or of the caller's buffer, for the log's copy to it. A sleep, in
 * which the process that shares the description seeks. */
static void page_fetch(void)
{
    sharer_parked = !park_run(sharer_call);
    current_proc = &proc;
}

/** A transfer of /proc/PID/mem as `procfs_mem_access` makes it: at the
 * address the offset has, through the page fetch, and the offset set past
 * the bytes moved. */
static int32_t mem_transfer(fd_entry_t *e, uint32_t count)
{
    uint32_t addr = (uint32_t)e->ofd->offset;

    page_fetch();
    e->ofd->offset = addr + count;
    return (int32_t)count;
}

/** A read of /proc/PID/mem, through `mem_transfer`. */
static int32_t mem_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    (void)buf;
    return mem_transfer(e, count);
}

/** A write of /proc/PID/mem, through `mem_transfer`. */
static int32_t mem_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)buf;
    return mem_transfer(e, count);
}

static const struct file_ops mem_ops = {mem_read, mem_write, 0, 0, 0, NULL, NULL, 0};

/** A read of /dev/bootlog as `dev_bootlog_read` makes it: from the offset,
 * through the copy to the caller's buffer, whose page fault sleeps, and the
 * offset set past the bytes read. */
static int32_t log_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    uint32_t off = (uint32_t)e->ofd->offset;

    (void)buf;
    page_fetch();
    e->ofd->offset = off + count;
    return (int32_t)count;
}

static const struct file_ops log_ops = {log_read, 0, 0, 0, 0, NULL, NULL, 0};

/* A transfer a row makes through the dispatch of read(2), or of write(2)
 * when `writing` is set, on a descriptor of `type` with `ops`; the seek of
 * the sharer that waits for it; and the failure that names the case. */
struct offset_case {
    fd_type_t type;
    const struct file_ops *ops;
    int writing;
    void (*seek)(void);
    const char *waits;
};

/**
 * A read, then a write, of 32 bytes of a /proc/PID/mem descriptor at 0x1000,
 * and a read of 32 bytes of /dev/bootlog there, as read(2) and write(2)
 * dispatch them, which sleep in a page fetch while a process that shares the
 * description seeks 16 bytes on, with lseek, and for the log with _llseek
 * too: the transfer holds the description as the seek does, so the seek
 * waits, and resumed in its wait once the transfer's end has woken it, it
 * moves the offset on from 0x1020, where the transfer left it, and is not
 * lost.
 */
static void offset_seek_waits(void)
{
    static const struct offset_case cases[4] = {
        {FD_TYPE_PROC, &mem_ops, 0, sharer_seeks,
         "a seek waits for a /proc read that sleeps in a page fetch"},
        {FD_TYPE_PROC, &mem_ops, 1, sharer_seeks,
         "a seek waits for a /proc write that sleeps in a page fetch"},
        {FD_TYPE_DEVICE, &log_ops, 0, sharer_seeks,
         "a seek waits for a /dev/bootlog read that sleeps in its copy"},
        {FD_TYPE_DEVICE, &log_ops, 0, sharer_llseeks,
         "an _llseek waits for a /dev/bootlog read that sleeps in its copy"},
    };
    char buf[32];
    fd_entry_t *e;
    int32_t moved_bytes;
    int i, fd, resumed;

    for (i = 0; i < 4; i++) {
        row_begin();
        fd = fd_alloc(proc.fds);
        if (fd != 0 || fd_table_init(&sharer_files) != 0) {
            check(0, "the offset row's two tables");
            row_end();
            return;
        }
        e = fd_slot(proc.fds, 0);
        e->type = cases[i].type;
        e->ops = cases[i].ops;
        e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
        e->ofd->offset = 0x1000;
        log_len = 0x2000;
        sharer_files.refs = 1;
        (*fd_slot(&sharer_files, 0)) = *e;
        e->ofd->refcount++;
        other.fds = &sharer_files;
        parked_on = 0;
        sharer_seek = 0;
        sharer_call = cases[i].seek;
        moved_bytes = cases[i].writing ? write_ops(e, buf, sizeof(buf), sizeof(buf))
                                       : read_ops(e, buf, sizeof(buf), sizeof(buf));
        check(moved_bytes == 32, "the transfer moves its 32 bytes");
        check(sharer_parked, cases[i].waits);
        if (sharer_parked) {
            check(parked_wakes != 0, "the end of the transfer wakes the seek that waits");
            current_proc = &other;
            resumed = park_resume();
            current_proc = &proc;
            check(resumed && sharer_seek == 0x1030,
                  "the seek that waited moves on from where the transfer left the offset");
        }
        check(e->ofd->offset == 0x1030 && !e->ofd->busy,
              "a seek made while a transfer sleeps is not lost");
        fd_free(&sharer_files, 0);
        fd_table_free(&sharer_files);
        parked_on = 0;
        row_end();
    }
}

/**
 * Seeks of /dev/bootlog 32 bytes on: from 0x7FFFFFF0 lseek is EOVERFLOW,
 * since the old call returns no offset past 2147483647, and from 0xFFFFFFF0,
 * which _llseek can set, lseek and _llseek are EINVAL, since the log's
 * offset has 32 bits, and so is an _llseek from 1 whose sum passes the
 * largest signed 64-bit offset; each leaves the offset. lseek to the end
 * gives the ring's length.
 */
static void log_seek_range(void)
{
    isr_params_t params;
    long long result = 0;
    fd_entry_t *e;
    int32_t rc;

    row_begin();
    check(fd_alloc(proc.fds) == 0, "the log row's descriptor");
    e = fd_slot(proc.fds, 0);
    e->type = FD_TYPE_DEVICE;
    e->ops = &log_ops;
    e->flags = FD_FLAG_READ;
    log_len = 0x2000;
    e->ofd->offset = 0x7FFFFFF0u;
    k_memset(&params, 0, sizeof(params));
    params.ecx = 0x20;
    params.edx = 1;
    rc = sys_lseek(&params);
    check(rc == -EOVERFLOW && e->ofd->offset == 0x7FFFFFF0u,
          "an lseek of /dev/bootlog past 2147483647 is EOVERFLOW and leaves the offset");
    e->ofd->offset = 0xFFFFFFF0u;
    rc = sys_lseek(&params);
    check(rc == -EINVAL && e->ofd->offset == 0xFFFFFFF0u,
          "an lseek of /dev/bootlog past 4294967295 is EINVAL and leaves the offset");
    rc = llseek_bootlog(e, 0x20, 1, (long long __user *)&result);
    check(rc == -EINVAL && e->ofd->offset == 0xFFFFFFF0u,
          "an _llseek of /dev/bootlog past 4294967295 is EINVAL and leaves the offset");
    e->ofd->offset = 1;
    rc = llseek_bootlog(e, 0x7FFFFFFFFFFFFFFFll, 1, (long long __user *)&result);
    check(rc == -EINVAL && e->ofd->offset == 1,
          "an _llseek of /dev/bootlog past the largest signed offset is EINVAL");
    params.ecx = 0;
    params.edx = 2;
    rc = sys_lseek(&params);
    check(rc == 0x2000 && e->ofd->offset == 0x2000,
          "an lseek to the end of /dev/bootlog gives the ring's length");
    row_end();
}

/* A type whose directory cursor is a byte, as ext2's, for `dir_wide_rows`. */
static const struct vfs_type bytes_type = {.name = "bytes", .dir_seeks_bytes = 1};

/**
 * A directory of a type whose cursor is a byte (`dir_seeks_bytes`) on a
 * volume whose largest file offset is ext2's at 4 KiB blocks: getdents64
 * at 4 GiB (`dir_past_end`) asks the type's `readdir` at 0xFFFFFFFF and
 * passes on its -EIO,
 * a failed mount's, and its -ENOENT, a removed directory's, and ends the
 * listing on its end, each leaving the offset; _llseek takes the largest
 * offset and refuses a byte past it with EINVAL by SEEK_SET and SEEK_CUR,
 * leaving the offset.
 */
static void dir_wide_rows(void)
{
    static const int answers[] = {-EIO, -ENOENT, 0};
    static const char *const what[] = {
        "getdents64 past 4 GiB of a directory on a failed mount is EIO",
        "getdents64 past 4 GiB of a removed directory is ENOENT",
        "getdents64 past 4 GiB of a directory at its end lists nothing"};
    const uint64_t most = 0x1FF7FDFE000ull;
    struct vfs_mount bytes_mount;
    long long result = 0;
    fd_entry_t *e;
    unsigned i;
    long rc;

    row_begin();
    k_memset(&bytes_mount, 0, sizeof(bytes_mount));
    bytes_mount.type = &bytes_type;
    bytes_mount.max_bytes = most;
    check(fd_alloc(proc.fds) == 0, "the wide-offset row's descriptor");
    e = fd_slot(proc.fds, 0);
    e->type = FD_TYPE_DIR;
    e->flags = FD_FLAG_READ;
    e->ofd->vmnt = &bytes_mount;
    e->ofd->vnode = &node;
    e->ofd->fhandle = &handle;
    dir_rows = 1;
    for (i = 0; i < sizeof(answers) / sizeof(answers[0]); i++) {
        e->ofd->offset = 0x100000000ull;
        dir_asked = 0;
        dir_answer = answers[i];
        rc = dir_past_end(e->ofd);
        check(rc == answers[i] && dir_asked == 0xFFFFFFFFu && e->ofd->offset == 0x100000000ull,
              what[i]);
    }
    rc = llseek_listing(e, (long long)most, 0, (long long __user *)&result);
    check(rc == 0 && result == (long long)most && e->ofd->offset == most,
          "an _llseek of a byte-cursor directory takes its volume's largest file offset");
    rc = llseek_listing(e, 1, 1, (long long __user *)&result);
    check(rc == -EINVAL && e->ofd->offset == most,
          "an _llseek one byte on from it by SEEK_CUR is EINVAL and leaves the offset");
    rc = llseek_listing(e, (long long)most + 1, 0, (long long __user *)&result);
    check(rc == -EINVAL && e->ofd->offset == most,
          "an _llseek one byte past it by SEEK_SET is EINVAL and leaves the offset");
    dir_rows = 0;
    e->ofd->vmnt = 0;
    row_end();
}

/** The close of the device row's device, its description's last. */
static void dev_close(fd_entry_t *e)
{
    (void)e;
    dev_closes++;
}

static const struct file_ops dev_ops = {.close = dev_close};

/** The close the caller's other thread makes of the device row's
 * descriptor 0 while the reopen's open of the device sleeps. */
static void dev_desc_close(void)
{
    check(close_fd(0) == 0, "the close of the device's descriptor");
}

/**
 * An open of /proc/PID/fd/0 for a device opened through a device node of a
 * serialised mounted type, as tmpfs is, whose open of the device sleeps
 * while the caller's other thread closes descriptor 0, the description's
 * last: the close runs the device's close, since the reopen holds the node
 * and its mount and not the description, and the reopen gives back its
 * holds and its entry to the mount when it ends.
 */
static void device_reopen_closed(void)
{
    fd_entry_t *e;
    int32_t rc;

    row_begin();
    dev_mount.type = &dev_type;
    dev_holds = dev_enters = dev_closes = 0;
    proc.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    check(fd_alloc(proc.fds) == 0, "the device row's descriptor");
    e = fd_slot(proc.fds, 0);
    e->type = FD_TYPE_DEVICE;
    e->ops = &dev_ops;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e->ofd->dev_mnt = &dev_mount;
    e->ofd->dev_node = &dev_inode;
    dev_holds = 1;
    refs++;
    in_dev_open = dev_desc_close;
    rc = proc_fd_reopen_device(e, 2 /* O_RDWR */);
    check(rc == -ENXIO && !in_dev_open, "the reopen opens the device, which is gone");
    check(fd_slot(proc.fds, 0)->type == FD_TYPE_NONE, "the device's descriptor is closed");
    check(dev_closes == 1,
          "a close of a device's last descriptor while a reopen of it sleeps closes the device");
    check(dev_holds == 0 && dev_enters == 0 && ns_depth == 0,
          "the reopen gives back its holds of the device's node and its mount");
    in_dev_open = 0;
    row_end();
}

/** Another process takes an extent of the root, under the root's lock, as
 * every allocation there does, and notes whether a reclaim was under way. */
static void third_allocates(void)
{
    current_proc = &third;
    vfs_ns_lock(&mount);
    extents_taken++;
    extents_in_reclaim += reclaiming;
    vfs_ns_unlock(&mount);
}

/** A descriptor in this process's table of `victim`, which lost its name: a
 * file, or with `fifo` an end of a FIFO. Its number. */
static int orphan_desc(int fifo)
{
    int fd = fd_alloc(proc.fds);
    fd_entry_t *e;

    check(fd >= 0, "a descriptor of the orphan");
    if (fd < 0) {
        return fd;
    }
    e = fd_slot(proc.fds, fd);
    victim.name[0] = '\0';
    if (fifo) {
        victim.mode = KRFFS_S_IFIFO | 0600;
        e->type = FD_TYPE_PIPE;
    } else {
        e->type = FD_TYPE_FILE;
        e->node = &victim;
    }
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e->ofd->vmnt = &mount;
    e->ofd->vnode = &victim;
    orphan_ofd = e->ofd;
    ofd_gone = 0;
    removals_locked = 0;
    return fd;
}

/* The descriptor a release row lets go of. */
static int orphan_fd = -1;

/** Close the orphan's descriptor. */
static void orphan_close(void)
{
    check(close_fd(orphan_fd) == 0, "the close of the orphan's descriptor");
}

/** Clear the orphan's descriptor as a dup2 onto it does (`dup_clear`). */
static void orphan_dup2(void)
{
    check(dup_clear(orphan_fd) == 0, "a dup2 onto the orphan's descriptor");
}

/** The release a table's end makes of each descriptor (`release_fd_table`),
 * and an exec's of each close-on-exec one (`close_cloexec_fds`). */
static void orphan_table_end(void)
{
    fd_release_backing(fd_slot(proc.fds, orphan_fd));
    fd_free_after(proc.fds, orphan_fd);
}

/** The close of the descriptor while another process's call holds it, which
 * leaves the release to that call's end (`fd_holds_release`). */
static void orphan_held_close(void)
{
    hold_fd(&other, orphan_fd);
    check(close_fd(orphan_fd) == 0 && fd_slot(proc.fds, orphan_fd)->type != FD_TYPE_NONE,
          "a close of the orphan's descriptor another call holds leaves it to that call");
    fd_holds_release();
    current_proc = &other;
    fd_holds_release();
    current_proc = &proc;
}

/**
 * Check that `path` freed `victim` once, under the root's lock, and that the
 * allocation `third_allocates` made in the reclaim's sleep waited for the
 * lock, was woken as the release let it go, and took its extent after the
 * reclaim.
 */
static void reclaim_waited(const char *path)
{
    int resumed = 0;

    check(removals == 1 && removals_locked == 1 && ns_depth == 0,
          "the release of a nameless file's last holder frees it once, under the root's lock");
    if (reclaim_parked) {
        check(parked_wakes != 0, "the release wakes the allocation that waits for the lock");
        current_proc = &third;
        resumed = park_resume();
        current_proc = &proc;
    }
    if (!(reclaim_parked && resumed && extents_taken == 1 && extents_in_reclaim == 0)) {
        printf("open-hosttest: fail: an allocation on the root waits for the reclaim of a nameless "
               "file to end (%s)\n",
               path);
        failures++;
    }
    check(ns_depth == 0, "the allocation gives the lock back");
}

/**
 * The release of the last holder of a file of the KRFFS root that lost its
 * name, by a close, a dup2 onto its descriptor, the end of another thread's
 * call that held the descriptor, the release a table's end and an exec
 * make, and the last close of a FIFO's end, while another process's
 * allocation on the root runs in the sleep of the reclaim: the count of the
 * holders and the reclaim run under the root's lock, so the allocation
 * waits, and takes its extent once the reclaim is over.
 */
static void orphan_reclaims(void)
{
    static void (*const paths[5])(void) = {orphan_close, orphan_dup2, orphan_held_close,
                                           orphan_table_end, orphan_close};
    static const char *const names[5] = {"close", "dup2", "the end of another call",
                                         "a table's end and an exec", "a FIFO's end"};
    int i;

    for (i = 0; i < 5; i++) {
        root_row_begin();
        if (paths[i] == orphan_held_close) {
            files.refs = 2;
            other.fds = &files;
            proc.next = &other;
        }
        orphan_fd = orphan_desc(i == 4);
        if (orphan_fd >= 0) {
            extents_taken = extents_in_reclaim = 0;
            reclaim_parked = 0;
            in_reclaim = third_allocates;
            paths[i]();
            in_reclaim = 0;
            reclaim_waited(names[i]);
        }
        orphan_ofd = 0;
        orphan_fd = -1;
        root_row_end();
    }
}

/* The descriptor of the file the mapping row maps, and the frees of the
 * file made by its close. */
static int mapped_fd = -1;
static int removals_at_close = -1;

/** The close of the mapped file's last descriptor, while the mapping's
 * release waits for the root's lock. */
static void mapped_close(void)
{
    check(close_fd(mapped_fd) == 0, "the close of the mapped file's descriptor");
    removals_at_close = removals;
}

/**
 * The end of a mapping of a file of the KRFFS root that lost its name, whose
 * last descriptor is closed while the mapping's release waits for the root's
 * lock: the mapping is still among the file's holders then, so the close
 * leaves the file, and the mapping's release frees it once, under the lock,
 * while another process's allocation waits in the reclaim's sleep.
 */
static void mapping_reclaims(void)
{
    struct vm_file *vf;

    root_row_begin();
    mapped_fd = orphan_desc(0);
    vf = mapped_fd >= 0 ? filemap_new(fd_slot(proc.fds, mapped_fd)) : NULL;
    check(vf != NULL, "a mapping of the orphan");
    if (vf) {
        removals_at_close = -1;
        extents_taken = extents_in_reclaim = 0;
        reclaim_parked = 0;
        waiting = mapped_close;
        in_reclaim = third_allocates;
        vf->release(vf);
        in_reclaim = 0;
        check(removals_at_close == 0,
              "a mapping counts among a file's holders until its release has the root's lock");
        reclaim_waited("the end of a mapping");
    }
    orphan_ofd = 0;
    mapped_fd = -1;
    root_row_end();
}

/** A mount is its own base here: no row makes a bind mount. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** No mount holds a device. */
int vfs_device_holds(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** Sync nothing: the model has no cache. */
int bcache_sync(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** Drop nothing: the model has no cache. */
int bcache_invalidate(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** No buffer of a device is held. */
int bcache_device_holds(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** No device is under another: 0. */
int block_depth(block_device_t *device)
{
    (void)device;
    return 0;
}

/** Run every row. Returns 0 when every check passed, 1 otherwise. */
/*
 * The model of a type that sets `file_locked`, mounted at /fl: a tree of
 * directories and files whose locks write each take and give to `fl_log`,
 * "L" and "U" with the inode number, and an "o" before the token of the
 * other process. The mount's rename lock writes "R" and "r", a look for a
 * mount on a name "M", and each change its name. A lock another process
 * holds is waited for by parking, logged as "W".
 */
struct fl_node {
    const char *name; /* "" for a node not made yet or removed */
    uint32_t ino;
    int parent; /* the index of the directory it is in, -1 for the root */
    int dir;
    uint32_t nlink;
    process_t *holder;
    int depth;
};

/* The tree: / (2), /a (10), /b (5), /a/c (7), /a/x (20), /a/z (15),
 * /b/d (30, empty), /b/w (12), and a slot for what a row makes (40). */
enum { FL_ROOT, FL_A, FL_B, FL_C, FL_X, FL_Z, FL_D, FL_W, FL_NEW, FL_NODES };
static const struct fl_node fl_tree[FL_NODES] = {
    {"/", 2, -1, 1, 4, 0, 0},    {"a", 10, FL_ROOT, 1, 3, 0, 0}, {"b", 5, FL_ROOT, 1, 3, 0, 0},
    {"c", 7, FL_A, 1, 2, 0, 0},  {"x", 20, FL_A, 0, 1, 0, 0},    {"z", 15, FL_A, 0, 1, 0, 0},
    {"d", 30, FL_B, 1, 2, 0, 0}, {"w", 12, FL_B, 0, 1, 0, 0},    {"", 40, -1, 0, 0, 0, 0},
};
static struct fl_node fl_nodes[FL_NODES];
/* The mode and owner a protection row gives a node, a mode of 0 leaving the
 * model's own; and the node a link was made to. */
static uint32_t fl_mode[FL_NODES], fl_uid[FL_NODES];
static const struct fl_node *fl_linked;
static char fl_log[512];
/* The node a mount covers, or NULL; the call another process makes when the
 * row's call first looks `fl_hook_name` up, or while its change sleeps; and
 * what the other process's mount answered. */
static struct fl_node *fl_mounted;
static const char *fl_hook_name;
static void (*fl_on_lookup)(void);
static void (*fl_in_change)(void);
static int fl_mount_rc;
static struct vfs_mount fl_child;
/* The names a row's change gives a node, kept past the call's buffers. */
static char fl_made_name[VFS_NAME_MAX + 1], fl_moved_name[VFS_NAME_MAX + 1];

/** Append `token` and a space to the log. */
static void fl_note(const char *token)
{
    uint32_t at = (uint32_t)k_strlen(fl_log);

    if (at + k_strlen(token) + 2 < sizeof(fl_log)) {
        k_strncpy(fl_log + at, token, sizeof(fl_log) - at);
        at += (uint32_t)k_strlen(token);
        fl_log[at] = ' ';
        fl_log[at + 1] = '\0';
    }
}

/** Log `kind` with the inode number of `n`, marked for the other process. */
static void fl_note_node(char kind, const struct fl_node *n)
{
    char token[16];
    int at = 0;
    uint32_t v = n->ino, div = 1;

    if (current_proc == &other) {
        token[at++] = 'o';
    }
    token[at++] = kind;
    while (v / div >= 10) {
        div *= 10;
    }
    for (; div > 0; div /= 10) {
        token[at++] = (char)('0' + (v / div) % 10);
    }
    token[at] = '\0';
    fl_note(token);
}

/** The node of `n`'s name in the directory `dir`, or NULL. */
static struct fl_node *fl_child_named(const struct fl_node *dir, const char *name)
{
    int i;

    for (i = 0; i < FL_NODES; i++) {
        if (fl_nodes[i].parent >= 0 && &fl_nodes[fl_nodes[i].parent] == dir &&
            fl_nodes[i].name[0] != '\0' && __builtin_strcmp(fl_nodes[i].name, name) == 0) {
            return &fl_nodes[i];
        }
    }
    return 0;
}

/** The type's lookup, `..` and `.` included; the first lookup of
 * `fl_hook_name` by the row's process runs `fl_on_lookup`, as if the read of
 * the directory slept. */
static int fl_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **found)
{
    struct fl_node *d = dir ? dir : &fl_nodes[FL_ROOT];
    struct fl_node *n;
    void (*hook)(void) = fl_on_lookup;

    check(mnt == &fl_mount, "a lookup of the file-locked model is on its mount");
    if (hook && current_proc == &proc && __builtin_strcmp(name, fl_hook_name) == 0) {
        fl_on_lookup = 0;
        hook();
    }
    if (__builtin_strcmp(name, ".") == 0) {
        n = d;
    } else if (__builtin_strcmp(name, "..") == 0) {
        n = d->parent >= 0 ? &fl_nodes[d->parent] : d;
    } else {
        n = fl_child_named(d, name);
    }
    if (!n) {
        return -ENOENT;
    }
    *found = n;
    return 0;
}

/** A directory of mode 0777, or a file of 0666, owned by root, unless a
 * protection row gave the node a mode and an owner. */
static int fl_getattr(struct vfs_mount *mnt, void *at, struct vfs_stat *st)
{
    const struct fl_node *n = at ? at : &fl_nodes[FL_ROOT];
    int i = (int)(n - fl_nodes);

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->ino = n->ino;
    st->nlink = n->nlink;
    st->mode = fl_mode[i] ? fl_mode[i] : n->dir ? 040777u : 0100666u;
    st->uid = fl_uid[i];
    return 0;
}

/** The attributes of the directory the node `ref` holds is in, a mode of 0
 * for the model's root or a node of another mount. */
static void fl_dir_attrs(const struct vfs_ref *ref, struct vfs_stat *dir)
{
    const struct fl_node *n = ref->node;

    if (ref->mnt != &fl_mount || n->parent < 0) {
        k_memset(dir, 0, sizeof(*dir));
        return;
    }
    fl_getattr(&fl_mount, &fl_nodes[n->parent], dir);
}

/** Make `name` in the directory `dir` a link to `at`, which the model
 * records and does not name. */
int vfs_link(struct vfs_mount *mnt, void *dir, const char *name, void *at)
{
    check(mnt == &fl_mount && fl_holds(dir) && fl_holds(at),
          "a link holds its directory's lock and its file's");
    check(!fl_child_named(dir, name), "a link's name is new");
    fl_note("link");
    fl_linked = at;
    return 0;
}

/** Take the lock of `at`, parking while another process holds it. */
static void fl_lock_node(struct vfs_mount *mnt, void *at)
{
    struct fl_node *n = at;

    (void)mnt;
    while (n->holder && n->holder != current_proc) {
        fl_note_node('W', n);
        if (!park_active()) {
            printf("open-hosttest: fail: a node lock wait that nothing can end\n");
            exit(1);
        }
        park();
    }
    fl_note_node('L', n);
    n->holder = current_proc;
    n->depth++;
}

/** Give back one take of the lock of `at`. */
static void fl_unlock_node(struct vfs_mount *mnt, void *at)
{
    struct fl_node *n = at;

    (void)mnt;
    check(n->holder == current_proc && n->depth > 0, "a node lock is given back by its holder");
    fl_note_node('U', n);
    if (--n->depth == 0) {
        n->holder = 0;
    }
}

static const struct vfs_ops fl_ops = {.lookup = fl_lookup,
                                      .getattr = fl_getattr,
                                      .lock_node = fl_lock_node,
                                      .unlock_node = fl_unlock_node};
static const struct vfs_type fl_type = {"flocked", 0, &fl_ops, 0, 0, 0, 1};

/** The rename lock of the model's mount: logged, never contended. */
void sleep_lock_take(struct sleep_lock *l)
{
    check(l == &fl_mount.rename, "the rename lock is the mount's");
    fl_note("R");
}

/** Give back the rename lock. */
void sleep_lock_give(struct sleep_lock *l)
{
    check(l == &fl_mount.rename, "the rename lock is the mount's");
    fl_note("r");
}

/** The model's locks need no setting up. */
void sleep_lock_init(struct sleep_lock *l, struct sleep_lock_record *record, int flags)
{
    (void)l;
    (void)record;
    (void)flags;
}

/** The model keeps no name cache. */
void dcache_drain(void)
{}

/** Walk a path below /fl to its node, or to its parent and last name with
 * VFS_WALK_PARENT, holding nothing, as a walk on a file-locked type does. */
static int fl_resolve(const char *path, int flags, struct vfs_ref *out, char *leaf,
                      uint32_t leafcap)
{
    struct fl_node *n = &fl_nodes[FL_ROOT];
    char name[VFS_NAME_MAX + 1];
    const char *p = path + 3;

    for (;;) {
        uint32_t len = 0;

        while (*p == '/') {
            p++;
        }
        while (p[len] && p[len] != '/') {
            len++;
        }
        check(len > 0 && len <= VFS_NAME_MAX, "a model path names something");
        k_memcpy(name, p, len);
        name[len] = '\0';
        p += len;
        if (*p == '\0' && (flags & VFS_WALK_PARENT)) {
            k_strncpy(leaf, name, leafcap);
            break;
        }
        n = fl_child_named(n, name);
        if (!n) {
            return -ENOENT;
        }
        if (*p == '\0') {
            break;
        }
    }
    out->mnt = &fl_mount;
    out->node = n;
    return 0;
}

/** Look for a mount on `name` in the model's directory `at`. */
static struct vfs_mount *fl_leaf_mount(void *at, const char *name)
{
    fl_note("M");
    return fl_mounted && fl_child_named(at, name) == fl_mounted ? &fl_child : 0;
}

/** Make `name` in the directory `dir`. */
static int fl_make(void *dir, const char *name, int isdir)
{
    struct fl_node *d = dir;

    if (fl_child_named(d, name)) {
        return -EEXIST;
    }
    k_strncpy(fl_made_name, name, sizeof(fl_made_name));
    fl_nodes[FL_NEW].name = fl_made_name;
    fl_nodes[FL_NEW].parent = (int)(d - fl_nodes);
    fl_nodes[FL_NEW].dir = isdir;
    fl_nodes[FL_NEW].nlink = isdir ? 2 : 1;
    return 0;
}

/** Whether the calling process holds the lock of the model's node `at`. */
static int fl_holds(void *at)
{
    return ((struct fl_node *)at)->holder == current_proc;
}

/** Run what the row has happen while a change sleeps for the disk. */
static void fl_change_sleeps(void)
{
    void (*run)(void) = fl_in_change;

    fl_in_change = 0;
    if (run) {
        check(!park_run(run), "the other process waits for the lock the change holds");
        current_proc = &proc;
    }
}

/** Remove the empty directory or the file `name` of `dir`. */
static int fl_remove(void *dir, const char *name, int isdir)
{
    struct fl_node *n = fl_child_named(dir, name);

    if (!n) {
        return -ENOENT;
    }
    if (n->dir != isdir) {
        return isdir ? -ENOTDIR : -EISDIR;
    }
    check(n->holder == current_proc, "a removal holds the lock of what it removes");
    fl_note(isdir ? "rmdir" : "unlink");
    fl_change_sleeps();
    n->name = "";
    n->nlink = 0;
    return 0;
}

/** Move `oldname` of `olddir` to `newname` of `newdir`, replacing what has
 * that name: EINVAL for a directory moved below itself, ENOTDIR for a
 * directory moved over a file and EISDIR for a file moved over a directory,
 * as the type answers them. */
static int fl_move(void *olddir, const char *oldname, void *newdir, const char *newname)
{
    struct fl_node *n = fl_child_named(olddir, oldname);
    struct fl_node *to = fl_child_named(newdir, newname);
    struct fl_node *up = newdir;

    fl_note("rename");
    if (!n) {
        return -ENOENT;
    }
    if (to && to->dir != n->dir) {
        return n->dir ? -ENOTDIR : -EISDIR;
    }
    for (; up; up = up->parent >= 0 ? &fl_nodes[up->parent] : 0) {
        if (up == n) {
            return -EINVAL;
        }
    }
    if (to && to != n) {
        to->name = "";
        to->nlink = 0;
    }
    k_strncpy(fl_moved_name, newname, sizeof(fl_moved_name));
    n->name = fl_moved_name;
    n->parent = (int)((struct fl_node *)newdir - fl_nodes);
    return 0;
}

/** A fresh tree, an empty log and no mount on it, for a row on /fl. */
static void fl_row_begin(void)
{
    row_begin();
    k_memcpy(fl_nodes, fl_tree, sizeof(fl_nodes));
    k_memset(fl_mode, 0, sizeof(fl_mode));
    k_memset(fl_uid, 0, sizeof(fl_uid));
    fl_linked = 0;
    k_memset(&fl_mount, 0, sizeof(fl_mount));
    fl_mount.type = &fl_type;
    fl_mount.root = &fl_nodes[FL_ROOT];
    fl_log[0] = '\0';
    fl_mounted = 0;
    fl_on_lookup = 0;
    fl_in_change = 0;
    fl_mount_rc = 1;
    k_memset(&row_fs, 0, sizeof(row_fs));
    proc.fs = &row_fs;
}

/** Require the log `want` and every node lock given back. */
static void fl_row_end(const char *want, const char *what)
{
    int i;

    if (__builtin_strcmp(fl_log, want) != 0) {
        printf("open-hosttest: lock order: got \"%s\", want \"%s\"\n", fl_log, want);
    }
    check(__builtin_strcmp(fl_log, want) == 0, what);
    for (i = 0; i < FL_NODES; i++) {
        check(fl_nodes[i].holder == 0 && fl_nodes[i].depth == 0, "every node lock given back");
    }
    proc.fs = 0;
    row_end();
}

/** The other process's mount on /fl/b/d, from its cover lock to its place,
 * as `vfs_do_mount` takes them. */
static void fl_mount_on_d(void)
{
    struct vfs_ref d;

    current_proc = &other;
    d.mnt = &fl_mount;
    d.node = &fl_nodes[FL_D];
    fl_mount_rc = vfs_cover_lock(&d);
    if (fl_mount_rc == 0) {
        fl_mounted = &fl_nodes[FL_D];
    }
    vfs_node_unlock(&d);
    current_proc = &proc;
}

/**
 * The order of the locks a system call takes on a type that sets
 * `file_locked`: a create the parent's; an unlink and an rmdir the parent's
 * and then the victim's, with the mounts looked for again under both; a
 * rename in one directory that directory's and then both names' nodes by
 * inode number; a rename between two directories the mount's rename lock,
 * then the directory above the other first even when its number is higher,
 * or the lower number first when neither is above, then the nodes, a
 * directory before a file, as a link holds its directory and then its
 * source; and a directory moved below itself leaves its own node unlocked,
 * since its lock would come after a directory below it.
 */
static void file_locked_orders(void)
{
    fl_row_begin();
    check(do_mkdir("/fl/b/n", 0777) == 0 && fl_nodes[FL_NEW].parent == FL_B,
          "a mkdir on a file-locked type makes the directory");
    fl_row_end("M L5 M mkdir U5 ", "a create locks its parent alone");

    fl_row_begin();
    check(unlink_named("/fl/b/w", 0) == 0 && fl_nodes[FL_W].nlink == 0,
          "an unlink on a file-locked type removes the file");
    fl_row_end("M L5 L12 M unlink U12 U5 ", "an unlink locks the parent, then the file");

    fl_row_begin();
    check(do_rmdir("/fl/b/d") == 0 && fl_nodes[FL_D].nlink == 0,
          "an rmdir on a file-locked type removes the directory");
    fl_row_end("M L5 L30 M rmdir U30 U5 ", "an rmdir locks the parent, then the victim");

    fl_row_begin();
    check(do_rename("/fl/a/x", "/fl/a/z", 0, 0) == 0 && fl_nodes[FL_X].parent == FL_A,
          "a rename in one directory moves the name");
    fl_row_end("M M L10 L15 L20 M M rename U20 U15 U10 ",
               "a rename in one directory locks it, then both nodes by number");

    fl_row_begin();
    check(do_rename("/fl/a/x", "/fl/a/c/y", 0, 0) == 0 && fl_nodes[FL_X].parent == FL_C,
          "a rename to a directory below moves the name");
    fl_row_end("M M R L10 L7 L20 M M rename U20 U7 U10 r ",
               "a rename to a directory below locks the one above first");

    fl_row_begin();
    check(do_rename("/fl/a/z", "/fl/b/v", 0, 0) == 0 && fl_nodes[FL_Z].parent == FL_B,
          "a rename between unrelated directories moves the name");
    fl_row_end("M M R L5 L10 L15 M M rename U15 U10 U5 r ",
               "a rename between unrelated directories locks the lower number first");

    fl_row_begin();
    check(do_rename("/fl/a", "/fl/a/c/q", 0, 0) == -EINVAL && fl_nodes[FL_A].parent == FL_ROOT,
          "a directory moved below itself is EINVAL");
    fl_row_end("M M R L2 L7 M M rename U7 U2 r ",
               "a directory moved below itself is left unlocked");

    fl_row_begin();
    check(do_rename("/fl/b/d", "/fl/b/w", 0, 0) == -ENOTDIR && fl_nodes[FL_D].parent == FL_B,
          "a directory renamed over a file is ENOTDIR");
    fl_row_end("M M L5 L30 L12 M M rename U12 U30 U5 ",
               "a rename locks a directory before a file of a lower number");
}

/**
 * A mount against an rmdir of the directory it covers, on a type that sets
 * `file_locked`: a mount placed while the rmdir looked the victim up is
 * found under the victim's lock (EBUSY), and a mount that asks while the
 * rmdir holds the victim waits for its lock, then finds the directory gone
 * (ENOENT).
 */
static void file_locked_mount_rmdir(void)
{
    fl_row_begin();
    fl_hook_name = "d";
    fl_on_lookup = fl_mount_on_d;
    check(do_rmdir("/fl/b/d") == -EBUSY && fl_nodes[FL_D].nlink == 2 && fl_mount_rc == 0,
          "an rmdir finds under the victim's lock a mount placed while it looked");
    fl_row_end("M L5 oL30 oU30 L30 M U30 U5 ", "the rmdir looks for the mount under both locks");

    fl_row_begin();
    fl_in_change = fl_mount_on_d;
    check(do_rmdir("/fl/b/d") == 0 && fl_mount_rc == 1,
          "an rmdir removes the directory a mount waits for");
    current_proc = &other;
    check(park_resume(), "the mount ends once the rmdir gives the victim back");
    current_proc = &proc;
    check(fl_mount_rc == -ENOENT && !fl_mounted, "a mount of a removed directory is ENOENT");
    fl_row_end("M L5 L30 M rmdir oW30 U30 U5 oL30 oU30 ", "the mount waits for the victim's lock");
}

/**
 * A read of a disk file whose two pages a shared mapping holds in memory,
 * with stores not written back, when `kmap_temp` has no free slot for a
 * page: the read returns the pages before that page, or -ENOMEM when it is
 * the first, and never reads the file's stale bytes in its place, as
 * Linux's filemap_read fails rather than read around a folio it has.
 */
static void resident_unmapped(void)
{
    static struct dshare ds;
    static char out[2 * PAGE_SIZE];
    struct open_file ofd;
    int big = 0;

    k_memset(&ofd, 0, sizeof(ofd));
    ofd.vmnt = &mount;
    ofd.vnode = &node;
    ofd.ino = 77;
    ds.mnt = &mount;
    ds.ino = 77;
    ds.sh.npages = 2;
    ds.stores = 1;
    k_memset(resident_pages[0], 'a', PAGE_SIZE);
    k_memset(resident_pages[1], 'b', PAGE_SIZE);
    *dshare_bucket(ds.ino) = &ds;
    dshare_count = 1;
    dshare_stores_in[dshare_index(77)] = 1;
    resident_row = 1;

    resident_fail = (uintptr_t)resident_pages[1];
    k_memset(out, 0, sizeof(out));
    check(file_read_locked(&ofd, (char __user *)out, sizeof(out), 0, &big) == PAGE_SIZE &&
              out[0] == 'a' && out[PAGE_SIZE - 1] == 'a' && out[PAGE_SIZE] == 0,
          "a resident read returns the page before the one it cannot map");
    resident_fail = (uintptr_t)resident_pages[0];
    check(file_read_locked(&ofd, (char __user *)out, sizeof(out), 0, &big) == -ENOMEM,
          "a resident read of a first page it cannot map fails with ENOMEM");
    check(resident_refs == 0, "a resident read lets every page's reference go");
    check(big == 0, "a resident read under the big lock takes it no more");

    resident_row = 0;
    resident_fail = 0;
    dshare_stores_in[dshare_index(77)] = 0;
    *dshare_bucket(ds.ino) = NULL;
    dshare_count = 0;
}

/* The race row's mapping of the file, which the other thread makes. */
static struct dshare race_ds;

/** The other thread, while the race row's read sleeps on the disk: it maps
 * the file shared through a descriptor open for writing, as
 * `filemap_disk_map` marks it, and stores 'S' over its second page, which
 * the fault brought into memory and nothing has written back. */
static void race_map(void)
{
    race_ds.mnt = &race_mount;
    race_ds.ino = 77;
    race_ds.stores = 1;
    race_ds.users = 1;
    race_ds.sh.npages = 2;
    *dshare_bucket(race_ds.ino) = &race_ds;
    dshare_count = 1;
    dshare_stores_in[dshare_index(77)] = 1;
    k_memset(resident_pages[1], 'S', PAGE_SIZE);
    race_mapped = 1;
}

/**
 * A read without the big lock of an ext2 file of two pages that no mapping
 * holds when it starts. While its first page waits for the disk, another
 * thread maps the file shared and writable and stores 'S' over the second
 * page (`race_map`). The read returns the first page from the disk and the
 * second with the store, as Linux's filemap_read takes each folio from the
 * file's pages when it reaches it (mm/filemap.c:2649-2700), and gives back
 * the big lock it took to read the mapping's page, and through the file
 * system to read the disk; once through the file system, `cache` 0, and
 * once through the file page cache, `cache` 1.
 */
static void resident_race_once(int cache, const char *how)
{
    static char out[2 * PAGE_SIZE];
    fd_entry_t *e;
    int fd;

    row_begin();
    race_mount.type = &vfs_ext2_type;
    fd = fd_alloc(proc.fds);
    e = fd_slot(proc.fds, fd);
    e->type = FD_TYPE_FILE;
    e->flags = FD_FLAG_READ;
    e->ofd->vmnt = &race_mount;
    e->ofd->vnode = &node;
    e->ofd->offset = 0;
    e->ofd->ino = 77;
    k_memset(resident_pages[1], 'b', PAGE_SIZE);
    k_memset(out, 0, sizeof(out));
    race_row = 1;
    race_cache = cache;
    race_reads = 0;
    bkl_host_free = 1;
    bkl_host_takes = 0;
    check(file_read(e, (char __user *)out, sizeof(out), NULL, sizeof(out)) == (int32_t)sizeof(out),
          how);
    check(out[0] == 'a' && out[PAGE_SIZE - 1] == 'a', "the first page comes from the disk");
    check(out[PAGE_SIZE] == 'S' && out[2 * PAGE_SIZE - 1] == 'S',
          "the second page has the store a mapping made while the first waited for the disk");
    check(race_reads == 1, "the second page is not read from the disk");
    check(e->ofd->offset == sizeof(out), "the offset moves past both pages");
    check(bkl_host_takes == (cache ? 1 : 2) && !bkl_held(),
          "the read takes the big lock for the mapping's page, and for the file system's read, "
          "and gives it back");
    check(resident_refs == 0, "the read lets the mapping's page go");
    bkl_host_free = 0;
    race_row = 0;
    race_cache = 0;
    race_mapped = 0;
    dshare_stores_in[dshare_index(77)] = 0;
    *dshare_bucket(race_ds.ino) = NULL;
    dshare_count = 0;
    e->ofd->ino = 0;
    e->ofd->vmnt = NULL;
    e->ofd->vnode = NULL;
    e->type = FD_TYPE_NONE;
    row_end();
}

/** Writes update a mapping made during refault, and core writes do likewise. */
static void mapped_writers(void)
{
    struct open_file ofd;
    fd_entry_t e;
    struct file_write_state state;
    struct core_file core;
    uint64_t at = PAGE_SIZE + 4;

    row_begin();
    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&e, 0, sizeof(e));
    k_memset(&state, 0, sizeof(state));
    k_memset(&core, 0, sizeof(core));
    race_mount.type = &mounted_type;
    ofd.vmnt = &race_mount;
    ofd.vnode = &node;
    ofd.ino = 77;
    e.type = FD_TYPE_FILE;
    e.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e.ofd = &ofd;
    state.file.mnt = &race_mount;
    state.file.node = &node;
    state.own = 1;
    proc.rlim[RLIM_FSIZE].cur = RLIM_INFINITY;
    mapped_write_row = race_row = 1;
    mapped_write_ready = race_mapped = 0;
    k_memset(mapped_disk, 'b', sizeof(mapped_disk));
    vfs_node_lock(&state.file);
    check(file_write_locked(&e, (const char __user *)"WR", 2, &at, &state) == 2 && race_mapped &&
              mapped_write_ready && resident_pages[1][4] == 'W' && resident_pages[1][5] == 'R' &&
              resident_pages[1][3] == 'S',
          "a first-buffer refault's new mapping sees the write and keeps its store beside it");
    vfs_node_unlock(&state.file);
    proc.sig_pending = 0;
    core.ofd = &ofd;
    core.pos = PAGE_SIZE + 8;
    check(core_file_put(&core, "CORE", 4) == 0 && resident_pages[1][8] == 'C' &&
              resident_pages[1][11] == 'E' && resident_pages[1][7] == 'S' && ns_depth == 0,
          "a core write updates the resident mapping under the file lock");
    mapped_write_row = race_row = race_mapped = 0;
    dshare_stores_in[dshare_index(77)] = 0;
    *dshare_bucket(77) = NULL;
    dshare_count = 0;
    row_end();
}

/** The race row, through the file system and through the file page cache
 * (`resident_race_once`). */
static void resident_race(void)
{
    resident_race_once(0, "a read through the file system returns both pages");
    resident_race_once(1, "a read through the file page cache returns both pages");
}

/**
 * A read without the big lock of a file whose description a sharer's seek
 * left at 2^63-1, while a writable shared mapping of the file has pages:
 * the read is -EINVAL, as Linux's rw_verify_area refuses it, before it
 * reaches a page, and holds no big lock after, as every answer of Linux's
 * ksys_read releases the position (fs/read_write.c:701-714).
 */
static void read_end_past_unlocked(void)
{
    static struct dshare ds;
    char out[2];
    fd_entry_t *e;
    int fd;

    row_begin();
    fd = fd_alloc(proc.fds);
    e = fd_slot(proc.fds, fd);
    e->type = FD_TYPE_FILE;
    e->flags = FD_FLAG_READ;
    e->ofd->vmnt = &mount;
    e->ofd->vnode = &node;
    e->ofd->offset = 0x7FFFFFFFFFFFFFFFull;
    e->ofd->ino = 77;
    ds.mnt = &mount;
    ds.ino = 77;
    ds.stores = 1;
    *dshare_bucket(ds.ino) = &ds;
    dshare_count = 1;
    dshare_stores_in[dshare_index(77)] = 1;
    bkl_host_free = 1;
    bkl_host_takes = 0;
    check(file_read(e, (char __user *)out, sizeof(out), NULL, sizeof(out)) == -EINVAL,
          "a read whose end a sharer's seek put past 2^63-1 is EINVAL");
    check(!bkl_held(), "and holds no big lock after");
    bkl_host_free = 0;
    dshare_stores_in[dshare_index(77)] = 0;
    *dshare_bucket(ds.ino) = NULL;
    dshare_count = 0;
    e->ofd->ino = 0;
    e->ofd->vmnt = NULL;
    e->ofd->vnode = NULL;
    e->type = FD_TYPE_NONE;
    row_end();
}

/**
 * An open without the big lock that the name cache finishes, of a regular
 * file on a type that reads without the lock, with descriptor 0 taken: it
 * returns descriptor 1, the one it installed, and is not made again under
 * the big lock, since the answer that sends a call there lies below every
 * errno and descriptor, as Linux's do_sys_openat2 returns the descriptor it
 * installs (fs/open.c:1419-1430).
 */
static void unlocked_open_one(void)
{
    int zero;

    row_begin();
    zero = fd_alloc(proc.fds);
    check(zero == 0, "descriptor 0 is taken first");
    mount.type = &unlocked_type;
    unlocked_row = 1;
    bkl_host_free = 1;
    bkl_host_takes = 0;
    check(open_from_user(AT_FDCWD, (const char __user *)"/f", 0, 0) == 1,
          "an unlocked open with descriptor 0 taken returns descriptor 1");
    check(bkl_host_takes == 0 && opens == 1,
          "and opens the file once, never again under the big lock");
    check(fd_get(proc.fds, 1) != NULL && fd_get(proc.fds, 2) == NULL,
          "and installs descriptor 1 alone");
    bkl_host_free = 0;
    unlocked_row = 0;
    mount.type = &mounted_type;
    row_end();
}

/* The uids of the protection rows: the caller and another user. */
#define PR_USER  1000u
#define PR_OTHER 1001u

/** End a protection row: no lock order is asked of it, and the caller's
 * group goes back to root's. */
static void pr_row_end(const char *what)
{
    fl_log[0] = '\0';
    fl_row_end("", what);
    proc.egid = 0;
}

/**
 * Link /fl/a/x, of `mode` and owned by `uid`, to the new name /fl/b/y, as
 * the caller `euid` in the group of the same number: the link's result. The
 * new name's directory is root's of mode 0777, so a link the protection lets
 * through is made.
 */
static int32_t pr_link(uint32_t mode, uint32_t uid, uint32_t euid)
{
    int32_t rc;

    fl_row_begin();
    proc.euid = euid;
    proc.egid = euid;
    fl_mode[FL_X] = mode;
    fl_uid[FL_X] = uid;
    rc = do_link("/fl/a/x", "/fl/b/y", 0, 0, 0);
    check(rc == 0 ? fl_linked == &fl_nodes[FL_X] : fl_linked == 0,
          "a link is made exactly when it is allowed");
    pr_row_end("a link gives back every lock");
    return rc;
}

/**
 * fs.protected_hardlinks, as Linux's may_linkat: a link to a file of another
 * user is EPERM unless the file is a regular one with no setuid bit, no
 * setgid bit with group execute, and both read and write open to the caller;
 * its owner and a caller with CAP_FOWNER link anything, and at 0 the rule is
 * off.
 */
static void protected_links(void)
{
    check(pr_link(0100644, PR_OTHER, PR_USER) == -EPERM,
          "a link to another user's file the caller may not write is EPERM");
    check(pr_link(0100666, PR_OTHER, PR_USER) == 0,
          "a link to another user's file the caller may read and write is made");
    check(pr_link(0104666, PR_OTHER, PR_USER) == -EPERM,
          "a link to another user's setuid file is EPERM");
    check(pr_link(0102676, PR_OTHER, PR_USER) == -EPERM,
          "a link to another user's setgid, group-executable file is EPERM");
    check(pr_link(0102666, PR_OTHER, PR_USER) == 0,
          "a link to another user's setgid file without group execute is made");
    check(pr_link(0010666, PR_OTHER, PR_USER) == -EPERM, "a link to another user's FIFO is EPERM");
    check(pr_link(0100400, PR_USER, PR_USER) == 0,
          "a link to the caller's own file it may not write is made");
    check(pr_link(0104600, PR_OTHER, 0) == 0, "root links another user's setuid file");
    vfs_protected_hardlinks = 0;
    check(pr_link(0100644, PR_OTHER, PR_USER) == 0,
          "with fs.protected_hardlinks at 0 any file is linked");
    vfs_protected_hardlinks = 1;
}

/**
 * Open /fl/b/w, of `mode` and owned by `uid`, with O_CREAT, O_DIRECTORY and
 * `extra`, as the caller `euid` in the group of the same number, with /fl/b
 * of `dmode` and owned by `duid`: the open's result. An open the protections
 * let through stops at O_DIRECTORY on a file that is no directory, ENOTDIR,
 * before anything is opened.
 */
static int32_t pr_create(uint32_t dmode, uint32_t duid, uint32_t mode, uint32_t uid, uint32_t euid,
                         int extra)
{
    int32_t rc;

    fl_row_begin();
    proc.euid = euid;
    proc.egid = euid;
    fl_mode[FL_B] = dmode;
    fl_uid[FL_B] = duid;
    fl_mode[FL_W] = mode;
    fl_uid[FL_W] = uid;
    rc = do_open("/fl/b/w", O_CREAT | O_DIRECTORY | extra, 0644);
    pr_row_end("an O_CREAT open gives back every lock");
    return rc;
}

/**
 * fs.protected_regular and fs.protected_fifos, as Linux's
 * may_create_in_sticky: an O_CREAT open of an existing file in a sticky
 * directory others may write is EACCES, for root too, when neither the
 * caller nor the directory's owner owns the file; at level 2 a directory its
 * group may write is enough; at 0 the rule is off for the kind; and a file
 * of another kind is judged whatever the levels.
 */
static void protected_creates(void)
{
    check(pr_create(041777, 0, 0100644, PR_OTHER, PR_USER, 0) == -EACCES,
          "O_CREAT on another user's file in /tmp is EACCES");
    check(pr_create(041777, 0, 0100644, PR_OTHER, 0, 0) == -EACCES,
          "O_CREAT on another user's file in /tmp is EACCES for root");
    check(pr_create(041777, 0, 0100644, PR_OTHER, PR_USER, O_EXCL) == -EEXIST,
          "O_CREAT|O_EXCL on another user's file is EEXIST");
    check(pr_create(041777, 0, 0100644, PR_USER, PR_USER, 0) == -ENOTDIR,
          "O_CREAT on the caller's own file in /tmp opens it");
    check(pr_create(041777, PR_OTHER, 0100644, PR_OTHER, PR_USER, 0) == -ENOTDIR,
          "O_CREAT on the directory owner's file opens it");
    check(pr_create(040777, 0, 0100644, PR_OTHER, PR_USER, 0) == -ENOTDIR,
          "O_CREAT on another user's file in a directory that is not sticky opens it");
    check(pr_create(041775, 0, 0100644, PR_OTHER, PR_USER, 0) == -EACCES,
          "O_CREAT on a file in a group-writable sticky directory is EACCES at level 2");
    vfs_protected_regular = 1;
    check(pr_create(041775, 0, 0100644, PR_OTHER, PR_USER, 0) == -ENOTDIR,
          "level 1 lets a group-writable sticky directory's file open");
    check(pr_create(041777, 0, 0100644, PR_OTHER, PR_USER, 0) == -EACCES,
          "level 1 still refuses a world-writable sticky directory's file");
    vfs_protected_regular = 0;
    check(pr_create(041777, 0, 0100644, PR_OTHER, PR_USER, 0) == -ENOTDIR,
          "fs.protected_regular at 0 lets the file open");
    vfs_protected_regular = 2;

    check(pr_create(041777, 0, 0010666, PR_OTHER, PR_USER, 0) == -EACCES,
          "O_CREAT on another user's FIFO in /tmp is EACCES");
    check(pr_create(041775, 0, 0010666, PR_OTHER, PR_USER, 0) == -ENOTDIR,
          "level 1 lets a group-writable sticky directory's FIFO open");
    vfs_protected_fifos = 2;
    check(pr_create(041775, 0, 0010666, PR_OTHER, PR_USER, 0) == -EACCES,
          "level 2 refuses a group-writable sticky directory's FIFO");
    vfs_protected_fifos = 0;
    check(pr_create(041777, 0, 0010666, PR_OTHER, PR_USER, 0) == -ENOTDIR,
          "fs.protected_fifos at 0 lets the FIFO open");

    vfs_protected_regular = 0;
    check(pr_create(041777, 0, 0020666, PR_OTHER, PR_USER, 0) == -EACCES,
          "O_CREAT on another user's device node in /tmp is EACCES at any level");
    vfs_protected_regular = 2;
    vfs_protected_fifos = 1;
}

/** A device's ioctl needs no inode lock; its last release still does. */
static void ioctl_entry_lock(void)
{
    static const struct file_ops unlocked_ops = {.flags = FILE_OPS_UNLOCKED};
    static const struct file_ops locked_ops = {0};
    struct open_file ofd;
    fd_entry_t entry;

    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&entry, 0, sizeof(entry));
    entry.type = FD_TYPE_DEVICE;
    entry.ofd = &ofd;
    entry.ops = &unlocked_ops;
    ofd.dev_mnt = &mount;
    check(file_ioctl_unlocked(&entry), "a marked device ioctl ignores its retained inode");
    check(!file_ops_unlocked(&entry), "a device inode's last release stays locked");
    entry.ops = &locked_ops;
    check(!file_ioctl_unlocked(&entry), "an unmarked device ioctl stays locked");
    entry.ops = NULL;
    check(!file_ioctl_unlocked(&entry), "a descriptor without device operations stays locked");
}

int main(int argc, char **argv)
{
    mount.type = &mounted_type;
    give_maps();
    if (argc > 1 && __builtin_strcmp(argv[1], "--race-only") == 0) {
        proc.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
        resident_race();
        if (!failures) {
            printf(
                "open-hosttest: result: pass: a read sees a mapping's store made while it slept\n");
        }
        return failures != 0;
    }
    ioctl_entry_lock();
    truncation();
    close_sleeping();
    deferred_release();
    dup2_source_closed();
    descriptor_errors();
    unlink_installing();
    program_kept();
    program_reclaim();
    rename_over_installing();
    mkdir_race();
    orphan_locks();
    rmdir_in_use();
    chdir_pins();
    attrs_after_move();
    stats_after_move();
    proc_fd_stat_errors();
    proc_fd_stat_closed();
    times_after_move();
    open_last_slot();
    loop_handles();
    lock_keys();
    offset_seek_waits();
    log_seek_range();
    dir_wide_rows();
    device_reopen_closed();
    orphan_reclaims();
    mapping_reclaims();
    file_locked_orders();
    file_locked_mount_rmdir();
    resident_unmapped();
    resident_race();
    mapped_writers();
    read_end_past_unlocked();
    unlocked_open_one();
    protected_links();
    protected_creates();
    if (!failures) {
        printf("open-hosttest: result: pass: mounted truncation, loop handles and root removals\n");
    }
    return failures != 0;
}

/** The file model has no watches to notify. */
void inotify_file(struct vfs_mount *mnt, uint64_t ino, uint32_t mask)
{
    (void)mnt;
    (void)ino;
    (void)mask;
}

/** The file model has no parent watches to notify. */
void inotify_child(struct vfs_mount *mnt, uint64_t ino, const char *name, uint32_t mask,
                   uint32_t cookie)
{
    (void)mnt;
    (void)ino;
    (void)name;
    (void)mask;
    (void)cookie;
}

/** Map changed attributes to the events the syscall layer reports. */
uint32_t vfs_setattr_events(int what)
{
    uint32_t mask = 0;
    if (what & (VFS_SET_MODE | VFS_SET_UID | VFS_SET_GID))
        mask |= IN_ATTRIB;
    if (what & VFS_SET_SIZE)
        mask |= IN_MODIFY;
    if ((what & (VFS_SET_ATIME | VFS_SET_MTIME)) == (VFS_SET_ATIME | VFS_SET_MTIME))
        mask |= IN_ATTRIB;
    else if (what & VFS_SET_ATIME)
        mask |= IN_ACCESS;
    else if (what & VFS_SET_MTIME)
        mask |= IN_MODIFY;
    return mask;
}
