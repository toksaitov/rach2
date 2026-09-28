/**
 * Check the core file `fs_core_file` writes: the descriptor it writes
 * through, which no other process sharing the descriptor table can use while
 * the dump runs, and the limit, which counts the zero that gives a file that
 * ends in a hole its length.
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
 * The dump first waits for the process's other threads, which sleeps. While
 * it does, a process that shares the descriptor table writes through the
 * core file's descriptor and duplicates it: both must find no descriptor,
 * since the end of the dump frees what the descriptor holds. With a limit of
 * the bytes the dump writes before the zero at its end, the zero must not be
 * written, the file must end with the last page written and the call must
 * answer EFBIG; with one byte more, the zero is written at the file's last
 * byte.
 */
#include "isr.h"
#include "lib/kuser.h"
#include "fs/fd.h"
#include "fs/vfs.h"
#include "fs/krffs.h"
#include "network/socket/socket.h"
static int fs_node_is_held(const krffs_node_t *node);
static void fs_node_moved(const krffs_node_t *from, krffs_node_t *to);
static void fd_release_backing(fd_entry_t *e);
static void fd_holds_release(void);
static int32_t sys_ftruncate(isr_params_t *params);
static int32_t sys_ftruncate64(isr_params_t *params);
static int32_t sys_write(isr_params_t *params);
static int32_t sys_writev(isr_params_t *params);
static int canonicalize_abs(char *path);
static int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath);
static int fd_node_check(fd_entry_t *entry);
static int cred_privileged(void);
static void cred_kill_setid(krffs_node_t *node);
static void vfs_kill_setid(struct vfs_mount *mnt, void *node);
static int mnt_is_root(const struct vfs_mount *mnt);
static int32_t cred_access_ref(const struct vfs_ref *r, int want, int use_real);
static int32_t cred_access_node(const krffs_node_t *node, int want, int use_real);
static int32_t sys_mount(isr_params_t *params);
static int32_t sys_umount2(isr_params_t *params);
static int32_t sys_open(isr_params_t *params);
static int32_t sys_openat(isr_params_t *params);
static int32_t sys_close(isr_params_t *params);
static int32_t sys_fsync(isr_params_t *params);
static int32_t sys_sync(isr_params_t *params);
static int32_t sys_lseek(isr_params_t *params);
static int sock_is_query(const struct socket *sock);
static int32_t sys_read(isr_params_t *params);
static int32_t sys_pread64(isr_params_t *params);
static int32_t sys_pwrite64(isr_params_t *params);
static int32_t readv_dgram(fd_entry_t *entry, uint32_t iov_user, int iovcnt, uint32_t *src_ip,
                           uint16_t *src_port);
static int32_t sys_readv(isr_params_t *params);
static int32_t sys_dup(isr_params_t *params);
static int32_t sys_dup2(isr_params_t *params);
static int32_t sys_chdir(isr_params_t *params);
static int32_t sys_fchdir(isr_params_t *params);
static int32_t sys_chroot(isr_params_t *params);
static int32_t sys_getcwd(isr_params_t *params);
static int32_t sys_readlink(isr_params_t *params);
static int32_t sys_readlinkat(isr_params_t *params);
static int32_t sys_stat64(isr_params_t *params);
static int32_t sys_lstat64(isr_params_t *params);
static int32_t sys_statfs64(isr_params_t *params);
static int32_t sys_fstatfs64(isr_params_t *params);
static int32_t sys_fstat64(isr_params_t *params);
static int32_t sys_fstatat64(isr_params_t *params);
static int32_t sys_access(isr_params_t *params);
static int32_t sys_faccessat(isr_params_t *params);
static int32_t sys_chmod(isr_params_t *params);
static int32_t sys_fchmodat(isr_params_t *params);
static int32_t sys_fchmod(isr_params_t *params);
static int32_t sys_chown(isr_params_t *params);
static int32_t sys_lchown(isr_params_t *params);
static int32_t sys_fchownat(isr_params_t *params);
static int32_t sys_fchown(isr_params_t *params);
static int32_t sys_truncate(isr_params_t *params);
static int32_t sys_truncate64(isr_params_t *params);
static int32_t sys_symlink(isr_params_t *params);
static int32_t sys_symlinkat(isr_params_t *params);
static int32_t sys_link(isr_params_t *params);
static int32_t sys_linkat(isr_params_t *params);
static int32_t sys__llseek(isr_params_t *params);
static int32_t sys_fcntl64(isr_params_t *params);
static int32_t sys_stat(isr_params_t *params);
static int32_t sys_lstat(isr_params_t *params);
static int32_t sys_creat(isr_params_t *params);
static int32_t sys_unlink(isr_params_t *params);
static int32_t sys_mkdir(isr_params_t *params);
static int32_t sys_mkdirat(isr_params_t *params);
static int32_t sys_rmdir(isr_params_t *params);
static int32_t sys_unlinkat(isr_params_t *params);
static int32_t sys_rename(isr_params_t *params);
static int32_t sys_renameat(isr_params_t *params);
static int32_t sys_renameat2(isr_params_t *params);
static void get_current_time(int64_t *seconds, int64_t *nanoseconds);
static int32_t sys_utimensat(isr_params_t *params);
static int32_t sys_getdents64(isr_params_t *params);
static int32_t sys_dup3(isr_params_t *params);
static int32_t sys_statx(isr_params_t *params);
static int32_t sys_memfd_create(isr_params_t *params);
static int32_t sys_flock(isr_params_t *params);
static int32_t sys_close_range(isr_params_t *params);
static int32_t sys_mknod(isr_params_t *params);
static int32_t sys_mknodat(isr_params_t *params);
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
process_t *current_proc = &self;
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
 * Write the model dump through `write`: the wait for the other threads,
 * during which the row's `meanwhile` runs, then the header, the pages and the
 * holes, the bytes counted toward `limit` as `core_write` counts them.
 */
int32_t core_write(core_write_fn write, void *ctx, const uint8_t *notes, uint32_t notes_size,
                   uint32_t limit)
{
    static const uint8_t header[MODEL_HEADER] = {0x7F, 'E', 'L', 'F'};
    static uint8_t page[PAGE_SIZE];
    const uint8_t *parts[4];
    uint32_t lens[4];
    uint32_t done = 0;
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

/** The heap is the host's. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

/** A path page from the heap, and its return. */
char *path_buf_get(void)
{
    return calloc(1, VFS_PATH_MAX);
}

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

/** The model type is the root mount, but its type is not KRFFS's: its nodes
 * take the paths of a mounted type. */
struct vfs_mount *vfs_root_mount(void)
{
    return &mount;
}

const struct vfs_type vfs_krffs_type = {"krffs", 0, &model_ops, 1, 0};

/** Resolve a path of the working directory: its directory with the last
 * name for a walk to the parent, and "core" once the open has made it. */
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

void vfs_walk_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
}

void vfs_ns_lock(struct vfs_mount *mnt)
{
    (void)mnt;
    ns_depth++;
}

void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    check(ns_depth > 0, "the namespace lock is taken before it is given back");
    ns_depth--;
}

/** No row keeps a program's file, and no row takes a second reference. */
int mm_exe_holds(const struct vfs_mount *mnt, const void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

void vfs_ref_hold(const struct vfs_ref *r)
{
    (void)r;
    check(0, "unexpected reference hold");
}

void vfs_ref_release(struct vfs_ref *ref)
{
    (void)ref;
}

const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return NULL;
}

struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node, const char *name)
{
    (void)mnt;
    (void)node;
    (void)name;
    return NULL;
}

/** The working directory, anyone's to write, and the file, root's. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = node == &cwd_node ? 040777u : 0100600u;
    st->ino = node == &cwd_node ? 1u : 2u;
    st->size = file_len;
    return 0;
}

int vfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    (void)what;
    (void)st;
    return 0;
}

/** No core file is there before the dump makes one. */
int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name)
{
    (void)mnt;
    (void)dir;
    (void)name;
    return -ENOENT;
}

int vfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    (void)mnt;
    check(dir == &cwd_node && __builtin_strcmp(name, "core") == 0 && mode == 0600u,
          "the dump makes core, of mode 0600, in the working directory");
    core_made = 1;
    *node = &core_node;
    return 0;
}

int vfs_open_node(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)mnt;
    (void)flags;
    opens++;
    *handle = node;
    return 0;
}

void vfs_desc_open(struct vfs_mount *mnt)
{
    (void)mnt;
}

void vfs_leave(struct vfs_mount *mnt)
{
    (void)mnt;
}

/** The last close of the file's description. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)mnt;
    check(node == &core_node && handle == &core_node, "the description closed is the file's");
    releases++;
}

/** Keep the dump's bytes at their place, and count a write of the process
 * that shares the table without keeping it. */
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

/** A user buffer of the model passes the range check, and no row copies one. */
int user_buffer_ok(uint32_t addr, uint32_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

int copy_from_user(void *dst, const void __user *src, uint32_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

int copy_to_user(void __user *dst, const void *src, uint32_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

/** No row's file backs a loop device, is locked, is held by a message in
 * flight or a mapping, or is a node of the KRFFS root. */
int loop_backs(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

int lock_any(void)
{
    return 0;
}

void lock_close(const struct lock_key *key)
{
    (void)key;
}

void lock_ofd_gone(const struct open_file *ofd)
{
    (void)ofd;
}

int unix_inflight_holds(const void *node)
{
    (void)node;
    return 0;
}

int filemap_holds(const void *node)
{
    (void)node;
    return 0;
}

krffs_file_system_t *krffs_fs;

uint64_t krffs_node_ino(const krffs_node_t *node)
{
    (void)node;
    return 0;
}

uint32_t krffs_mode_type(uint16_t mode)
{
    return mode & 0170000u;
}

void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    (void)fs;
    (void)node;
    check(0, "unexpected root node removal");
}

void krffs_unlink_held(krffs_node_t *node)
{
    (void)node;
    check(0, "unexpected root node unlink");
}

/** No row waits for a description or wakes a waiter. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "unexpected description wait");
    return 0;
}

/** The opens of the display, the mouse, /dev and /proc, and the writes of
 * sockets and the console, which no row reaches. */
volatile uint32_t fb_owner_pid;
bool vga_graphics_mode;
struct tty console_tty;

uint8_t *vga_mode13h_enter(void)
{
    return NULL;
}

void vga_mode13h_exit(void)
{}

void kbd_clear_scancodes(void)
{}

bool mouse_is_available(void)
{
    return 0;
}

int input_mouse_present(void)
{
    return 0;
}

void mouse_clear_packets(void)
{}

int devfs_open(const char *name, int flags)
{
    (void)name;
    (void)flags;
    return -ENOENT;
}

int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino)
{
    (void)name;
    (void)mode;
    (void)rdev;
    (void)ino;
    return -ENOENT;
}

int devfs_fd_is_raw(struct fd_entry *e)
{
    (void)e;
    return 0;
}

struct pty *pty_get(int index)
{
    (void)index;
    return NULL;
}

int pty_open_slave(int index, int flags)
{
    (void)index;
    (void)flags;
    return -ENXIO;
}

int64_t rtc_boot_time(void)
{
    return 0;
}

void socket_ref(struct socket *sock)
{
    (void)sock;
}

void socket_unref(struct socket *sock)
{
    (void)sock;
}

int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock)
{
    (void)sock;
    (void)buf;
    (void)len;
    (void)nonblock;
    return -EBADF;
}

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

int tty_is_background(struct tty *t, int sid, int pgid)
{
    (void)t;
    (void)sid;
    (void)pgid;
    return 0;
}

void tty_signal_group(int pgid, int signo)
{
    (void)pgid;
    (void)signo;
}

int tty_write(struct tty *t, const unsigned char *kbuf, int count)
{
    (void)t;
    (void)kbuf;
    return count;
}

int procfs_open(const struct vpath *vp, int flags)
{
    (void)vp;
    (void)flags;
    return -ENOENT;
}

int procfs_resolve_pid(int pid)
{
    (void)pid;
    return -1;
}

int procfs_resolve_id(const struct vpath *vp)
{
    (void)vp;
    return -1;
}

uint32_t procfs_task_links(int pid)
{
    (void)pid;
    return 0;
}

int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    (void)pid;
    (void)fd;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

int procfs_readlink_exe(int pid, char *buf, int cap)
{
    (void)pid;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

uint32_t procfs_ino_pid(int pid)
{
    (void)pid;
    return 0;
}

uint32_t procfs_ino_dir(void)
{
    return 0;
}

uint32_t procfs_ino_cmdline(void)
{
    return 0;
}

uint32_t procfs_ino_netdir(void)
{
    return 0;
}

uint32_t procfs_ino_netdev(void)
{
    return 0;
}

uint32_t procfs_ino_disk(void)
{
    return 0;
}

uint32_t procfs_ino_uptime(void)
{
    return 0;
}

uint32_t procfs_ino_statsys(void)
{
    return 0;
}

uint32_t procfs_ino_meminfo(void)
{
    return 0;
}

uint32_t procfs_ino_mounts(void)
{
    return 0;
}

uint32_t procfs_ino_usb(void)
{
    return 0;
}

uint32_t procfs_ino_audio(void)
{
    return 0;
}

uint32_t procfs_ino_loop(void)
{
    return 0;
}

uint32_t procfs_ino_interrupts(void)
{
    return 0;
}

uint32_t procfs_ino_console(void)
{
    return 0;
}

uint32_t procfs_ino_filesystems(void)
{
    return 0;
}

uint32_t procfs_ino_netroute(void)
{
    return 0;
}

uint32_t procfs_ino_netarp(void)
{
    return 0;
}

uint32_t procfs_ino_nettcp(void)
{
    return 0;
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
