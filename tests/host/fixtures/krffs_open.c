/**
 * Open mounted KRFFS at the flush that publishes a relocated node.
 *
 * The proof script extracts complete resolver and open functions unchanged.
 * The block model and allocator are shared with krffsmount_hosttest.c.
 * Host threads switch only at the publication flush and namespace-lock wait.
 */
#define KRFFS_OPEN_TEST 1
#define main            krffsmount_baseline_main
#include "../krffsmount_hosttest.c"
#undef main
#include "fs/synthfs.h"
#include "panic.h"

extern void exit(int status);
extern void open_thread_start(void (*fn)(void));
extern void open_thread_wait(void);
extern void open_thread_finish(void);

static struct vfs_mount *mount_table[2];
static struct vfs_mount **mounts = mount_table;
static int mount_slots = 1;
static process_t writer, reader;
static fd_table_t reader_files;
static struct fs_struct dirs;
static int opened_fd, publication_seen, lock_waits, opening_flags, before_publication;

#define VFS_IS_DIR(mode) (((mode) & 0170000u) == 0040000u)
struct ns_turn {
    struct vfs_mount *mnt;
    uint32_t place;
};

/** Stop on a broken production lock or reference invariant. */
void k_panic(const char *message)
{
    printf("krffs-open: panic: %s\n", message);
    exit(2);
}

/** No device is removed in these rows. */
uint32_t block_flags(block_device_t *dev)
{
    return dev->flags;
}

/** Suspend the opener until the writer has finished its relocation. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    check(current_proc == &reader, "only opener waits for namespace lock");
    lock_waits++;
    open_thread_wait();
    check(cond(arg), "writer releases namespace lock before opener resumes");
    return 0;
}

/** These files do not back a loop device. */
int loop_backs(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** A page for a path from the host heap, where the kernel keeps a set. */
static char *path_buf_get(void)
{
    return calloc(1, VFS_PATH_MAX);
}

/** Give back a page `path_buf_get` returned. */
static void path_buf_put(char *buf)
{
    free(buf);
}

/** The resolver's step through /proc/<pid>/exe, which no row's walk takes:
 * the fixture has no /proc. */
static int walk_exe(struct vfs_ref *cur, void *found, int locked)
{
    (void)cur;
    (void)found;
    (void)locked;
    check(0, "no walk goes through /proc/<pid>/exe");
    return -ENOENT;
}

/** inotify's report of a new entry, which no row watches. */
static void watch_made(struct vfs_mount *mnt, void *dir, const char *name, uint32_t isdir)
{
    (void)mnt;
    (void)dir;
    (void)name;
    (void)isdir;
}

/** Every mount is in the fixture's one mount namespace. */
int vfs_mount_seen(const struct vfs_mount *m)
{
    (void)m;
    return 1;
}

#include "vfs_open.inc"

/** All fixture files use the mounted KRFFS adapter. */
static int mnt_is_root(const struct vfs_mount *mnt)
{
    return mnt->type == &vfs_krffs_type;
}

/** No fixture node belongs to a synthetic file system. */
const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return NULL;
}

/** Both fixture processes have root's credentials. */
static int32_t cred_access_ref(const struct vfs_ref *ref, int want, int real)
{
    (void)ref;
    (void)want;
    (void)real;
    return 0;
}

/** The memory-root permission branch is outside these rows. */
static int32_t cred_access_node(const krffs_node_t *node, int want, int real)
{
    (void)node;
    (void)want;
    (void)real;
    return 0;
}

/** Root does not clear set-id bits. */
static int cred_privileged(void)
{
    return 1;
}

/** Reject an unexpected set-id update. */
static void vfs_kill_setid(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    check(0, "unexpected set-id update");
}

/** Reject creation through the memory-root adapter. */
static void krffs_stamp_new_node(krffs_node_t *node, const krffs_node_t *parent, uint32_t mode)
{
    (void)node;
    (void)parent;
    (void)mode;
    check(0, "unexpected memory-root creation");
}

/** Reject an unexpected memory-root open. */
static int open_root_node(struct vfs_ref *ref, int flags)
{
    (void)ref;
    (void)flags;
    check(0, "unexpected memory-root open");
    return -EIO;
}

/** No synthetic link is opened in these rows. */
static int synth_is_link(const struct vpath *vp)
{
    (void)vp;
    return 0;
}

/** Reject an unexpected synthetic open. */
static int32_t synth_open(const struct vpath *vp, int flags)
{
    (void)vp;
    (void)flags;
    check(0, "unexpected synthetic open");
    return -EIO;
}

/** Reject an unexpected framebuffer open. */
static int32_t open_framebuffer(void)
{
    check(0, "unexpected framebuffer open");
    return -EIO;
}

/** Reject an unexpected mouse open. */
static int32_t open_mouse(void)
{
    check(0, "unexpected mouse open");
    return -EIO;
}

/** Reject an unexpected O_PATH open. */
static int32_t open_path(const char *name, int flags)
{
    (void)name;
    (void)flags;
    check(0, "unexpected O_PATH open");
    return -EIO;
}

/** No row takes a lock. */
int lock_any(void)
{
    return 0;
}

void lock_ofd_gone(const struct open_file *ofd)
{
    (void)ofd;
}

/** No node of the fixture is a FIFO, a socket file or a device node. */
static int open_special(struct vfs_ref *ref, int fd, int flags, int32_t *result)
{
    (void)ref;
    (void)fd;
    (void)flags;
    (void)result;
    return 0;
}

#include "syscall_open.inc"

/** Run the real open path in the second process. */
static void open_published(void)
{
    opened_fd = do_open("target", opening_flags, 0644);
}

/**
 * Start an open before publication or when lookup first sees the copy.
 */
static void publication_flush(void)
{
    uint64_t found = krffs_child_at(&vol_of(&mount)->fs, 0, "target");
    krffs_node_t old;

    if (found == KRFFS_NO_NODE || (before_publication ? found != offset_of(&mount, original)
                                                      : found >= offset_of(&mount, original))) {
        return;
    }
    check(krffs_head(&vol_of(&mount)->fs, offset_of(&mount, original), &old) == 0 &&
              old.type == KRFFS_RESERVED_NODE,
          "open starts before old node is freed");
    flush_hook = NULL;
    publication_seen++;
    current_proc = &reader;
    open_thread_start(open_published);
    current_proc = &writer;
}

/**
 * Require both opens to survive a second move and reuse of the first copy.
 */
static void publication_open(int flags, int before)
{
    void *hole, *moved, *reused;
    struct open_file *first, *second;
    int fd, reuse;
    char byte;

    k_memset(&writer, 0, sizeof(writer));
    k_memset(&reader, 0, sizeof(reader));
    writer.pid = 1;
    reader.pid = 2;
    writer.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    reader.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    current_proc = &writer;
    setup();
    writer.fds = &table;
    writer.fs = &dirs;
    reader.fds = &reader_files;
    reader.fs = &dirs;
    check(fd_table_init(reader.fds) == 0, "reader descriptor table");
    mount_table[0] = &mount;
    mount.state = VFS_ACTIVE;
    mount.ns_holder = -1;
    hole = create("hole");
    fd = do_open("hole", 2, 0);
    check(fd >= 0, "open free-extent fixture");
    k_memset(payload, 'H', 4096);
    check(kv_write(&mount, writer.fds->entries[fd].ofd->fhandle, payload, 4096, 0) == 4096,
          "size free-extent fixture");
    fd_free(writer.fds, fd);
    create("separator");
    original = create("target");
    fd = do_open("target", 2, 0);
    check(fd >= 0, "open original target");
    first = writer.fds->entries[fd].ofd;
    check(kv_write(&mount, first->fhandle, "A", 1, 0) == 1, "seed concurrent target");
    create("blocker");
    vfs_ns_lock(&mount);
    check(kv_unlink(&mount, mount.root, "hole") == 0, "free earlier extent");
    vfs_ns_unlock(&mount);
    opening_flags = flags;
    before_publication = before;
    opened_fd = -1;
    publication_seen = lock_waits = 0;
    flush_hook = publication_flush;
    k_memset(payload, 'B', 4096);
    check(kv_write(&mount, first->fhandle, payload, 4096, 0) == 4096,
          "relocate into earlier extent");
    check(publication_seen == 1,
          before ? "open injected before publication" : "open injected at publication flush");
    if (!publication_seen) {
        exit(2);
    }
    current_proc = &reader;
    open_thread_finish();
    current_proc = &writer;
    check(opened_fd >= 0, "publication open succeeds");
    if (opened_fd < 0) {
        exit(2);
    }
    second = reader.fds->entries[opened_fd].ofd;
    check(first->vnode == hole, "first relocation uses earlier free extent");
    moved = first->vnode;
    k_memset(payload, 'C', sizeof(payload));
    check(kv_write(&mount, first->fhandle, payload, sizeof(payload), 0) == sizeof(payload),
          "relocate again after publication open");
    check(first->vnode != moved, "second relocation changes offset");
    reused = create("reuse-copy");
    check(reused == moved, "first copy offset reused");
    reuse = do_open("reuse-copy", 2, 0);
    check(reuse >= 0, "open reused copy offset");
    check(kv_write(&mount, writer.fds->entries[reuse].ofd->fhandle, "R", 1, 0) == 1,
          "write reused copy offset");
    check(kv_read(&mount, first->fhandle, &byte, 1, 0) == 1 && byte == 'C',
          "original open follows second move despite copy reuse");
    check(kv_read(&mount, second->fhandle, &byte, 1, 0) == 1 && byte == 'C',
          "concurrent open follows second move despite copy reuse");
    check(first->fhandle == second->fhandle && lock_waits == 1,
          "publication open waits and shares one open record");
    fd_free(reader.fds, opened_fd);
    fd_table_free(reader.fds);
    teardown();
    check(mount.refs == 0 && mount.inprogress == 0 && mount.ns_depth == 0,
          "open releases every mount reference and lock");
}

/** Check lock transfer across covered mounts and cleanup on open errors. */
static void locked_walks(void)
{
    struct vfs_mount child;
    struct vfs_ref ref;
    void *covered;
    int fd;

    current_proc = &writer;
    writer.rlim[RLIM_NOFILE].cur = FD_TABLE_LIMIT;
    setup();
    writer.fds = &table;
    writer.fs = &dirs;
    mount_table[0] = &mount;
    mount.state = VFS_ACTIVE;
    mount.ns_holder = -1;
    fd = do_open("created", 2 | O_CREAT | O_EXCL, 0644);
    check(fd >= 0 && mount.ns_depth == 0 && mount.inprogress == 0,
          "creating open registers and releases lock");
    if (fd >= 0) {
        fd_free(writer.fds, fd);
    }
    check(do_open("created", O_CREAT | O_EXCL, 0644) == -EEXIST && mount.ns_depth == 0,
          "exclusive open error releases lock");
    check(do_open("created", O_DIRECTORY, 0) == -ENOTDIR && mount.ns_depth == 0,
          "directory open error releases lock");
    check(do_open("absent", 0, 0) == -ENOENT && mount.ns_depth == 0, "missing path releases lock");
    check(do_open("created/child", 0, 0) == -ENOTDIR && mount.ns_depth == 0,
          "non-directory prefix releases lock");
    mount.flags |= VFS_MS_RDONLY;
    check(do_open("created", 2, 0) == -EROFS && mount.ns_depth == 0,
          "read-only open error releases lock");
    check(do_open("absent", O_CREAT, 0644) == -EROFS && mount.ns_depth == 0,
          "read-only creation error releases lock");
    mount.flags = 0;
    vfs_ns_lock(&mount);
    check(kv_mkdir(&mount, mount.root, "covered", 0755) == 0, "create covered directory");
    check(kv_lookup(&mount, mount.root, "covered", &covered) == 0, "find covered directory");
    vfs_ns_unlock(&mount);
    child = mount;
    child.parent = &mount;
    child.covered = covered;
    mount_table[1] = &child;
    mount_slots = 2;
    check(vfs_resolve(NULL, "covered/created", VFS_WALK_LOCK, &ref, NULL, 0) == 0 &&
              ref.mnt == &child && child.ns_depth == 1 && mount.ns_depth == 0,
          "covered mount receives the resolution lock");
    vfs_ns_unlock(ref.mnt);
    vfs_ref_release(&ref);
    check(vfs_resolve(NULL, "covered/../created", VFS_WALK_LOCK, &ref, NULL, 0) == 0 &&
              ref.mnt == &mount && mount.ns_depth == 1 && child.ns_depth == 0,
          "parent climb transfers the resolution lock");
    vfs_ns_unlock(ref.mnt);
    vfs_ref_release(&ref);
    check(vfs_resolve(NULL, "covered/absent", VFS_WALK_LOCK, &ref, NULL, 0) == -ENOENT &&
              mount.ns_depth == 0 && child.ns_depth == 0 && child.inprogress == 0,
          "failed walk releases the child mount lock and reference");
    mount_slots = 1;
    teardown();
    check(mount.inprogress == 0 && mount.ns_depth == 0, "failed opens release all references");
}

/** Exercise ordinary and creating opens at relocation publication. */
int main(void)
{
    publication_open(0, 0);
    publication_open(O_CREAT, 0);
    publication_open(0, 1);
    if (!failures) {
        locked_walks();
    }
    if (!failures) {
        printf("krffs-open: result: pass: publication open and offset reuse\n");
    }
    return failures != 0;
}
