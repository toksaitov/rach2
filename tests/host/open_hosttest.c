/**
 * Check mounted open truncation, cleanup and loop handle relocation.
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

static process_t proc;
process_t *current_proc = &proc;
krffs_file_system_t *krffs_fs;
const struct vfs_type vfs_krffs_type = {0};
static const struct vfs_type mounted_type = {0};
static struct vfs_mount mount;
static int node, handle, moved;
static int failures, allocations, opens, releases, refs, leaves, truncates;
static int open_error, truncate_error, dropped, attrs, relocate;

/** Record every failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("open-hosttest: fail: %s\n", what);
        failures++;
    }
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

/** Release exactly one successfully opened handle. */
void vfs_release(struct vfs_mount *mnt, void *at)
{
    check(mnt == &mount && at == &handle && opens == releases + 1,
          "release balances successful open");
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

/** Close the handle when the description is freed. */
void vfs_desc_close(struct vfs_mount *mnt, void *at, void *opened)
{
    check(at == (relocate ? &moved : &node), "close retains current node identity");
    vfs_release(mnt, opened);
    refs--;
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
        check(fd_table_init(&proc.fds) == 0, "initialize descriptor table");
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
                fd_free(&proc.fds, rc);
            }
        } else {
            check(rc == (mode == 1 ? -ENOMEM : -EIO) && refs == 0 && leaves == (mode == 2) &&
                      dropped == (mode == 1),
                  "failed open returns error and releases resolution");
            check(allocations == 1 && attrs == 0,
                  "failed open frees description without changing mode");
        }
        check(opens == releases && refs == 0, "every open handle released");
        fd_table_free(&proc.fds);
        check(allocations == 0, "all descriptor storage released");
    }
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
    loop_handles();
    if (!failures) {
        printf("open-hosttest: result: pass: mounted truncation and loop handles\n");
    }
    return failures != 0;
}
