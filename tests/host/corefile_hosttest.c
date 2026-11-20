/**
 * corefile_hosttest.c: the core file `fs_core_file` writes, its descriptor
 * and its limit, a write that brings its buffer in again under the file's
 * write lock, and the SIGPIPE of a writev to a TCP stream.
 *
 * host-deps: tests/host/compat/vfs_name_none.c tests/host/compat/devtree_none.c
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-deps: tests/host/compat/bkl_none.c tests/host/compat/rcu_none.c
 * host-deps: tests/host/compat/atomic_host.c rach2/sys/fs/fsize.c
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
 * more, the zero is written at the file's last byte. A loop device attached
 * to the file while the dump waits lets it write the whole file, unless the
 * file is on KRFFS, where the next write answers EBUSY.
 *
 * The refault rows write 32 bytes through `file_lock` and
 * `file_write_locked` as on a type that sets `file_locked`, whose file has
 * a write lock and a node lock (`vfs_write_lock`, `vfs_node_lock`), which
 * the model counts. The model type copies only while the buffer is
 * resident, as ext2 does under a lock it cannot let go: it stops at a page
 * the row made absent, and a row may also have a call stop as if its page
 * were absent at its look and back by the caller's (`type_caps`). Every
 * row requires the full count. At the first fault another process may
 * append, overwrite or truncate the file (`competitor`); it takes the
 * file's write lock, so it runs when the buffer is brought in only if that
 * lock is free, and otherwise when the writer gives it back. Neither
 * write's bytes may interleave, and a truncation must come wholly before or
 * after the write.
 *
 * The writev row writes a vector of two elements to a TCP stream whose
 * model send takes the first and fails the second with EPIPE: the call
 * returns the first's count and posts no SIGPIPE, which only a failure of
 * the first element posts. Its user pointers are the host's.
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
static int32_t resolve_named(const char *name, int walk, int dir, struct vfs_ref *ref);
static int fd_node_check(fd_entry_t *entry);
static int cred_privileged(void);
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
static int pc_sync_wait(void);
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
static long sys_mknod(isr_params_t *params);
static long sys_mknodat(isr_params_t *params);
static long sys_name_to_handle_at(isr_params_t *params);
static long sys_open_by_handle_at(isr_params_t *params);
static int32_t fd_stat_ids(int pid, int fd, uint32_t *mnt_id, uint64_t *ino);
static int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino);
static void fs_socket_unbind(struct vfs_ref *ref);
static int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino);
static int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off);
#include "syscall_fs.c"
#include "fs_caps_first.h"

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
/* A hold map as wide as the model's table grows: the kernel's record with
 * room for its words after it. */
struct held_room {
    struct fd_hold_map map;
    unsigned int more[FD_TABLE_LIMIT / 32 - 1];
};

/* The hold maps of the two. */
static struct held_room self_held, sharer_held;
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
/* The model of a type that sets `file_locked`, for the refault rows: nonzero
 * while a row's mount is one, and the takes of the file's write lock. */
static int model_file_locked, write_depth;
/* The refault row's buffer, and the offset of its first byte not resident,
 * NO_ABSENT for none; a fault brings the rest in. */
#define NO_ABSENT 0xFFFFFFFFu
static const char *row_buf;
static uint32_t absent_at;
/* The most bytes each of the type's next writes moves, as if the page after
 * them was absent at its look and brought in by another thread before the
 * caller's; `caps_left` of them remain, from `caps_next`. */
static uint32_t type_caps[4];
static int caps_next, caps_left;
/* What another process does to the file at the first fault, which takes
 * the file's write lock and so waits while the writer holds it; how often it
 * ran; and how often the buffer was brought in. */
static void (*competitor)(void);
static int competitor_runs, prefaults;

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
        if (fd_slot(&files, fd)->ofd && fd_slot(&files, fd)->ofd->vnode == &core_node) {
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

/** The kernel's allocation, zeroed here too. */
void *kmalloc(size_t size)
{
    return kzalloc(size);
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

/** The model has no separate proc mount. */
void path_buf_set_mount(char *buf, struct vfs_mount *mnt)
{
    (void)buf;
    (void)mnt;
}

/** Record the held process directory as the walk's starting point. */
int path_buf_set_dir(char *buf, struct vfs_mount *mnt, void *node, const char *base)
{
    struct vfs_ref ref = {mnt, node};
    (void)buf;
    (void)base;
    return mnt && vfs_proc_dir_owner(&ref) != NULL;
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

/** Copy the working directory of the model's ordinary mount. */
int proc_cwd_read(const struct process *p, char *out)
{
    k_strncpy(out, proc_cwd(p), VFS_PATH_MAX);
    return 0;
}

/** The model mount is the root mount. Its type is not KRFFS's, so its nodes
 * take the paths of a mounted type. */
struct vfs_mount *vfs_root_mount(void)
{
    return &mount;
}

/* KRFFS's type, which no mount of the model has. */
const struct vfs_type vfs_krffs_type = {"krffs", 0, &model_ops, 1, 0};
/* The other types whose locks are found by node (`mnt_locks_by_node`),
 * which no mount of the model has either. */
const struct vfs_type vfs_krffs_volume_type = {"krffs", 0, 0, 1, 0};
const struct vfs_type vfs_vfat_type = {"vfat", 0, 0, 0, 0};
const struct vfs_type vfs_exfat_type = {"exfat", 0, 0, 0, 0};

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

/** The walk with its path recorded, which the model does not record. */
int vfs_resolve_real(const char *path, int flags, struct vfs_ref *out, char *real)
{
    if (real) {
        real[0] = '\0';
    }
    return vfs_resolve(0, path, flags, out, 0, 0);
}

/** This resolver model has no captured entry; its original walk is unchanged.
 */
int vfs_resolve_entry(const char *path, int flags, struct vfs_ref *out, char *real,
                      struct vfs_name **entry, struct vfs_stat *dir)
{
    if (dir) {
        k_memset(dir, 0, sizeof(*dir));
    }
    if (entry) {
        *entry = NULL;
    }
    return vfs_resolve_real(path, flags, out, real);
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

/** The model has no counted node holds separate from its walk. */
void vfs_ref_put(struct vfs_ref *ref)
{
    (void)ref;
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

/** No node of the model is a process directory. */
const struct synth_owner *vfs_proc_dir_owner(const struct vfs_ref *ref)
{
    (void)ref;
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

/** The model has no extended attributes; keep the VFS default answers. */
int vfs_xattr(struct vfs_mount *mnt, void *node, struct vfs_xattr *arg)
{
    (void)mnt;
    (void)node;
    return arg->op == VFS_XATTR_LIST ? 0 : -EOPNOTSUPP;
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

/** Start a description's cursors into the write-back errors at none,
 * which no row reports. */
void vfs_wb_err_sample(struct vfs_mount *mnt, uint32_t *file, uint32_t *fs)
{
    (void)mnt;
    *file = 0;
    *fs = 0;
}

/** Start a description's cursor into its file's sequence at none. */
uint32_t vfs_ino_err_sample(struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
    return 0;
}

/** The report of the file's own sequence, which holds no error in a row. */
int vfs_ino_err_check(struct vfs_mount *mnt, uint64_t ino, uint32_t *seen)
{
    (void)mnt;
    (void)ino;
    (void)seen;
    return 0;
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
    if (row_buf && absent_at != NO_ABSENT && (const char *)buf <= row_buf + absent_at &&
        row_buf + absent_at < (const char *)buf + count) {
        count = (uint32_t)(row_buf + absent_at - (const char *)buf);
    }
    if (caps_left > 0) {
        if (count > type_caps[caps_next]) {
            count = type_caps[caps_next];
        }
        caps_next++;
        caps_left--;
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

/** A user range of the model lies in user memory. */
int user_access_ok(uintptr_t addr, size_t len)
{
    (void)addr;
    (void)len;
    return 1;
}

/** Nothing: no row writes to the console, whose writes yield here. */
void sched_service_point(void)
{}

/** A user range of the model ends in user space. */
int user_span_ok(uintptr_t addr, size_t len)
{
    (void)addr;
    (void)len;
    return 1;
}

/** No PCI function of /sys exists, so none has a BAR. */
uint64_t sysfs_pci_bar_size(int key, int bar)
{
    (void)key;
    (void)bar;
    return 0;
}

/** Every page of a model buffer is present, but for a refault row's from
 * its absent byte until it is brought in. */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    (void)write;
    if (!row_buf || absent_at == NO_ABSENT) {
        return 1;
    }
    return addr + len <= (uintptr_t)(row_buf + absent_at);
}

/** Run the row's other process once, as it runs when it has the file's
 * write lock and node lock. */
static void competitor_run(void)
{
    void (*run)(void) = competitor;

    check(write_depth == 0 && ns_depth == 0,
          "another process changes the file only with its locks");
    competitor = 0;
    competitor_runs++;
    run();
}

/** Bring the rest of a refault row's buffer in, with the file's node lock
 * let go; the row's other process gets in now when the write lock is free. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    if (row_buf) {
        check(ns_depth == 0, "a buffer is brought in with the file's node lock let go");
        absent_at = NO_ABSENT;
        prefaults++;
        if (competitor && write_depth == 0) {
            competitor_run();
        }
    }
    return 1;
}

/** A refault row's mount locks each file; no other row's does. */
int vfs_file_locked(const struct vfs_mount *mnt)
{
    (void)mnt;
    return model_file_locked;
}

/** The node lock, which the model counts as the namespace lock. */
void vfs_node_lock(const struct vfs_ref *ref)
{
    vfs_ns_lock(ref->mnt);
}

/** Give back a take of `vfs_node_lock`. */
void vfs_node_unlock(const struct vfs_ref *ref)
{
    vfs_ns_unlock(ref->mnt);
}

/** Take the file's write lock, which the model counts. */
void vfs_write_lock(const struct vfs_ref *ref)
{
    (void)ref;
    check(ns_depth == 0, "the write lock comes before the node lock");
    write_depth++;
}

/** Give back a take of the write lock; once it is free, the row's other
 * process, which waited for it, runs. */
void vfs_write_unlock(const struct vfs_ref *ref)
{
    (void)ref;
    check(write_depth > 0, "the write lock is taken before it is given back");
    write_depth--;
    if (write_depth == 0 && ns_depth == 0 && competitor) {
        competitor_run();
    }
}

/** No row locks a child, renames or covers: nothing is taken. */
int vfs_child_lock(const struct vfs_ref *dir, const char *name, struct vfs_ref *child)
{
    (void)name;
    child->mnt = dir->mnt;
    child->node = 0;
    return 0;
}

/** Nothing: `vfs_child_lock` took nothing. */
void vfs_child_unlock(struct vfs_ref *child)
{
    child->node = 0;
}

/** The namespace lock, as for a type without `file_locked`. */
int vfs_rename_lock(const struct vfs_ref *opar, const char *oleaf, const struct vfs_ref *npar,
                    const char *nleaf, struct vfs_rename_hold *h)
{
    (void)oleaf;
    (void)npar;
    (void)nleaf;
    h->renaming = 0;
    h->dir[0] = *opar;
    h->dir[1].mnt = 0;
    h->dir[1].node = 0;
    h->child[0] = h->dir[1];
    h->child[1] = h->dir[1];
    vfs_ns_lock(opar->mnt);
    return 0;
}

/** Give back what `vfs_rename_lock` took. */
void vfs_rename_unlock(struct vfs_rename_hold *h)
{
    vfs_ns_unlock(h->dir[0].mnt);
}

/** The namespace lock of the cover's mount. */
int vfs_cover_lock(const struct vfs_ref *tref)
{
    vfs_ns_lock(tref->mnt);
    return 0;
}

static int host_copies; /* nonzero while a row's user pointers are the host's */

/** Copy from a host pointer while `host_copies` is set, as the writev row's
 * vector is; no other row copies from a user buffer: 0 or -1. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (!host_copies) {
        return -1;
    }
    k_memcpy(dst, (const void __force *)src, n);
    return 0;
}

/** No row copies to a user buffer: -1. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

/* Whether a loop device stands on every file of the model, as the
 * attachment rows set it while a dump runs, and whether the model type is
 * one whose files a change may move, KRFFS's. */
static int files_attached, files_pinned;

/** Whether the file backs a loop device: `files_attached`. */
int loop_backs(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return files_attached;
}

/** Whether the file backs a loop device on a type whose files a change may
 * move: `files_attached` and `files_pinned`. */
int loop_length_pinned(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return files_attached && files_pinned;
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
/* The protections of /proc/sys/fs, at the values rach2/sys/fs/vfs.c gives
 * them, which the open of the core file meets. */
int vfs_protected_hardlinks = 1;
int vfs_protected_symlinks = 1;
int vfs_protected_fifos = 1;
int vfs_protected_regular = 2;

/** A pid as the caller's namespace sees it, which no row reads: the open of
 * a /proc directory names one, and the core file is no such open. */
uint32_t pid_seen(uint32_t kid)
{
    return kid;
}

/** No node of the KRFFS root is in a row: 0. */
uint64_t krffs_node_ino(const krffs_node_t *node)
{
    (void)node;
    return 0;
}

/** Fail the row: no row reads a header of the KRFFS root. */
int krffs_node_head(const krffs_node_t *node, krffs_node_t *out)
{
    (void)node;
    (void)out;
    check(0, "unexpected root header read");
    return -1;
}

/** Fail the row: no row asks the type of a node of the KRFFS root. */
int krffs_node_is_dir(const krffs_node_t *node)
{
    (void)node;
    check(0, "unexpected root type read");
    return -1;
}

/** No node of the KRFFS root is in a row: 0. */
uint64_t krffs_ino_of(const krffs_node_t *node)
{
    (void)node;
    return 0;
}

/** Fail the row: no row stores a mode on the KRFFS root. */
int krffs_node_set_mode(krffs_node_t *node, uint16_t mode)
{
    (void)node;
    (void)mode;
    check(0, "unexpected root mode store");
    return -1;
}

/** Fail the row: no row stores an owner on the KRFFS root. */
int krffs_node_set_owner(krffs_node_t *node, uint32_t uid, uint32_t gid, int which)
{
    (void)node;
    (void)uid;
    (void)gid;
    (void)which;
    check(0, "unexpected root owner store");
    return -1;
}

/** Fail the row: no row stores a time on the KRFFS root. */
int krffs_node_set_time(krffs_node_t *node, uint32_t field, int64_t seconds, int64_t nanoseconds)
{
    (void)node;
    (void)field;
    (void)seconds;
    (void)nanoseconds;
    check(0, "unexpected root time store");
    return -1;
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
int krffs_unlink_held(krffs_node_t *node)
{
    (void)node;
    check(0, "unexpected root node unlink");
    return -1;
}

/** No row halts the KRFFS root. */
int krffs_root_halted(void)
{
    return 0;
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

/** No row puts a file in an epoll set, so a last close has no
 * registration to take. */
void epoll_release_file(struct open_file *ofd)
{
    (void)ofd;
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
struct tty console_tty;

/** No description of the model's console is hung up. */
int fd_tty_hung(fd_entry_t *e)
{
    (void)e;
    return 0;
}

/** No row opens the framebuffer: -EBUSY. */
int32_t fb_take(struct fd_entry *e)
{
    (void)e;
    return -EBUSY;
}

/* The framebuffer's operations, which no row reaches. */
const struct file_ops fb_file_ops;

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

/** No row's descriptor is /dev/mem: 0. */
int devfs_fd_is_mem(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No row's descriptor is a device that seeks to 0: 0. */
int devfs_lseek_zero(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No row opens a /proc/PID/mem: 0. */
int procfs_is_mem(const struct fd_entry *e)
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
int64_t rtc_boot_epoch(void)
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

/* The writev row's: what `socket_tcp_send` answers to each call, the bytes
 * sent or -errno, the calls made, and the SIGPIPEs `sock_stream_sent` would
 * post. */
static int32_t stream_answer[2];
static int stream_sends, sigpipes;

/** Count the SIGPIPE a TCP send's EPIPE posts when `post` is set: `rc`. */
int32_t sock_stream_sent(int32_t rc, int post)
{
    sigpipes += rc == -EPIPE && post;
    return rc;
}

/** Answer the writev row's next call from `stream_answer`, no more than
 * `len` bytes, and -EPIPE past its two. */
int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock)
{
    int32_t answer;

    (void)sock;
    (void)buf;
    (void)nonblock;
    if (stream_sends >= 2) {
        return -EPIPE;
    }
    answer = stream_answer[stream_sends++];
    return answer > len ? len : answer;
}

/** No row writes to a datagram socket: -EBADF. */
int32_t dgram_send(struct socket *sock, net_addr_t dst_ip, uint16_t dst_port, const uint8_t *buf,
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
int tty_is_background(struct tty *t, int sid, int pgid, uint64_t serial)
{
    (void)t;
    (void)sid;
    (void)pgid;
    (void)serial;
    return 0;
}

/** No process of a row is in the background, so none is stopped: 0. */
int tty_job_check(struct tty *t, int signo)
{
    (void)t;
    (void)signo;
    return 0;
}

/** No row signals a process group. */
void tty_signal_group(int pgid, uint64_t serial, int signo)
{
    (void)pgid;
    (void)serial;
    (void)signo;
}

/** Take every byte written to the console: `count`. */
int32_t tty_write_user(struct tty *t, const char __user *buf, uint32_t count, int nonblock,
                       const uint32_t *opened)
{
    (void)t;
    (void)buf;
    (void)nonblock;
    (void)opened;
    return (int32_t)count;
}

/** No row opens a /proc file: -ENOENT. */
int procfs_open(const struct vpath *vp, int flags)
{
    (void)vp;
    (void)flags;
    return -ENOENT;
}

/** No alternate proc mount exists in the model. */
int procfs_dir_name(const struct vfs_mount *mnt, char *path, int cap)
{
    (void)mnt;
    (void)path;
    (void)cap;
    return -ENOENT;
}

/** No row names a /proc process: -1. */
int procfs_dir_relative(const struct synth_dir *sd, const struct vfs_mount *mnt, char *buf, int cap)
{
    (void)sd;
    (void)mnt;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

/** Use the kernel number in the model's PID namespace. */
uint32_t procfs_pid_number(uint32_t kid, uint32_t pidns_ino)
{
    (void)pidns_ino;
    return kid;
}

/** Provide the process lookup of the model. */
int procfs_resolve_pid(int pid, uint32_t pidns_ino)
{
    (void)pidns_ino;
    (void)pid;
    return -1;
}

/** No row reads /proc: 0. */
int procfs_may_read(int pid)
{
    (void)pid;
    return 0;
}

/** No row reads /proc: 0. */
int procfs_may_read_as(int pid, int use_real)
{
    (void)pid;
    (void)use_real;
    return 0;
}

/** No model descriptor has a proc link inode. */
uint32_t procfs_fd_link_ino(const struct vpath *vp, int id)
{
    (void)vp;
    (void)id;
    return 0;
}

/** No row's descriptor is of /proc: -ENOENT. */
int procfs_fd_link_mode(int pid, int fd, uint32_t *mode)
{
    (void)pid;
    (void)fd;
    (void)mode;
    return -ENOENT;
}

/** No row's descriptor is of /proc, so a chown has no kept owner to set. */
void procfs_owner_chown(const struct vpath *vp, const struct synth_owner *held, uint32_t uid,
                        uint32_t gid)
{
    (void)vp;
    (void)held;
    (void)uid;
    (void)gid;
}

/** No kind is a system-wide file of /proc here, so the opens and stats
 * that ask fall through to ENOENT. */
uint32_t procfs_ino_global(int kind)
{
    (void)kind;
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

/** No row's path is under /dev, /proc or /sys: every one is VNODE_KRFFS. */
int synth_classify(const char *path, struct vpath *vp)
{
    (void)path;
    vp->kind = VNODE_KRFFS;
    return vp->kind;
}

/** No row reaches a directory of /dev's table: none is present. */
int devfs_subdir_present(int row)
{
    (void)row;
    return 0;
}

/** No row reaches /dev's table of directories: it has no rows. */
const struct synth_dev_dir *synth_dev_dir_at(int n)
{
    (void)n;
    return NULL;
}

/** No row names a directory of /dev's table: -1. */
int synth_dev_dir_find(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

/** No row has a descriptor on a directory of /dev, /proc or /sys: "/". */
void synth_dir_path(int kind, int pid, int tid, char *out, int cap)
{
    (void)kind;
    (void)pid;
    (void)tid;
    if (cap > 1) {
        out[0] = '/';
        out[1] = '\0';
    }
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

/** The owner of a node of a process under /proc, which no row reads:
 * -ENOENT. */
int procfs_pid_owner(int pid, uint32_t mode, uint32_t *uid, uint32_t *gid)
{
    (void)pid;
    (void)mode;
    (void)uid;
    (void)gid;
    return -ENOENT;
}

/* The descriptor, process and mount lookups of fstat and of a descriptor's
 * path, which no row reaches: GCC keeps their callers, so a link needs
 * them. */

/** The core writer's descriptors are ordinary files, never dma-bufs. */
int is_dma_buf_file(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

/** The model queues hold no notification. */
void mq_flush(const fd_entry_t *entry)
{
    (void)entry;
}

/** No queue descriptor is present in the model. */
int mq_fd_ref(const struct fd_entry *e, struct vfs_ref *r)
{
    (void)e;
    (void)r;
    return 0;
}

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

/** No descriptor is open in a model proc fd directory. */
int procfs_fd_count(int pid)
{
    (void)pid;
    return 0;
}

/** The host model has a single PID namespace. */
int procfs_dir_coords(const struct synth_dir *sd, int *pid, int *tid)
{
    *pid = sd->pid;
    *tid = sd->tid;
    return 0;
}

/** No row holds a descriptor in the caller's own proc fd directory. */
int procfs_same_group(int pid)
{
    (void)pid;
    return 0;
}

/** Return the model inode's retained kernel PID. */
int procfs_held_pid(const struct synth_owner *own)
{
    return own->kept ? own->node[2] : -1;
}

/** No model descriptor is open on fdinfo. */
int procfs_fdinfo_access(const struct fd_entry *e, int use_real)
{
    (void)e;
    (void)use_real;
    return 0;
}

/** The model has no proc descriptor directories. */
int procfs_fd_dir_access(int kind, int pid, int want, int use_real)
{
    (void)kind;
    (void)pid;
    (void)want;
    (void)use_real;
    return -ENOENT;
}

/** No row's descriptor is a /proc file, so a lookup has no owner to set. */
int procfs_lookup_owner(const struct vpath *vp)
{
    (void)vp;
    return 0;
}

/** No row's descriptor is of /proc, so an open keeps no owner. */
void procfs_owner_keep(struct synth_owner *o, const struct vpath *vp)
{
    (void)o;
    (void)vp;
}

/** Copy the inode of a second description in the model. */
void procfs_owner_copy(struct synth_owner *to, const struct synth_owner *from)
{
    *to = *from;
}

/** No row's descriptor is of /proc, so a close has no kept owner to drop. */
void procfs_owner_drop(struct synth_owner *o)
{
    (void)o;
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

/** The slot of `m` in the mount table, which has none here: -1. */
int vfs_mount_index(const struct vfs_mount *m)
{
    (void)m;
    return -1;
}

/** The device a stat reports for mount `i`, which no row reads: 0. */
uint32_t procfs_mount_dev(int i, const struct vfs_mount *m)
{
    (void)i;
    (void)m;
    return 0;
}

/** The inode number of /proc, which no row reads: 0. */
uint32_t procfs_ino_dir(void)
{
    return 0;
}

/** No message interrupt is present in the core-file model: 0. */
int procfs_irq_exists(int vector)
{
    (void)vector;
    return 0;
}

/** The inode number of an interrupt affinity file, which no row reads: 0. */
uint32_t procfs_ino_irq(int vector, int attr)
{
    (void)vector;
    (void)attr;
    return 0;
}

/** The mode of a /proc/asound file, which no row stats. */
int asound_mode(int file)
{
    (void)file;
    return 0444;
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

/** No row's file system is watched. */
int inotify_mount_watched(const struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

/** No row's descriptor is an inotify instance: 0. */
int inotify_is(int fd)
{
    (void)fd;
    return 0;
}

/** No row's file is watched. */
void inotify_file(struct vfs_mount *mnt, uint64_t ino, uint32_t mask)
{
    (void)mnt;
    (void)ino;
    (void)mask;
}

/** No row's directory is watched. */
void inotify_child(struct vfs_mount *mnt, uint64_t dir_ino, const char *name, uint32_t mask,
                   uint32_t cookie)
{
    (void)mnt;
    (void)dir_ino;
    (void)name;
    (void)mask;
    (void)cookie;
}

/** No row changes a file of the boot root, whose times are not kept. */
void krffs_stamp(krffs_node_t *node, int what)
{
    (void)node;
    (void)what;
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

/** The inode numbers of /proc/acpi and its call, which no row reads: 0. */
uint32_t procfs_ino_acpidir(void)
{
    return 0;
}

uint32_t procfs_ino_acpicall(void)
{
    return 0;
}

/** The inode numbers of /proc/fs and its lockwait, which no row reads: 0. */
uint32_t procfs_ino_fsdir(void)
{
    return 0;
}

uint32_t procfs_ino_lockwait(void)
{
    return 0;
}

/** The inode numbers of /proc/asound's and /proc/bus's files, which no
 * row reads: 0. */
uint32_t procfs_ino_asound(int file, int card)
{
    (void)file;
    (void)card;
    return 0;
}

uint32_t procfs_ino_bus(int file, int dir)
{
    (void)file;
    (void)dir;
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

/** Each /proc/sys file is read-only: 0444. */
unsigned synth_sys_mode(int entry)
{
    (void)entry;
    return 0444;
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
void filemap_disk_written(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, const void *buf,
                          uint32_t len)
{
    (void)mnt;
    (void)ino;
    (void)off;
    (void)buf;
    (void)len;
}

/** No row's file has a shared mapping: 0. */
int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
    return 0;
}

/** The sync of an O_SYNC or O_DSYNC write, which no row opens: 0. */
int vfs_fsync(struct vfs_mount *mnt, void *handle, uint32_t *seen)
{
    (void)mnt;
    (void)handle;
    (void)seen;
    return 0;
}

/** As `vfs_fsync`. */
int vfs_fdatasync(struct vfs_mount *mnt, void *handle, uint32_t *seen)
{
    (void)mnt;
    (void)handle;
    (void)seen;
    return 0;
}

/** The cache drop of an O_DIRECT write, which no row opens. */
void vfs_uncache(struct vfs_mount *mnt, void *handle, uint64_t offset, uint64_t len)
{
    (void)mnt;
    (void)handle;
    (void)offset;
    (void)len;
}

/** The disk-backed window's sync, which no row reaches: 0. */
int pc_sync(void)
{
    return 0;
}

/** The release count `pc_sync_wait` reads before each pass: 0. */
uint32_t pc_releases(void)
{
    return 0;
}

/** The sleep of `pc_sync_wait` while a slot is busy, which none is here. */
void pc_wait_release(uint32_t seen)
{
    (void)seen;
}

/** The truncation of an open with O_TRUNC, which no row asks for: -EROFS. */
int vfs_truncate(struct vfs_mount *mnt, void *at, uint64_t size, int stamp_times)
{
    (void)stamp_times;
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

/** Every mount's id is 1, as the first slot's. */
uint32_t procfs_mount_id(const struct vfs_mount *m)
{
    (void)m;
    return 1;
}

/** No synthetic file system is mounted: 0. */
uint32_t procfs_synth_mount_id(uint64_t dev, const struct vfs_mount *hint)
{
    (void)dev;
    (void)hint;
    return 0;
}

/** Every attribute of a fixed /sys directory is there. */
int sysfs_attr_present(int node, int attr)
{
    (void)node;
    (void)attr;
    return 1;
}

/** No PCI function's attribute is there: no row has a PCI device. */
int sysfs_pci_attr_present(int key, int attr)
{
    (void)key;
    (void)attr;
    return 0;
}

/** No attribute of /sys is private. */
int sysfs_attr_private(int node, int attr)
{
    (void)node;
    (void)attr;
    return 0;
}

/** The mode stat(2) gives a fixed attribute, as synthfs.c's. */
uint32_t sysfs_attr_mode(int node, int attr)
{
    return sysfs_attr_writable(node, attr) ? 0644u : sysfs_attr_private(node, attr) ? 0400u : 0444u;
}

/** No row's mount is debugfs. */
const struct vfs_type vfs_procfs_type = {.name = "proc"};
const struct vfs_type vfs_mqueue_type = {.name = "mqueue"};
const struct file_ops mq_ops = {0};
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

/** Fail the row: no row stats a driver's entry of /sys. */
uint32_t sysfs_node_mode(int handle)
{
    (void)handle;
    check(0, "unexpected stat of a /sys file");
    return 0;
}

/** Every row's caller is native: no call comes through the i386 entry. */
int in_compat_syscall(void)
{
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
    model_file_locked = write_depth = 0;
    row_buf = NULL;
    absent_at = NO_ABSENT;
    caps_next = caps_left = 0;
    competitor = 0;
    competitor_runs = prefaults = 0;
    current_proc = &self;
}

/** Free what the row left in the table, and require the file's description
 * closed as often as it was opened and no namespace lock held. */
static void row_end(void)
{
    int fd;

    for (fd = 0; fd < files.cap; fd++) {
        if (fd_slot(&files, fd)->type != FD_TYPE_NONE || fd_slot(&files, fd)->ofd) {
            fd_free(&files, fd);
        }
    }
    check(opens == releases && ns_depth == 0,
          "the file's description is closed and no namespace lock is held");
    fd_table_free(&files);
    k_memset(&self_held, 0, sizeof(self_held));
    self_held.map.words = FD_TABLE_LIMIT / 32;
    self.fd_holding = 0;
    k_memset(&sharer_held, 0, sizeof(sharer_held));
    sharer_held.map.words = FD_TABLE_LIMIT / 32;
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

/** A loop device is attached to the core file while the dump waits. */
static void core_gets_attached(void)
{
    files_attached = 1;
}

/**
 * A loop device attached to the core file while the dump runs: on a type
 * whose files keep their place the dump writes the whole file, as Linux's
 * does; on KRFFS, where a write may move the file, it stops at its next
 * write with EBUSY.
 */
static void attached_core_rows(void)
{
    row_begin();
    meanwhile = core_gets_attached;
    check(fs_core_file(SIGABRT, 0xFFFFFFFFu) == 0 && file_len == MODEL_LENGTH &&
              __builtin_memcmp(file, "\177ELF", 4) == 0,
          "a dump into a file a loop device stands on writes the whole file");
    files_attached = 0;
    row_end();
    row_begin();
    files_pinned = 1;
    meanwhile = core_gets_attached;
    check(fs_core_file(SIGABRT, 0xFFFFFFFFu) == -EBUSY && file_written == 0,
          "on KRFFS the dump stops at its next write with EBUSY");
    files_attached = files_pinned = 0;
    row_end();
}

/** Another process's append of 16 bytes of 'B' at the file's end. */
static void competing_append(void)
{
    k_memset(file + file_len, 'B', 16);
    file_len += 16;
}

/** Another process's write of 32 bytes of 'B' at the file's start. */
static void competing_overwrite(void)
{
    k_memset(file, 'B', 32);
    if (file_len < 32) {
        file_len = 32;
    }
}

/** Another process's truncation of the file to nothing. */
static void competing_truncate(void)
{
    k_memset(file, 0, sizeof(file));
    file_len = 0;
}

/** Whether the `len` bytes of the file from `from` are all `c`: 1 or 0. */
static int file_is(uint32_t from, uint32_t len, int c)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (file[from + i] != (uint8_t)c) {
            return 0;
        }
    }
    return 1;
}

/**
 * Write 32 bytes of 'A' as write(2) does on a type that sets `file_locked`,
 * through `file_lock` and `file_write_locked`: at the file's end with
 * `append`, and else at its start, as pwrite64 writes. The buffer is not
 * resident from its byte `absent` on, NO_ABSENT for none. Returns the
 * write's result.
 */
static int32_t refault_write(uint32_t absent, int append)
{
    static char buf[32];
    struct open_file ofd;
    fd_entry_t entry;
    struct file_write_state state;
    unsigned long was = self.rlim[RLIM_FSIZE].cur;
    uint64_t at = 0;
    int32_t n;

    k_memset(buf, 'A', sizeof(buf));
    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&entry, 0, sizeof(entry));
    ofd.vmnt = &mount;
    ofd.vnode = &core_node;
    ofd.fhandle = &core_node;
    ofd.status_flags = append ? OFD_APPEND : 0;
    entry.ofd = &ofd;
    model_file_locked = 1;
    row_buf = buf;
    absent_at = absent;
    self.rlim[RLIM_FSIZE].cur = RLIM_INFINITY;
    state.own = file_lock(&ofd, &state.file);
    check(state.own && write_depth == 1 && ns_depth == 1,
          "a write takes the file's write lock and then its node lock");
    state.append = append;
    state.big = 0;
    n = file_write_locked(&entry, (const char __user *)buf, sizeof(buf), append ? NULL : &at,
                          &state);
    check(write_depth == 1 && ns_depth == 1, "and holds both when the write ends");
    file_unlock(&state.file, state.own);
    check(write_depth == 0 && ns_depth == 0, "and gives both back");
    self.rlim[RLIM_FSIZE].cur = was;
    row_buf = NULL;
    model_file_locked = 0;
    return n;
}

/**
 * A write that meets a page of its buffer absent brings it in and goes on
 * to the full count, after bytes moved and before, and as often as the
 * type's stop races another thread that brings the page in. The file's
 * write lock stays held across the fault, so another process's append,
 * overwrite or truncation, which takes it, comes wholly after the write.
 */
static void refault_rows(void)
{
    row_begin();
    check(refault_write(8, 0) == 32 && prefaults == 1,
          "a write that faults after 8 bytes brings the rest in and writes all 32");
    check(file_len == 32 && file_is(0, 32, 'A'), "and the file has every byte");
    row_end();
    row_begin();
    type_caps[0] = 0;
    caps_left = 1;
    check(refault_write(NO_ABSENT, 0) == 32 && prefaults == 0,
          "a write whose type stopped at a page another thread brought in goes on at once");
    check(file_len == 32 && file_is(0, 32, 'A'), "and writes every byte");
    row_end();
    row_begin();
    type_caps[0] = type_caps[1] = 0;
    caps_left = 2;
    check(refault_write(NO_ABSENT, 0) == 32 && file_len == 32 && file_is(0, 32, 'A'),
          "the same race twice before any byte moved still writes all 32");
    row_end();
    row_begin();
    type_caps[0] = 8;
    type_caps[1] = type_caps[2] = 0;
    caps_left = 3;
    check(refault_write(NO_ABSENT, 0) == 32 && file_len == 32 && file_is(0, 32, 'A'),
          "and so does it twice after 8 bytes moved");
    row_end();
    row_begin();
    competitor = competing_append;
    check(refault_write(8, 1) == 32 && competitor_runs == 1,
          "an append that faults after 8 bytes writes all 32 while another append waits");
    check(file_len == 48 && file_is(0, 32, 'A') && file_is(32, 16, 'B'),
          "and the other append lands wholly after it");
    row_end();
    row_begin();
    competitor = competing_overwrite;
    check(refault_write(8, 0) == 32 && competitor_runs == 1,
          "a write that faults after 8 bytes writes all 32 while another write waits");
    check(file_len == 32 && file_is(0, 32, 'B'), "and the other write lands wholly after it");
    row_end();
    row_begin();
    competitor = competing_truncate;
    check(refault_write(8, 0) == 32 && competitor_runs == 1,
          "a write that faults after 8 bytes writes all 32 while a truncation waits");
    check(file_len == 0, "and the truncation comes wholly after it");
    row_end();
}

/** A mount is its own base here: no row makes a bind mount. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** sysfs's type, which no row mounts: what `attr_read_only` compares. */
const struct vfs_type vfs_sysfs_type = {"sysfs"};

/** vfs.c's: a bind's own flags, or a base's with its own read-only flag. */
uint32_t vfs_mount_flags(const struct vfs_mount *m)
{
    if (m->bind_of) {
        return m->flags;
    }
    return (m->flags & ~VFS_MS_RDONLY) | (m->mnt_rdonly ? VFS_MS_RDONLY : 0);
}

/** vfs.c's: the base's VFS_MS_RDONLY, the file system's state. */
int vfs_sb_rdonly(const struct vfs_mount *m)
{
    return (vfs_mount_base(m)->flags & VFS_MS_RDONLY) != 0;
}

/** vfs.c's: the mount's own flag or its file system's. */
int vfs_mount_rdonly(const struct vfs_mount *m)
{
    return (vfs_mount_flags(m) & VFS_MS_RDONLY) || vfs_sb_rdonly(m);
}

/** The SIGXFSZ of a row past RLIMIT_FSIZE, which no row reads. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)signo;
    (void)info;
}

/**
 * A writev to a TCP stream posts SIGPIPE only for an error it returns, as
 * Linux's tcp_sendmsg returns what it copied: one whose first element goes
 * and whose second fails with EPIPE returns the first's count with no
 * signal, and one whose first fails posts it.
 */
static void stream_writev(void)
{
    static uint8_t bytes[8];
    struct kernel_iovec v[2];
    struct socket s;
    int fd = 5;

    row_begin();
    k_memset(&s, 0, sizeof(s));
    s.type = SOCK_STREAM;
    check(fd < files.cap, "the table has a descriptor 5");
    fd_slot(&files, fd)->type = FD_TYPE_SOCKET;
    fd_slot(&files, fd)->sock = &s;
    v[0].iov_base = (uintptr_t)bytes;
    v[0].iov_len = 4;
    v[1].iov_base = (uintptr_t)(bytes + 4);
    v[1].iov_len = 4;
    host_copies = 1;
    stream_answer[0] = 4;
    stream_answer[1] = -EPIPE;
    stream_sends = sigpipes = 0;
    check(writev_elements(fd, (uintptr_t)v, 2, 8) == 4 && stream_sends == 2 && sigpipes == 0,
          "a writev whose second element fails returns the first's count with no SIGPIPE");
    stream_answer[0] = -EPIPE;
    stream_sends = 0;
    check(writev_elements(fd, (uintptr_t)v, 2, 8) == -EPIPE && sigpipes == 1,
          "a writev whose first element fails with EPIPE posts SIGPIPE");
    host_copies = 0;
    fd_slot(&files, fd)->type = FD_TYPE_NONE;
    fd_slot(&files, fd)->sock = NULL;
    row_end();
}

/**
 * Set up the dumping process and the process that shares its table, and run the
 * rows; 0 when every check passed, 1 otherwise.
 */
int main(void)
{
    self.pid = self.tgid = 3;
    self_held.map.words = FD_TABLE_LIMIT / 32;
    sharer_held.map.words = FD_TABLE_LIMIT / 32;
    self.fd_held = &self_held.map;
    sharer.fd_held = &sharer_held.map;
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
    attached_core_rows();
    refault_rows();
    stream_writev();
    if (!failures) {
        printf("corefile-hosttest: result: pass: the core file's descriptor, its trailing "
               "zero under the limit, a write's refault under the file's write lock, and a "
               "writev's SIGPIPE\n");
    }
    return failures != 0;
}
