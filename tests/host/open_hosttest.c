/**
 * Check mounted open truncation, cleanup and loop handle relocation, and the
 * close of a descriptor whose release sleeps, by one thread or by the end of
 * another thread's call.
 *
 * host-deps: rach2/sys/fs/fd.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * Unused syscall and loop exports are private so their hardware paths are
 * removed. The model uses distinct nodes and open handles.
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
static int32_t fs_core_file(int signo, uint32_t limit);
static int32_t sys_mknod(isr_params_t *params);
static int32_t sys_mknodat(isr_params_t *params);
static int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino);
static void fs_socket_unbind(struct vfs_ref *ref);
static int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino);
static int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off);
static block_device_t *loop_device_at(int index);
static int loop_attach(int index, const char *path, int writable);
static int loop_detach(int index);
static int loop_backs(struct vfs_mount *mnt, void *node);
static void loop_repoint(struct vfs_mount *mnt, const void *from, void *to);
static void loop_device_gone(block_device_t *device);
static int loop_line(int index, char *buffer, uint32_t cap);
static int loop_control_write(const char *buffer, uint32_t count);
#include "syscall_fs.c"
#include "block/loop.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static process_t proc, other, third;
static fd_table_t files;
process_t *current_proc = &proc;
process_t *proc_list;
volatile uint32_t fb_owner_pid;
krffs_file_system_t *krffs_fs;
const struct vfs_type vfs_krffs_type = {0};
static const struct vfs_type mounted_type = {0};
static struct vfs_mount mount;
static int node, handle, moved;
static int failures, allocations, opens, releases, refs, leaves, truncates;
static int open_error, truncate_error, dropped, attrs, relocate;
/* The descriptor a release is under way for, and what happens while the
 * file system's release of its description sleeps. */
static int releasing = -1;
static void (*in_release)(void);
/* Whether a record lock exists, and the drops of one process's locks. */
static int locking, lock_closes;
static process_t *lock_closer;

/** Record every failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("open-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** A KRFFS node's inode number, which no row reads. */
uint64_t krffs_node_ino(const krffs_node_t *node)
{
    (void)node;
    return 0;
}

/** Whether the row has a record lock somewhere, which is when a close
 * drops the closer's. */
int lock_any(void)
{
    return locking;
}

void lock_ofd_gone(const struct open_file *ofd)
{
    (void)ofd;
}

/** Count host allocations. */
void *kzalloc(size_t size)
{
    void *p = calloc(1, size);
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

/** Report an accessible regular file with set-id bits. */
int vfs_getattr(struct vfs_mount *mnt, void *at, struct vfs_stat *st)
{
    check(mnt == &mount && at == (relocate ? &moved : &node), "attributes use node identity");
    k_memset(st, 0, sizeof(*st));
    st->mode = 0106777;
    return 0;
}

/** Require set-id removal after successful truncation. */
int vfs_setattr(struct vfs_mount *mnt, void *at, int what, const struct vfs_stat *st)
{
    check(mnt == &mount && at == (relocate ? &moved : &node) && what == VFS_SET_MODE &&
              !(st->mode & 06000),
          "successful truncation clears set-id bits");
    attrs++;
    return 0;
}

/** Return an open identity distinct from the node. */
int vfs_open_node(struct vfs_mount *mnt, void *at, int flags, void **out)
{
    (void)flags;
    check(mnt == &mount && at == &node, "open uses node identity");
    if (open_error) {
        return open_error;
    }
    *out = &handle;
    opens++;
    return 0;
}

/** Require a live open handle before truncation. */
int vfs_truncate(struct vfs_mount *mnt, void *at, uint64_t size)
{
    check(mnt == &mount && at == &handle && opens == releases + 1 && size == 0,
          "mounted truncation uses a live open handle");
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
 * sleeps: run what the row has happen meanwhile. */
void vfs_desc_close(struct vfs_mount *mnt, void *at, void *opened)
{
    check(at == (relocate ? &moved : &node), "close retains current node identity");
    if (in_release) {
        void (*meanwhile)(void) = in_release;

        in_release = 0;
        meanwhile();
    }
    vfs_release(mnt, opened);
    refs--;
}

/** No row's file is held by a message in flight or a mapping. */
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

/** No row frees a node of the KRFFS root, owns the display or has a
 * socket. */
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    (void)fs;
    (void)node;
    check(0, "unexpected root node removal");
}

void vga_mode13h_exit(void)
{
    check(0, "unexpected display release");
}

void socket_ref(struct socket *sock)
{
    (void)sock;
    check(0, "unexpected socket reference");
}

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

/** Initialize an unused wait queue. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Ignore unused descriptor wakeups. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Reject unexpected description waits. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "unexpected description wait");
    return 0;
}

/** No root namespace lock is needed by these descriptions. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    (void)mnt;
}

/** Balance the unused root namespace lock. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
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
        rc = open_install(&ref, "target", 2 | (mode == 3 ? 0 : O_TRUNC));
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
    open_error = truncate_error = 0;
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

/** A descriptor on the model file, open for reading and writing. */
static int model_open(void)
{
    struct vfs_ref ref;

    ref.mnt = &mount;
    ref.node = &node;
    return open_install(&ref, "target", 2);
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

/** Exercise mounted open and loop references. */
int main(void)
{
    mount.type = &mounted_type;
    truncation();
    close_sleeping();
    deferred_release();
    dup2_source_closed();
    loop_handles();
    if (!failures) {
        printf("open-hosttest: result: pass: mounted truncation and loop handles\n");
    }
    return failures != 0;
}
