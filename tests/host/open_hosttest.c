/**
 * Check mounted open truncation, cleanup and loop handle relocation, the
 * close of a descriptor whose release sleeps, by one thread or by the end of
 * another thread's call, an unlink and a rename on the KRFFS root of a file
 * an open is installing or a program runs from, and a chmod and a chown of a
 * file a write moves.
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
static struct open_file *fs_exe_open(const struct vfs_ref *ref);
static void fs_exe_close(struct open_file *ofd);
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
static int root_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **found);
static const struct vfs_ops root_ops = {.lookup = root_lookup};
const struct vfs_type vfs_krffs_type = {"krffs", 0, &root_ops, 1, 0};
static const struct vfs_type mounted_type = {0};
/* A mounted type whose uses are serialised under its namespace lock, as
 * KRFFS volumes and ext2 are, for the attribute rows. */
static const struct vfs_type locked_type = {"locked", 0, 0, 1, 0};
static struct vfs_mount mount;
static int node, handle, moved;
static int failures, allocations, opens, releases, refs, leaves, truncates;
static int open_error, truncate_error, dropped, attrs, relocate;
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
/* The node of the root an address space keeps as its program's file. */
static const krffs_node_t *program;
/* The call that holds the root's namespace lock when a row's call first
 * asks for it, which runs before the row's call has the lock, the depth of
 * the lock, the number of times it was taken from a depth of 0, and that
 * number at the row's first lookup in the root. */
static void (*waiting)(void);
static int ns_depth, ns_takes, lookup_take;
/* Set in the rows of chmod and chown on a mounted type. */
static int attr_row;

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

/** Require the node the file is at, and, but in the attribute rows,
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

/** An address space keeps `program`, a node of the root, as its program's
 * file. */
int mm_exe_holds(const struct vfs_mount *mnt, const void *n)
{
    return n != NULL && n == program && mnt == &mount;
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

/** Take the namespace lock, first running the call that holds it, which
 * the caller waits for. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    void (*run)(void) = waiting;

    (void)mnt;
    waiting = 0;
    if (run) {
        run();
    }
    if (ns_depth++ == 0) {
        ns_takes++;
    }
}

/** Require balanced namespace locking. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    check(ns_depth > 0, "balanced namespace lock");
    ns_depth--;
}

/** Find `victim` or `source` by its name in the root directory. */
static int root_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **found)
{
    krffs_node_t *nodes[2];
    int i;

    nodes[0] = &victim;
    nodes[1] = &source;
    check(mnt == &mount && dir == &root_dir, "a lookup in the root directory");
    check(ns_depth > 0, "a lookup in the root runs under the root's lock");
    if (lookup_take == 0) {
        lookup_take = ns_takes;
    }
    check(ns_takes == lookup_take,
          "the lookups, the count and the change run under one hold of the root's lock");
    for (i = 0; i < 2; i++) {
        if (nodes[i]->name[0] != '\0' &&
            __builtin_strcmp((const char *)nodes[i]->name, name) == 0) {
            *found = nodes[i];
            return 0;
        }
    }
    return -ENOENT;
}

/** Resolve a path of the root to the root directory and its last name,
 * taking the root's lock for a locked walk, as the resolver does for a type
 * that asks for it. */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t leafcap)
{
    const char *name = path;
    const char *p;

    (void)start;
    check(ns_depth == 0, "a walk starts with no namespace lock held");
    if (!(flags & VFS_WALK_PARENT)) {
        /* The attribute rows' file, found under the lock a locked walk
         * takes, as the resolver takes it before each lookup. */
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

struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node, const char *name)
{
    (void)mnt;
    (void)node;
    (void)name;
    return 0;
}

/** Whether a change to the root runs in the hold of the root's lock that
 * its lookups ran in, so that the holder count between them did too. */
static void changed_in_lookup_hold(void)
{
    check(ns_depth > 0 && ns_takes == lookup_take,
          "the lookups, the count and the change run under one hold of the root's lock");
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
    check(do_rename("/src", "/kept", 0) == 0, "a rename over a file an open is installing");
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
    check(do_rename("/src", "/kept", 0) == 0 && keeps == 1 && removals == 0,
          "a rename over the file a program runs from keeps it");
    program = NULL;
    root_row_end();
}

/** The write a row's call waits for: it holds the mount's lock and moves
 * the file, as a growing write on a mounted KRFFS volume does. */
static void write_moves(void)
{
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
        check((round == 0 ? do_chmod("f", 0600) : do_chown("f", 5, (uint32_t)-1, 1)) == 0,
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

/** A descriptor on the model file, open for reading and writing. */
static int model_open(void)
{
    struct vfs_ref ref;

    ref.mnt = &mount;
    ref.node = &node;
    int fd = fd_alloc(proc.fds);

    return fd < 0 ? fd : open_install(&ref, fd, "target", 2);
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
    unlink_installing();
    program_kept();
    rename_over_installing();
    attrs_after_move();
    loop_handles();
    if (!failures) {
        printf("open-hosttest: result: pass: mounted truncation, loop handles and root removals\n");
    }
    return failures != 0;
}
