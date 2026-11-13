/**
 * Open mounted KRFFS at the flush that publishes a relocated node, walk a
 * model type's paths by the cached walk without the big lock, and change a
 * model ext2 file through the VFS by an open handle that is not its node.
 *
 * The rule of mk/host-fixtures.mk extracts complete resolver and open
 * functions unchanged, with the namespace lock and the sleeping lock of
 * fs/sleeplock.c under it.
 * The block model and allocator are shared with krffsmount_hosttest.c.
 * Host threads switch only at the publication flush and namespace-lock wait.
 */
#define KRFFS_OPEN_TEST 1
#define main            krffsmount_baseline_main
#include "../krffsmount_hosttest.c"
#undef main
#include "fs/synthfs.h"
#include "fs/name.h"
#include "proc/smp.h"
#include "block/loop.h"
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
/* The opener's descriptor, the flush hook's count of the open it started,
 * the namespace-lock waits, and the open flags and whether the open starts
 * before the copy is published. */
static int opened_fd, publication_seen, lock_waits, opening_flags, before_publication;

#define VFS_IS_DIR(mode) (((mode) & 0170000u) == 0040000u)

/*
 * The cached walk's model: a type whose names the name cache keeps, which
 * reads without the big lock, and whose links hold their targets when
 * `fast`, as ext2's fast links do. Its nodes are one table, each named in
 * its parent; `refs` counts the walk's holds, which every row gives back.
 * Reading the link `moving` moves the walker's root, as a chroot by a
 * thread that shares it (CLONE_FS) would during the walk, and reading
 * `renaming` counts a change of names, as a rename by another thread would.
 */
struct cnode {
    struct cnode *parent;
    const char *name;
    uint32_t mode;
    const char *target; /* a link's target, NULL for any other node */
    int fast;           /* the target is in the node, for `readlink_cached` */
    int refs;
};

enum {
    C_ROOT,
    C_BIN,
    C_TOOL,
    C_REL,
    C_ABS,
    C_TOP,
    C_SLOW,
    C_COVER,
    C_UNDER,
    C_UNSEEN,
    C_HERE,
    C_UNSURE,
    C_TARGET,
    C_ALIAS,
    C_MOVING,
    C_OROOT,
    C_INNER,
    C_SOURCE,
    C_BACK,
    C_OUT,
    C_DROOT,
    C_UP,
    C_PFX,
    C_DOS,
    C_CLINK,
    C_DLINK,
    C_DRIVE,
    C_WIN,
    C_SECRET,
    C_DSECRET,
    C_VROOT,
    C_VSECRET,
    C_SRC,
    C_SSECRET,
    C_NODES
};

static struct cnode cn[C_NODES];
#define C_MOUNTS 9
static struct vfs_mount croot, cother, cunseen, cunsure, cbind, cnamed, cdcover, cdbind, crcover;
static struct vfs_mount *cache_table[C_MOUNTS] = {&croot,  &cother,  &cunseen, &cunsure, &cbind,
                                                  &cnamed, &cdcover, &cdbind,  &crcover};
/* The model's count of changes of names, which reading `renaming` moves. */
static uint32_t names_changed;
/* The record the cached walk captures for a regular file's last name, and
 * the counts of those captured and dropped. */
static int name_record, names_taken, names_put;

/** Name node `i` in `parent`, of `mode`, a link to `target` when given. */
static void cnode_set(int i, int parent, const char *name, uint32_t mode, const char *target,
                      int fast)
{
    cn[i].parent = parent < 0 ? NULL : &cn[parent];
    cn[i].name = name;
    cn[i].mode = mode;
    cn[i].target = target;
    cn[i].fast = fast;
    cn[i].refs = 0;
}

/** Take a hold on a model node. */
static void cache_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct cnode *)node)->refs++;
}

/** Give a hold back; a hold that was never taken fails the row. */
static void cache_put(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    check(((struct cnode *)node)->refs > 0, "the cached walk puts only a hold it took");
    ((struct cnode *)node)->refs--;
}

/** Give a hold back unless it is the last, which `put` gives under the big
 * lock: 1 when given back. */
static int cache_put_unless_last(struct vfs_mount *mnt, void *node)
{
    struct cnode *n = node;

    (void)mnt;
    if (n->refs > 1) {
        n->refs--;
        return 1;
    }
    return 0;
}

/** A node's attributes: its mode and root's ownership. */
static int cache_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = ((struct cnode *)node)->mode;
    st->nlink = 1;
    return 0;
}

/** Copy a link's target into `buf`: its length, or -ENAMETOOLONG. */
static int cache_target(const struct cnode *n, char *buf, uint32_t cap)
{
    uint32_t len = (uint32_t)k_strlen(n->target);

    if (len + 1 > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(buf, n->target, len + 1);
    return (int)len;
}

/** A link's target, read with the node's lock: any link. */
static int cache_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    (void)mnt;
    return ((struct cnode *)node)->target ? cache_target(node, buf, cap) : -EINVAL;
}

/** A link's target without its lock, for the cached walk: a fast link only,
 * -1 for any other. Reading `moving` moves the walker's root. */
static int cache_readlink_cached(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct cnode *n = node;

    (void)mnt;
    if (!n->target || !n->fast) {
        return -1;
    }
    if (n == &cn[C_MOVING]) {
        dirs.root_seq++;
    }
    if (n == &cn[C_DLINK]) {
        names_changed++;
    }
    return cache_target(n, buf, cap);
}

/** Whether the names `a` and `b` are the same. */
static int cache_same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The name cache's entry for `name` in `dir` of the model, held: 0, or
 * -EAGAIN for a name it does not keep. */
static int cache_hit(void *dir, const char *name, void **node)
{
    int i;

    for (i = 0; i < C_NODES; i++) {
        if (cn[i].parent == dir && cn[i].name && cache_same(cn[i].name, name)) {
            cn[i].refs++;
            *node = &cn[i];
            return 0;
        }
    }
    return -EAGAIN;
}

/** A name in a directory, for the full walk, which the rows that compare
 * the two walks take: the model's names, held. 0, or -ENOENT. */
static int cache_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    (void)mnt;
    return cache_hit(dir, name, node) == 0 ? 0 : -ENOENT;
}

static const struct vfs_ops cache_ops = {
    .lookup = cache_lookup,
    .hold = cache_hold,
    .put = cache_put,
    .getattr = cache_getattr,
    .readlink = cache_readlink,
    .put_unless_last = cache_put_unless_last,
    .readlink_cached = cache_readlink_cached,
};
static const struct vfs_type cache_type = {
    .name = "cache", .ops = &cache_ops, .name_cache = 1, .reads_unlocked = 1};

/** Print `message` and exit 2: a lock or reference invariant of the kernel
 * code failed. */
void k_panic(const char *message)
{
    printf("krffs-open: panic: %s\n", message);
    exit(2);
}

/** The clock, which the lock reads for its record of holds; these rows
 * make no disk wait, so no hold is recorded. */
uint32_t timer_now_us(void)
{
    return 0;
}

/** The cycle counter: the clock's microseconds, a cycle each. */
uint64_t arch_cycles(void)
{
    return timer_now_us();
}

/** A cycle is a microsecond here. */
uint64_t timer_cycles_us(uint64_t cycles)
{
    return cycles;
}

/** The cycles here are the clock's microseconds, the same on every
 * processor. */
int timer_cycles_agree(void)
{
    return 1;
}

/** No device is removed in these rows. */
uint32_t block_flags(block_device_t *dev)
{
    return dev->flags;
}

/* Whether the opener sleeps in the namespace lock's wait, for `wq_active`;
 * changed only across a hand-off of the threads' mutex
 * (krffs_open_threads.c). */
static int opener_asleep;

/** The namespace-lock wait: check that the opener is the waiter, count the
 * wait, and suspend the opener until the writer has finished its
 * relocation, when the lock must be free. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    check(current_proc == &reader, "only opener waits for namespace lock");
    lock_waits++;
    opener_asleep = 1;
    open_thread_wait();
    opener_asleep = 0;
    check(cond(arg), "writer releases namespace lock before opener resumes");
    return 0;
}

/* kernel.lock_stat, off as at boot: the extracted lock times no hold. */
int sleep_lock_stat;

/** Whether a thread sleeps in a lock's wait: the opener, from its wait for
 * the namespace lock until the writer resumes it. */
int wq_active(const wait_queue_t *wq)
{
    (void)wq;
    return opener_asleep;
}

/* The loop devices, which `loop_backs` of rach2/sys/block/loop.c reads
 * through this pointer, since the structure is complete only in
 * vfs_open.inc; `main` points it at `loop_table`. */
static struct loop_device *loops;
/* The lock `loop_backs` reads an attachment's `base` and `node` under. */
static struct spinlock loop_lock = SPINLOCK_INIT;

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
static int walk_exe(struct vfs_ref *cur, void *found, int locked, struct vfs_name **entry)
{
    (void)cur;
    (void)found;
    (void)locked;
    (void)entry;
    check(0, "no walk goes through /proc/<pid>/exe");
    return -ENOENT;
}

/** No fixture node is a link of /proc followed to its object. */
static int walk_to_object(const struct vpath *vp, int rc, int parent, int flags)
{
    (void)vp;
    (void)rc;
    (void)parent;
    (void)flags;
    return 0;
}

/* What `walk_object` returns at the link's object, and the flag of an
 * unmount's walk, as vfs.c defines them. */
#define WALK_AT_OBJECT 2
#define WALK_UNMOUNT   0x100

/* The record of the path a walk took, as vfs.c defines it. */
struct walk_name {
    char *buf;
    int len;
};

/** The resolver's step through a /proc link to its object, which no row's
 * walk takes. */
static int walk_object(struct vfs_ref *cur, void *found, const char *rest, char **linkpath,
                       char **target, int locked)
{
    (void)cur;
    (void)found;
    (void)rest;
    (void)linkpath;
    (void)target;
    (void)locked;
    check(0, "no walk goes through a /proc link to its object");
    return -ENOENT;
}

/* The mount and node the last `filecache_forget` was given. */
static struct vfs_mount *forgot_mnt;
static void *forgot_node;

/** The file page cache, which keeps no page of the fixture's files: record
 * what the wrapper asked it to forget. */
static void filecache_forget(struct vfs_mount *mnt, void *node)
{
    forgot_mnt = mnt;
    forgot_node = node;
}

/** inotify's report of a new entry, which no row watches. */
static void watch_made(struct vfs_mount *mnt, void *dir, const char *name, uint32_t isdir)
{
    (void)mnt;
    (void)dir;
    (void)name;
    (void)isdir;
}

/** The fixture's processes work in the root, which every walk may search. */
const char *proc_cwd(const struct process *p)
{
    (void)p;
    return "/";
}

/** Every mount is in the fixture's one mount namespace. */
int vfs_mount_seen(const struct vfs_mount *m)
{
    (void)m;
    return 1;
}

/** And the cached walk reads that namespace's view in the set's first word:
 * of the model's mounts, `cunseen` is another namespace's and whether this
 * one sees `cunsure` the word cannot say. */
int vfs_mount_seen_word(const struct vfs_mount *m)
{
    return m == &cunseen ? 0 : m == &cunsure ? -1 : 1;
}

/** The fixture's types keep no name cache: each lookup goes to the type. */
static int dcache_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    return mnt->type->ops->lookup(mnt, dir, name, node);
}

/** Nothing to forget without a name cache. */
static void dcache_forget(struct vfs_mount *mnt, void *dir, const char *name)
{
    (void)mnt;
    (void)dir;
    (void)name;
}

/** The name cache keeps nothing for later. */
static void dcache_drain(void)
{}

/** RESOLVE_CACHED finds no KRFFS name in the cache. */
static int dcache_lookup_cached(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    (void)mnt;
    (void)dir;
    (void)name;
    (void)node;
    return -EAGAIN;
}

/** The cached walk's lookup: the model's names, and none of KRFFS's, which
 * keeps no names in the cache. */
static int dcache_lookup_hit(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    if (mnt->type == &cache_type) {
        return cache_hit(dir, name, node);
    }
    return dcache_lookup_cached(mnt, dir, name, node);
}

/** The cached walk's hits are not counted here. */
static void dcache_count_hits(uint32_t n)
{
    (void)n;
}

/* The lock the cached walk reads the table under, and the tree's sequence,
 * which no row moves. */
static struct spinlock mount_table_lock = SPINLOCK_INIT;
static uint32_t tree_seq;

/* The lock over the sleeping locks' records, which sleeplock.c defines. */
static struct spinlock records_lock = SPINLOCK_INIT;

/* The protection of /proc/sys/fs that a final link meets, at its default,
 * and the debugfs type, which no row opens. */
int vfs_protected_symlinks = 1;
const struct vfs_type vfs_debugfs_type;

/** No fixture mount is detached, so a closed description reaps nothing,
 * whatever count `left` its put returned. */
static void reap_if_detached(const struct vfs_mount *mnt, uint32_t left)
{
    (void)mnt;
    (void)left;
}

/** The count of changes of names, of which none is ever under way. */
int vfs_name_seq(uint32_t *seq)
{
    *seq = names_changed;
    return 1;
}

/** The same count, read as the cached walk reads it at its start. */
int vfs_name_seq_peek(uint32_t *seq)
{
    return vfs_name_seq(seq);
}

/** Whether the count is still `seq`. */
int vfs_name_seq_still(uint32_t seq)
{
    return names_changed == seq;
}

/** Start a description's cursor into its file's write-back error sequence at
 * none, since no row records a write-back error. */
uint32_t vfs_ino_err_sample(struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
    return 0;
}

/** Capture a regular file's last name for the cached walk: the model's one
 * record, counted. */
int vfs_name_capture_at(const struct vfs_ref *parent, const char *leaf, uint32_t seq,
                        struct vfs_name **out)
{
    (void)parent;
    (void)leaf;
    (void)seq;
    names_taken++;
    *out = (struct vfs_name *)&name_record;
    return 0;
}

/** Drop the record the cached walk captured, counted; the resolver's code
 * calls this through the name below, while fd.c keeps the stub of
 * compat/vfs_name_none.c, which no record reaches. */
static void fixture_name_put(struct vfs_name *name)
{
    if (name) {
        check(name == (struct vfs_name *)&name_record, "a dropped record is the one captured");
        names_put++;
    }
}

/* A waiter of the sleeping lock always sleeps here: the threads take turns,
 * so a holder never runs beside it (proc/sched.h). */
static int sched_spin_on(const struct process *owner, int cpu)
{
    (void)owner;
    (void)cpu;
    return 0;
}

#define vfs_name_put fixture_name_put
#include "vfs_open.inc"
#undef vfs_name_put

static struct loop_device loop_table[LOOP_DEVICES];

/*
 * The ext2 model of `ext2_handles`: a type whose `open` gives each open a
 * handle of its own that holds the node, as ext2's `struct ext2_handle`
 * does, and whose operations record the handle or node they were given.
 */
struct e2m_handle {
    void *node;
    int open;
};

static struct e2m_handle e2m_handles[2];
static char e2m_file; /* the one file's node */
static uint32_t e2m_flags;
/* The last handle the data operations and the last node `getattr` got. */
static void *e2m_data, *e2m_attr;
static int e2m_calls;

/** The model's `open`: the next free handle, holding `node`. */
static int e2m_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    int i;

    (void)mnt;
    (void)flags;
    for (i = 0; i < 2 && e2m_handles[i].open; i++) {
    }
    check(i < 2, "the ext2 model opens no more than two handles");
    e2m_handles[i].node = node;
    e2m_handles[i].open = 1;
    *handle = &e2m_handles[i];
    return 0;
}

/** The model's `release`: the handle is free again. */
static void e2m_release(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    ((struct e2m_handle *)handle)->open = 0;
}

/** The model's `handle_node`, as ext2's `open_node`. */
static void *e2m_node(void *handle)
{
    return ((struct e2m_handle *)handle)->node;
}

/** The model's `getattr`, which knows the inode flags of its one file. */
static int e2m_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    e2m_attr = node;
    if (node != &e2m_file) {
        return -EINVAL;
    }
    st->mode = 0100644u;
    st->flags = e2m_flags;
    st->flags_known = 1;
    return 0;
}

/** The model's `write`: the count, with the handle recorded. */
static int32_t e2m_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                         uint64_t offset)
{
    (void)mnt;
    (void)buf;
    (void)offset;
    e2m_data = handle;
    e2m_calls++;
    return (int32_t)count;
}

/** The model's `truncate`: 0, with the handle recorded. */
static int e2m_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    (void)mnt;
    (void)size;
    e2m_data = handle;
    e2m_calls++;
    return 0;
}

/** The model's `fallocate`: 0, with the handle recorded. */
static int e2m_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                         uint64_t len)
{
    (void)mnt;
    (void)mode;
    (void)offset;
    (void)len;
    e2m_data = handle;
    e2m_calls++;
    return 0;
}

static const struct vfs_ops e2m_ops = {
    .getattr = e2m_getattr,
    .open = e2m_open,
    .release = e2m_release,
    .write = e2m_write,
    .truncate = e2m_truncate,
    .fallocate = e2m_fallocate,
    .handle_node = e2m_node,
};
static const struct vfs_type e2m_type = {.name = "ext2", .ops = &e2m_ops};
static struct vfs_mount e2m_mount;

/**
 * Open the ext2 model's file twice, as a loop attachment and an unrelated
 * opener of its image would, and require vfs_truncate and vfs_fallocate to
 * refuse either handle with EBUSY while the file backs a loop device; then,
 * with no attachment, require vfs_write, vfs_truncate and vfs_fallocate to
 * give the type the handle and the file page cache and the flag check the
 * node.
 */
static void ext2_handles(void)
{
    void *attached = NULL, *other = NULL;

    k_memset(&e2m_mount, 0, sizeof(e2m_mount));
    e2m_mount.type = &e2m_type;
    e2m_mount.state = VFS_ACTIVE;
    check(vfs_open_node(&e2m_mount, &e2m_file, 2, &attached) == 0 &&
              vfs_open_node(&e2m_mount, &e2m_file, 2, &other) == 0,
          "two opens of an ext2 file");
    check(attached != other && attached != &e2m_file && other != &e2m_file,
          "each open of an ext2 file has a handle of its own, never the node");
    loop_table[0].mnt = &e2m_mount;
    loop_table[0].base = &e2m_mount;
    loop_table[0].node = &e2m_file;
    loop_table[0].handle = attached;
    loop_table[0].attached = 1;
    e2m_calls = 0;
    check(vfs_fallocate(&e2m_mount, other, 0, 0, 4096) == -EBUSY,
          "fallocate by another open of a loop's backing file is EBUSY");
    check(vfs_truncate(&e2m_mount, other, 0) == -EBUSY, "and so is a truncation by that open");
    check(vfs_fallocate(&e2m_mount, attached, 0, 0, 4096) == -EBUSY &&
              vfs_truncate(&e2m_mount, attached, 0) == -EBUSY,
          "and by the attachment's own handle");
    check(e2m_calls == 0, "the type never sees a change of a loop's backing file");
    k_memset(&loop_table[0], 0, sizeof(loop_table[0]));
    forgot_node = NULL;
    check(vfs_write(&e2m_mount, other, "x", 1, 0) == 1 && e2m_data == other &&
              forgot_mnt == &e2m_mount && forgot_node == &e2m_file,
          "vfs_write gives the type the handle and forgets the node's cached pages");
    forgot_node = NULL;
    check(vfs_truncate(&e2m_mount, other, 0) == 0 && e2m_data == other && forgot_node == &e2m_file,
          "and so does vfs_truncate");
    forgot_node = NULL;
    e2m_attr = NULL;
    check(vfs_fallocate(&e2m_mount, other, 0, 0, 4096) == 0 && e2m_data == other &&
              forgot_node == &e2m_file && e2m_attr == &e2m_file,
          "and vfs_fallocate, which reads the node's flags");
    e2m_flags = VFS_FL_IMMUTABLE;
    e2m_calls = 0;
    check(vfs_fallocate(&e2m_mount, other, 0, 0, 4096) == -EPERM && e2m_calls == 0,
          "fallocate of an immutable ext2 file is EPERM by its node's flags");
    e2m_flags = VFS_FL_APPEND;
    check(vfs_fallocate(&e2m_mount, other, 1, 0, 4096) == 0 &&
              vfs_fallocate(&e2m_mount, other, 2, 0, 4096) == -EPERM,
          "and an append-only file takes only a plain or keep-size allocation");
    e2m_flags = 0;
    vfs_release(&e2m_mount, attached);
    vfs_release(&e2m_mount, other);
    check(!e2m_handles[0].open && !e2m_handles[1].open && e2m_mount.opens == e2m_mount.releases,
          "both ext2 opens are released");
}

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

/** No fixture name is a mount point. */
struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node, const char *name)
{
    (void)mnt;
    (void)node;
    (void)name;
    return NULL;
}

/** No fixture directory is a process's root. */
int vfs_leaf_root(struct vfs_mount *mnt, void *node, const char *name)
{
    (void)mnt;
    (void)node;
    (void)name;
    return 0;
}

/** No fixture file is mapped. */
static void filemap_file_truncated(const struct vfs_mount *mnt, uint64_t ino, uint64_t size)
{
    (void)mnt;
    (void)ino;
    (void)size;
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
static int cred_fs_privileged(void)
{
    return 1;
}

/** No row changes the set-id bits of a file on the memory root. */
static void cred_kill_setid(krffs_node_t *node)
{
    (void)node;
    check(0, "unexpected memory-root set-id update");
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
static int32_t open_framebuffer(int flags)
{
    (void)flags;
    check(0, "unexpected framebuffer open");
    return -EIO;
}

/** Reject an unexpected mouse open. */
static int32_t open_mouse(int flags)
{
    (void)flags;
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

/** Nothing to release: no row takes a lock. */
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

/** No fixture path goes through /proc or /dev. */
static void path_expand_links(char *path)
{
    (void)path;
}

/** Reject an unexpected synthetic read-only check; no row opens /proc. */
static int proc_read_only(const struct vfs_ref *ref, int flags, int dirs)
{
    (void)ref;
    (void)flags;
    (void)dirs;
    check(0, "unexpected /proc read-only check");
    return 0;
}

/** Reject an unexpected synthetic mount check. */
static int mount_permanent_at(const struct vfs_mount *mnt)
{
    (void)mnt;
    check(0, "unexpected synthetic mount check");
    return 0;
}

/** Reject recording a synthetic mount; every row uses mounted KRFFS. */
static void synth_mount_record(int fd, const struct vfs_ref *ref, const struct vpath *vp, int flags)
{
    (void)fd;
    (void)ref;
    (void)vp;
    (void)flags;
    check(0, "unexpected synthetic mount record");
}

/** No row opens a file of debugfs. */
static int32_t debugfs_bind(void *node, fd_entry_t *e, int flags)
{
    (void)node;
    (void)e;
    (void)flags;
    check(0, "unexpected debugfs open");
    return -EINVAL;
}

/** No row creates in a sticky directory, whose rule the fixture does not
 * model. */
static int32_t may_create_in_sticky(const struct vfs_ref *ref, const struct vfs_stat *dir)
{
    (void)ref;
    check(!(dir->mode & 01000u), "unexpected sticky directory");
    return 0;
}

/** Start a description's cursors into the write-back errors at none; no
 * row syncs. */
void vfs_wb_err_sample(struct vfs_mount *mnt, uint32_t *file, uint32_t *fs)
{
    (void)mnt;
    *file = 0;
    *fs = 0;
}

/** Reject recording a synthetic directory; every row uses mounted KRFFS. */
static void synth_dir_record(int fd, int permanent, const char *name)
{
    (void)fd;
    (void)permanent;
    (void)name;
    check(0, "unexpected synthetic directory record");
}

#include "syscall_open.inc"

/** Run the real open path in the second process. */
static void open_published(void)
{
    opened_fd = do_open("target", opening_flags, 0644);
}

/**
 * The flush hook: start the open in the reader at the first flush where the
 * name still finds the original node (`before_publication`) or first finds
 * the copy, then remove the hook. The open runs on its own thread until it
 * waits for the namespace lock.
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
 * Relocate an open file while a second process opens it with `flags` at
 * the publication flush, or before it when `before` is set, and require
 * both opens to survive a second move and reuse of the first copy. A row
 * that never reaches the flush ends the test with status 2.
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
    mount.ns.holder = -1;
    hole = create("hole");
    fd = do_open("hole", 2, 0);
    check(fd >= 0, "open free-extent fixture");
    k_memset(payload, 'H', 4096);
    check(kv_write(&mount, fd_slot(writer.fds, fd)->ofd->fhandle, payload, 4096, 0) == 4096,
          "size free-extent fixture");
    fd_free(writer.fds, fd);
    create("separator");
    original = create("target");
    fd = do_open("target", 2, 0);
    check(fd >= 0, "open original target");
    first = fd_slot(writer.fds, fd)->ofd;
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
    second = fd_slot(reader.fds, opened_fd)->ofd;
    check(first->vnode == hole, "first relocation uses earlier free extent");
    check(handle_node(&mount, first->fhandle) == first->vnode,
          "a mounted KRFFS open record gives the node where the file now is");
    loop_table[0].mnt = &mount;
    loop_table[0].base = &mount;
    loop_table[0].node = first->vnode;
    check(vfs_truncate(&mount, first->fhandle, 0) == -EBUSY,
          "a truncation of a mounted KRFFS loop's backing file by its open record is EBUSY");
    k_memset(&loop_table[0], 0, sizeof(loop_table[0]));
    moved = first->vnode;
    k_memset(payload, 'C', sizeof(payload));
    check(kv_write(&mount, first->fhandle, payload, sizeof(payload), 0) == sizeof(payload),
          "relocate again after publication open");
    check(first->vnode != moved, "second relocation changes offset");
    reused = create("reuse-copy");
    check(reused == moved, "first copy offset reused");
    reuse = do_open("reuse-copy", 2, 0);
    check(reuse >= 0, "open reused copy offset");
    check(kv_write(&mount, fd_slot(writer.fds, reuse)->ofd->fhandle, "R", 1, 0) == 1,
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
    check(mount.refs == 0 && mount.inprogress == 0 && mount.ns.depth == 0,
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
    mount.ns.holder = -1;
    fd = do_open("created", 2 | O_CREAT | O_EXCL, 0644);
    check(fd >= 0 && mount.ns.depth == 0 && mount.inprogress == 0,
          "creating open registers and releases lock");
    if (fd >= 0) {
        fd_free(writer.fds, fd);
    }
    check(do_open("created", O_CREAT | O_EXCL, 0644) == -EEXIST && mount.ns.depth == 0,
          "exclusive open error releases lock");
    check(do_open("created", O_DIRECTORY, 0) == -ENOTDIR && mount.ns.depth == 0,
          "directory open error releases lock");
    check(do_open("absent", 0, 0) == -ENOENT && mount.ns.depth == 0, "missing path releases lock");
    check(do_open("created/child", 0, 0) == -ENOTDIR && mount.ns.depth == 0,
          "non-directory prefix releases lock");
    mount.flags |= VFS_MS_RDONLY;
    check(do_open("created", 2, 0) == -EROFS && mount.ns.depth == 0,
          "read-only open error releases lock");
    check(do_open("absent", O_CREAT, 0644) == -EROFS && mount.ns.depth == 0,
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
              ref.mnt == &child && child.ns.depth == 1 && mount.ns.depth == 0,
          "covered mount receives the resolution lock");
    vfs_ns_unlock(ref.mnt);
    vfs_ref_release(&ref);
    check(vfs_resolve(NULL, "covered/../created", VFS_WALK_LOCK, &ref, NULL, 0) == 0 &&
              ref.mnt == &mount && mount.ns.depth == 1 && child.ns.depth == 0,
          "parent climb transfers the resolution lock");
    vfs_ns_unlock(ref.mnt);
    vfs_ref_release(&ref);
    check(vfs_resolve(NULL, "covered/absent", VFS_WALK_LOCK, &ref, NULL, 0) == -ENOENT &&
              mount.ns.depth == 0 && child.ns.depth == 0 && child.inprogress == 0,
          "failed walk releases the child mount lock and reference");
    mount_slots = 1;
    teardown();
    check(mount.inprogress == 0 && mount.ns.depth == 0, "failed opens release all references");
}

/** Lay out the model: its root with a directory and its file, links to
 * that file relative, absolute and through "/", a slow link, a directory
 * another mount of the model covers, one another namespace's mount covers,
 * one under a mount the namespace word cannot judge, a file a file bind
 * covers with a link to it, the link that moves the root, and links that
 * climb a `..` from the root, from a covering mount's root and from a
 * nameless mount's root. That mount, entered as /data, holds Wine's prefix
 * with its link `c:` to `../drive_c` and a link `d:` read as a name
 * changes. A file `secret` is in the root, in /data and in two directories
 * that three mounts made later show: another file system's root over
 * /data's root, a bind of /src over it, and that file system's root over
 * the root's root, as `mount --bind /x /` makes one. */
static void cache_layout(void)
{
    int i;

    cnode_set(C_ROOT, -1, NULL, 0040755u, NULL, 0);
    cnode_set(C_BIN, C_ROOT, "bin", 0040755u, NULL, 0);
    cnode_set(C_TOOL, C_BIN, "tool", 0100644u, NULL, 0);
    cnode_set(C_REL, C_ROOT, "rel", 0120777u, "bin/tool", 1);
    cnode_set(C_ABS, C_ROOT, "abs", 0120777u, "/bin/tool", 1);
    cnode_set(C_TOP, C_ROOT, "top", 0120777u, "/", 1);
    cnode_set(C_SLOW, C_ROOT, "slow", 0120777u, "bin/tool", 0);
    cnode_set(C_COVER, C_ROOT, "cover", 0040755u, NULL, 0);
    cnode_set(C_UNDER, C_COVER, "inner", 0100644u, NULL, 0);
    cnode_set(C_UNSEEN, C_ROOT, "unseen", 0040755u, NULL, 0);
    cnode_set(C_HERE, C_UNSEEN, "here", 0100644u, NULL, 0);
    cnode_set(C_UNSURE, C_ROOT, "unsure", 0040755u, NULL, 0);
    cnode_set(C_TARGET, C_ROOT, "target", 0100644u, NULL, 0);
    cnode_set(C_ALIAS, C_ROOT, "alias", 0120777u, "/target", 1);
    cnode_set(C_MOVING, C_ROOT, "moving", 0120777u, "/bin/tool", 1);
    cnode_set(C_OROOT, -1, NULL, 0040755u, NULL, 0);
    cnode_set(C_INNER, C_OROOT, "inner", 0100644u, NULL, 0);
    cnode_set(C_SOURCE, -1, NULL, 0100644u, NULL, 0);
    cnode_set(C_BACK, C_ROOT, "back", 0120777u, "../bin/tool", 1);
    cnode_set(C_OUT, C_OROOT, "out", 0120777u, "../bin/tool", 1);
    cnode_set(C_DROOT, -1, NULL, 0040755u, NULL, 0);
    cnode_set(C_UP, C_DROOT, "up", 0120777u, "../bin/tool", 1);
    cnode_set(C_PFX, C_DROOT, "pfx", 0040755u, NULL, 0);
    cnode_set(C_DOS, C_PFX, "dosdevices", 0040755u, NULL, 0);
    cnode_set(C_CLINK, C_DOS, "c:", 0120777u, "../drive_c", 1);
    cnode_set(C_DLINK, C_DOS, "d:", 0120777u, "../drive_c", 1);
    cnode_set(C_DRIVE, C_PFX, "drive_c", 0040755u, NULL, 0);
    cnode_set(C_WIN, C_DRIVE, "win", 0100644u, NULL, 0);
    cnode_set(C_SECRET, C_ROOT, "secret", 0100644u, NULL, 0);
    cnode_set(C_DSECRET, C_DROOT, "secret", 0100644u, NULL, 0);
    cnode_set(C_VROOT, -1, NULL, 0040755u, NULL, 0);
    cnode_set(C_VSECRET, C_VROOT, "secret", 0100644u, NULL, 0);
    cnode_set(C_SRC, C_ROOT, "src", 0040755u, NULL, 0);
    cnode_set(C_SSECRET, C_SRC, "secret", 0100644u, NULL, 0);
    names_changed = 0;
    for (i = 0; i < C_MOUNTS; i++) {
        k_memset(cache_table[i], 0, sizeof(*cache_table[i]));
        cache_table[i]->type = &cache_type;
        cache_table[i]->state = VFS_ACTIVE;
        cache_table[i]->ns.holder = -1;
        cache_table[i]->parent = i ? &croot : NULL;
    }
    croot.root = &cn[C_ROOT];
    cother.root = &cn[C_OROOT];
    cother.covered = &cn[C_COVER];
    cunseen.root = &cn[C_OROOT];
    cunseen.covered = &cn[C_UNSEEN];
    cunsure.root = &cn[C_OROOT];
    cunsure.covered = &cn[C_UNSURE];
    cbind.root = &cn[C_SOURCE];
    cbind.covered = &cn[C_TARGET];
    k_memcpy(croot.covered_path, "/", 2);
    cnamed.root = &cn[C_DROOT];
    k_memcpy(cnamed.covered_path, "/data", 6);
    cdcover.parent = &cnamed;
    cdcover.root = &cn[C_VROOT];
    cdcover.covered = &cn[C_DROOT];
    cdcover.state = VFS_FREE;
    cdbind.parent = &cnamed;
    cdbind.root = &cn[C_SRC];
    cdbind.covered = &cn[C_DROOT];
    cdbind.state = VFS_FREE;
    crcover.root = &cn[C_VROOT];
    crcover.covered = &cn[C_ROOT];
    crcover.state = VFS_FREE;
}

/** Resolve `path` by the cached walk alone, with a name record when
 * `entry`: 0 with the walk's answer in `out` when it ends on `node` of
 * `mnt`, given back; VFS_CACHED_MISS or another answer otherwise. */
static int cached_at(const char *path, struct vfs_name **entry, struct vfs_mount *mnt, int node)
{
    struct vfs_ref out = {NULL, NULL};
    int rc = vfs_resolve_cached(path, 0, &out, entry);

    if (rc == 0) {
        rc = out.mnt == mnt && out.node == &cn[node] ? 0 : -1;
        vfs_ref_release(&out);
    }
    return rc;
}

/** Resolve `path` by the full walk alone, as `cached_at` says. */
static int full_at(const char *path, struct vfs_mount *mnt, int node)
{
    struct vfs_ref out = {NULL, NULL};
    int rc = resolve_full(NULL, path, 0, &out, NULL, 0, NULL);

    if (rc == 0) {
        rc = out.mnt == mnt && out.node == &cn[node] ? 0 : -1;
        vfs_ref_release(&out);
    }
    return rc;
}

/** Whether every hold the cached walk took and every mount it entered was
 * given back. */
static int cache_quiet(void)
{
    int i, held = 0;

    for (i = 0; i < C_NODES; i++) {
        held += cn[i].refs;
    }
    for (i = 0; i < C_MOUNTS; i++) {
        held += (int)cache_table[i]->inprogress;
    }
    return held == 0;
}

/**
 * The cached walk without the big lock, as Linux's walk under RCU
 * (fs/namei.c): it follows a link whose target the node holds, relative
 * from the link's directory and absolute from the root, as pick_link and
 * nd_jump_root (fs/namei.c:970-1000, 1877-1882), "/" among them, and
 * leaves any other link to the full walk (fs/namei.c:1860-1870); it
 * crosses a covering mount its namespace sees, as __follow_mount_rcu
 * (fs/namei.c:1521-1563), stays on the directory under one it does not,
 * and leaves to the full walk one it cannot judge. A file bind's crossing
 * drops the last name's record, which names the covered file. An absolute
 * link met after the root moved goes to the full walk. It enters a
 * nameless mount by its name from its parent's root, as the full walk's
 * mount_named does, and climbs a `..` to the directory it came down from,
 * as follow_dotdot_rcu climbs to d_parent (fs/namei.c:1968-2006): from a
 * covering mount's root to the directory the covered one is in, and from a
 * nameless mount's root to its parent's root; at the root it stays. A `..`
 * after a change of names goes to the full walk, as a d_seq that moved
 * does (fs/namei.c:1994). A mount made over a nameless mount's root, or
 * over the parent's root that a `..` from it reaches, hides the files under
 * it from both walks, whatever the cache held before, as handle_mounts
 * crosses mounts stacked on a mount's root (fs/namei.c:1544) and
 * handle_dots steps into the parent through it (fs/namei.c:2040-2056).
 * Each row gives back every hold and mount it took.
 */
static void cached_walks(void)
{
    struct vfs_name *entry = NULL;
    struct vfs_mount **was = mounts;
    int was_slots = mount_slots;

    current_proc = &writer;
    writer.fs = &dirs;
    dirs.root_mnt = NULL;
    dirs.root_node = NULL;
    cache_layout();
    mounts = cache_table;
    mount_slots = C_MOUNTS;
    check(cached_at("/bin/tool", NULL, &croot, C_TOOL) == 0, "the cached walk finds a file");
    check(cached_at("/rel", NULL, &croot, C_TOOL) == 0,
          "and follows a fast link's relative target from the link's directory");
    check(cached_at("/abs", NULL, &croot, C_TOOL) == 0, "and an absolute target from the root");
    check(cached_at("/top/bin/tool", NULL, &croot, C_TOOL) == 0,
          "and a link to \"/\", whose spliced path begins with two slashes");
    check(cached_at("/slow", NULL, &croot, C_TOOL) == VFS_CACHED_MISS,
          "and leaves a link whose target the node does not hold to the full walk");
    check(cached_at("/cover/inner", NULL, &cother, C_INNER) == 0,
          "it crosses a covering mount onto that mount's root");
    check(cached_at("/unseen/here", NULL, &croot, C_HERE) == 0,
          "and stays under a mount that another namespace sees");
    check(cached_at("/unsure/here", NULL, &croot, C_HERE) == VFS_CACHED_MISS,
          "and leaves a mount whose namespace it cannot judge to the full walk");
    check(cached_at("/bin/tool", &entry, &croot, C_TOOL) == 0 && entry != NULL &&
              names_taken == 1 && names_put == 0,
          "it records a regular file's last name");
    fixture_name_put(entry);
    entry = NULL;
    check(cached_at("/alias", &entry, &cbind, C_SOURCE) == 0 && entry == NULL && names_taken == 2 &&
              names_put == 2,
          "and drops that record when a file bind's crossing moves it onto the bind");
    check(cached_at("/target", &entry, &cbind, C_SOURCE) == 0 && entry == NULL &&
              names_taken == 3 && names_put == 3,
          "whether a link or the name itself reaches the bound file");
    check(cached_at("/moving", NULL, &croot, C_TOOL) == VFS_CACHED_MISS,
          "an absolute link met after the root moved goes to the full walk");
    check(cached_at("/abs", NULL, &croot, C_TOOL) == 0,
          "and a walk that begins after the move takes the root as it now is");
    check(cached_at("/bin/../bin/tool", NULL, &croot, C_TOOL) == 0,
          "a `..` climbs to the directory the walk came down from");
    check(cached_at("/back", NULL, &croot, C_TOOL) == 0, "and stays at the root");
    check(cached_at("/cover/out", NULL, &croot, C_TOOL) == 0,
          "and climbs from a covering mount's root to the covered directory's parent");
    check(cached_at("/data/pfx/drive_c/win", NULL, &cnamed, C_WIN) == 0,
          "the walk enters a nameless mount by its name");
    check(cached_at("/data/up", NULL, &croot, C_TOOL) == 0,
          "and climbs from its root to its parent's root");
    check(cached_at("/data/pfx/dosdevices/c:/win", NULL, &cnamed, C_WIN) == 0,
          "a relative link's `..` climbs inside the nameless mount");
    check(cached_at("/data/pfx/dosdevices/d:/win", NULL, &cnamed, C_WIN) == VFS_CACHED_MISS,
          "a `..` after a change of names goes to the full walk");
    check(cached_at("/data/pfx/dosdevices/c:/win", NULL, &cnamed, C_WIN) == 0,
          "and a walk that begins after the change climbs it");
    check(cached_at("/data/secret", NULL, &cnamed, C_DSECRET) == 0 &&
              full_at("/data/secret", &cnamed, C_DSECRET) == 0,
          "a nameless mount's file is found while nothing is over its root");
    cdcover.state = VFS_ACTIVE;
    check(cached_at("/data/secret", NULL, &cdcover, C_VSECRET) == 0 &&
              full_at("/data/secret", &cdcover, C_VSECRET) == 0,
          "a mount over a nameless mount's root shows its own file in both walks, never the "
          "hidden one");
    cdcover.state = VFS_FREE;
    cdbind.state = VFS_ACTIVE;
    check(cached_at("/data/secret", NULL, &cdbind, C_SSECRET) == 0 &&
              full_at("/data/secret", &cdbind, C_SSECRET) == 0,
          "and so does a bind over that warm path");
    cdbind.state = VFS_FREE;
    check(cached_at("/data/../secret", NULL, &croot, C_SECRET) == 0 &&
              full_at("/data/../secret", &croot, C_SECRET) == 0,
          "a `..` from a nameless mount's root reaches the parent's root");
    crcover.state = VFS_ACTIVE;
    check(cached_at("/data/../secret", NULL, &crcover, C_VSECRET) == 0 &&
              full_at("/data/../secret", &crcover, C_VSECRET) == 0,
          "and crosses a mount over that root in both walks, as Linux's handle_dots does");
    crcover.state = VFS_FREE;
    check(cache_quiet(), "the cached walks give back every hold and mount");
    mounts = was;
    mount_slots = was_slots;
}

/** Run the opens at relocation publication, then the locked walks once
 * those pass: 0 when every row passes, 1 otherwise. */
int main(void)
{
    loops = loop_table;
    publication_open(0, 0);
    publication_open(O_CREAT, 0);
    publication_open(0, 1);
    if (!failures) {
        locked_walks();
    }
    if (!failures) {
        cached_walks();
    }
    if (!failures) {
        ext2_handles();
    }
    if (!failures) {
        printf("krffs-open: result: pass: publication open, offset reuse, cached walks and "
               "ext2 handles\n");
    }
    return failures != 0;
}
