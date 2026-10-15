/**
 * corefile_hosttest.c: the core file `fs_core_file` writes, its descriptor
 * and its limit.
 *
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * `syscall_fs.c` is included whole, its unused exports private so that the
 * compiler removes them and what only they reach. The notes and the bytes of
 * the file come from a model of `core_notes` and `core_write`.
 * coredump_hosttest.c checks `core_write` itself, and `core_notes` cannot
 * run here: it reads each thread's registers at the 32-bit address of its
 * kernel stack, where a 64-bit host has nothing. The model writes what the
 * dump of a process whose last page was never touched writes: a header, a
 * hole to the next page, a page, and a hole of a page at the end; it counts
 * the bytes toward the limit as `core_write` does. The file is on a model
 * mounted type whose one directory is the working directory; the open makes
 * "core" there, and its writes are kept in memory.
 *
 * The writes of the dump can sleep, and while one does, a process that
 * shares the descriptor table can run. The model runs that process at the
 * start of `core_write`: it writes through the core file's descriptor and
 * duplicates it, and both must find no descriptor, since the end of the dump
 * frees what the descriptor holds. With a limit of the bytes the dump writes
 * before the zero at its end, the zero must not be written, the file must end
 * with the last page written and the call must answer EFBIG; with one byte
 * more, the zero is written at the file's last byte.
 */
#include "isr.h"
#include "lib/kuser.h"
#include "fs/fd.h"
#include "fs/vfs.h"
#include "fs/krffs.h"
#include "network/socket/socket.h"
/* The exports of syscall_fs.c that no row calls, made static. */
static int fs_node_is_held(const krffs_node_t *node);
static void fs_node_moved(const krffs_node_t *from, krffs_node_t *to);
static void fd_release_backing(fd_entry_t *e);
static void fd_holds_release(void);
static long sys_ftruncate(isr_params_t *params);
static long ksys_ftruncate(int fd, uint64_t length);
static long ksys_truncate(const char __user *upath, long long length);
static long ksys_pread64(int fd, char __user *buf, uint32_t count, long long pos);
static long ksys_pwrite64(int fd, const char __user *buf, uint32_t count, long long pos);
static long ksys_fadvise64(int fd, long long len, uint32_t advice);
static long ksys_fallocate(int fd, uint32_t mode, uint64_t offset, uint64_t len);
static long sys_write(isr_params_t *params);
static long sys_writev(isr_params_t *params);
static int canonicalize_abs(char *path);
static int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath, int *end);
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
static long sys_mknod(isr_params_t *params);
static long sys_mknodat(isr_params_t *params);
static int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino);
static void fs_socket_unbind(struct vfs_ref *ref);
static int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino);
static int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off);
#include "syscall_fs.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

/* The model dump: a header, a hole to the next page, a page the process
 * has, and a page it never touched, a hole at the file's end. */
#define MODEL_HEADER 96u
#define MODEL_LENGTH (3u * PAGE_SIZE)
#define MODEL_BYTES  (MODEL_HEADER + PAGE_SIZE)

/* The dumping process and the process that shares its descriptor table,
 * each the only thread of its process. */
static process_t self, sharer;
static struct tgroup group, sharer_group;
static struct fs_struct dirs;
static fd_table_t files;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
process_t *proc_list = &self;
static int failures;

/* The model mounted type: its working directory, the file "core" once the
 * open has made it, and the file's bytes. */
static const struct vfs_ops model_ops;
static const struct vfs_type model_type = {"model", 0, &model_ops, 0, 0};
static struct vfs_mount mount;
static int cwd_node, core_node;
static int core_made, opens, releases, ns_depth;
static uint8_t file[MODEL_LENGTH];
static uint32_t file_len, file_written;

/* What the process sharing the table does while the dump waits, and what
 * its calls answered. */
static void (*meanwhile)(void);
static int32_t sharer_write, sharer_dup;
static int sharer_writes;

/** Record every failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("corefile-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** The descriptor whose description is the core file's, or -1. */
static int core_fd(void)
{
    int fd;

    for (fd = 0; fd < files.cap; fd++) {
        if (files.entries[fd].ofd && files.entries[fd].ofd->vnode == &core_node) {
            return fd;
        }
    }
    return -1;
}

/** The notes of the model dump, from the heap as `core_notes` gives them. */
uint8_t *core_notes(int signo, uint32_t *size)
{
    uint8_t *notes = kzalloc(20);

    check(signo == SIGABRT, "the notes are built for the signal that ends the process");
    *size = 20;
    return notes;
}

/**
 * Write the model dump through `write`: first the row's `meanwhile`, which
 * runs once, as another process runs while a write sleeps; then the header,
 * the pages and the holes, the bytes counted toward `limit` as `core_write`
 * counts them. Returns 0, -EFBIG when a part with bytes passes the limit, or
 * the error of `write`.
 */
int32_t core_write(core_write_fn write, void *ctx, const uint8_t *notes, uint32_t notes_size,
                   uint64_t limit)
{
    static const uint8_t header[MODEL_HEADER] = {0x7F, 'E', 'L', 'F'};
    static uint8_t page[PAGE_SIZE];
    const uint8_t *parts[4];
    uint32_t lens[4];
    uint64_t done = 0;
    int32_t rc;
    int i;

    check(notes && notes_size == 20, "the dump writes the notes it was given");
    if (meanwhile) {
        void (*run)(void) = meanwhile;

        meanwhile = 0;
        run();
    }
    k_memset(page, 0x5A, sizeof(page));
    parts[0] = header;
    lens[0] = MODEL_HEADER;
    parts[1] = NULL;
    lens[1] = PAGE_SIZE - MODEL_HEADER;
    parts[2] = page;
    lens[2] = PAGE_SIZE;
    parts[3] = NULL;
    lens[3] = PAGE_SIZE;
    for (i = 0; i < 4; i++) {
        if (parts[i] && lens[i] > limit - done) {
            return -EFBIG;
        }
        rc = write(ctx, parts[i], lens[i]);
        if (rc != 0) {
            return rc;
        }
        if (parts[i]) {
            done += lens[i];
        }
    }
    return 0;
}

/** The kernel's zeroed allocation, from the host's heap. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** Free a block from `kzalloc`. */
void kfree(void *p)
{
    free(p);
}

/** A path buffer of `VFS_PATH_MAX` bytes from the heap. */
char *path_buf_get(void)
{
    return calloc(1, VFS_PATH_MAX);
}

/** Free a buffer from `path_buf_get`. */
void path_buf_put(char *buf)
{
    free(buf);
}

/** Every process's working directory is the model type's one directory. */
const char *proc_cwd(const struct process *p)
{
    (void)p;
    return "/";
}

/** The model mount is the root mount. Its type is not KRFFS's, so its nodes
 * take the paths of a mounted type. */
struct vfs_mount *vfs_root_mount(void)
{
    return &mount;
}

/* KRFFS's type, which no mount of the model has. */
const struct vfs_type vfs_krffs_type = {"krffs", 0, &model_ops, 1, 0};

/** Resolve "core" in the working directory: the directory, with "core" in
 * `leaf`, for a walk to the parent, and the file once the open has made it,
 * -ENOENT before. */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t leafcap)
{
    (void)start;
    check(ns_depth == 0, "a walk starts with no namespace lock held");
    check(__builtin_strcmp(path, "/core") == 0 || __builtin_strcmp(path, "core") == 0,
          "the dump names the file core in the working directory");
    out->mnt = &mount;
    if (flags & VFS_WALK_PARENT) {
        k_strncpy(leaf, "core", leafcap);
        out->node = &cwd_node;
        return 0;
    }
    if (!core_made) {
        return -ENOENT;
    }
    out->node = &core_node;
    return 0;
}

/** The model type takes no walk lock. */
void vfs_walk_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
}

/** Count a hold of the namespace lock, which no walk may start under. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    (void)mnt;
    ns_depth++;
}

/** Let go of a hold of the namespace lock that `vfs_ns_lock` counted. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    check(ns_depth > 0, "the namespace lock is taken before it is given back");
    ns_depth--;
}

/** No row's file is a program's: 0. */
int mm_exe_holds(const struct vfs_mount *mnt, const void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** No row takes a second reference; a call fails the row. */
void vfs_ref_hold(const struct vfs_ref *r)
{
    (void)r;
    check(0, "unexpected reference hold");
}

/** Let go of a reference, which the model does not count. */
void vfs_ref_release(struct vfs_ref *ref)
{
    (void)ref;
}

/** No node of the model is in /dev, /proc or /sys: NULL. */
const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return NULL;
}

/** Nothing is mounted on a node of the model: NULL. */
struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node, const char *name)
{
    (void)mnt;
    (void)node;
    (void)name;
    return NULL;
}

/** The attributes of the working directory, anyone's to write, and of the
 * file, root's, with the length written so far. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = node == &cwd_node ? 040777u : 0100600u;
    st->ino = node == &cwd_node ? 1u : 2u;
    st->size = file_len;
    return 0;
}

/** Take every change of attributes: 0. */
int vfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    (void)what;
    (void)st;
    return 0;
}

/** No core file is there before the dump makes one: -ENOENT. */
int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name)
{
    (void)mnt;
    (void)dir;
    (void)name;
    return -ENOENT;
}

/** Make "core", which must be of mode 0600 in the working directory. */
int vfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    (void)mnt;
    check(dir == &cwd_node && __builtin_strcmp(name, "core") == 0 && mode == 0600u,
          "the dump makes core, of mode 0600, in the working directory");
    core_made = 1;
    *node = &core_node;
    return 0;
}

/** Open `node`, whose handle is the node itself, and count the open. */
int vfs_open_node(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)mnt;
    (void)flags;
    opens++;
    *handle = node;
    return 0;
}

/** The model type counts no open descriptions. */
void vfs_desc_open(struct vfs_mount *mnt)
{
    (void)mnt;
}

/** The model type counts no calls in progress. */
void vfs_leave(struct vfs_mount *mnt)
{
    (void)mnt;
}

/** Count the last close of the file's description. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)mnt;
    check(node == &core_node && handle == &core_node, "the description closed is the file's");
    releases++;
}

/** Keep the dump's bytes at their place in `file`, and count a write of the
 * process that shares the table without keeping it. Returns `count`, or
 * -EFBIG past the end of `file`. */
int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset)
{
    (void)mnt;
    check(handle == &core_node, "a write goes to the file's open handle");
    if (current_proc == &sharer) {
        sharer_writes++;
        return (int32_t)count;
    }
    if (offset + count > sizeof(file)) {
        return -EFBIG;
    }
    k_memcpy(file + offset, buf, count);
    if (offset + count > file_len) {
        file_len = (uint32_t)(offset + count);
    }
    file_written += count;
    return (int32_t)count;
}

/** A user buffer of the model passes the range check. */
int user_buffer_ok(uintptr_t addr, size_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

/** No row copies from a user buffer: -1. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

/** No row copies to a user buffer: -1. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

/** No row's file backs a loop device: 0. */
int loop_backs(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** No row's file is locked: 0. */
int lock_any(void)
{
    return 0;
}

/** No row's file has a lock to drop. */
void lock_close(const struct lock_key *key)
{
    (void)key;
}

/** No row's description has a lock to drop. */
void lock_ofd_gone(const struct open_file *ofd)
{
    (void)ofd;
}

/** No row's file is held by a message in flight: 0. */
int unix_inflight_holds(const void *node)
{
    (void)node;
    return 0;
}

/** No row's file is held by a mapping: 0. */
int filemap_holds(const void *node)
{
    (void)node;
    return 0;
}

krffs_file_system_t *krffs_fs;

/** No node of the KRFFS root is in a row: 0. */
uint64_t krffs_node_ino(const krffs_node_t *node)
{
    (void)node;
    return 0;
}

/** The file type bits of `mode`. */
uint32_t krffs_mode_type(uint16_t mode)
{
    return mode & 0170000u;
}

/** No row removes a node of the KRFFS root; a call fails the row. */
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    (void)fs;
    (void)node;
    check(0, "unexpected root node removal");
}

/** No row unlinks a node of the KRFFS root; a call fails the row. */
void krffs_unlink_held(krffs_node_t *node)
{
    (void)node;
    check(0, "unexpected root node unlink");
}

/** Make `wq` empty. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** No row has a waiter to wake. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** No row waits for a description; a call fails the row. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "unexpected description wait");
    return 0;
}

/* The state of the display and the console, which no row reaches. */
volatile uint32_t fb_owner_pid;
gfx_info_t gfx_current;
struct tty console_tty;

/** No row opens the framebuffer: NULL. */
uint8_t *fb_enter(void)
{
    return NULL;
}

/** No row lets go of the framebuffer. */
void fb_exit(void)
{}

/** No row reads the keyboard. */
void kbd_clear_scancodes(void)
{}

/** No mouse: 0. */
bool mouse_is_available(void)
{
    return 0;
}

/** No mouse: 0. */
int input_mouse_present(void)
{
    return 0;
}

/** No row reads the mouse. */
void mouse_clear_packets(void)
{}

/** No row opens a /dev name: -ENOENT. */
int devfs_open(const char *name, int flags)
{
    (void)name;
    (void)flags;
    return -ENOENT;
}

/** No row looks up a /dev name: -ENOENT. */
int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino)
{
    (void)name;
    (void)mode;
    (void)rdev;
    (void)ino;
    return -ENOENT;
}

/** The type of a device descriptor, which no row has: 0. */
uint32_t devfs_fd_mode(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No row's descriptor is a raw device: 0. */
int devfs_fd_is_raw(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No row's descriptor is the kernel log: 0. */
int devfs_fd_log_size(struct fd_entry *e, uint32_t *size)
{
    (void)e;
    (void)size;
    return 0;
}

/** No row's descriptor is a device whose transfers move the offset: 0. */
int devfs_fd_moves_offset(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No pseudo-terminal: NULL. */
struct pty *pty_get(int index)
{
    (void)index;
    return NULL;
}

/** No pseudo-terminal: -ENXIO. */
int pty_open_slave(int index, int flags)
{
    (void)index;
    (void)flags;
    return -ENXIO;
}

/** The boot time, in seconds of the epoch: 0. */
int64_t rtc_boot_time(void)
{
    return 0;
}

/** No row's descriptor is a socket. */
void socket_ref(struct socket *sock)
{
    (void)sock;
}

/** No row's descriptor is a socket. */
void socket_unref(struct socket *sock)
{
    (void)sock;
}

/** No row writes to a TCP socket: -EBADF. */
int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock)
{
    (void)sock;
    (void)buf;
    (void)len;
    (void)nonblock;
    return -EBADF;
}

/** No row writes to a datagram socket: -EBADF. */
int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port, const uint8_t *buf,
                   uint16_t len)
{
    (void)sock;
    (void)dst_ip;
    (void)dst_port;
    (void)buf;
    (void)len;
    return -EBADF;
}

/** No process of a row is in the background: 0. */
int tty_is_background(struct tty *t, int sid, int pgid)
{
    (void)t;
    (void)sid;
    (void)pgid;
    return 0;
}

/** No row signals a process group. */
void tty_signal_group(int pgid, int signo)
{
    (void)pgid;
    (void)signo;
}

/** Take every byte written to the console: `count`. */
int tty_write(struct tty *t, const unsigned char *kbuf, int count)
{
    (void)t;
    (void)kbuf;
    return count;
}

/** No row opens a /proc file: -ENOENT. */
int procfs_open(const struct vpath *vp, int flags)
{
    (void)vp;
    (void)flags;
    return -ENOENT;
}

/** No row names a /proc process: -1. */
int procfs_resolve_pid(int pid)
{
    (void)pid;
    return -1;
}

/** No row reads /proc: 0. */
int procfs_may_read(int pid)
{
    (void)pid;
    return 0;
}

/** No row names a /proc process: -1. */
int procfs_resolve_id(const struct vpath *vp)
{
    (void)vp;
    return -1;
}

/** The link count of a /proc task directory, which no row asks for: 0. */
uint32_t procfs_task_links(int pid)
{
    (void)pid;
    return 0;
}

/** No row reads a /proc/PID/fd link: -ENOENT. */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    (void)pid;
    (void)fd;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

/** No row reads a /proc/PID/exe link: -ENOENT. */
int procfs_readlink_exe(int pid, char *buf, int cap)
{
    (void)pid;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

/** The inode number of a /proc/PID directory, which no row reads: 0. */
uint32_t procfs_ino_pid(int pid)
{
    (void)pid;
    return 0;
}

/* The descriptor, process and mount lookups of fstat and of a descriptor's
 * path, which no row reaches: GCC keeps their callers, so a link needs
 * them. */

/** No row looks up a process: NULL. */
process_t *get_process(uint32_t pid)
{
    (void)pid;
    return NULL;
}

/** No row reads a /proc/PID/oom file: -ENOENT. */
int procfs_oom_owner(int pid, uint32_t *uid, uint32_t *gid)
{
    (void)pid;
    (void)uid;
    (void)gid;
    return -ENOENT;
}

/** No row's descriptor is a /proc file: -1. */
int procfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid)
{
    (void)e;
    (void)uid;
    (void)gid;
    return -1;
}

/** No row's descriptor is a /proc file: 0. */
int procfs_fd_writable(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No row's descriptor is a /proc file: 0. */
uint32_t procfs_fd_ino(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No row's descriptor is a pseudo-terminal: -1. */
int pty_slave_index(struct fd_entry *e)
{
    (void)e;
    return -1;
}

/** No row's descriptor is a Unix socket: 0. */
uint32_t unix_inode(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

/** No row resolves a path on the KRFFS root: NULL. */
krffs_node_t *krffs_resolve(const char *path)
{
    (void)path;
    return NULL;
}

/** No row asks a KRFFS node's path: -1. */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size)
{
    (void)node;
    (void)buf;
    (void)size;
    return -1;
}

/** No mount table to walk: 0 slots. */
int vfs_mount_slots(void)
{
    return 0;
}

/** No mount table to walk: NULL. */
struct vfs_mount *vfs_mount_at(int idx)
{
    (void)idx;
    return NULL;
}

/** The inode number of /proc, which no row reads: 0. */
uint32_t procfs_ino_dir(void)
{
    return 0;
}

/** The inode number of /proc/cmdline, which no row reads: 0. */
uint32_t procfs_ino_cmdline(void)
{
    return 0;
}

/** The inode number of /proc/net, which no row reads: 0. */
uint32_t procfs_ino_netdir(void)
{
    return 0;
}

/** The inode number of /proc/net/dev, which no row reads: 0. */
uint32_t procfs_ino_netdev(void)
{
    return 0;
}

/** The inode number of /proc/disk, which no row reads: 0. */
uint32_t procfs_ino_disk(void)
{
    return 0;
}

/** The inode number of /proc/uptime, which no row reads: 0. */
uint32_t procfs_ino_uptime(void)
{
    return 0;
}

/** The inode number of /proc/stat, which no row reads: 0. */
uint32_t procfs_ino_statsys(void)
{
    return 0;
}

/** The inode number of /proc/meminfo, which no row reads: 0. */
uint32_t procfs_ino_meminfo(void)
{
    return 0;
}

/* inotify, which no row watches, and the calls of the path walk of
 * inotify_add_watch, which no row makes. */
int inotify_watches;

/** No row's description is watched. */
void inotify_desc(struct open_file *ofd, uint32_t mask)
{
    (void)ofd;
    (void)mask;
}

/** No row's descriptor is an inotify instance: 0. */
int inotify_is(int fd)
{
    (void)fd;
    return 0;
}

/** No row adds a watch: -EINVAL. */
int32_t inotify_watch(int fd, struct vfs_mount *mnt, uint64_t ino, int isdir, uint32_t mask)
{
    (void)fd;
    (void)mnt;
    (void)ino;
    (void)isdir;
    (void)mask;
    return -EINVAL;
}

/** No row copies a path from user memory: -EFAULT. */
int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -EFAULT;
}

/** No row's path walk enters a mount: -EIO. */
int vfs_enter(struct vfs_mount *mnt)
{
    (void)mnt;
    return -EIO;
}

/** No row asks a node's path: -ENOENT. */
int vfs_node_path(struct vfs_mount *mnt, void *node, char *out, uint32_t cap)
{
    (void)mnt;
    (void)node;
    (void)out;
    (void)cap;
    return -ENOENT;
}

/** The inode number of /proc/cpuinfo, which no row reads: 0. */
uint32_t procfs_ino_cpuinfo(void)
{
    return 0;
}

/** The inode number of /proc/loadavg, which no row reads: 0. */
uint32_t procfs_ino_loadavg(void)
{
    return 0;
}

/** The inode number of /proc/diskstats, which no row reads: 0. */
uint32_t procfs_ino_diskstats(void)
{
    return 0;
}

/** The inode number of /proc/version, which no row reads: 0. */
uint32_t procfs_ino_version(void)
{
    return 0;
}

/** The inode number of /proc/partitions, which no row reads: 0. */
uint32_t procfs_ino_partitions(void)
{
    return 0;
}

/** The inode number of /proc/vmstat, which no row reads: 0. */
uint32_t procfs_ino_vmstat(void)
{
    return 0;
}

/** The inode number of /proc/slabinfo, which no row reads: 0. */
uint32_t procfs_ino_slabinfo(void)
{
    return 0;
}

/** The inode number of /proc/swaps, which no row reads: 0. */
uint32_t procfs_ino_swaps(void)
{
    return 0;
}

/** The inode number of /proc/locks, which no row reads: 0. */
uint32_t procfs_ino_locks(void)
{
    return 0;
}

/** The inode number of /proc/net/stat, which no row reads: 0. */
uint32_t procfs_ino_netstatdir(void)
{
    return 0;
}

/** The inode number of /proc/sysvipc, which no row reads: 0. */
uint32_t procfs_ino_sysvipcdir(void)
{
    return 0;
}

/** The inode number of a /proc/sysvipc file, which no row reads: 0. */
uint32_t procfs_ino_sysvipc(int origin)
{
    (void)origin;
    return 0;
}

/** No /sys file is writable: 0. */
int synth_sys_writable(int entry)
{
    (void)entry;
    return 0;
}

/** No row remounts: -EINVAL. */
int vfs_remount(struct vfs_mount *m, uint32_t flags, const char *options)
{
    (void)m;
    (void)flags;
    (void)options;
    return -EINVAL;
}

/** The inode number of a /proc/sys file, which no row reads: 0. */
uint32_t procfs_ino_sys(int entry)
{
    (void)entry;
    return 0;
}

/** A write to a file a shared mapping has, which no row makes. */
void filemap_file_written(fd_entry_t *e, uint64_t off, uint32_t len)
{
    (void)e;
    (void)off;
    (void)len;
}

/** No row's file has a shared mapping: 0. */
int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
    return 0;
}

/** The truncation of an open with O_TRUNC, which no row asks for: -EROFS. */
int vfs_truncate(struct vfs_mount *mnt, void *at, uint64_t size)
{
    (void)mnt;
    (void)at;
    (void)size;
    return -EROFS;
}

/** fallocate, which no row asks for: -EOPNOTSUPP. */
int vfs_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset, uint64_t len)
{
    (void)mnt;
    (void)handle;
    (void)mode;
    (void)offset;
    (void)len;
    return -EOPNOTSUPP;
}

/** Open a FIFO again through its descriptor's link, which no row does:
 * -ENXIO. */
int32_t fifo_attach(struct fd_entry *e, const void *mnt, uint64_t ino, int flags)
{
    (void)e;
    (void)mnt;
    (void)ino;
    (void)flags;
    return -ENXIO;
}

/** No row names a device: NULL. */
const char *devfs_name_of(int block, uint32_t rdev)
{
    (void)block;
    (void)rdev;
    return NULL;
}

/** The inode number of /proc/mounts, which no row reads: 0. */
uint32_t procfs_ino_mounts(void)
{
    return 0;
}

/** The inode number of /proc/usb, which no row reads: 0. */
uint32_t procfs_ino_usb(void)
{
    return 0;
}

/** The inode number of /proc/audio, which no row reads: 0. */
uint32_t procfs_ino_audio(void)
{
    return 0;
}

/** The inode number of /proc/loop, which no row reads: 0. */
uint32_t procfs_ino_loop(void)
{
    return 0;
}

/** The inode number of /proc/interrupts, which no row reads: 0. */
uint32_t procfs_ino_interrupts(void)
{
    return 0;
}

/** The inode number of /proc/console, which no row reads: 0. */
uint32_t procfs_ino_console(void)
{
    return 0;
}

/** The inode number of /proc/filesystems, which no row reads: 0. */
uint32_t procfs_ino_filesystems(void)
{
    return 0;
}

/** The inode number of /proc/net/route, which no row reads: 0. */
uint32_t procfs_ino_netroute(void)
{
    return 0;
}

/** The inode number of /proc/net/arp, which no row reads: 0. */
uint32_t procfs_ino_netarp(void)
{
    return 0;
}

/** The inode number of /proc/net/tcp, which no row reads: 0. */
uint32_t procfs_ino_nettcp(void)
{
    return 0;
}

/** The inode number of a /proc/net file, which no row reads: 0. */
uint32_t procfs_ino_net(int origin)
{
    (void)origin;
    return 0;
}

/** The inode number of a /sys file, which no row reads: 0. */
uint32_t sysfs_ino(int node, int key, int file)
{
    (void)node;
    (void)key;
    (void)file;
    return 0;
}

/** No /sys attribute is writable: 0. */
int sysfs_attr_writable(int node, int attr)
{
    (void)node;
    (void)attr;
    return 0;
}

/** No /sys device exists: 0. */
int sysfs_device_exists(int node, int key)
{
    (void)node;
    (void)key;
    return 0;
}

/** No /sys device exists: -1. */
int sysfs_device_key(int node, int n)
{
    (void)node;
    (void)n;
    return -1;
}

/** No row's descriptor is a /sys file: 0. */
int procfs_fd_is_sys(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No /sys path: an empty string in `out`. */
void synth_sys_path(int node, int key, char *out, int cap)
{
    (void)node;
    (void)key;
    if (cap > 0) {
        out[0] = '\0';
    }
}

/** A fresh table the two processes share, with the file system's counts and
 * the file at zero, and the dumping process the one that runs. */
static void row_begin(void)
{
    check(fd_table_init(&files) == 0, "initialize the descriptor table");
    files.refs = 2;
    self.fds = &files;
    sharer.fds = &files;
    core_made = opens = releases = ns_depth = 0;
    file_len = file_written = 0;
    k_memset(file, 0, sizeof(file));
    sharer_write = sharer_dup = 0;
    sharer_writes = 0;
    meanwhile = 0;
    current_proc = &self;
}

/** Free what the row left in the table, and require the file's description
 * closed as often as it was opened and no namespace lock held. */
static void row_end(void)
{
    int fd;

    for (fd = 0; fd < files.cap; fd++) {
        if (files.entries[fd].type != FD_TYPE_NONE || files.entries[fd].ofd) {
            fd_free(&files, fd);
        }
    }
    check(opens == releases && ns_depth == 0,
          "the file's description is closed and no namespace lock is held");
    fd_table_free(&files);
    k_memset(self.fd_held, 0, sizeof(self.fd_held));
    self.fd_holding = 0;
    k_memset(sharer.fd_held, 0, sizeof(sharer.fd_held));
    sharer.fd_holding = 0;
}

/** The process that shares the table writes one byte through the core file's
 * descriptor and duplicates it, each a call that ends before the dump goes
 * on. */
static void sharer_uses_core(void)
{
    isr_params_t params;
    int fd = core_fd();

    check(fd >= 0, "the dump's descriptor is in the shared table while it waits");
    current_proc = &sharer;
    k_memset(&params, 0, sizeof(params));
    params.ebx = (uint32_t)fd;
    params.edx = 1;
    sharer_write = sys_write(&params);
    fd_holds_release();
    sharer_dup = sys_dup(&params);
    fd_holds_release();
    current_proc = &self;
}

/**
 * A process that shares the descriptor table finds no descriptor where the
 * core file's is while the dump waits for the other threads: a write through
 * it and a dup of it answer EBADF, and nothing it writes reaches the file.
 * The dump then writes the whole file and frees its descriptor.
 */
static void sharer_finds_no_descriptor(void)
{
    row_begin();
    meanwhile = sharer_uses_core;
    check(fs_core_file(SIGABRT, 0xFFFFFFFFu) == 0, "the dump writes its file");
    check(!meanwhile, "the process that shares the table ran while the dump waited");
    check(sharer_write == -EBADF && sharer_writes == 0,
          "a write through the core file's descriptor by another process finds none");
    check(sharer_dup == -EBADF,
          "a dup of the core file's descriptor by another process finds none");
    check(file_len == MODEL_LENGTH && __builtin_memcmp(file, "\177ELF", 4) == 0,
          "the file has the dump's bytes and its whole length");
    check(core_fd() < 0, "the dump frees its descriptor");
    row_end();
}

/**
 * The zero that gives a file ending in a hole its length counts toward the
 * limit: with a limit of the bytes the dump writes before it, it is not
 * written, the file ends with the last page written and the call answers
 * EFBIG; with one byte more it is written, at the file's last byte.
 */
static void trailing_zero_limit(void)
{
    row_begin();
    check(fs_core_file(SIGABRT, MODEL_BYTES) == -EFBIG,
          "a limit of the bytes before the zero at the end answers EFBIG");
    check(file_written == MODEL_BYTES && file_len == 2u * PAGE_SIZE, "and the zero is not written");
    row_end();
    row_begin();
    check(fs_core_file(SIGABRT, MODEL_BYTES + 1u) == 0 && file_written == MODEL_BYTES + 1u &&
              file_len == MODEL_LENGTH && file[MODEL_LENGTH - 1u] == 0,
          "one byte more writes the zero at the file's last byte");
    row_end();
}

/** A mount is its own base here: no row makes a bind mount. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** No row sends a signal. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)signo;
    (void)info;
}

/**
 * Set up the dumping process and the process that shares its table, and run the
 * rows; 0 when every check passed, 1 otherwise.
 */
int main(void)
{
    self.pid = self.tgid = 3;
    self.tg = &group;
    self.fs = &dirs;
    self.state = PROC_RUNNING;
    self.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    group.leader = &self;
    group.live = 1;
    dirs.umask = 022;
    sharer.pid = sharer.tgid = 4;
    sharer.tg = &sharer_group;
    sharer.fs = &dirs;
    sharer.state = PROC_READY;
    sharer.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    sharer_group.leader = &sharer;
    sharer_group.live = 1;
    self.next = &sharer;
    mount.type = &model_type;
    sharer_finds_no_descriptor();
    trailing_zero_limit();
    if (!failures) {
        printf("corefile-hosttest: result: pass: the core file's descriptor and its trailing "
               "zero under the limit\n");
    }
    return failures != 0;
}
