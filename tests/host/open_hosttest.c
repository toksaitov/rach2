/**
 * open_hosttest.c: the open, close, dup and path calls of
 * rach2/sys/syscall_fs.c, with the descriptor tables of rach2/sys/fs/fd.c,
 * the loop devices of rach2/sys/block/loop.c and the file mappings of
 * rach2/sys/mm/filemap.c, on a model mounted type and a model KRFFS root.
 *
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/park_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The rows check:
 *
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
 * - a loop device's handle when its node moves;
 * - the key a file's locks are found by on each kind of mount;
 * - a seek of a /proc file's or /dev/bootlog's description while a read or
 *   a write of it sleeps, and the log's seeks past the offsets they can
 *   return;
 * - the release of the last holder of a file of the KRFFS root that lost
 *   its name, by each path a descriptor or a file mapping ends on, while
 *   another process's allocation on the root runs in the reclaim's sleep.
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
static long ksys_pread64(int fd, char __user *buf, uint32_t count, long long pos);
static long ksys_pwrite64(int fd, const char __user *buf, uint32_t count, long long pos);
static long ksys_fadvise64(int fd, long long len, uint32_t advice);
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
static long sys_mount(isr_params_t *params);
static long sys_umount2(isr_params_t *params);
static long sys_open(isr_params_t *params);
static long sys_openat(isr_params_t *params);
static long sys_close(isr_params_t *params);
static long sys_fsync(isr_params_t *params);
static long sys_sync(isr_params_t *params);
static long sys_syncfs(isr_params_t *params);
static long sys_lseek(isr_params_t *params);
static int sock_is_query(const struct socket *sock);
static long sys_read(isr_params_t *params);
static long sys_pread64(isr_params_t *params);
static long sys_pwrite64(isr_params_t *params);
static long sys_preadv(isr_params_t *params);
static long sys_pwritev(isr_params_t *params);
static int32_t readv_dgram(fd_entry_t *entry, uintptr_t iov_user, int iovcnt, uint32_t *src_ip,
                           uint16_t *src_port, int peek, int32_t *whole);
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
static long sys_getdents64(isr_params_t *params);
static long sys_dup3(isr_params_t *params);
static long sys_statx(isr_params_t *params);
static long sys_memfd_create(isr_params_t *params);
static long sys_flock(isr_params_t *params);
static long sys_close_range(isr_params_t *params);
static int32_t fs_core_file(int signo, unsigned long limit);
static long sys_mknod(isr_params_t *params);
static long sys_mknodat(isr_params_t *params);
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
#include "block/loop.c"
#include "mm/filemap.c"
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
struct percpu cpu_table[NR_CPUS] = {{.current = &proc}};
process_t *proc_list;
volatile uint32_t fb_owner_pid;
krffs_file_system_t *krffs_fs;
static int root_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **found);
static const struct vfs_ops root_ops = {.lookup = root_lookup};
const struct vfs_type vfs_krffs_type = {"krffs", 0, &root_ops, 1, 0};
/* A mounted KRFFS volume's type and the two FAT types, which the lock key
 * row names. */
const struct vfs_type vfs_krffs_volume_type = {"krffs", 0, 0, 1, 0};
const struct vfs_type vfs_vfat_type = {"vfat", 0, 0, 0, 0};
const struct vfs_type vfs_exfat_type = {"exfat", 0, 0, 0, 0};
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
int64_t rtc_boot_time(void)
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

/** Fail the row: no row opens a device node. */
const char *devfs_name_of(int block, uint32_t rdev)
{
    (void)block;
    (void)rdev;
    check(0, "unexpected device node open");
    return 0;
}

/** Fail the row: no row opens a device by name. */
int devfs_open(const char *name, int flags)
{
    (void)name;
    (void)flags;
    check(0, "unexpected device open");
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

/** No row opens the framebuffer or the mouse by name. */
uint8_t *fb_enter(void)
{
    check(0, "unexpected framebuffer open");
    return 0;
}

/** Fail the row: no row opens the framebuffer. */
void kbd_clear_scancodes(void)
{
    check(0, "unexpected framebuffer open");
}

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

/** Report an accessible regular file with set-id bits, or in the mkdir rows
 * the directory a name is made in. A read the mount's lock does not cover
 * may sleep for the disk, and the call queued for the row, a write that
 * moves the file, runs then. */
int vfs_getattr(struct vfs_mount *mnt, void *at, struct vfs_stat *st)
{
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

/** Return an open identity distinct from the node. */
int vfs_open_node(struct vfs_mount *mnt, void *at, int flags, void **out)
{
    (void)flags;
    check(mnt == &mount && at == &node, "open uses node identity");
    check(installing < 0 || (fd_get(proc.fds, installing) == NULL &&
                             proc.fds->entries[installing].type == FD_TYPE_NONE &&
                             proc.fds->entries[installing].ofd),
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
uint32_t filecache_epoch(void)
{
    return 0;
}

/** Keep no page. */
void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    (void)e;
    (void)pgoff;
    (void)phys;
    (void)epoch;
}

/** Require a live open handle before truncation. */
int vfs_truncate(struct vfs_mount *mnt, void *at, uint64_t size)
{
    check(mnt == &mount && at == &handle && opens == releases + 1 && size == 0,
          "mounted truncation uses a live open handle");
    check(installing < 0 || (fd_get(proc.fds, installing) == NULL &&
                             proc.fds->entries[installing].type == FD_TYPE_NONE),
          "an open's descriptor is not published while it truncates");
    fd_ofd_repoint(mnt, &node, &moved);
    relocate = 1;
    truncates++;
    return truncate_error;
}

/** Release a successfully opened handle, never more than were opened. */
void vfs_release(struct vfs_mount *mnt, void *at)
{
    check(mnt == &mount && at == &handle && opens > releases, "release balances successful open");
    releases++;
}

/** Record reference cleanup on failed open. */
void vfs_ref_release(struct vfs_ref *ref)
{
    check(ref->mnt == &mount, "failed open releases resolution");
    dropped++;
}

/** Record successful transfer into a description. */
void vfs_desc_open(struct vfs_mount *mnt)
{
    (void)mnt;
    refs++;
}

/** Record completion of the resolution's mount operation. */
void vfs_leave(struct vfs_mount *mnt)
{
    (void)mnt;
    leaves++;
}

/** Close the handle when the description is freed, which is where a release
 * sleeps: run what the row has happen meanwhile. The orphan rows'
 * descriptions open nothing on the root, and have nothing to close. */
void vfs_desc_close(struct vfs_mount *mnt, void *at, void *opened)
{
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

/** No row reads, shares or cuts the pages of a mapping. */
void *kmap_temp(uintptr_t phys)
{
    (void)phys;
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

/** The shared mappings of disk files, which no row makes: a row's mapping
 * is private, and no page of a shared one is resident. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
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

/** Fail the row: no shared page gains a reference. */
void frame_ref(uintptr_t phys)
{
    (void)phys;
    check(0, "unexpected reference to a shared page");
}

/** Fail the row: no shared page is freed. */
void free_frame(uintptr_t phys)
{
    (void)phys;
    check(0, "unexpected free of a shared page");
}

/** Fail the row: no row holds a mapping. */
void vm_file_get(struct vm_file *f)
{
    (void)f;
    check(0, "unexpected hold of a mapping");
}

/** Fail the row: no row puts a mapping. */
void vm_file_put(struct vm_file *f)
{
    (void)f;
    check(0, "unexpected put of a mapping");
}

/** Fail the row: no row reads a mapped file. */
int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset)
{
    (void)mnt;
    (void)handle;
    (void)buf;
    (void)count;
    (void)offset;
    check(0, "unexpected read of a mapped file");
    return -EIO;
}

/** Fail the row: no row writes a mapped file. */
int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset)
{
    (void)mnt;
    (void)handle;
    (void)buf;
    (void)count;
    (void)offset;
    check(0, "unexpected write of a mapped file");
    return -EIO;
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

/** Fail the row: no row owns the display. */
void fb_exit(void)
{
    check(0, "unexpected display release");
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

/** The fsync of a file whose shared mapping is synced, which no row makes. */
int vfs_fsync(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
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
/** No device of /sys has a key: -1. */
int sysfs_device_key(int node, int n)
{
    (void)node;
    (void)n;
    return -1;
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

/** No row seeks a directory, whose listing a seek would read. */
int vfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    (void)mnt;
    (void)handle;
    (void)cursor;
    (void)ent;
    check(0, "unexpected directory listing");
    return -EIO;
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
    (void)mnt;
    (void)node;
    (void)name;
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

/** No row's path is a directory of /proc. */
int procfs_resolve_id(const struct vpath *vp)
{
    (void)vp;
    check(0, "unexpected /proc lookup");
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

/** Fail the row: no row enters a process's root. */
int vfs_enter(struct vfs_mount *mnt)
{
    (void)mnt;
    check(0, "unexpected entry to a process's root");
    return -EBUSY;
}

/** Fail the row: no row holds a process's root. */
void vfs_ref_hold(const struct vfs_ref *r)
{
    (void)r;
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
        check(rc < 0 || (rc == installing && fd_get(proc.fds, rc) == &proc.fds->entries[rc]),
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
        if (proc.fds->entries[fd].type != FD_TYPE_NONE || proc.fds->entries[fd].ofd) {
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
    k_memset(proc.fd_held, 0, sizeof(proc.fd_held));
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
        proc.fds->entries[fd].node = &victim;
        proc.fds->entries[fd].type = FD_TYPE_FILE;
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
    orphan_ofd = proc.fds->entries[program_fd].ofd;
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
        e = &proc.fds->entries[fd];
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
    e = &proc.fds->entries[fd];
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
        proc.fds->entries[fd].ofd->vmnt = 0;
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
        check((round == 0 ? do_chmod("f", 0600, 0) : do_chown("f", 5, (uint32_t)-1, 1, 0)) == 0,
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
            rc = fstatat_path("/f", 1, &wide);
        } else {
            rc = do_access("f", R_OK, 0);
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
        struct user_stat st;
        struct open_file *ofd;
        int fd;

        row_begin();
        fd = model_open();
        check(fd >= 0, "open a descriptor for a followed stat");
        if (fd >= 0) {
            proc.fds->entries[fd].type = directory ? FD_TYPE_DIR : FD_TYPE_FILE;
            ofd = proc.fds->entries[fd].ofd;
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
        proc.fds->entries[fd].type = FD_TYPE_FILE;
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
              (proc.fds->entries[releasing].flags & FD_FLAG_RELEASING),
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
        check(proc.fds->entries[fd].type == FD_TYPE_NONE && !proc.fds->entries[fd].ofd,
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
    third.fd_held[releasing / 32] |= 1u << (releasing % 32);
    third.fd_holding = 1;
    fd_holds_release();
    current_proc = self;
    check(proc.fds->entries[releasing].type == FD_TYPE_FILE && proc.fds->entries[releasing].ofd,
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
        other.fd_held[fd / 32] |= 1u << (fd % 32);
        other.fd_holding = 1;
        check(close_fd(fd) == 0 && (proc.fds->entries[fd].flags & FD_FLAG_CLOSING) && refs == 1,
              "a close of a descriptor another call holds leaves it to that call");
        check(lock_closes == 1 && lock_closer == &proc,
              "that close drops the closer's record locks at once");
        fd_holds_release();
        check(proc.fds->entries[fd].type == FD_TYPE_FILE, "the closer's call end leaves it");
        current_proc = &other;
        releasing = fd;
        in_release = third_call_ends;
        fd_holds_release();
        current_proc = &proc;
        check(proc.fds->entries[fd].type == FD_TYPE_NONE && refs == 0 && in_release == 0,
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
    check(close_fd(dup_source) == 0 && (proc.fds->entries[dup_source].flags & FD_FLAG_CLOSING),
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
        check(fd_get(proc.fds, target) == &proc.fds->entries[target] &&
                  !(proc.fds->entries[target].flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)),
              "the copy is an open descriptor");
        fd_holds_release();
        current_proc = &other;
        fd_holds_release();
        current_proc = &proc;
        check(proc.fds->entries[dup_source].type == FD_TYPE_NONE &&
                  proc.fds->entries[target].type == FD_TYPE_FILE &&
                  proc.fds->entries[target].ofd->refcount == 1 && refs == 1,
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

/** Check node repointing without changing a mounted open identity. */
static void loop_handles(void)
{
    loops[0].mnt = &mount;
    loops[0].attached = 1;
    loops[0].node = &node;
    loops[0].handle = &node;
    loop_repoint(&mount, &node, &moved);
    check(loops[0].node == &moved && loops[0].handle == &node,
          "mounted loop handle stays stable when node moves");
    check(loop_backs(&mount, &node) && loop_backs(&mount, &moved),
          "loop attachment recognizes node and open handle");
    mount.type = &vfs_krffs_type;
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
        llseek_bootlog(&sharer_files.entries[0], 16, 1, (long long __user *)&sharer_result);
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

static const struct file_ops mem_ops = {mem_read, mem_write, 0, 0, 0};

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

static const struct file_ops log_ops = {log_read, 0, 0, 0, 0};

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
        e = &proc.fds->entries[0];
        e->type = cases[i].type;
        e->ops = cases[i].ops;
        e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
        e->ofd->offset = 0x1000;
        log_len = 0x2000;
        sharer_files.refs = 1;
        sharer_files.entries[0] = *e;
        e->ofd->refcount++;
        other.fds = &sharer_files;
        parked_on = 0;
        sharer_seek = 0;
        sharer_call = cases[i].seek;
        moved_bytes =
            cases[i].writing ? write_ops(e, buf, sizeof(buf)) : read_ops(e, buf, sizeof(buf));
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
    e = &proc.fds->entries[0];
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
    e = &proc.fds->entries[fd];
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
    fd_release_backing(&proc.fds->entries[orphan_fd]);
    fd_free(proc.fds, orphan_fd);
}

/** The close of the descriptor while another process's call holds it, which
 * leaves the release to that call's end (`fd_holds_release`). */
static void orphan_held_close(void)
{
    other.fd_held[orphan_fd / 32] |= 1u << (orphan_fd % 32);
    other.fd_holding = 1;
    check(close_fd(orphan_fd) == 0 && proc.fds->entries[orphan_fd].type != FD_TYPE_NONE,
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
    vf = mapped_fd >= 0 ? filemap_new(&proc.fds->entries[mapped_fd]) : NULL;
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
int main(void)
{
    mount.type = &mounted_type;
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
    times_after_move();
    open_last_slot();
    loop_handles();
    lock_keys();
    offset_seek_waits();
    log_seek_range();
    orphan_reclaims();
    mapping_reclaims();
    if (!failures) {
        printf("open-hosttest: result: pass: mounted truncation, loop handles and root removals\n");
    }
    return failures != 0;
}
