/*
 * The VFS of vfs.h: the mount table, the resolver, the wrappers, mount(2),
 * binds and umount2(2), and the operations of the KRFFS root and of the
 * three synthetic types, /dev, /proc and /sys.
 */

#include "../mm/kheap.h"
#include "vfs.h"
#include "name.h"

#include "../arch/arch.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/pagecache.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/ns.h"
#include "../proc/smp.h"
#include "../lib/spinlock.h"
#include "../tty/pty.h"
#include "bcache.h"
#include "dcache.h"
#include "devfs.h"
#include "devtree.h"
#include "fat32.h"
#include "exfat.h"
#include "ext2.h"
#include "krffs.h"
#include "../block/loop.h"
#include "krffsmount.h"
#include "lock.h"
#include "pathbuf.h"
#include "procfs.h"
#include "synthfs.h"
#include "tmpfs.h"
#include "../ipc/mqueue.h"
#include "debugfs.h"
#include "../pstore/pstore.h"
#include "../mm/filecache.h"
#include "../io/inotify.h"

#define VFS_S_IFMT       0170000u
#define VFS_S_IFDIR      0040000u
#define VFS_S_IFREG      0100000u
#define VFS_IS_DIR(mode) (((mode) & VFS_S_IFMT) == VFS_S_IFDIR)

/*
 * The mounts, by number: an array of pointers that grows, each record
 * allocated from the heap when the mount is made and freed when it is
 * unmounted; a NULL slot is free. The first VFS_PERMANENT entries are made
 * at boot and last the life of the kernel.
 *
 * The table changes under the big lock. `mount_table_lock` guards the
 * array, its length and what each slot holds, so that a walk without the
 * big lock may read the table under it (`resolve_cached`): an old array and
 * a record that left its slot are freed after the lock is let go of. A
 * record's fields change under the big lock alone, and such a walk reads
 * them as single words, seeing a mount being made or unmade either way.
 */
static struct vfs_mount **mounts;
static int mount_slots;
static struct spinlock mount_table_lock = SPINLOCK_INIT;
/* The `seq` of the last mount made; the permanent entries have their own
 * indexes. */
static uint32_t mount_seq = VFS_PERMANENT;

int vfs_mount_slots(void)
{
    return mount_slots;
}

/** Make the array of eight slots, once: both root installers of the boot,
 * `vfs_init` and `vfs_init_root`, need it, and either can run first. Panics
 * when the heap has no room. */
static void mount_table_init(void)
{
    if (mounts) {
        return;
    }
    mount_slots = 8;
    mounts = kzalloc(sizeof(*mounts) * mount_slots);
    if (!mounts) {
        k_panic("vfs: no memory for the mount table");
    }
}

/** The record in slot `idx`, allocated if the slot is empty; NULL when the
 * heap has no room. The heap may sleep, so a record another caller put in
 * the slot meanwhile is taken instead. */
static struct vfs_mount *mount_record(int idx)
{
    struct vfs_mount *m;
    uint32_t flags;

    if (!mounts[idx]) {
        m = kzalloc(sizeof(struct vfs_mount));
        if (!m) {
            return NULL;
        }
        flags = spin_lock_irqsave(&mount_table_lock);
        if (!mounts[idx]) {
            mounts[idx] = m;
            m = NULL;
        }
        spin_unlock_irqrestore(&mount_table_lock, flags);
        kfree(m);
    }
    return mounts[idx];
}

/** The first free slot past the permanent ones, the array grown by doubling
 * when every slot is taken, with a record in it; -1 when the heap has no
 * room. The new array is made before the lock is taken, since the heap may
 * sleep, and the search starts over when the table grew meanwhile. */
static int mount_new_slot(void)
{
    int i;

    for (;;) {
        int had = mount_slots;
        struct vfs_mount **grown;
        struct vfs_mount **old = NULL;
        uint32_t flags;

        for (i = VFS_PERMANENT; i < mount_slots; i++) {
            if (!mounts[i]) {
                return mount_record(i) ? i : -1;
            }
        }
        grown = kzalloc(sizeof(*grown) * had * 2);
        if (!grown) {
            return -1;
        }
        flags = spin_lock_irqsave(&mount_table_lock);
        if (mount_slots == had) {
            k_memcpy(grown, mounts, sizeof(*grown) * had);
            old = mounts;
            mounts = grown;
            mount_slots = had * 2;
        }
        spin_unlock_irqrestore(&mount_table_lock, flags);
        if (old) {
            kfree(old);
            return mount_record(had) ? had : -1;
        }
        kfree(grown);
    }
}

/** Free the record `m` of a mount placed by mount(2), and empty its slot; a
 * record outside those slots is marked VFS_FREE instead. */
static void mount_release(struct vfs_mount *m)
{
    uint32_t flags;
    int i;

    vfs_mount_view_free(m);
    for (i = VFS_PERMANENT; i < mount_slots; i++) {
        if (mounts[i] == m) {
            flags = spin_lock_irqsave(&mount_table_lock);
            mounts[i] = NULL;
            spin_unlock_irqrestore(&mount_table_lock, flags);
            kfree(m);
            return;
        }
    }
    m->state = VFS_FREE;
}

struct vfs_mount *vfs_mount_at(int idx)
{
    if (idx < 0 || idx >= mount_slots || !mounts[idx] || mounts[idx]->state == VFS_FREE) {
        return 0;
    }
    return mounts[idx];
}

int vfs_mount_index(const struct vfs_mount *m)
{
    uint32_t flags = spin_lock_irqsave(&mount_table_lock);
    int i, at = -1;

    for (i = 0; m && i < mount_slots && at < 0; i++) {
        if (mounts[i] == m && m->state != VFS_FREE) {
            at = i;
        }
    }
    spin_unlock_irqrestore(&mount_table_lock, flags);
    return at;
}

/* Read under `mount_table_lock`, since a caller without the big lock, such
 * as the unlocked close, may read while a mount grows the array and frees
 * the old one, as Linux's `lookup_mnt` reads the mount hash under the
 * `mount_lock` seqlock (fs/namespace.c:835-849). The record itself is
 * permanent. */
struct vfs_mount *vfs_root_mount(void)
{
    uint32_t flags = spin_lock_irqsave(&mount_table_lock);
    struct vfs_mount *root = mounts[0];

    spin_unlock_irqrestore(&mount_table_lock, flags);
    return root;
}

int vfs_mount_gone(const struct vfs_mount *mnt)
{
    return mnt->dev && ((mnt->dev->flags | block_flags(mnt->dev)) & BLOCK_GONE);
}

/** The mount whose file system `m` shows, as `vfs_mount_base` gives it, for
 * the callers that change it. */
static struct vfs_mount *mount_base(struct vfs_mount *m)
{
    return m->bind_of ? m->bind_of : m;
}

const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m->bind_of ? m->bind_of : m;
}

void vfs_mount_watches(const struct vfs_mount *m, int delta)
{
    int i;

    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] == m) {
            mounts[i]->inotify_watched += delta;
            return;
        }
    }
}

/** Whether a write through `mnt` is refused for a read-only mount: its own
 * flags, or for a bind mount those of the file system it shows. */
static int mount_rdonly(struct vfs_mount *mnt)
{
    return ((mnt->flags | mount_base(mnt)->flags) & VFS_MS_RDONLY) != 0;
}

/* Reference and transient counts. */

void vfs_ref_hold(const struct vfs_ref *r)
{
    if (r->mnt->type->ops->hold) {
        r->mnt->type->ops->hold(mount_base(r->mnt), r->node);
    }
}

void vfs_ref_put(struct vfs_ref *r)
{
    if (r->mnt && r->mnt->type->ops->put) {
        r->mnt->type->ops->put(mount_base(r->mnt), r->node);
    }
    r->mnt = 0;
    r->node = 0;
}

/* The tree's sequence (`vfs_tree_seq`). */
static uint32_t tree_seq;

uint32_t vfs_tree_seq(void)
{
    return tree_seq;
}

void vfs_tree_changed(void)
{
    tree_seq++;
}

void vfs_replay(int magic, uint32_t seq, struct vfs_mount *xdev)
{
    current_proc->walk_replay = VFS_REPLAY_ON | (magic ? VFS_REPLAY_MAGIC : 0u);
    current_proc->walk_replay_seq = seq;
    current_proc->walk_replay_mnt = xdev;
}

int vfs_replay_end(void)
{
    int tripped = (current_proc->walk_replay & VFS_REPLAY_TRIPPED) != 0;

    current_proc->walk_replay = 0;
    current_proc->walk_replay_mnt = NULL;
    return tripped;
}

/**
 * Add `delta` to the mount count at `count` in one atomic step and return
 * the new value. The counts of a mount change so, since a call on a
 * descriptor whose operations run without the big lock (`file_ops_lock`)
 * may take or give one beside a caller that holds it.
 */
static uint32_t mount_count_add(uint32_t *count, int delta)
{
    volatile unsigned int *p = (volatile unsigned int *)count;
    unsigned int old;

    do {
        old = *p;
    } while (arch_atomic_cmpxchg(p, old, old + (unsigned int)delta) != old);
    return old + (unsigned int)delta;
}

void vfs_mount_get(struct vfs_mount *mnt)
{
    mount_count_add(&mnt->refs, 1);
}

uint32_t vfs_mount_put(struct vfs_mount *mnt)
{
    if (mnt->refs == 0) {
        k_panic("vfs: a mount's last reference was put twice");
    }
    return mount_count_add(&mnt->refs, -1);
}

/**
 * The count goes up before the state is read, and the unmount stores its
 * state with a full barrier before it reads the count (`unmount_mount`), so
 * either this sees the unmount and leaves, or the unmount sees the count
 * and waits, as Linux's `__legitimize_mnt` pairs with `mnt_make_shortterm`.
 */
int vfs_enter(struct vfs_mount *mnt)
{
    mount_count_add(&mnt->inprogress, 1);
    if (*(volatile int *)&mnt->state != VFS_ACTIVE) {
        vfs_leave(mnt);
        return -EBUSY;
    }
    return 0;
}

/** The unmount waits for the count to reach zero on `drain_wq`. A leave
 * without an enter panics. */
void vfs_leave(struct vfs_mount *mnt)
{
    if (mnt->inprogress == 0) {
        k_panic("vfs: an operation left a mount it never entered");
    }
    if (mount_count_add(&mnt->inprogress, -1) == 0 &&
        *(volatile int *)&mnt->state == VFS_UNMOUNTING) {
        wq_wake_all(&mnt->drain_wq);
    }
}

/* The namespace lock. */

/* The record of every mount's namespace lock (sleeplock.h). */
static struct sleep_lock_record mount_lock_record = SLEEP_LOCK_RECORD("mount");

/** Make the namespace and rename locks of `m` free ones, and its drain
 * queue empty. */
static void mount_lock_init(struct vfs_mount *m)
{
    sleep_lock_init(&m->ns, &mount_lock_record, 0);
    vfs_rename_lock_init(m);
    wait_queue_init(&m->drain_wq);
}

int vfs_ns_held_by_caller(void)
{
    int i;

    if (current_proc && current_proc->file_locks > 0) {
        return 1;
    }
    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && sleep_lock_held(&mounts[i]->ns)) {
            return 1;
        }
    }
    return 0;
}

int vfs_ns_held_other(const struct vfs_mount *mnt)
{
    const struct vfs_mount *base = vfs_mount_base(mnt);
    int i;

    if (current_proc && current_proc->file_locks > 0) {
        return 1;
    }
    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i] != base &&
            sleep_lock_held(&mounts[i]->ns)) {
            return 1;
        }
    }
    return 0;
}

/** A bind's lock is its base's (`mount_base`); the lock itself is
 * sleeplock.h's. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    sleep_lock_take(&mount_base(mnt)->ns);
}

void vfs_ns_unlock(struct vfs_mount *mnt)
{
    sleep_lock_give(&mount_base(mnt)->ns);
}

/* The permission the resolver checks. */

/** Whether `gid` is one of the calling process's supplementary groups. */
static int in_groups(uint32_t gid)
{
    uint32_t i;

    for (i = 0; i < current_proc->ngroups; i++) {
        if (current_proc->groups[i] == gid) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether the caller may search the directory `node` of `mnt`: 0, -EACCES,
 * -ENOTDIR for a node that is not a directory, or the error of `getattr`.
 *
 * The rule is the nine-bit rule over the attributes the type reports: one
 * identity class is chosen and judged alone, and a uid of 0 passes unless
 * the thread gave up its capabilities (`nocaps`). The ids are the
 * file-system ones (`proc_fsuid`, `proc_fsgid`); `use_real` judges by the
 * real ids, for access(2). A node of a type without `getattr` is
 * searchable, and so is every directory to the kernel when no process
 * runs.
 */
static int may_search(struct vfs_mount *mnt, void *node, int use_real)
{
    struct vfs_stat st;
    uint32_t uid, bits;
    int rc;

    if (!mnt->type->ops->getattr) {
        return 0;
    }
    rc = mnt->type->ops->getattr(mount_base(mnt), node, &st);
    if (rc != 0) {
        return rc;
    }
    if (!VFS_IS_DIR(st.mode)) {
        return -ENOTDIR;
    }
    if (!current_proc) {
        return 0;
    }
    uid = use_real ? current_proc->ruid : proc_fsuid(current_proc);
    if (uid == 0 && !current_proc->nocaps) {
        return 0;
    }
    if (uid == st.uid) {
        bits = st.mode >> 6;
    } else if ((use_real ? current_proc->rgid : proc_fsgid(current_proc)) == st.gid ||
               in_groups(st.gid)) {
        bits = st.mode >> 3;
    } else {
        bits = st.mode;
    }
    return (bits & 1u) ? 0 : -EACCES;
}

/* The protections of /proc/sys/fs, at the values Debian and Ubuntu set at
 * boot; Linux's own are 0 (fs/namei.c:1042-1045). */
int vfs_protected_hardlinks = 1;
int vfs_protected_symlinks = 1;
int vfs_protected_fifos = 1;
int vfs_protected_regular = 2;

/**
 * Whether the walk may follow the symbolic link of attributes `lst`, the
 * last name of a path, found in the directory `dir` of `mnt`, as Linux's
 * may_follow_link (fs/namei.c:1112-1140): 0, or -EACCES under
 * fs.protected_symlinks for a link in a sticky directory that others may
 * write, owned by neither the follower nor the directory's owner. Root is
 * refused too, since the rule guards it against a link another user plants
 * in /tmp. The follower is the file-system uid, or the real uid with
 * `use_real`, for access(2).
 */
static int may_follow_link(struct vfs_mount *mnt, void *dir, const struct vfs_stat *lst,
                           int use_real)
{
    struct vfs_stat dst;
    uint32_t uid;
    int rc;

    if (!vfs_protected_symlinks || !current_proc) {
        return 0;
    }
    uid = use_real ? current_proc->ruid : proc_fsuid(current_proc);
    if (lst->uid == uid) {
        return 0;
    }
    rc = mnt->type->ops->getattr(mount_base(mnt), dir, &dst);
    if (rc != 0) {
        return rc;
    }
    if ((dst.mode & 01002u) != 01002u || dst.uid == lst->uid) {
        return 0;
    }
    return -EACCES;
}

/* The resolver. */

/**
 * Whether the component of `path` at `p`, with the part before it, is the
 * caller's working directory or one of the directories on the way to it,
 * for a walk of `path` from the caller's root: a path with its leading
 * slash, or without it and no `start`, as the open calls give one.
 *
 * The syscall layer joins a relative name to the working directory's path
 * and the walk starts at the root, where Linux starts a relative walk at the
 * working directory and checks no permission on the directories above it. A
 * process that keeps a directory it may not reach by its path, as `su` to an
 * ordinary user from root's home leaves one, may still name files there
 * relatively, so the walk checks no search permission on a component on
 * that way; any other component of such a directory still needs it.
 */
static int toward_cwd(const struct vfs_ref *start, const char *path, const char *p)
{
    const char *cwd = current_proc ? proc_cwd(current_proc) : 0;
    uint32_t at, len = 0, i;

    if (!cwd || cwd[0] != '/' || (start && path[0] != '/') || (path[0] == '/' && path[1] == '/')) {
        return 0;
    }
    while (*path == '/') {
        path++;
    }
    while (*cwd == '/') {
        cwd++;
    }
    at = (uint32_t)(p - path);
    if (p < path || at > k_strlen(path)) {
        return 0;
    }
    while (p[len] != '\0' && p[len] != '/') {
        len++;
    }
    for (i = 0; i < at + len; i++) {
        if (cwd[i] != path[i]) {
            return 0;
        }
    }
    return cwd[i] == '/' || cwd[i] == '\0';
}

/**
 * The search check of the full walk at `cur` for the component of `path` at
 * `p`: 0, or may_search's error. A component on the way to the working
 * directory passes (`toward_cwd`) unless `cwd_ok` is 0; that is asked only
 * after the check failed, since it scans the path and the working
 * directory's path.
 */
static int walk_may_search(const struct vfs_ref *cur, int use_real, int cwd_ok,
                           const struct vfs_ref *start, const char *path, const char *p)
{
    int rc = may_search(cur->mnt, cur->node, use_real);

    return rc != 0 && cwd_ok && toward_cwd(start, path, p) ? 0 : rc;
}

/**
 * The mount whose root replaces `node` when the walk arrives at it: a child
 * of `mnt` covering that node that the caller's mount namespace sees. A
 * mount being made or unmounted is returned too, and the walk's
 * `vfs_enter` answers EBUSY for it. A nameless mount covers no node; it is
 * entered by name instead (`mount_named`).
 */
static struct vfs_mount *mount_covering(struct vfs_mount *mnt, void *node)
{
    int i;

    for (i = 1; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->parent == mnt &&
            mounts[i]->covered && mounts[i]->covered == node && vfs_mount_seen(mounts[i])) {
            return mounts[i];
        }
    }
    return 0;
}

/** Whether the strings `a` and `b` are equal. */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether `path` is below the directory whose path is `dir[0..len)`. */
static int path_under(const char *path, const char *dir, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (path[i] != dir[i]) {
            return 0;
        }
    }
    return path[len] == '/';
}

/** The last component of a mount's recorded path: for a nameless mount, the
 * name it is entered by from its parent's root directory. */
static const char *path_last(const char *path)
{
    const char *last = path;

    for (; *path; path++) {
        if (*path == '/') {
            last = path + 1;
        }
    }
    return last;
}

static uint32_t path_below(const struct vfs_mount *m);

/**
 * The name a nameless mount is entered by from its parent's root directory:
 * its recorded path below its parent's, such as "shm" for /dev/shm, or
 * "kernel/debug", of more than one name, for /sys/kernel/debug inside /sys;
 * the last component of its path when that is not below its parent's.
 */
static const char *mount_entry_name(const struct vfs_mount *m)
{
    uint32_t pl = path_below(m->parent);

    if ((pl == 0 || path_under(m->covered_path, m->parent->covered_path, pl)) &&
        m->covered_path[pl] == '/' && m->covered_path[pl + 1] != '\0') {
        return m->covered_path + pl + 1;
    }
    return path_last(m->covered_path);
}

/** Whether a nameless mount's entry name has more than one name in it. */
static int mount_deep(const struct vfs_mount *m)
{
    const char *c;

    for (c = mount_entry_name(m); *c; c++) {
        if (*c == '/') {
            return 1;
        }
    }
    return 0;
}

/** The active nameless mount, of those the caller's mount namespace sees,
 * entered by the name comp[0..len) from the root directory of `mnt`, as
 * /dev, /proc and /sys are from the root's and /dev/shm and /dev/mqueue
 * from /dev's, or by the names comp[0..len) when `len` spans several, as
 * /sys/kernel/debug is from /sys's; or NULL. */
static struct vfs_mount *mount_named_at(struct vfs_mount *mnt, const char *comp, uint32_t len)
{
    int i;

    for (i = 1; i < mount_slots; i++) {
        struct vfs_mount *m = mounts[i];
        const char *name;
        uint32_t n;

        if (!m || m->state != VFS_ACTIVE || m->parent != mnt || m->covered || !vfs_mount_seen(m)) {
            continue;
        }
        name = mount_entry_name(m);
        for (n = 0; n < len && name[n] == comp[n]; n++) {
        }
        if (n == len && name[n] == '\0') {
            return m;
        }
    }
    return 0;
}

/** The nameless mount entered by `name` from `node` of `mnt` when `node` is
 * the mount's root directory, as `mount_named_at` finds it; or NULL. */
static struct vfs_mount *mount_named(struct vfs_mount *mnt, void *node, const char *name)
{
    return node == mnt->root ? mount_named_at(mnt, name, (uint32_t)k_strlen(name)) : 0;
}

int vfs_named_mount(const struct vfs_mount *parent, int n, const char **name, uint64_t *ino)
{
    int i;

    for (i = 1; i < mount_slots; i++) {
        struct vfs_mount *m = mounts[i];
        struct vfs_stat st;

        if (!m || m->state != VFS_ACTIVE || m->parent != parent || m->covered ||
            !vfs_mount_seen(m) || mount_deep(m) || n-- > 0) {
            continue;
        }
        *name = mount_entry_name(m);
        *ino = m->type->ops->getattr && m->type->ops->getattr(m, m->root, &st) == 0 ? st.ino : 0;
        return 1;
    }
    return 0;
}

struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node, const char *name)
{
    void *found;
    struct vfs_mount *m = mount_named(mnt, node, name);
    int i;

    if (m) {
        return m;
    }
    if (!mnt->type->ops->lookup ||
        mnt->type->ops->lookup(mount_base(mnt), node, name, &found) != 0) {
        return 0;
    }
    for (i = 1; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->parent == mnt &&
            mounts[i]->covered == found && vfs_mount_seen(mounts[i])) {
            m = mounts[i];
        }
    }
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mount_base(mnt), found);
    }
    return m;
}

/** Put a node a lookup returned, on a type that counts holds. */
static void node_put(struct vfs_mount *mnt, void *node)
{
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mount_base(mnt), node);
    }
}

/** Whether a bind shows the file system of `mnt` from `node`. */
static int bind_rooted_at(struct vfs_mount *mnt, const void *node)
{
    int i;

    for (i = 0; i < mount_slots; i++) {
        struct vfs_mount *m = mounts[i];

        if (m && m->state != VFS_FREE && m->bind_of == mount_base(mnt) && m->root == node) {
            return 1;
        }
    }
    return 0;
}

int vfs_leaf_root(struct vfs_mount *mnt, void *node, const char *name)
{
    void *found;
    process_t *pr;
    int is = 0;

    if (!mnt->type->ops->lookup ||
        mnt->type->ops->lookup(mount_base(mnt), node, name, &found) != 0) {
        return 0;
    }
    for (pr = proc_list; pr && !is; pr = pr->next) {
        is = pr->fs && pr->fs->root_mnt == mnt && pr->fs->root_node == found;
    }
    is = is || bind_rooted_at(mnt, found);
    node_put(mnt, found);
    return is;
}

/**
 * The recorded path of the root mount, "/", goes in front of nothing, since
 * the type's path is already the whole. Through a bind, the bind's root is
 * taken off the front of the type's path. The path of a mount's root is the
 * path of the directory it covers, with no slash after it.
 */
int vfs_node_path(struct vfs_mount *mnt, void *node, char *out, uint32_t cap)
{
    uint32_t ml = (uint32_t)k_strlen(mnt->covered_path);
    int root_krffs = mount_base(mnt) == mounts[0] && mnt->type == &vfs_krffs_type;
    int r;

    if (mnt == mounts[0] && root_krffs) {
        return krffs_node_path((krffs_node_t *)node, out, cap) == 0 ? 0 : -ENAMETOOLONG;
    }
    if ((!root_krffs && !mnt->type->ops->path) || mnt == vfs_memfd_mount()) {
        return -ENOENT;
    }
    if (node == mnt->root && mnt != mounts[0]) {
        /* A mount's root, a file's among them, is its place. */
        if (ml + 1 > cap) {
            return -ENAMETOOLONG;
        }
        k_memcpy(out, mnt->covered_path, ml + 1);
        return 0;
    }
    if (ml <= 1) {
        ml = 0;
    }
    if (ml + 2 > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(out, mnt->covered_path, ml);
    r = root_krffs
            ? (krffs_node_path((krffs_node_t *)node, out + ml, cap - ml) == 0 ? 0 : -ENAMETOOLONG)
            : mnt->type->ops->path(mount_base(mnt), node, out + ml, cap - ml);
    if (r != 0) {
        return r;
    }
    if (mnt->bind_of && mnt->bind_root[1] != '\0') {
        uint32_t bl = (uint32_t)k_strlen(mnt->bind_root);

        if (!path_under(out + ml, mnt->bind_root, bl) && !streq(out + ml, mnt->bind_root)) {
            return -ENOENT;
        }
        k_memmove(out + ml, out + ml + bl, k_strlen(out + ml + bl) + 1);
        if (out[ml] == '\0') {
            out[ml] = '/';
            out[ml + 1] = '\0';
        }
    }
    if (ml > 0 && out[ml + 1] == '\0') {
        out[ml] = '\0';
    }
    return 0;
}

/**
 * Take the namespace lock of `mnt` for a locked walk, VFS_WALK_LOCK, on a
 * type that sets `ns_serialised` only. KRFFS moves a file's bytes while they
 * are open and ext2 keeps a batch of dirty blocks between calls, so an open
 * of either must register its description under the lock or it can land on
 * an offset another file has taken. On a type without the bit the lock would
 * serialise lookups that are meant to run together: two processes opening
 * one cold file on a FAT volume must both reach the buffer cache, which is
 * what `tests/run mount` counts.
 */
static void walk_lock(struct vfs_mount *mnt, int locked)
{
    if (locked && mnt->type->ns_serialised) {
        vfs_ns_lock(mnt);
    }
}

/** The release that matches `walk_lock`. */
static void walk_unlock(struct vfs_mount *mnt, int locked)
{
    if (locked && mnt->type->ns_serialised) {
        vfs_ns_unlock(mnt);
    }
}

void vfs_walk_unlock(struct vfs_mount *mnt)
{
    walk_unlock(mnt, 1);
}

/**
 * Move the walk at `cur` to `node` of `mnt`, a covered directory or a
 * mount's root: hold the node, put the old one, and when the mount changes,
 * enter the new mount before the old one is left. The put, which may sleep,
 * comes before the old mount is left, and a locked walk releases the old
 * mount's lock before it waits for the new one. 0, or -EBUSY for a mount
 * that is not active, with `cur` as it was.
 */
static int walk_move(struct vfs_ref *cur, struct vfs_mount *mnt, void *node, int locked)
{
    struct vfs_ref next;

    next.mnt = mnt;
    next.node = node;
    if (mnt != cur->mnt) {
        if (vfs_enter(mnt) != 0) {
            return -EBUSY;
        }
    }
    vfs_ref_hold(&next);
    {
        struct vfs_mount *was = cur->mnt;

        vfs_ref_put(cur);
        if (mnt != was) {
            walk_unlock(was, locked);
            vfs_leave(was);
            walk_lock(mnt, locked);
        }
    }
    *cur = next;
    return 0;
}

/**
 * Move the walk at `cur` onto the root of the mount covering its node, and
 * on through each mount made over that mount's root in turn, to the last,
 * which is the one a path through this place reaches. 0, or the error of
 * `walk_move`.
 */
static int walk_cross(struct vfs_ref *cur, int locked)
{
    struct vfs_mount *m;
    int rc = 0;

    while (rc == 0 && (m = mount_covering(cur->mnt, cur->node)) != 0) {
        rc = walk_move(cur, m, m->root, locked);
    }
    return rc;
}

/** Where an absolute path starts, into `r`: the calling process's root,
 * which chroot set, or the root of the root mount. */
static void walk_root(struct vfs_ref *r)
{
    if (current_proc && current_proc->fs->root_mnt) {
        r->mnt = current_proc->fs->root_mnt;
        r->node = current_proc->fs->root_node;
    } else {
        r->mnt = mounts[0];
        r->node = mounts[0]->root;
    }
}

/**
 * Whether `root`, the root a walk started from, is still the calling
 * process's root. A thread that shares the root (CLONE_FS) may chroot while
 * the walk sleeps in a lookup, and the walk then starts over from the new
 * root rather than judge one path by two roots. While the answer is yes, the
 * process's root holds the node, so the walk may move to it.
 */
static int root_current(const struct vfs_ref *root)
{
    struct vfs_ref now;

    walk_root(&now);
    return now.mnt == root->mnt && now.node == root->node;
}

/** Whether `..` at `cur` stays there: at the walk's root, and at the root of
 * the root mount, above which there is nothing, whatever root the walk has. */
static int dotdot_stays(const struct vfs_ref *cur, const struct vfs_ref *root)
{
    return (cur->mnt == root->mnt && cur->node == root->node) ||
           (cur->mnt == mounts[0] && cur->node == mounts[0]->root);
}

/** Replace the node `cur` holds with `node`, held already, on the same mount:
 * the put clears the reference, so the mount is kept across it. */
static void ref_move_node(struct vfs_ref *cur, void *node)
{
    struct vfs_mount *stay = cur->mnt;

    vfs_ref_put(cur);
    cur->mnt = stay;
    cur->node = node;
}

/**
 * Whether the path at `p` names a nameless mount first, when the walk is at
 * the root directory of a type that takes the rest of a path in one call:
 * such a mount, /dev/shm in /dev, is entered by the ordinary step of one
 * name, before that type sees the rest.
 */
static int named_in_rest(const struct vfs_ref *cur, const char *p)
{
    uint32_t len = 0;

    while (p[len] != '\0' && p[len] != '/') {
        len++;
    }
    return cur->node == cur->mnt->root && mount_named_at(cur->mnt, p, len) != 0;
}

/** Whether the path at `p` begins with `..` at the root directory of a type
 * that takes the rest of a path in one call: the ordinary step takes that
 * `..`, as at the root of any other mount. */
static int climbs(const struct vfs_ref *cur, const char *p)
{
    return cur->node == cur->mnt->root && p[0] == '.' && p[1] == '.' &&
           (p[2] == '\0' || p[2] == '/');
}

/** For a VFS_WALK_PARENT walk, copy the last component of `path`, trailing
 * slashes passed over, into `leaf` of `leafcap` bytes, and point `*last` at
 * it in `path`. 0, -EINVAL for a path with no component, or
 * -ENAMETOOLONG. */
static int walk_leaf(const char *path, char *leaf, uint32_t leafcap, const char **last)
{
    const char *end = path + k_strlen(path);
    const char *start;

    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return -EINVAL;
    }
    start = end;
    while (start > path && start[-1] != '/') {
        start--;
    }
    if ((uint32_t)(end - start) + 1 > leafcap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(leaf, start, (uint32_t)(end - start));
    leaf[end - start] = '\0';
    *last = start;
    return 0;
}

/** Take the two path pages a walk follows links in, `*linkpath` and
 * `*target`, at its first link: 0, or -ENOMEM. */
static int walk_pages(char **linkpath, char **target)
{
    if (!*linkpath) {
        *linkpath = path_buf_get();
        *target = path_buf_get();
    }
    return *linkpath && *target ? 0 : -ENOMEM;
}

/**
 * Put the `tlen` bytes of `target` in front of `rest`, the part of the path
 * after a link, in `linkpath`. The rest may already be inside `linkpath`,
 * from a link followed before, so it moves before the target is written over
 * it. 1 when the target is absolute, 0 when it is relative, -ENOENT for an
 * empty target, or -ENAMETOOLONG.
 */
static int walk_splice(char *linkpath, const char *target, int tlen, const char *rest)
{
    uint32_t rlen = (uint32_t)k_strlen(rest);
    uint32_t join = rlen != 0 ? 1u : 0u;

    if (tlen == 0) {
        return -ENOENT;
    }
    if ((uint32_t)tlen + join + rlen + 1 > VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    k_memmove(linkpath + tlen + join, rest, rlen + 1);
    k_memcpy(linkpath, target, (uint32_t)tlen);
    if (join) {
        linkpath[tlen] = '/';
    }
    return target[0] == '/';
}

/**
 * Splice the target of the link `found` on `mnt` in front of `rest`, the
 * part of the path after the link, in the path page `*linkpath`, taking it
 * and `*target` at the first link of the walk; `found` is put. The walk goes
 * on at `*linkpath`: 1 when the target is absolute, so from the root, 0 when
 * it is relative, or -ELOOP past VFS_LINK_MAX links, -ENOENT for an empty
 * target, -ENAMETOOLONG, -ENOMEM, or the type's error.
 */
static int walk_link(struct vfs_mount *mnt, void *found, const char *rest, char **linkpath,
                     char **target, int *links)
{
    int tlen;

    if (++*links > VFS_LINK_MAX) {
        node_put(mnt, found);
        return -ELOOP;
    }
    if (walk_pages(linkpath, target) != 0) {
        node_put(mnt, found);
        return -ENOMEM;
    }
    tlen = mnt->type->ops->readlink(mount_base(mnt), found, *target, VFS_PATH_MAX);
    node_put(mnt, found);
    if (tlen < 0) {
        return tlen;
    }
    return walk_splice(*linkpath, *target, tlen, rest);
}

/**
 * Follow the text link `found` that a type taking the rest of a path found
 * in the part of it from `from` to `link_end`, as `walk_link` does with the
 * rest of the path, from `link_end` on. The walk goes on from where it
 * handed that part over, so a relative target, which Linux reads from the
 * link's directory, gets the part before the link's name in front of it,
 * as a link made under /dev, /dev/char/13:64 to ../input/event0, needs.
 * Returns what `walk_link` does, or -ENAMETOOLONG.
 */
static int walk_rest_link(struct vfs_mount *mnt, void *found, const char *from,
                          const char *link_end, char **linkpath, char **target, int *links)
{
    char dir[256];
    const char *s = link_end;
    uint32_t dlen, tl;
    int rc;

    while (s > from && s[-1] == '/') {
        s--;
    }
    while (s > from && s[-1] != '/') {
        s--;
    }
    dlen = (uint32_t)(s - from);
    if (dlen >= sizeof(dir)) {
        node_put(mnt, found);
        return -ENAMETOOLONG;
    }
    k_memcpy(dir, from, dlen);
    rc = walk_link(mnt, found, link_end, linkpath, target, links);
    if (rc != 0) {
        return rc;
    }
    tl = (uint32_t)k_strlen(*linkpath);
    if (dlen + tl + 1 > VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    k_memmove(*linkpath + dlen, *linkpath, tl + 1);
    k_memcpy(*linkpath, dir, dlen);
    return 0;
}

static int walk_exe(struct vfs_ref *cur, void *found, int locked, struct vfs_name **entry);
static int synth_object_link(const struct vpath *vp);
static int walk_object(struct vfs_ref *cur, void *found, const char *rest, char **linkpath,
                       char **target, int locked);

/* What `walk_object` returns when the walk has moved to the link's object. */
#define WALK_AT_OBJECT 2

/* Flags of `resolve_path` beside the VFS_WALK_ ones: for an unmount's
 * walk, and for a scoped walk through an anchor of a type that takes the
 * rest of a path (`walk_rules`). */
#define WALK_UNMOUNT 0x100
#define WALK_SYNTH   0x200

/* What `walk_rest_kind` finds a node of a type that takes the rest of a
 * path to be. */
#define WALK_REST_OBJECT  1
#define WALK_REST_THROUGH 2
#define WALK_REST_LINK    3

/** The start of the last component of `path`, trailing slashes passed over:
 * the leaf a parent walk returns once a link's target is spliced into its
 * path. */
static const char *walk_last_comp(const char *path)
{
    const char *end = path + k_strlen(path);

    while (end > path && end[-1] == '/') {
        end--;
    }
    while (end > path && end[-1] != '/') {
        end--;
    }
    return end;
}

/** Whether `node` on `mnt` is a symbolic link its type can read. */
static int walk_is_link(struct vfs_mount *mnt, void *node)
{
    struct vfs_stat st;

    return mnt->type->ops->readlink && mnt->type->ops->getattr &&
           mnt->type->ops->getattr(mount_base(mnt), node, &st) == 0 &&
           (st.mode & 0170000u) == 0120000u;
}

/** Whether a type taking the rest of a path answered `rc`, VFS_LINK, with
 * `found` a text link the walk goes on past, as a link made under /dev. */
static int walk_rest_through(int rc, struct vfs_mount *mnt, void *found)
{
    return rc == VFS_LINK && walk_is_link(mnt, found);
}

/** The length of the rest of the path at `p` that a type taking the rest
 * is handed: all of it, or for a parent walk the part before the leaf at
 * `last`, without the slashes after it; 0 when that part is empty. */
static uint32_t walk_rest_len(const char *p, const char *last)
{
    uint32_t len;

    if (!last) {
        return (uint32_t)k_strlen(p);
    }
    len = (uint32_t)(last - p);
    while (len > 0 && p[len - 1] == '/') {
        len--;
    }
    return len;
}

/**
 * Whether the walk goes on from the object of the synthetic node `vp`, which
 * `lookup_rest` returned with `rc`: a link followed to its object
 * (`synth_object_link`) with more of the path after it (VFS_LINK), before
 * the leaf of a parent walk (`parent`), or at the end of the path for cwd
 * and root unless `flags` has VFS_WALK_NOFOLLOW. A /proc/PID/fd/N at the end
 * stays the link's node, for the system call layer.
 */
static int walk_to_object(const struct vpath *vp, int rc, int parent, int flags)
{
    if (!synth_object_link(vp)) {
        return 0;
    }
    return rc == VFS_LINK || parent ||
           (!(flags & VFS_WALK_NOFOLLOW) && vp->kind != VNODE_PROC_FD_LINK);
}

/* The path a walk took from its root, its symbolic links resolved, which
 * `vfs_resolve_real` gives: `len` bytes at `buf`, with no terminator until the
 * walk ends; `len` is -1 while nothing is recorded, for no buffer or for a
 * path past VFS_PATH_MAX. */
struct walk_name {
    char *buf;
    int len;
};

/** Start the record `wn` in `buf`, which may be NULL, again at the walk's
 * root, as a walk and an absolute link's target do. */
static void walk_name_root(struct walk_name *wn, char *buf)
{
    wn->buf = buf;
    wn->len = buf ? 0 : -1;
}

/**
 * Apply the component `c` of `clen` bytes to the record `wn`: `..` drops its
 * last name, `.` and an empty component change nothing, and any other name is
 * appended. A path that would not fit VFS_PATH_MAX ends the record.
 */
static void walk_name_add(struct walk_name *wn, const char *c, uint32_t clen)
{
    if (wn->len < 0 || clen == 0 || (clen == 1 && c[0] == '.')) {
        return;
    }
    if (clen == 2 && c[0] == '.' && c[1] == '.') {
        while (wn->len > 0 && wn->buf[wn->len - 1] != '/') {
            wn->len--;
        }
        wn->len -= wn->len > 0;
        return;
    }
    if ((uint32_t)wn->len + 1 + clen + 1 > VFS_PATH_MAX) {
        wn->len = -1;
        return;
    }
    wn->buf[wn->len] = '/';
    k_memcpy(wn->buf + wn->len + 1, c, clen);
    wn->len += 1 + (int)clen;
}

/** Apply each component of the text from `p` to `end`, which a type that
 * takes the rest of a path used up, to the record `wn`. */
static void walk_name_text(struct walk_name *wn, const char *p, const char *end)
{
    while (p < end) {
        const char *c = p;

        while (p < end && *p != '/') {
            p++;
        }
        walk_name_add(wn, c, (uint32_t)(p - c));
        p += p < end;
    }
}

/**
 * Prepare `..` at the root of the nameless mount `m`, which the walk takes
 * to its parent's root directory: for a mount entered by more than one
 * name, as /sys/kernel/debug is from /sys, put the names before its last in
 * front of the rest of the path at `*p`, in the path page `*linkpath`, as a
 * link's target goes, so that the parent's type takes them from there. The
 * record `wn` drops a name for each name the mount is entered by, since the
 * walk records those it takes again. 0, -ENOMEM or -ENAMETOOLONG.
 */
static int nameless_up(const struct vfs_mount *m, struct walk_name *wn, const char **p,
                       const char **last_comp, char **linkpath, char **target)
{
    const char *entry = mount_entry_name(m);
    uint32_t up = (uint32_t)k_strlen(entry);
    const char *c;
    int rc;

    while (up > 0 && entry[up - 1] != '/') {
        up--;
    }
    walk_name_add(wn, "..", 2);
    if (up == 0) {
        return 0;
    }
    for (c = entry; c < entry + up; c++) {
        if (*c == '/') {
            walk_name_add(wn, "..", 2);
        }
    }
    if (walk_pages(linkpath, target) != 0) {
        return -ENOMEM;
    }
    k_memcpy(*target, entry, up - 1);
    rc = walk_splice(*linkpath, *target, (int)(up - 1), *p);
    if (rc < 0) {
        return rc;
    }
    *p = *linkpath;
    if (*last_comp) {
        *last_comp = walk_last_comp(*linkpath);
    }
    return 0;
}

/** Move the walk at `cur` onto the root of the mount `m`, and on through
 * the mounts made over that root (`walk_cross`). 0, or `walk_move`'s
 * error. */
static int walk_enter(struct vfs_ref *cur, struct vfs_mount *m, int locked)
{
    int rc = walk_move(cur, m, m->root, locked);

    return rc == 0 ? walk_cross(cur, locked) : rc;
}

/** End the record `wn` of a walk that succeeded: its path, "/" for the root,
 * or an empty string when nothing was recorded. */
static void walk_name_end(struct walk_name *wn)
{
    if (!wn->buf) {
        return;
    }
    if (wn->len < 0) {
        wn->len = 0;
    } else if (wn->len == 0) {
        wn->buf[wn->len++] = '/';
    }
    wn->buf[wn->len] = '\0';
}

struct walk_record {
    char *real;
    struct vfs_name **entry;
    const struct vfs_scope *scope; /* an openat2's, or NULL */
    /* The attributes of the directory the last name was looked up in, or
     * NULL; a mode of 0 when no name was (`vfs_resolve_entry`). */
    struct vfs_stat *dir;
};

/* The restrictions one walk applies: an openat2's scope (`vfs_resolve_scoped`)
 * and the calling thread's replay (`vfs_replay`). */
struct walk_rules {
    const struct vfs_scope *scope; /* NULL for any other walk */
    uint32_t resolve;              /* the scope's VFS_RESOLVE_*, or 0 */
    int scoped;                    /* BENEATH or IN_ROOT */
    /* The anchor is inside a type that takes the rest of a path from its
     * mount's root, so the walk goes from the caller's root through the
     * anchor's real path, whose `..`s the scope has checked in the text
     * (`scope_text`). */
    int synth;
    uint32_t replay;        /* the thread's VFS_REPLAY_* */
    struct vfs_mount *xdev; /* the mount NO_XDEV keeps the walk on */
    int xdev_armed;         /* the walk has reached `xdev` */
};

/**
 * Set `rules` for a walk of `flags` with `record`, and return where a
 * relative path starts: a scoped walk's anchor, unless it goes through the
 * anchor's real path (`synth`), or `start`.
 */
static const struct vfs_ref *walk_rules_init(struct walk_rules *rules,
                                             const struct walk_record *record, int flags,
                                             const struct vfs_ref *start)
{
    rules->scope = record ? record->scope : NULL;
    rules->resolve = rules->scope ? rules->scope->resolve : 0;
    rules->scoped = (rules->resolve & (VFS_RESOLVE_BENEATH | VFS_RESOLVE_IN_ROOT)) != 0;
    rules->synth = (flags & WALK_SYNTH) != 0;
    rules->replay = !rules->scope && current_proc ? current_proc->walk_replay : 0;
    if (rules->replay && current_proc->walk_replay_mnt) {
        rules->resolve |= VFS_RESOLVE_NO_XDEV;
    }
    rules->xdev = NULL;
    rules->xdev_armed = 0;
    return rules->scope && !rules->synth ? &rules->scope->anchor : start;
}

/** Whether the synthetic node `vp` is a link of /proc to an object, which
 * Linux calls a magic link: a descriptor, a working directory, a root, a
 * program or a namespace. */
static int walk_magic(const struct vpath *vp)
{
    return vp && (vp->kind == VNODE_PROC_FD_LINK || vp->kind == VNODE_PROC_CWD ||
                  vp->kind == VNODE_PROC_ROOT || vp->kind == VNODE_PROC_EXE ||
                  vp->kind == VNODE_PROC_NS_LINK);
}

/**
 * Whether the walk under `rules` may follow a link, a magic one when
 * `magic`: 0, or -ELOOP under NO_SYMLINKS, and for a magic link under
 * NO_MAGICLINKS; -EXDEV for a magic link of a scoped walk; -EAGAIN in a
 * replay for any link but a magic one it allows.
 */
static int walk_link_ok(const struct walk_rules *rules, int magic)
{
    if ((rules->resolve & VFS_RESOLVE_NO_SYMLINKS) ||
        (magic && (rules->resolve & VFS_RESOLVE_NO_MAGICLINKS))) {
        return -ELOOP;
    }
    if (magic && rules->scoped) {
        return -EXDEV;
    }
    if (rules->replay && !(magic && (rules->replay & VFS_REPLAY_MAGIC))) {
        current_proc->walk_replay |= VFS_REPLAY_TRIPPED;
        return -EAGAIN;
    }
    return 0;
}

/**
 * Whether the walk under `rules` may follow the symbolic link of attributes
 * `lst` that it found in the directory `cur`: the last name of the path
 * unless `more` comes after it, which fs.protected_symlinks judges first
 * (`may_follow_link`), as Linux's pick_link does, and then the link rules
 * of `walk_link_ok`. 0 or a negative errno.
 */
static int walk_follow_ok(const struct walk_rules *rules, const struct vfs_ref *cur,
                          const struct vfs_stat *lst, int more, int use_real)
{
    int rc = more ? 0 : may_follow_link(cur->mnt, cur->node, lst, use_real);

    return rc != 0 ? rc : walk_link_ok(rules, 0);
}

/** Start the record `wn` at the anchor of the walk `rules` has, or at the
 * root when `rules` is NULL or has no scope, as `walk_name_root` does. */
static void walk_name_anchor(struct walk_name *wn, char *buf, const struct walk_rules *rules)
{
    walk_name_root(wn, buf);
    if (buf && rules && rules->scope) {
        k_memcpy(buf, rules->scope->anchor_real, rules->scope->anchor_len);
        wn->len = (int)rules->scope->anchor_len;
    }
}

/** Whether a scoped walk under `rules` has its anchor as its root. */
static int walk_anchor_root(const struct walk_rules *rules)
{
    return rules->scoped && !rules->synth;
}

/**
 * Begin the walk under `rules` of the path `p`: a scoped walk's root is its
 * anchor, and the record `wn` starts at the anchor for a path that starts
 * there.
 */
static void walk_scope_begin(const struct walk_rules *rules, struct vfs_ref *root,
                             struct walk_name *wn, const char *p)
{
    if (walk_anchor_root(rules)) {
        *root = rules->scope->anchor;
    }
    if (rules->scope && !rules->synth && (p[0] != '/' || rules->scoped)) {
        walk_name_anchor(wn, wn->buf, rules);
    }
}

/** Set the mount NO_XDEV keeps the walk under `rules` on: the one it starts
 * on, `mnt`, or for a walk through the anchor's real path, the anchor's,
 * from when the walk reaches it. */
static void walk_xdev_begin(struct walk_rules *rules, struct vfs_mount *mnt)
{
    if (rules->replay && current_proc->walk_replay_mnt) {
        rules->xdev = current_proc->walk_replay_mnt;
        rules->xdev_armed = 0;
    } else {
        rules->xdev = rules->synth ? rules->scope->anchor.mnt : mnt;
        rules->xdev_armed = !rules->synth;
    }
}

/** Note that the walk under `rules` is on `mnt`: -EXDEV under NO_XDEV once
 * it has left its mount, else 0. */
static int walk_xdev(struct walk_rules *rules, struct vfs_mount *mnt)
{
    rules->xdev_armed |= mnt == rules->xdev;
    return (rules->resolve & VFS_RESOLVE_NO_XDEV) && rules->xdev_armed && mnt != rules->xdev
               ? -EXDEV
               : 0;
}

/** Whether the walk under `rules` must start again for a jump to its root
 * or a `..`, since the caller's root has moved; a scoped walk's root is its
 * anchor, which the caller holds. */
static int walk_root_moved(const struct walk_rules *rules, const struct vfs_ref *root)
{
    return !walk_anchor_root(rules) && !root_current(root);
}

/**
 * Whether the walk under `rules` may take `..` from `cur`, whose root is
 * `root`: -EXDEV for a walk through the anchor's real path, which met a `..`
 * the text did not have, and at the anchor under BENEATH; -EAGAIN for a
 * scoped walk once a rename or a mount has moved the tree's sequence.
 */
static int walk_dotdot_ok(const struct walk_rules *rules, const struct vfs_ref *cur,
                          const struct vfs_ref *root)
{
    if (!rules->scoped) {
        return 0;
    }
    if (rules->synth || ((rules->resolve & VFS_RESOLVE_BENEATH) && cur->mnt == root->mnt &&
                         cur->node == root->node)) {
        return -EXDEV;
    }
    return vfs_tree_seq() != rules->scope->seq ? -EAGAIN : 0;
}

/** Look `name` up in `dir` as the walk under `rules` does: from the name
 * cache alone under CACHED (`dcache_lookup_cached`). */
static int walk_lookup(const struct walk_rules *rules, struct vfs_mount *mnt, void *dir,
                       const char *name, void **out)
{
    return (rules->resolve & VFS_RESOLVE_CACHED) ? dcache_lookup_cached(mnt, dir, name, out)
                                                 : dcache_lookup(mnt, dir, name, out);
}

/**
 * The end of a walk under `rules` on `mnt` that found its node: -EXDEV under
 * NO_XDEV off the walk's mount, -EAGAIN for a replay once the tree's
 * sequence has moved, else 0.
 */
static int walk_end_ok(const struct walk_rules *rules, struct vfs_mount *mnt)
{
    if ((rules->resolve & VFS_RESOLVE_NO_XDEV) && mnt != rules->xdev) {
        return -EXDEV;
    }
    if (rules->replay && vfs_tree_seq() != current_proc->walk_replay_seq) {
        current_proc->walk_replay |= VFS_REPLAY_TRIPPED;
        return -EAGAIN;
    }
    return 0;
}

/** Whether the text `t` has a `..` component. */
static int text_climbs(const char *t)
{
    while (*t) {
        if (t[0] == '.' && t[1] == '.' && (t[2] == '/' || t[2] == '\0')) {
            return 1;
        }
        while (*t && *t != '/') {
            t++;
        }
        while (*t == '/') {
            t++;
        }
    }
    return 0;
}

/**
 * Prepare the jump of the walk under `rules` to its root for an absolute
 * link's target, which is now at the start of `linkpath`: -EXDEV under
 * BENEATH; under IN_ROOT, the anchor is the root, which a walk through an
 * anchor of a type that takes the rest of a path reaches by its real path
 * put in front of the target, with the target's `..`s -EXDEV; 0 with the
 * record `wn` started at the root the walk goes to.
 */
static int walk_jump(const struct walk_rules *rules, struct walk_name *wn, char *linkpath)
{
    uint32_t tl;

    if (rules->resolve & VFS_RESOLVE_BENEATH) {
        return -EXDEV;
    }
    if (rules->scoped && rules->synth) {
        if (text_climbs(linkpath)) {
            return -EXDEV;
        }
        tl = (uint32_t)k_strlen(linkpath);
        if (rules->scope->anchor_len + tl + 1 > VFS_PATH_MAX) {
            return -ENAMETOOLONG;
        }
        k_memmove(linkpath + rules->scope->anchor_len, linkpath, tl + 1);
        k_memcpy(linkpath, rules->scope->anchor_real, rules->scope->anchor_len);
    }
    walk_name_anchor(wn, wn->buf, walk_anchor_root(rules) ? rules : 0);
    return 0;
}

/**
 * Check the target of a link the walk under `rules` follows, now at the
 * start of `linkpath`: an absolute one as `walk_jump` does; a relative one
 * with a `..` is -EXDEV for a scoped walk through the anchor's real path,
 * whose text it was not checked in.
 */
static int walk_target(const struct walk_rules *rules, struct walk_name *wn, char *linkpath,
                       int absolute)
{
    if (absolute) {
        return walk_jump(rules, wn, linkpath);
    }
    return rules->synth && rules->scoped && text_climbs(linkpath) ? -EXDEV : 0;
}

/** Whether nothing but slashes follows `p`: the name before it is the
 * path's last. */
static int walk_at_end(const char *p)
{
    while (*p == '/') {
        p++;
    }
    return *p == '\0';
}

/** Whether the walk of `flags` ends at the missing name it creates: the
 * lookup gave `rc`, and `p` follows the name. */
static int walk_creates(int rc, int flags, const char *p)
{
    return rc == -ENOENT && (flags & VFS_WALK_CREATE) && walk_at_end(p);
}

/**
 * How the walk of `flags` goes on from the node `found` of `mnt` that a
 * type's `lookup_rest` gave with `rc`, whose /proc meaning is `vp`, before
 * the leaf of a parent walk (`parent`): WALK_REST_OBJECT to the object of a
 * /proc link, WALK_REST_THROUGH or WALK_REST_LINK through a link it
 * follows, or 0 when it stays at the node.
 */
static int walk_rest_kind(const struct vpath *vp, int rc, struct vfs_mount *mnt, void *found,
                          int parent, int flags)
{
    if (walk_to_object(vp, rc, parent, flags)) {
        return WALK_REST_OBJECT;
    }
    if (walk_rest_through(rc, mnt, found)) {
        return WALK_REST_THROUGH;
    }
    if (!parent && !(flags & VFS_WALK_NOFOLLOW) && walk_is_link(mnt, found)) {
        return WALK_REST_LINK;
    }
    return 0;
}

/** Capture a regular file's final entry before its parent hold is released. */
static int walk_record_entry(struct walk_record *record, const struct vfs_ref *parent, void *found,
                             const char *leaf, const char *rest)
{
    struct vfs_stat st;
    int rc;

    if (!record || !record->entry) {
        return 0;
    }
    while (*rest == '/') {
        rest++;
    }
    if (*rest || (mount_base(parent->mnt) == mounts[0] && parent->mnt->type == &vfs_krffs_type)) {
        return 0;
    }
    rc = vfs_getattr(parent->mnt, found, &st);
    if (rc != 0 || (st.mode & 0170000u) != 0100000u) {
        return rc;
    }
    return vfs_name_capture(parent, leaf, record->entry);
}

/** Release an unsuccessful walk's entry, or initialize its empty slot, and
 * mark its directory's attributes as not read. */
static void walk_record_clear(struct walk_record *record, int release)
{
    if (record && record->dir) {
        record->dir->mode = 0;
    }
    if (record && record->entry) {
        if (release) {
            vfs_name_put(*record->entry);
        }
        *record->entry = NULL;
    }
}

/** Move to a looked-up node after recording its final directory entry and,
 * for the last name of the path, its directory's attributes. */
static int walk_record_move(struct walk_record *record, struct vfs_ref *cur, void *found,
                            const char *leaf, const char *rest)
{
    int rc = walk_record_entry(record, cur, found, leaf, rest);

    if (rc == 0 && record && record->dir) {
        while (*rest == '/') {
            rest++;
        }
        if (*rest == '\0') {
            rc = vfs_getattr(cur->mnt, cur->node, record->dir);
        }
    }

    if (rc != 0) {
        node_put(cur->mnt, found);
    } else {
        ref_move_node(cur, found);
    }
    return rc;
}

/**
 * Cross the mounts over the node the walk has just moved to (`walk_cross`).
 * A file bound over the recorded entry is the bind's root, on another mount,
 * which the entry does not name, so a crossing drops the record. 0, or
 * `walk_cross`'s error.
 */
static int walk_record_cross(struct walk_record *record, struct vfs_ref *cur, int locked)
{
    struct vfs_mount *from = cur->mnt;
    int rc = walk_cross(cur, locked);

    if (rc == 0 && cur->mnt != from) {
        walk_record_clear(record, 1);
    }
    return rc;
}

/** Release the current node before dropping its namespace lock and mount. */
static void walk_release(struct vfs_ref *cur, int locked)
{
    struct vfs_mount *mnt = cur->mnt;

    vfs_ref_put(cur);
    walk_unlock(mnt, locked);
    vfs_leave(mnt);
}

/** Validate the final node, including a parent walk's search permission. */
static int walk_finish(const struct vfs_ref *cur, int flags)
{
    if (vfs_mount_gone(cur->mnt) && !(flags & WALK_UNMOUNT)) {
        return -EIO;
    }
    return (flags & VFS_WALK_PARENT)
               ? may_search(cur->mnt, cur->node, (flags & VFS_WALK_REAL_IDS) != 0)
               : 0;
}

/* The cached walk's answer when the full walk must take the path. */
#define WALK_FULL 1

/** Whether the cached walk may stand on `mnt`: a type whose names the name
 * cache keeps, which a walk needs no namespace lock for and which looks
 * names up one at a time, and whose `getattr`, `hold` and `put` run without
 * the big lock (`reads_unlocked`), since the walk and `stat_at_unlocked`
 * call them without it. */
static int walk_cacheable(const struct vfs_mount *mnt)
{
    const struct vfs_type *t = mnt->type;

    return t->name_cache && t->reads_unlocked && !t->ns_serialised && !t->ops->lookup_rest &&
           t->ops->hold && t->ops->put && t->ops->put_unless_last && t->ops->getattr;
}

/** Drop the cached walk's hold on `node` of `mnt`: without the big lock
 * when it is not the last, and under it otherwise. May sleep. */
static void cached_put(struct vfs_mount *mnt, void *node)
{
    if (!mnt->type->ops->put_unless_last(mount_base(mnt), node)) {
        bkl_enter();
        mnt->type->ops->put(mount_base(mnt), node);
        bkl_exit();
    }
}

/** Record the directory entry `leaf` of `parent` in `*entry` when `found`,
 * the walk's last node, is a regular file, as `walk_record_entry` does for
 * the full walk; `seq` is the count of changes of names the walk began at.
 * 0, or nonzero with nothing recorded, for the full walk. */
static int cached_entry(const struct vfs_ref *parent, void *found, const char *leaf, uint32_t seq,
                        struct vfs_name **entry)
{
    struct vfs_stat st;
    int rc = parent->mnt->type->ops->getattr(mount_base(parent->mnt), found, &st);

    if (rc != 0 || (st.mode & 0170000u) != 0100000u) {
        return rc;
    }
    return vfs_name_capture_at(parent, leaf, seq, entry);
}

/* Where the cached walk records the last name's entry, NULL for nowhere,
 * and the count of changes of names it began at (`vfs_name_seq`). */
struct cached_record {
    struct vfs_name **entry;
    uint32_t seq;
};

/* The cached walk's answer for a symbolic link it follows (`cached_found`). */
#define WALK_LINK 2

/**
 * The cached walk's checks on `found`, the node of the name `leaf` it just
 * looked up in `cur`, with `rest` the path after it: 0, with a regular
 * file's entry recorded at the last name as `rec` asks (`cached_entry`);
 * WALK_LINK, with `found` held, for a symbolic link the walk follows,
 * which fs.protected_symlinks allows at the last name (`may_follow_link`),
 * as Linux's pick_link asks (fs/namei.c:1837); or WALK_FULL, with `found`
 * put, for a link it refuses, a failed `getattr` or an entry it cannot
 * record.
 */
static int cached_found(const struct vfs_ref *cur, void *found, const char *leaf, const char *rest,
                        int flags, const struct cached_record *rec)
{
    struct vfs_stat st;

    if (cur->mnt->type->ops->readlink) {
        if (cur->mnt->type->ops->getattr(mount_base(cur->mnt), found, &st) != 0) {
            cached_put(cur->mnt, found);
            return WALK_FULL;
        }
        if ((st.mode & 0170000u) == 0120000u && (*rest || !(flags & VFS_WALK_NOFOLLOW))) {
            if (*rest ||
                may_follow_link(cur->mnt, cur->node, &st, (flags & VFS_WALK_REAL_IDS) != 0) == 0) {
                return WALK_LINK;
            }
            cached_put(cur->mnt, found);
            return WALK_FULL;
        }
    }
    if (rec->entry && !*rest && cached_entry(cur, found, leaf, rec->seq, rec->entry) != 0) {
        cached_put(cur->mnt, found);
        return WALK_FULL;
    }
    return 0;
}

/** Enter `mnt` and hold `node` on it for the cached walk, into `cur`, when
 * the walk may stand there: 0, or WALK_FULL with nothing taken. The caller
 * holds a lock or a hold that keeps both while this runs, and may hold a
 * spinlock: nothing here sleeps. */
static int cached_take(struct vfs_mount *mnt, void *node, struct vfs_ref *cur)
{
    if (!mnt || !node || !walk_cacheable(mnt) || vfs_enter(mnt) != 0) {
        return WALK_FULL;
    }
    mnt->type->ops->hold(mount_base(mnt), node);
    cur->mnt = mnt;
    cur->node = node;
    return 0;
}

/**
 * Where the cached walk of `path` starts, entered and held into `cur`: the
 * caller's `start` for a relative path, which the caller holds, else the
 * calling process's root, read under its lock, or the root mount's, read
 * under the table's, as Linux's `set_root` reads `fs->root` under its
 * sequence (fs/namei.c:943-965). 0, or WALK_FULL with nothing taken, for a
 * path the full walk starts at the root of the root mount, one that begins
 * with two slashes, among others.
 */
static int cached_start(const struct vfs_ref *start, const char *path, struct vfs_ref *cur)
{
    struct fs_struct *fs = current_proc ? current_proc->fs : NULL;
    uint32_t flags;
    int rc;

    if (path[0] != '/' && start) {
        return cached_take(start->mnt, start->node, cur);
    }
    if (path[0] == '/' && path[1] == '/') {
        return WALK_FULL;
    }
    if (fs) {
        flags = spin_lock_irqsave(&fs->lock);
        rc = fs->root_mnt ? cached_take(fs->root_mnt, fs->root_node, cur) : -1;
        spin_unlock_irqrestore(&fs->lock, flags);
        if (rc >= 0) {
            return rc;
        }
    }
    flags = spin_lock_irqsave(&mount_table_lock);
    rc = cached_take(mounts[0], mounts[0]->root, cur);
    spin_unlock_irqrestore(&mount_table_lock, flags);
    return rc;
}

/** Whether a nameless mount may be entered by the name `comp` from the
 * root directory of `mnt`: a search of the table under its lock that judges
 * no namespace's view and no state but free, so that it errs only toward
 * the full walk. */
static int cached_mount_named(struct vfs_mount *mnt, const char *comp)
{
    uint32_t flags = spin_lock_irqsave(&mount_table_lock);
    int i, near = 0;

    for (i = 1; i < mount_slots && !near; i++) {
        const struct vfs_mount *m = mounts[i];

        if (!m || m->state == VFS_FREE || m->parent != mnt) {
            continue;
        }
        near = !m->covered && streq(path_last(m->covered_path), comp);
    }
    spin_unlock_irqrestore(&mount_table_lock, flags);
    return near;
}

/**
 * Move the cached walk at `cur` onto the root of the mount covering its
 * node, and on through each mount made over that root in turn, as
 * `walk_cross` moves the full walk and Linux's `__follow_mount_rcu` moves
 * a walk under RCU (fs/namei.c:1521-1563). The mounts are searched under the
 * table's lock, which keeps a record in its slot while the walk enters it,
 * as `cached_start` takes the root mount. 0, with `cur` where a path through
 * this place arrives; or WALK_FULL, with `cur` on the last mount reached,
 * for a mount the walk may not judge (`vfs_mount_seen_word`) or stand on
 * (`cached_take`), one being made or unmounted among them. May sleep, to
 * drop a hold it moved off (`cached_put`).
 */
static int cached_cross(struct vfs_ref *cur)
{
    for (;;) {
        struct vfs_mount *over = NULL;
        struct vfs_ref next;
        uint32_t flags = spin_lock_irqsave(&mount_table_lock);
        int i, rc = 0;

        for (i = 1; i < mount_slots && !over && rc == 0; i++) {
            struct vfs_mount *m = mounts[i];
            int seen;

            if (!m || m->state == VFS_FREE || m->parent != cur->mnt || !m->covered ||
                m->covered != cur->node) {
                continue;
            }
            seen = vfs_mount_seen_word(m);
            if (seen < 0) {
                rc = WALK_FULL;
            } else if (seen) {
                over = m;
            }
        }
        if (over) {
            rc = cached_take(over, over->root, &next);
        }
        spin_unlock_irqrestore(&mount_table_lock, flags);
        if (rc != 0 || !over) {
            return rc;
        }
        cached_put(cur->mnt, cur->node);
        vfs_leave(cur->mnt);
        *cur = next;
    }
}

/* What the cached walk keeps while it follows links: the two path pages,
 * taken at the first link (`walk_pages`), and the count of links. */
struct cached_links {
    char *linkpath;
    char *target;
    int links;
};

/**
 * Splice the target of the link `found` on `cur`'s mount in front of `rest`
 * in `lk->linkpath`, as `walk_link` does for the full walk; `found` is put.
 * The target must be one the node holds (`readlink_cached`), as Linux's walk
 * under RCU reads `i_link` and leaves any other link to the walk with
 * references (fs/namei.c:1860-1870). An absolute target moves `cur` to the
 * caller's root (`cached_start`); a relative one goes on from `cur`. 0, with
 * the walk to go on at `lk->linkpath`; or WALK_FULL, with `cur` held as it
 * was, past VFS_LINK_MAX links, for a target the node does not hold, a root
 * it cannot take or an error, which the full walk reports.
 */
static int cached_link(struct vfs_ref *cur, void *found, const char *rest, struct cached_links *lk)
{
    struct vfs_ref root;
    int tlen = -1, rc;

    if (cur->mnt->type->ops->readlink_cached && ++lk->links <= VFS_LINK_MAX &&
        walk_pages(&lk->linkpath, &lk->target) == 0) {
        tlen = cur->mnt->type->ops->readlink_cached(mount_base(cur->mnt), found, lk->target,
                                                    VFS_PATH_MAX);
    }
    cached_put(cur->mnt, found);
    rc = tlen < 0 ? -1 : walk_splice(lk->linkpath, lk->target, tlen, rest);
    if (rc < 0 || (rc == 1 && cached_start(NULL, lk->linkpath, &root) != 0)) {
        return WALK_FULL;
    }
    if (rc == 1) {
        cached_put(cur->mnt, cur->node);
        vfs_leave(cur->mnt);
        *cur = root;
    }
    return 0;
}

/**
 * Resolve `path` from the name cache alone, without the big lock, as Linux
 * first walks a path under RCU and walks it again with references when that
 * fails (`filename_lookup`, fs/namei.c:2651-2653). 0 with `out` held and its
 * mount entered, as the full walk returns it; -ENOENT when the cache has a
 * negative entry for a name; or WALK_FULL, with nothing held, for the full
 * walk to take the path whole.
 *
 * Each step must stand on a mount of a type `walk_cacheable` admits, find
 * its name in the cache and pass the search check, which reads the held
 * directory alone (`name_cache`). A symbolic link whose target the node
 * holds it follows (`cached_link`), from the root for an absolute target
 * and from the link's directory for a relative one, and a directory a
 * mount covers it crosses (`cached_cross`); a nameless mount entered by
 * name it leaves to the full walk. Anything else, `..`, any other link and
 * a failed search check among them, which the full walk may excuse on the
 * way to the working directory (`toward_cwd`), goes to the full walk, which
 * gives the same answer for every path this one finishes. The walk takes no
 * namespace lock, since its type needs none, and a mount it ends on is the
 * one `walk_unlock` leaves.
 */
static int resolve_cached(const struct vfs_ref *start, const char *path, int flags,
                          struct vfs_ref *out, struct vfs_name **entry)
{
    const char *p = path;
    struct vfs_ref cur;
    int use_real = (flags & VFS_WALK_REAL_IDS) != 0;
    struct cached_record rec;
    struct cached_links lk = {NULL, NULL, 0};
    uint32_t found_names = 0;
    int rc;

    rec.entry = entry;
    rec.seq = 0;
    if ((entry && !vfs_name_seq(&rec.seq)) || cached_start(start, path, &cur) != 0) {
        return WALK_FULL;
    }
    for (;;) {
        char comp[VFS_NAME_MAX + 1];
        const char *rest;
        uint32_t len = 0;
        void *found;

        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (vfs_mount_gone(cur.mnt) || may_search(cur.mnt, cur.node, use_real) != 0) {
            goto full;
        }
        while (p[len] != '\0' && p[len] != '/') {
            len++;
        }
        if (len > VFS_NAME_MAX) {
            goto full;
        }
        k_memcpy(comp, p, len);
        comp[len] = '\0';
        p += len;
        if (streq(comp, ".")) {
            continue;
        }
        if (streq(comp, "..") || (cur.node == cur.mnt->root && cached_mount_named(cur.mnt, comp))) {
            goto full;
        }
        rc = dcache_lookup_hit(mount_base(cur.mnt), cur.node, comp, &found);
        if (rc == -ENOENT) {
            dcache_count_hits(found_names + 1);
            cached_put(cur.mnt, cur.node);
            vfs_leave(cur.mnt);
            path_buf_put(lk.linkpath);
            path_buf_put(lk.target);
            return rc;
        }
        if (rc != 0) {
            goto full;
        }
        found_names++;
        for (rest = p; *rest == '/'; rest++) {
        }
        rc = cached_found(&cur, found, comp, rest, flags, &rec);
        if (rc == WALK_LINK) {
            rc = cached_link(&cur, found, rest, &lk);
            p = lk.linkpath;
            if (rc == 0) {
                continue;
            }
        }
        if (rc != 0) {
            goto full;
        }
        cached_put(cur.mnt, cur.node);
        cur.node = found;
        if (cached_cross(&cur) != 0) {
            goto full;
        }
    }
    if (vfs_mount_gone(cur.mnt)) {
        goto full;
    }
    dcache_count_hits(found_names);
    *out = cur;
    path_buf_put(lk.linkpath);
    path_buf_put(lk.target);
    return 0;

full:
    cached_put(cur.mnt, cur.node);
    vfs_leave(cur.mnt);
    if (entry) {
        vfs_name_put(*entry);
        *entry = NULL;
    }
    path_buf_put(lk.linkpath);
    path_buf_put(lk.target);
    return WALK_FULL;
}

/**
 * Resolve one path, as `vfs_resolve` says; with WALK_UNMOUNT, for an unmount,
 * the walk may end on a mount whose device has gone, though it looks up no
 * name inside one, so that the unmount can release that mount.
 *
 * The loop takes one component at a time and handles five cases: `.`, `..`,
 * a mount whose type takes the rest of the path in one call, a nameless
 * mount entered by name from the root directory, and the ordinary lookup of
 * one name, after which a symbolic link is followed and a covering mount is
 * crossed.
 *
 * A type that takes the rest is handed the rest of the path; with
 * VFS_WALK_PARENT, the part before the leaf, and when that part is empty the
 * walk ends at the mount's root. On its VFS_CLIMB or VFS_ROOT the walk goes
 * on at the mount's root directory with what the type left: a `..` that
 * climbs, or the name of a nameless mount there. A `..` at that root is
 * never handed to the type; the ordinary step takes it, as at the root of
 * any other mount.
 *
 * On a type with symbolic links, a component that is one is followed when
 * more of the path comes after it, and at the last component unless the
 * caller passed VFS_WALK_NOFOLLOW, where fs.protected_symlinks may refuse it
 * (`may_follow_link`). Its target replaces the component, an
 * absolute target restarts at the root, and the walk goes on from the
 * directory the link was found in. A mount covers a directory, so a link is
 * never a covered node. /proc/<pid>/exe is followed to the program's file
 * itself (`walk_exe`) rather than by its text, and /proc/<pid>/fd/<n>, cwd
 * and root to the file or directory they refer to (`walk_object`): on the
 * type's VFS_LINK, when more of the path comes after the link, and for the
 * last component of a parent walk; cwd and root also at the last component
 * unless VFS_WALK_NOFOLLOW. A /proc/<pid>/fd/<n> at the last component stays
 * the link's node, which the system call layer opens and reports itself. The
 * walk goes on from the object as from a directory it looked up, though a
 * mount over it is not crossed, as Linux crosses none there, and no
 * component after it is excused from the search check as one on the way to
 * the working directory is (`toward_cwd`).
 *
 * The target and the path it is joined to are in two path pages, taken at
 * the first link and given back on every return; a walk that meets no link
 * takes none.
 *
 * `..` at the root of a mount over a directory runs again at the covered
 * directory, in the parent mount, since the mount's root replaces that
 * directory. A nameless mount is entered by name from its parent's root
 * directory, as /dev/shm is from /dev's, so `..` at its root is that
 * directory and is used up there: run again, the synthetic type would climb
 * once more.
 *
 * The walk keeps the root it starts from: `..` stops there, and at the root
 * of the root mount whatever the root, and an absolute link's target starts
 * there. A thread that shares the root may chroot while the walk sleeps; at
 * its next `..` or absolute link the walk sees that (`root_current`), lets
 * go of where it is and starts over from the new root, so no walk mixes two.
 * A path that begins with two slashes, which the system call layer makes
 * for a name relative to a descriptor's directory outside the caller's
 * root, starts at the root of the root mount, and keeps the caller's root
 * for `..` and for links.
 *
 * VFS_WALK_LOCK holds one namespace lock at a time and returns with the
 * final mount locked when its type sets `ns_serialised`, so an open can
 * register its description before a relocation.
 *
 * With `record->real` not NULL the walk records the path it takes in it, as
 * `vfs_resolve_real` says. Each name is recorded when it is looked up or
 * entered, and a symbolic link found so is dropped again for its target,
 * whose components are recorded as the walk takes them. What a type that
 * takes the rest uses up is recorded as text: its links of /proc are objects
 * or names, not paths of files, so the record keeps the components that
 * named them, and a text link it follows goes on at the mount's root, as the
 * walk does.
 */
static int resolve_full(const struct vfs_ref *start, const char *path, int flags,
                        struct vfs_ref *out, char *leaf, uint32_t leafcap,
                        struct walk_record *record)
{
    struct walk_name name;
    struct walk_rules rules;
    struct vfs_ref cur;
    struct vfs_ref root;
    const char *p;
    const char *leaf_comp = 0;
    const char *last_comp;
    int use_real = (flags & VFS_WALK_REAL_IDS) ? 1 : 0;
    int links;
    int nocwd;
    int kind;
    int locked = (flags & VFS_WALK_LOCK) != 0;
    int missing = 0;
    char *linkpath = 0;
    char *target = 0;
    int rc;

    walk_record_clear(record, 0);
    if (!path || !out) {
        return -EINVAL;
    }
    if (mounts[0]->state != VFS_ACTIVE) {
        return -ENOENT;
    }
    start = walk_rules_init(&rules, record, flags, start);

    if (flags & VFS_WALK_PARENT) {
        rc = walk_leaf(path, leaf, leafcap, &leaf_comp);
        if (rc != 0) {
            return rc;
        }
    }

begin:
    p = path;
    last_comp = leaf_comp;
    links = 0;
    nocwd = rules.scope != NULL;
    walk_name_root(&name, record ? record->real : NULL);
    walk_root(&root);
    walk_scope_begin(&rules, &root, &name, p);
    cur = (p[0] == '/' || !start) ? root : *start;
    if (p[0] == '/' && p[1] == '/' && !rules.scope) {
        cur.mnt = mounts[0];
        cur.node = mounts[0]->root;
    }
    walk_xdev_begin(&rules, cur.mnt);
    if (vfs_enter(cur.mnt) != 0) {
        path_buf_put(linkpath);
        path_buf_put(target);
        return -EBUSY;
    }
    vfs_ref_hold(&cur);
    walk_lock(cur.mnt, locked);

    for (;;) {
        char comp[VFS_NAME_MAX + 1];
        uint32_t len;
        void *found;
        struct vfs_mount *m;

        while (*p == '/') {
            p++;
        }
        rc = walk_xdev(&rules, cur.mnt);
        if (rc != 0) {
            goto out_err;
        }
        if (*p == '\0' || (last_comp && p >= last_comp)) {
            break;
        }
        if (vfs_mount_gone(cur.mnt)) {
            rc = -EIO;
            goto out_err;
        }

        if (cur.mnt->type->ops->lookup_rest && !named_in_rest(&cur, p) && !climbs(&cur, p)) {
            const char *from = p;
            const char *left = 0;
            const struct vpath *vp;
            uint32_t rlen = walk_rest_len(p, last_comp);

            if (rlen == 0) {
                break;
            }
            rc = cur.mnt->type->ops->lookup_rest(cur.mnt, p, rlen, &found, &left);
            if (rc == VFS_CLIMB || rc == VFS_ROOT) {
                p = left;
                continue;
            }
            if (rc == VFS_ENTER) {
                walk_name_text(&name, from, left);
                p = left;
                rc = walk_enter(&cur, found, locked);
                if (rc != 0) {
                    goto out_err;
                }
                continue;
            }
            if (rc < 0) {
                goto out_err;
            }
            p = rc == VFS_LINK ? left : p + rlen;
            vp = vfs_synth_vpath(cur.mnt, found);
            kind = walk_rest_kind(vp, rc, cur.mnt, found, last_comp != 0, flags);
            rc = kind ? walk_link_ok(&rules, walk_magic(vp)) : 0;
            if (rc != 0) {
                node_put(cur.mnt, found);
                goto out_err;
            }
            if (kind == WALK_REST_OBJECT) {
                nocwd = 1;
                walk_name_text(&name, from, p);
                rc = walk_object(&cur, found, p, &linkpath, &target, locked);
            } else if (kind == WALK_REST_THROUGH) {
                rc = walk_rest_link(cur.mnt, found, from, p, &linkpath, &target, &links);
            } else if (kind == WALK_REST_LINK) {
                if (vp->kind == VNODE_PROC_EXE) {
                    walk_name_text(&name, from, p);
                    rc = walk_exe(&cur, found, locked, record ? record->entry : NULL);
                    if (rc != 0) {
                        goto out_err;
                    }
                    break;
                }
                rc = walk_rest_link(cur.mnt, found, from, p, &linkpath, &target, &links);
            } else {
                walk_name_text(&name, from, p);
                ref_move_node(&cur, found);
                break;
            }
            if (rc < 0) {
                goto out_err;
            }
            if (rc == WALK_AT_OBJECT) {
                continue;
            }
            kind = rc;
            rc = walk_target(&rules, &name, linkpath, kind);
            if (rc != 0) {
                goto out_err;
            }
            p = linkpath;
            if (last_comp) {
                last_comp = walk_last_comp(linkpath);
            }
            if (kind == 1) {
                if (walk_root_moved(&rules, &root)) {
                    goto restart;
                }
                rc = walk_move(&cur, root.mnt, root.node, locked);
                if (rc != 0) {
                    goto out_err;
                }
            }
            continue;
        }

        rc = walk_may_search(&cur, use_real, !nocwd, start, path, p);
        if (rc != 0) {
            goto out_err;
        }

        len = 0;
        while (p[len] != '\0' && p[len] != '/') {
            len++;
        }
        if (len > VFS_NAME_MAX) {
            rc = -ENAMETOOLONG;
            goto out_err;
        }
        k_memcpy(comp, p, len);
        comp[len] = '\0';
        p += len;

        if (comp[0] == '.' && comp[1] == '\0') {
            continue;
        }
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (walk_root_moved(&rules, &root)) {
                goto restart;
            }
            rc = walk_dotdot_ok(&rules, &cur, &root);
            if (rc != 0) {
                goto out_err;
            }
            if (dotdot_stays(&cur, &root)) {
                continue;
            }
            if (cur.node == cur.mnt->root && cur.mnt->covered) {
                p -= 2;
                rc = walk_move(&cur, cur.mnt->parent, cur.mnt->covered, locked);
                if (rc != 0) {
                    goto out_err;
                }
                continue;
            }
            if (cur.node == cur.mnt->root) {
                rc = nameless_up(cur.mnt, &name, &p, &last_comp, &linkpath, &target);
                rc = rc == 0 ? walk_move(&cur, cur.mnt->parent, cur.mnt->parent->root, locked) : rc;
                if (rc != 0) {
                    goto out_err;
                }
                continue;
            }
        }

        m = mount_named(cur.mnt, cur.node, comp);
        if (m) {
            walk_name_add(&name, comp, len);
            rc = walk_enter(&cur, m, locked);
            if (rc != 0) {
                goto out_err;
            }
            continue;
        }

        rc = walk_lookup(&rules, mount_base(cur.mnt), cur.node, comp, &found);
        if (walk_creates(rc, flags, p)) {
            k_memcpy(leaf, comp, len + 1); /* VFS_NAME_MAX + 1 bytes */
            missing = 1;
            break;
        }
        if (rc != 0) {
            goto out_err;
        }
        walk_name_add(&name, comp, len);
        if (cur.mnt->type->ops->readlink && cur.mnt->type->ops->getattr) {
            struct vfs_stat lst;
            const char *rest = p;
            int more;

            while (*rest == '/') {
                rest++;
            }
            more = *rest != '\0';
            rc = cur.mnt->type->ops->getattr(mount_base(cur.mnt), found, &lst);
            if (rc != 0) {
                node_put(cur.mnt, found);
                goto out_err;
            }
            if ((lst.mode & 0170000u) == 0120000u && (more || !(flags & VFS_WALK_NOFOLLOW))) {
                int absolute = walk_follow_ok(&rules, &cur, &lst, more, use_real);

                if (absolute != 0) {
                    node_put(cur.mnt, found);
                    rc = absolute;
                    goto out_err;
                }
                absolute = walk_link(cur.mnt, found, rest, &linkpath, &target, &links);

                if (absolute < 0) {
                    rc = absolute;
                    goto out_err;
                }
                walk_name_add(&name, "..", 2);
                rc = walk_target(&rules, &name, linkpath, absolute);
                if (rc != 0) {
                    goto out_err;
                }
                if (last_comp) {
                    last_comp = walk_last_comp(linkpath);
                }
                p = linkpath;
                if (absolute) {
                    if (walk_root_moved(&rules, &root)) {
                        goto restart;
                    }
                    rc = walk_move(&cur, root.mnt, root.node, locked);
                    if (rc != 0) {
                        goto out_err;
                    }
                }
                continue;
            }
        }
        rc = walk_record_move(record, &cur, found, comp, p);
        if (rc != 0) {
            goto out_err;
        }

        rc = walk_record_cross(record, &cur, locked);
        if (rc != 0) {
            goto out_err;
        }
    }

    rc = walk_finish(&cur, missing ? flags | VFS_WALK_PARENT : flags);
    rc = rc == 0 ? walk_end_ok(&rules, cur.mnt) : rc;
    if (rc != 0) {
        goto out_err;
    }
    path_buf_put(linkpath);
    path_buf_put(target);
    walk_name_end(&name);
    *out = cur;
    return missing ? VFS_MISSING : 0;

restart:
    walk_release(&cur, locked);
    goto begin;

out_err:
    walk_record_clear(record, 1);
    path_buf_put(linkpath);
    path_buf_put(target);
    walk_release(&cur, locked);
    return rc;
}

/**
 * Resolve one path, as `vfs_resolve` says: from the name cache alone
 * without the big lock (`resolve_cached`) when the walk records nothing,
 * has no scope, replay or parent and makes nothing, and otherwise, or when
 * that walk cannot finish, by the full walk under the big lock
 * (`resolve_full`).
 */
static int resolve_path(const struct vfs_ref *start, const char *path, int flags,
                        struct vfs_ref *out, char *leaf, uint32_t leafcap,
                        struct walk_record *record)
{
    int rc;

    if (path && out && !record && vfs_root_mounted() &&
        !(flags & ~(VFS_WALK_NOFOLLOW | VFS_WALK_REAL_IDS | VFS_WALK_LOCK)) &&
        !(current_proc && current_proc->walk_replay)) {
        rc = resolve_cached(start, path, flags, out, NULL);
        if (rc != WALK_FULL) {
            return rc;
        }
    }
    bkl_enter();
    rc = resolve_full(start, path, flags, out, leaf, leafcap, record);
    bkl_exit();
    return rc;
}

int vfs_resolve_cached(const char *path, int flags, struct vfs_ref *out, struct vfs_name **entry)
{
    int rc;

    if (entry) {
        *entry = NULL;
    }
    if (!path || path[0] != '/' || !vfs_root_mounted() ||
        (flags & ~(VFS_WALK_NOFOLLOW | VFS_WALK_REAL_IDS | VFS_WALK_LOCK)) ||
        (current_proc && current_proc->walk_replay)) {
        return VFS_CACHED_MISS;
    }
    rc = resolve_cached(0, path, flags, out, entry);
    return rc == WALK_FULL ? VFS_CACHED_MISS : rc;
}

int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t leafcap)
{
    return resolve_path(start, path, flags & ~VFS_WALK_CREATE, out, leaf, leafcap, 0);
}

/**
 * Put in the path page `out` the anchor's real path joined to `path`, for a
 * walk through an anchor of a type that takes the rest of a path: under
 * BENEATH and IN_ROOT with the path's `..`s applied in the text, where one
 * that would climb above the anchor is -EXDEV under BENEATH and stays at it
 * under IN_ROOT, and an absolute path is taken from the anchor; otherwise
 * as written. 0, or -ENAMETOOLONG.
 */
static int scope_text(const struct vfs_scope *scope, const char *path, char *out)
{
    int scoped = (scope->resolve & (VFS_RESOLVE_BENEATH | VFS_RESOLVE_IN_ROOT)) != 0;
    uint32_t len = scope->anchor_len;
    const char *from = path;

    k_memcpy(out, scope->anchor_real, len);
    while (*path) {
        const char *c;
        uint32_t clen;

        while (*path == '/') {
            path++;
        }
        c = path;
        while (*path && *path != '/') {
            path++;
        }
        clen = (uint32_t)(path - c);
        if (clen == 0 || (scoped && clen == 1 && c[0] == '.')) {
            continue;
        }
        if (scoped && clen == 2 && c[0] == '.' && c[1] == '.') {
            if (len == scope->anchor_len) {
                if (scope->resolve & VFS_RESOLVE_BENEATH) {
                    return -EXDEV;
                }
                continue;
            }
            while (out[len - 1] != '/') {
                len--;
            }
            len--;
            continue;
        }
        if (len + 1 + clen + 1 > VFS_PATH_MAX) {
            return -ENAMETOOLONG;
        }
        out[len] = '/';
        k_memcpy(out + len + 1, c, clen);
        len += 1 + clen;
    }
    if (path > from && path[-1] == '/' && len + 2 <= VFS_PATH_MAX) {
        out[len++] = '/';
    }
    out[len] = '\0';
    return 0;
}

int vfs_resolve_scoped(const struct vfs_scope *scope, const char *path, int flags,
                       struct vfs_ref *out, char *leaf, char *real)
{
    int scoped = (scope->resolve & (VFS_RESOLVE_BENEATH | VFS_RESOLVE_IN_ROOT)) != 0;
    struct walk_record record;
    char *joined;
    int rc;

    record.real = real;
    record.entry = NULL;
    record.scope = scope;
    record.dir = NULL;
    flags &= VFS_WALK_NOFOLLOW | VFS_WALK_LOCK | VFS_WALK_CREATE;
    if (path[0] == '/' && (scope->resolve & VFS_RESOLVE_BENEATH)) {
        return -EXDEV;
    }
    if (!scope->anchor.mnt->type->ops->lookup_rest ||
        scope->anchor.node == scope->anchor.mnt->root || (path[0] == '/' && !scoped)) {
        return resolve_path(0, path, flags, out, leaf, VFS_NAME_MAX + 1, &record);
    }
    joined = path_buf_get();
    if (!joined) {
        return -ENOMEM;
    }
    rc = scope_text(scope, path, joined);
    if (rc == 0) {
        rc = resolve_path(0, joined, flags | WALK_SYNTH, out, leaf, VFS_NAME_MAX + 1, &record);
    }
    path_buf_put(joined);
    return rc;
}

int vfs_resolve_real(const char *path, int flags, struct vfs_ref *out, char *real)
{
    return vfs_resolve_entry(path, flags, out, real, NULL, NULL);
}

int vfs_resolve_entry(const char *path, int flags, struct vfs_ref *out, char *real,
                      struct vfs_name **entry, struct vfs_stat *dir)
{
    struct walk_record record;

    record.real = real;
    record.entry = entry;
    record.scope = NULL;
    record.dir = dir;
    return resolve_path(0, path, flags & ~(VFS_WALK_PARENT | VFS_WALK_CREATE), out, 0, 0, &record);
}

void vfs_ref_release(struct vfs_ref *r)
{
    struct vfs_mount *mnt = r->mnt;

    vfs_ref_put(r);
    if (mnt) {
        vfs_leave(mnt);
    }
}

void vfs_desc_open(struct vfs_mount *mnt)
{
    vfs_mount_get(mnt);
}

static void reap_if_detached(const struct vfs_mount *mnt, uint32_t left);

void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    struct vfs_ref r;

    if (handle) {
        vfs_release(mnt, handle);
    }
    r.mnt = mnt;
    r.node = node;
    vfs_ref_put(&r);
    if (mnt->refs == 0) {
        k_panic("vfs: a description closed on a mount that had none");
    }
    reap_if_detached(mnt, vfs_mount_put(mnt));
}

/* Repointing, for a KRFFS relocation. */

/** `vfs_repoint` for one mount that shows the moving file system: the
 * working directories and roots on it, the mounts over its directories, its
 * own root, and its descriptions, loop devices and locks. */
static void repoint_one(struct vfs_mount *mnt, const void *from, void *to)
{
    process_t *pr;
    uint32_t flags;
    int i;

    for (pr = proc_list; pr; pr = pr->next) {
        if (!pr->fs) {
            continue;
        }
        flags = spin_lock_irqsave(&pr->fs->lock);
        if (pr->fs->cwd_mnt == mnt && pr->fs->cwd_node == from) {
            pr->fs->cwd_node = to;
        }
        if (pr->fs->root_mnt == mnt && pr->fs->root_node == from) {
            pr->fs->root_node = to;
        }
        spin_unlock_irqrestore(&pr->fs->lock, flags);
    }
    for (i = 0; i < mount_slots; i++) {
        if (!mounts[i] || mounts[i]->state == VFS_FREE) {
            continue;
        }
        if (mounts[i]->parent == mnt && mounts[i]->covered == from) {
            mounts[i]->covered = to;
        }
    }
    if (mnt->root == from) {
        mnt->root = to;
    }
    vfs_name_repoint(mnt, from, to);
    fd_ofd_repoint(mnt, from, to);
    loop_repoint(mnt, from, to);
    lock_repoint(mnt, (uint64_t)(uintptr_t)from, (uint64_t)(uintptr_t)to);
}

void vfs_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    process_t *pr;
    int i;

    for (pr = proc_list; pr; pr = pr->next) {
        if (mnt == mounts[0] && pr->fds) {
            fd_table_repoint(pr->fds, from, to);
        }
    }
    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE &&
            (mounts[i] == mnt || mounts[i]->bind_of == mnt)) {
            repoint_one(mounts[i], from, to);
        }
    }
}

/* The default-answer wrappers. */

int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (!mnt->type->ops->read) {
        return -EINVAL;
    }
    return mnt->type->ops->read(mount_base(mnt), handle, buf, count, offset);
}

int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset)
{
    int32_t n;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->write) {
        return -EROFS;
    }
    n = mnt->type->ops->write(mount_base(mnt), handle, buf, count, offset);
    filecache_forget(mnt, handle);
    return n;
}

int vfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (loop_backs(mnt, handle)) {
        return -EBUSY;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->truncate) {
        return -EROFS;
    }
    rc = mnt->type->ops->truncate(mount_base(mnt), handle, size);
    filecache_forget(mnt, handle);
    return rc;
}

static uint32_t guard_node(struct vfs_mount *mnt, void *node);

void vfs_cachestat(struct vfs_mount *mnt, void *node, uint64_t first, uint64_t last,
                   struct vfs_cachestat *cs)
{
    struct vfs_stat st;

    k_memset(cs, 0, sizeof(*cs));
    if (vfs_mount_gone(mnt) || !mnt->type->ops->cachestat || vfs_getattr(mnt, node, &st) != 0 ||
        st.size == 0) {
        return;
    }
    if (last > (st.size - 1u) >> 12) {
        last = (st.size - 1u) >> 12;
    }
    if (first <= last) {
        mnt->type->ops->cachestat(mount_base(mnt), node, first, last, cs);
    }
}

int vfs_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset, uint64_t len)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (loop_backs(mnt, handle)) {
        return -EBUSY;
    }
    if (mount_rdonly(mnt)) {
        return -EROFS;
    }
    if (!mnt->type->ops->fallocate) {
        return -EOPNOTSUPP;
    }
    /* The handle of a type with the operation is its node. */
    if ((guard_node(mnt, handle) & VFS_FL_IMMUTABLE) ||
        ((guard_node(mnt, handle) & VFS_FL_APPEND) && (mode & ~1u))) {
        return -EPERM; /* vfs_fallocate's rule */
    }
    rc = mnt->type->ops->fallocate(mount_base(mnt), handle, mode, offset, len);
    filecache_forget(mnt, handle);
    return rc;
}

/** The body of `vfs_fsync` and `vfs_fdatasync`: the type's operation `op`, or
 * the device's sync for a type with neither, then the device's flush. */
static int sync_handle(struct vfs_mount *mnt, void *handle,
                       int (*op)(struct vfs_mount *mnt, void *handle))
{
    int rc;

    if (!op) {
        rc = mnt->dev ? bcache_sync(mnt->dev) : 0;
    } else {
        rc = op(mount_base(mnt), handle);
    }
    if (rc == 0 && mnt->dev) {
        rc = block_flush(mnt->dev);
        if (rc != 0) {
            vfs_device_failed(mnt->dev, 0, rc);
        }
    }
    return rc;
}

int vfs_fsync(struct vfs_mount *mnt, void *handle)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    return sync_handle(mnt, handle, mnt->type->ops->fsync);
}

int vfs_fdatasync(struct vfs_mount *mnt, void *handle)
{
    const struct vfs_ops *ops = mnt->type->ops;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    return sync_handle(mnt, handle, ops->fdatasync ? ops->fdatasync : ops->fsync);
}

void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    int i;

    for (i = 0; i < mount_slots; i++) {
        if (!mounts[i] || mounts[i]->state == VFS_FREE || mounts[i]->dev != dev) {
            continue;
        }
        if (!mounts[i]->write_error) {
            mounts[i]->write_error = rc;
        }
        mounts[i]->flags |= VFS_MS_RDONLY;
        klog(KLOG_WARN, "vfs", "read-only: %s: write of sector %d failed (%d)",
             mounts[i]->covered_path, (int)lba, rc);
    }
}

int vfs_device_holds(block_device_t *dev)
{
    int i;
    int held = 0;

    for (i = 0; i < mount_slots; i++) {
        if (!mounts[i] || mounts[i]->state == VFS_FREE || mounts[i]->dev != dev) {
            continue;
        }
        held += 1 + (int)mounts[i]->refs + (int)mounts[i]->inprogress;
    }
    return held;
}

int vfs_sync_all(void)
{
    int i;

    for (i = 0; i < mount_slots; i++) {
        struct vfs_mount *m = mounts[i];

        if (!m || m->state != VFS_ACTIVE || !m->dev || m->bind_of || vfs_mount_gone(m) ||
            vfs_enter(m) != 0) {
            continue;
        }
        if (m->type->ops->sync) {
            m->type->ops->sync(m);
        }
        if (bcache_sync(m->dev) == 0) {
            int rc = block_flush(m->dev);

            if (rc != 0) {
                vfs_device_failed(m->dev, 0, rc);
            }
        }
        vfs_leave(m);
    }
    return 0;
}

int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (!mnt->type->ops->getattr) {
        return -EINVAL;
    }
    return mnt->type->ops->getattr(mount_base(mnt), node, st);
}

/* The attributes the wrappers read for inotify and for the inode flags.
 * Those inotify reports are read only while a watch exists
 * (`inotify_watches`). */

/** The attributes of `node`, into `st`; 0, or -1 when the type has no
 * `getattr` or it fails. */
static int watched_node(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    return mnt->type->ops->getattr && mnt->type->ops->getattr(mount_base(mnt), node, st) == 0 ? 0
                                                                                              : -1;
}

/** The attributes of the entry `name` of `dir`, into `st`; 0, or -1 when
 * there is none. */
static int watched_entry(struct vfs_mount *mnt, void *dir, const char *name, struct vfs_stat *st)
{
    void *found;
    int rc;

    if (!mnt->type->ops->lookup ||
        mnt->type->ops->lookup(mount_base(mnt), dir, name, &found) != 0) {
        return -1;
    }
    rc = watched_node(mnt, found, st);
    node_put(mnt, found);
    return rc;
}

/** The immutable and append-only flags of `node`, 0 for a type that keeps
 * no inode flags. */
static uint32_t guard_node(struct vfs_mount *mnt, void *node)
{
    struct vfs_stat st;

    if (watched_node(mnt, node, &st) != 0 || !st.flags_known) {
        return 0;
    }
    return st.flags & (VFS_FL_IMMUTABLE | VFS_FL_APPEND);
}

/** The immutable and append-only flags of the entry `name` of `dir`, 0 for
 * none or no such entry. */
static uint32_t guard_entry(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct vfs_stat st;

    if (watched_entry(mnt, dir, name, &st) != 0 || !st.flags_known) {
        return 0;
    }
    return st.flags & (VFS_FL_IMMUTABLE | VFS_FL_APPEND);
}

/** IN_ISDIR for the attributes `st` of a directory, else 0. */
static uint32_t watched_isdir(const struct vfs_stat *st)
{
    return (st->mode & 0170000u) == 0040000u ? IN_ISDIR : 0u;
}

/** Tell inotify, while a watch exists, that the entry `name` was made in
 * `dir`; `isdir` is IN_ISDIR for a directory, else 0. */
static void watch_made(struct vfs_mount *mnt, void *dir, const char *name, uint32_t isdir)
{
    struct vfs_stat st;

    if (inotify_watches && watched_node(mnt, dir, &st) == 0) {
        inotify_child(mnt, st.ino, name, IN_CREATE | isdir, 0);
    }
}

/**
 * Tell inotify that the entry `name` of the directory `dir_ino` is gone,
 * and `was` is what it named: a file's links changed, as Linux reports with
 * IN_ATTRIB, and a directory, or a file with no link left, is gone itself.
 * Linux reports that of a file at its last close; here it is at its last
 * link's removal.
 */
static void watch_removed(struct vfs_mount *mnt, uint64_t dir_ino, const char *name,
                          const struct vfs_stat *was)
{
    uint32_t isdir = watched_isdir(was);

    if (!isdir) {
        inotify_file(mnt, was->ino, IN_ATTRIB);
    }
    inotify_child(mnt, dir_ino, name, IN_DELETE | isdir, 0);
    if (isdir || was->nlink <= 1) {
        inotify_gone(mnt, was->ino);
    }
}

uint32_t vfs_setattr_events(int what)
{
    uint32_t mask = 0;

    if (what & (VFS_SET_MODE | VFS_SET_UID | VFS_SET_GID)) {
        mask |= IN_ATTRIB;
    }
    if (what & VFS_SET_SIZE) {
        mask |= IN_MODIFY;
    }
    if ((what & (VFS_SET_ATIME | VFS_SET_MTIME)) == (VFS_SET_ATIME | VFS_SET_MTIME)) {
        mask |= IN_ATTRIB;
    } else if (what & VFS_SET_ATIME) {
        mask |= IN_ACCESS;
    } else if (what & VFS_SET_MTIME) {
        mask |= IN_MODIFY;
    }
    return mask;
}

int vfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct vfs_stat now;
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((what & VFS_SET_SIZE) && loop_backs(mnt, node)) {
        return -EBUSY;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->setattr) {
        return -EROFS;
    }
    if ((what & ~VFS_SET_FLAGS) && guard_node(mnt, node)) {
        return -EPERM; /* notify_change's rule for an immutable or append-only file */
    }
    rc = mnt->type->ops->setattr(mount_base(mnt), node, what, st);
    if (what & VFS_SET_SIZE) {
        filecache_forget(mnt, node);
    }
    if (rc == 0 && inotify_watches && watched_node(mnt, node, &now) == 0) {
        inotify_file(mnt, now.ino, vfs_setattr_events(what));
    }
    return rc;
}

int vfs_open_node(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mnt->type->ops->open) {
        rc = mnt->type->ops->open(mount_base(mnt), node, flags, handle);
        if (rc != 0) {
            return rc;
        }
    } else {
        *handle = node;
    }
    mount_count_add(&mnt->opens, 1);
    return 0;
}

void vfs_release(struct vfs_mount *mnt, void *handle)
{
    if (mnt->type->ops->release) {
        mnt->type->ops->release(mount_base(mnt), handle);
    }
    mount_count_add(&mnt->releases, 1);
}

int vfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (!mnt->type->ops->readdir) {
        return -EINVAL;
    }
    k_memset(ent, 0, sizeof(*ent));
    return mnt->type->ops->readdir(mount_base(mnt), handle, cursor, ent);
}

int vfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->create) {
        return -EROFS;
    }
    rc = mnt->type->ops->create(mount_base(mnt), dir, name, mode, node);
    dcache_forget(mount_base(mnt), dir, name);
    if (rc == 0) {
        watch_made(mnt, dir, name, 0);
    }
    return rc;
}

int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct vfs_stat was, parent;
    int watched;
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->unlink) {
        return -EROFS;
    }
    if (guard_node(mnt, dir) || guard_entry(mnt, dir, name)) {
        return -EPERM; /* may_delete's rule */
    }
    watched = inotify_watches && watched_entry(mnt, dir, name, &was) == 0 &&
              watched_node(mnt, dir, &parent) == 0;
    vfs_name_change_begin();
    dcache_forget(mount_base(mnt), dir, name);
    rc = mnt->type->ops->unlink(mount_base(mnt), dir, name);
    dcache_forget(mount_base(mnt), dir, name);
    if (rc == 0) {
        vfs_name_unlink(mnt, dir, name);
    }
    vfs_name_change_end();
    if (rc == 0 && watched) {
        watch_removed(mnt, parent.ino, name, &was);
    }
    return rc;
}

int vfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->mkdir) {
        return -EROFS;
    }
    rc = mnt->type->ops->mkdir(mount_base(mnt), dir, name, mode);
    dcache_forget(mount_base(mnt), dir, name);
    if (rc == 0) {
        watch_made(mnt, dir, name, IN_ISDIR);
    }
    return rc;
}

int vfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct vfs_stat was, parent;
    void *victim;
    int watched;
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->rmdir) {
        return -EROFS;
    }
    if (guard_node(mnt, dir) || guard_entry(mnt, dir, name)) {
        return -EPERM; /* may_delete's rule */
    }
    watched = inotify_watches && watched_entry(mnt, dir, name, &was) == 0 &&
              watched_node(mnt, dir, &parent) == 0;
    victim = dcache_forget_victim(mount_base(mnt), dir, name);
    rc = mnt->type->ops->rmdir(mount_base(mnt), dir, name);
    dcache_forget(mount_base(mnt), dir, name);
    dcache_forget_dir(mount_base(mnt), victim);
    if (rc == 0 && watched) {
        watch_removed(mnt, parent.ino, name, &was);
    }
    return rc;
}

/**
 * Whether a rename may not take `oldname` out of `olddir` into `newname` of
 * `newdir`, Linux's may_delete and may_create: a flag on the old directory
 * or the name moved, an immutable new directory, or, when the new name has
 * a file, a flag on that file or an append-only new directory.
 */
static int rename_guarded(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                          const char *newname)
{
    struct vfs_stat st;
    uint32_t to = guard_node(mnt, newdir);

    if (guard_node(mnt, olddir) || guard_entry(mnt, olddir, oldname) || (to & VFS_FL_IMMUTABLE)) {
        return 1;
    }
    if (watched_entry(mnt, newdir, newname, &st) != 0) {
        return 0;
    }
    return (to & VFS_FL_APPEND) || guard_entry(mnt, newdir, newname);
}

int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
               const char *newname)
{
    struct vfs_stat moved, replaced, from, to;
    int watched, had_target = 0, same;
    void *victim;
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt) || !mnt->type->ops->rename) {
        return -EROFS;
    }
    if (rename_guarded(mnt, olddir, oldname, newdir, newname)) {
        return -EPERM;
    }
    watched = inotify_watches && watched_entry(mnt, olddir, oldname, &moved) == 0 &&
              watched_node(mnt, olddir, &from) == 0 && watched_node(mnt, newdir, &to) == 0;
    if (watched) {
        had_target =
            watched_entry(mnt, newdir, newname, &replaced) == 0 && replaced.ino != moved.ino;
    }
    vfs_name_change_begin();
    same = vfs_name_same(mnt, olddir, oldname, newdir, newname);
    dcache_forget(mount_base(mnt), olddir, oldname);
    victim = dcache_forget_victim(mount_base(mnt), newdir, newname);
    rc = mnt->type->ops->rename(mount_base(mnt), olddir, oldname, newdir, newname);
    dcache_forget(mount_base(mnt), olddir, oldname);
    dcache_forget(mount_base(mnt), newdir, newname);
    dcache_forget_dir(mount_base(mnt), victim);
    if (rc == 0 && !same) {
        vfs_name_rename(mnt, olddir, oldname, newdir, newname);
    }
    vfs_name_change_end();
    if (rc == 0 && watched) {
        uint32_t cookie = inotify_cookie();
        uint32_t isdir = watched_isdir(&moved);

        inotify_child(mnt, from.ino, oldname, IN_MOVED_FROM | isdir, cookie);
        inotify_child(mnt, to.ino, newname, IN_MOVED_TO | isdir, cookie);
        inotify_file(mnt, moved.ino, IN_MOVE_SELF);
        if (had_target) {
            if (!watched_isdir(&replaced)) {
                inotify_file(mnt, replaced.ino, IN_ATTRIB);
            }
            if (watched_isdir(&replaced) || replaced.nlink <= 1) {
                inotify_gone(mnt, replaced.ino);
            }
        }
    }
    return rc;
}

int vfs_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    struct vfs_stat st;
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt)) {
        return -EROFS;
    }
    if (!mnt->type->ops->link) {
        return -EPERM;
    }
    if (guard_node(mnt, node) || (guard_node(mnt, dir) & VFS_FL_IMMUTABLE)) {
        return -EPERM; /* vfs_link's rule */
    }
    rc = mnt->type->ops->link(mount_base(mnt), dir, name, node);
    dcache_forget(mount_base(mnt), dir, name);
    if (rc == 0 && inotify_watches && watched_node(mnt, node, &st) == 0) {
        inotify_file(mnt, st.ino, IN_ATTRIB);
        watch_made(mnt, dir, name, 0);
    }
    return rc;
}

int vfs_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt)) {
        return -EROFS;
    }
    if (!mnt->type->ops->symlink) {
        return -EPERM;
    }
    rc = mnt->type->ops->symlink(mount_base(mnt), dir, name, target);
    dcache_forget(mount_base(mnt), dir, name);
    if (rc == 0) {
        watch_made(mnt, dir, name, 0);
    }
    return rc;
}

int vfs_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, uint32_t rdev,
              void **node)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mount_rdonly(mnt)) {
        return -EROFS;
    }
    if (!mnt->type->ops->mknod) {
        return -EPERM;
    }
    rc = mnt->type->ops->mknod(mount_base(mnt), dir, name, mode, rdev, node);
    dcache_forget(mount_base(mnt), dir, name);
    if (rc == 0) {
        watch_made(mnt, dir, name, 0);
    }
    return rc;
}

int vfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (!mnt->type->ops->statfs) {
        return -EINVAL;
    }
    k_memset(st, 0, sizeof(*st));
    return mnt->type->ops->statfs(mount_base(mnt), st);
}

/* The operations of the KRFFS root, over the core in krffs.c. */

/* A node handle is the node's address in the mapped image, and the root
 * directory's is `krffs_fs->node`. Hold and put are NULL: the root counts no
 * holds, and the system call layer finds a node's holders by a scan
 * (`fd_node_holders` in syscall_fs.c). */

/** The `lookup` of the KRFFS root: `.`, `..`, which at the root directory
 * is the root directory, or a child by name, -ENOENT when there is none.
 * `dir` NULL is the root directory. */
static int kfs_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    krffs_node_t *d = dir ? dir : krffs_fs->node;
    krffs_node_t *n;

    (void)mnt;
    if (name[0] == '.' && name[1] == '\0') {
        *node = d;
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        n = krffs_parent_node(d);
        *node = n ? n : krffs_fs->node;
        return 0;
    }
    n = krffs_child(d, name);
    if (!n) {
        return -ENOENT;
    }
    *node = n;
    return 0;
}

/** The `getattr` of the KRFFS root: a node's mode, owner, size and times,
 * one link, and the inode number `krffs_node_ino` gives, as
 * `krffs_fill_stat64` reports them for the boot root. The root directory
 * reports mode 0755 whatever its stored mode says. */
static int kfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    krffs_node_t *n = node;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    if (n == krffs_fs->node) {
        /* The root's stored mode has the dirty flag, KRFFS_MODE_DIRTY. */
        st->mode = VFS_S_IFDIR | 0755u;
    } else {
        st->mode = krffs_mode_type(n->mode) | (uint32_t)(n->mode & 07777u);
    }
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = 1;
    st->ino = krffs_node_ino(n);
    st->size = n->data_size;
    st->atime_sec = n->access_time.seconds;
    st->atime_nsec = n->access_time.nanoseconds;
    st->mtime_sec = n->modification_time.seconds;
    st->mtime_nsec = n->modification_time.nanoseconds;
    st->ctime_sec = n->attribute_change_time.seconds;
    st->ctime_nsec = n->attribute_change_time.nanoseconds;
    return 0;
}

/** The `statfs` of the KRFFS root: blocks of 1 KiB, the image's total and
 * free bytes in them, and the longest name the format keeps. */
static int kfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    (void)mnt;
    st->magic = 0x4B524646u; /* "KRFF" */
    st->bsize = 1024;
    st->blocks = krffs_total_bytes() / 1024u;
    st->bfree = krffs_free_bytes() / 1024u;
    st->namelen = KRFFS_FILE_NAME_BUFFER_SIZE - 1;
    return 0;
}

/** The `sync` of the KRFFS root: write the dirty pages of the page cache
 * (`pc_sync`); 0, or -EIO when a page failed or was busy. */
static int kfs_sync(struct vfs_mount *mnt)
{
    (void)mnt;
    return pc_sync() == 0 ? 0 : -EIO;
}

/** Read `count` bytes at `offset` of the file whose node is `handle`, with
 * the mount's lock held: the bytes read, 0 past the end, or -EIO. */
static int32_t kfs_read_locked(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                               uint64_t offset)
{
    int n;

    (void)mnt;
    if (offset > 0xFFFFFFFFu) {
        return 0; /* past the end of any KRFFS file */
    }
    n = krffs_read_node(handle, buf, count, (uint32_t)offset);
    return n < 0 ? -EIO : n;
}

/** Write `count` bytes at `offset` of the file whose node is `handle`, with
 * the mount's lock held: the bytes written, -EFBIG past 4 GiB, -ENOSPC, or
 * -EIO. A grow can relocate the node; every holder, this writer's
 * description included, is repointed before the byte count returns. */
static int32_t kfs_write_locked(struct vfs_mount *mnt, void *handle, const void *buf,
                                uint32_t count, uint64_t offset)
{
    krffs_node_t *n = handle;
    int written;

    if (offset > 0xFFFFFFFFu || count > 0xFFFFFFFFu - offset) {
        return -EFBIG;
    }
    written = krffs_write_node(&n, buf, count, (uint32_t)offset);
    if (written == KRFFS_ENOSPACE) {
        return -ENOSPC;
    }
    if (written < 0) {
        return -EIO;
    }
    if (n != (krffs_node_t *)handle) {
        vfs_repoint(mnt, handle, n);
    }
    return written;
}

/** Truncate the file whose node is `handle` to `size`, with the mount's lock
 * held, and repoint its holders if the node moves, even when the truncation
 * then fails: 0, -EFBIG past 4 GiB, -ENOSPC, or -EIO. */
static int kfs_truncate_locked(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    krffs_node_t *n = handle;
    int rc;

    if (size > 0xFFFFFFFFu) {
        return -EFBIG;
    }
    rc = krffs_truncate_node(&n, (uint32_t)size);
    if (n && n != (krffs_node_t *)handle) {
        vfs_repoint(mnt, handle, n);
    }
    if (rc == KRFFS_ENOSPACE) {
        return -ENOSPC;
    }
    return rc == 0 ? 0 : -EIO;
}

/** The `fsync` of the KRFFS root, with the mount's lock held: the page cache
 * has no write of one file, so every dirty page is written, as `kfs_sync`
 * writes them; 0, or -EIO. */
static int kfs_fsync_locked(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    return pc_sync() == 0 ? 0 : -EIO;
}

/**
 * The entry of the directory `handle` at or after `*cursor`, with the
 * mount's lock held: 1 with `ent` filled, or 0 at the end.
 *
 * The cursor is the entry node's byte offset in the image plus one, so a
 * deletion between calls never renumbers the survivors; the traversal
 * visits nodes in ascending offsets, so the first match at or past the
 * cursor is the next entry. An orphan, a node unlinked while it is held,
 * keeps its parent id but has no name, and is left out, since no path
 * reaches it.
 */
static int kfs_readdir_locked(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                              struct vfs_dirent *ent)
{
    krffs_node_t *dirn = handle;
    uint64_t dir_id = (dirn == krffs_fs->node) ? (uint64_t)KRFFS_ROOT_ID : dirn->id;
    krffs_node_t *node = krffs_fs->node;

    (void)mnt;
    do {
        uint32_t node_off = (uint32_t)((uintptr_t)node - (uintptr_t)krffs_fs->node);

        if (node->type == KRFFS_RESERVED_NODE && node->parent_id == dir_id &&
            node->name[0] != '\0' && node_off >= *cursor) {
            uint32_t nlen = (uint32_t)k_strlen((const char *)node->name);

            if (nlen > VFS_NAME_MAX) {
                nlen = VFS_NAME_MAX;
            }
            k_memcpy(ent->name, node->name, nlen);
            ent->name[nlen] = '\0';
            ent->ino = krffs_node_ino(node);
            ent->is_dir = KRFFS_IS_DIR(node->mode) ? 1 : 0;
            ent->type = (int)VFS_DT_OF(krffs_mode_type(node->mode));
            *cursor = node_off + 1;
            return 1;
        }
    } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);
    return 0;
}

/** Join `name` to the path of the directory `dir`, NULL for the root
 * directory, in `path` of `cap` bytes: the namespace operations call the
 * core by path. 0, or -ENAMETOOLONG. */
static int kfs_join(void *dir, const char *name, char *path, uint32_t cap)
{
    krffs_node_t *d = dir ? dir : krffs_fs->node;
    uint32_t dlen, nlen;

    if (krffs_node_path(d, path, cap) != 0) {
        return -ENAMETOOLONG;
    }
    dlen = (uint32_t)k_strlen(path);
    nlen = (uint32_t)k_strlen(name);
    if (dlen == 0 || path[dlen - 1] != '/') {
        if (dlen + 1 >= cap) {
            return -ENAMETOOLONG;
        }
        path[dlen++] = '/';
    }
    if (dlen + nlen + 1 > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(path + dlen, name, nlen + 1);
    return 0;
}

/** The `unlink` of the KRFFS root, of `name` in `dir` by its path: 0,
 * -ENAMETOOLONG, or -EIO when the core fails. */
static int kfs_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    char path[256];
    int rc = kfs_join(dir, name, path, sizeof(path));

    (void)mnt;
    if (rc != 0) {
        return rc;
    }
    return krffs_unlink(path + (path[0] == '/')) == 0 ? 0 : -EIO;
}

/**
 * The `mkdir` of the KRFFS root, of `name` in `dir`. The parent is resolved
 * and held, so the core fails for one of two reasons: a name whose
 * directory identifier another directory has (`KRFFS_ETAKEN`), which is
 * EINVAL, the answer for a name the file system cannot hold, and a root
 * with no room, ENOSPC. `mode` is not used: the system call layer sets the
 * new node's mode and owner afterwards (`krffs_stamp_new_node`).
 */
static int kfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    char path[256];
    int rc = kfs_join(dir, name, path, sizeof(path));

    (void)mnt;
    (void)mode;
    if (rc != 0) {
        return rc;
    }
    rc = krffs_mkdir(path);
    if (rc == KRFFS_ETAKEN) {
        return -EINVAL;
    }
    return rc == 0 ? 0 : -ENOSPC;
}

/** The `rmdir` of the KRFFS root, of `name` in `dir`: 0, -ENAMETOOLONG, or
 * -ENOTEMPTY. The caller has resolved the name to a directory, so emptiness
 * is the one thing left for the core to fail on. */
static int kfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    char path[256];
    int rc = kfs_join(dir, name, path, sizeof(path));

    (void)mnt;
    if (rc != 0) {
        return rc;
    }
    return krffs_rmdir(path) == 0 ? 0 : -ENOTEMPTY;
}

/** The `rename` of the KRFFS root, by the two paths: 0, -ENAMETOOLONG, or
 * -EIO when the core fails. */
static int kfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                      const char *newname)
{
    char oldp[256];
    char newp[256];
    int rc;

    (void)mnt;
    rc = kfs_join(olddir, oldname, oldp, sizeof(oldp));
    if (rc == 0) {
        rc = kfs_join(newdir, newname, newp, sizeof(newp));
    }
    if (rc != 0) {
        return rc;
    }
    return krffs_rename(oldp + (oldp[0] == '/'), newp + (newp[0] == '/')) == 0 ? 0 : -EIO;
}

/** The `create` of the KRFFS root: the regular file `name` in `dir`, made by
 * its path, and its node in `*node`. 0, -ENAMETOOLONG, -ENOSPC when the core
 * cannot make it, or -EIO when the name is not found after. `mode` is not
 * used: the system call layer sets the new node's mode and owner afterwards
 * (`krffs_stamp_new_node`). */
static int kfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                      void **node)
{
    char path[256];
    krffs_node_t *n;
    int rc;

    (void)mnt;
    (void)mode;
    rc = kfs_join(dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    if (krffs_create(path + (path[0] == '/')) != 0) {
        return -ENOSPC;
    }
    n = krffs_child(dir ? dir : krffs_fs->node, name);
    if (!n) {
        return -EIO;
    }
    *node = n;
    return 0;
}

/**
 * The `mknod` of the KRFFS root: a FIFO or a socket, made as `kfs_create`
 * makes a file, with its type bits on the medium before its name. A device
 * node has a number KRFFS has no field for, and is -EPERM. A name already
 * there is -EEXIST, checked first, since the core's create succeeds for an
 * existing name and the node found after would be that name's.
 */
static int kfs_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                     uint32_t rdev, void **node)
{
    uint32_t type = mode & KRFFS_S_IFMT;
    char path[256];
    krffs_node_t *n;
    int rc;

    (void)mnt;
    (void)rdev;
    if (type != KRFFS_S_IFIFO && type != KRFFS_S_IFSOCK) {
        return -EPERM;
    }
    if (krffs_child(dir ? dir : krffs_fs->node, name)) {
        return -EEXIST;
    }
    rc = kfs_join(dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    if (krffs_create_node(path + (path[0] == '/'), (uint16_t)(type | (mode & 07777u))) != 0) {
        return -ENOSPC;
    }
    n = krffs_child(dir ? dir : krffs_fs->node, name);
    if (!n) {
        return -EIO;
    }
    *node = n;
    return 0;
}

/*
 * The data operations take the mount's lock themselves; the namespace
 * operations are called with it held, and a description's lock
 * (`fd_ofd_lock`) may hold it already for a data operation, which then takes
 * it again. The disk driver sleeps for its commands inside a write, and a
 * second process that grows a file of its own meanwhile moves nodes under
 * the first: without the lock, a file whose node moved would be written at
 * the old place, over the header of the node after it. The lock is the same
 * one the namespace operations hold, so a create and a write cannot
 * interleave either.
 */

/** The `read` of the KRFFS root: `kfs_read_locked` under the lock. */
static int32_t kfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                        uint64_t offset)
{
    int32_t rc;

    vfs_ns_lock(mnt);
    rc = kfs_read_locked(mnt, handle, buf, count, offset);
    vfs_ns_unlock(mnt);
    return rc;
}

/** The `write` of the KRFFS root: `kfs_write_locked` under the lock. */
static int32_t kfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                         uint64_t offset)
{
    int32_t rc;

    vfs_ns_lock(mnt);
    rc = kfs_write_locked(mnt, handle, buf, count, offset);
    vfs_ns_unlock(mnt);
    return rc;
}

/** The `truncate` of the KRFFS root: `kfs_truncate_locked` under the lock. */
static int kfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    int rc;

    vfs_ns_lock(mnt);
    rc = kfs_truncate_locked(mnt, handle, size);
    vfs_ns_unlock(mnt);
    return rc;
}

/** The `fsync` of the KRFFS root: `kfs_fsync_locked` under the lock. */
static int kfs_fsync(struct vfs_mount *mnt, void *handle)
{
    int rc;

    vfs_ns_lock(mnt);
    rc = kfs_fsync_locked(mnt, handle);
    vfs_ns_unlock(mnt);
    return rc;
}

/** The `readdir` of the KRFFS root: `kfs_readdir_locked` under the lock. */
static int kfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                       struct vfs_dirent *ent)
{
    int rc;

    vfs_ns_lock(mnt);
    rc = kfs_readdir_locked(mnt, handle, cursor, ent);
    vfs_ns_unlock(mnt);
    return rc;
}

static const struct vfs_ops krffs_ops = {
    kfs_lookup,  0, /* lookup_rest */
    0,              /* hold */
    0,              /* put */
    kfs_getattr, 0, /* setattr: the syscall layer sets KRFFS attributes */
    0,              /* open: the node is the handle */
    0,              /* release */
    kfs_read,    kfs_write,   kfs_truncate,
    kfs_fsync,   kfs_readdir, kfs_create,
    kfs_unlink,  kfs_mkdir,   kfs_rmdir,
    kfs_rename,  0,              /* link: KRFFS has none */
    0,                           /* readlink: KRFFS has none */
    kfs_statfs,  kfs_sync,    0, /* prepare_unmount: the root is permanent */
    0,                           /* abort_unmount */
    0,                           /* unmount */
    0,                           /* path: vfs_node_path reads the core's */
    0,                           /* symlink */
    kfs_mknod,   0,              /* pin: the syscall layer's holders keep a node */
    0,                           /* unpin */
    0,                           /* remount */
    0,                           /* fallocate */
    0,                           /* fdatasync */
    0,                           /* lock_node */
    0,                           /* unlock_node */
    0,                           /* shutdown */
    0,                           /* xattr */
    0,                           /* cachestat */
    0,                           /* put_unless_last */
    0,                           /* readlink_cached */
};

/** The constructor of the root's entry: the mount's root is the image's root
 * directory; -ENOENT when no image is up. */
static int kfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                     const char *options)
{
    (void)dev;
    (void)flags;
    (void)options;
    if (!krffs_fs) {
        return -ENOENT;
    }
    mnt->root = krffs_fs->node;
    return 0;
}

const struct vfs_type vfs_krffs_type = {"krffs", kfs_mount, &krffs_ops, 1, 0, 0, 0, 0};

/* The three synthetic types, of /dev, /proc and /sys. */

/*
 * A synthetic node is a classified path. The classification has a device
 * leaf name in it, which a value packed into the pointer cannot, so the
 * nodes are records from the heap and hold/put count references to them.
 * The open that follows a resolution sets up its descriptor from the
 * classification, so a node seldom outlives its system call; a mount's root
 * is held for the life of the mount.
 */
struct synth_node {
    struct vpath vp;
    int refs; /* the holds; the last put frees the record */
};

/** A new node of the classification `vp`, with one hold, in `*node`: 0, or
 * -ENOMEM, with a record in the kernel log, when the heap has no room. */
static int synth_take(const struct vpath *vp, void **node)
{
    struct synth_node *n = kzalloc(sizeof(*n));

    if (!n) {
        klog(KLOG_WARN, "vfs", "synthetic node: no memory");
        return -ENOMEM;
    }
    n->vp = *vp;
    n->refs = 1;
    *node = n;
    return 0;
}

/** The `hold` of the synthetic types: one more reference to `node`. */
static void synth_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct synth_node *)node)->refs++;
}

/** The `put` of the synthetic types: one reference fewer, and the record
 * freed with the last. */
static void synth_put(struct vfs_mount *mnt, void *node)
{
    struct synth_node *n = node;

    (void)mnt;
    if (n->refs > 0 && --n->refs == 0) {
        kfree(n);
    }
}

/**
 * Whether the file the VNODE_ENOTDIR path `vp` goes on past is there: its
 * process or thread, its pty, its device of /dev, or the device of its /sys
 * directory, as far as `vp` names one. A path past a file that is gone is
 * ENOENT, as on Linux, and not ENOTDIR.
 */
static int synth_past_present(const struct vpath *vp)
{
    if (vp->pid != 0 && procfs_resolve_id(vp) < 0) {
        return 0;
    }
    if (vp->pts_index >= 0 && !pty_get(vp->pts_index)) {
        return 0;
    }
    if (vp->name[0] != '\0' && devfs_stat(vp->name, 0, 0, 0) != 0) {
        return 0;
    }
    return !SYSFS_KEYED(vp->sysent) || sysfs_device_exists(vp->sysent, vp->syskey);
}

/**
 * Give `vp`, the classification of the path `full`, the entry root made
 * under /dev that the path names (devtree.h), when the classifier gave the
 * path to a device devfs does not have, or to a path past a file that is
 * not there: VNODE_DEV_MADE with the entry's id in `syskey`. A path in
 * /dev/pts, a device that is there and a path past a file that is there
 * keep their classification, and a path that goes on past a made link,
 * which the walk follows before, is VNODE_ENOENT.
 */
static void synth_made(const char *full, struct vpath *vp)
{
    int id;

    if (full[0] != 'd' || full[1] != 'e' || full[2] != 'v' || full[3] != '/' ||
        (full[4] == 'p' && full[5] == 't' && full[6] == 's' &&
         (full[7] == '\0' || full[7] == '/')) ||
        (vp->kind != VNODE_DEV && vp->kind != VNODE_ENOTDIR) ||
        (vp->kind == VNODE_DEV && devfs_stat(vp->name, 0, 0, 0) == 0) ||
        (vp->kind == VNODE_ENOTDIR && synth_past_present(vp))) {
        return;
    }
    id = devtree_resolve(full + 4, (uint32_t)k_strlen(full + 4));
    if (id > 0) {
        vp->kind = VNODE_DEV_MADE;
        vp->syskey = id;
        vp->name[0] = '\0';
    } else if (id == -ENOTDIR) {
        vp->kind = VNODE_ENOENT;
    }
}

/** Whether the synthetic node `vp` is a link the walk follows to the object
 * it refers to, not to a text: /proc/PID/fd/N, which /dev/fd/N, /dev/stdin,
 * /dev/stdout and /dev/stderr are too, and /proc/PID/cwd and root. */
static int synth_object_link(const struct vpath *vp)
{
    return vp->kind == VNODE_PROC_FD_LINK || vp->kind == VNODE_PROC_CWD ||
           vp->kind == VNODE_PROC_ROOT;
}

/** Whether the synthetic node is /proc/self, a text link to the caller. */
static int synth_self_link(const struct vpath *vp)
{
    return vp->kind == VNODE_PROC_PID && vp->pid == SYNTH_PID_SELF && vp->tid == 0;
}

/** The length of the first `out` bytes of `path` without their last name
 * and the slash before it: what `..` leaves of a path resolved as text. */
static uint32_t text_parent(const char *path, uint32_t out)
{
    while (out > 0 && path[out - 1] != '/') {
        out--;
    }
    return out > 0 ? out - 1 : 0;
}

/** The nameless mount entered from the root directory of `mnt` by the
 * names text[0..len), when they are more than one, as /sys/kernel/debug is
 * by kernel/debug from /sys's; or NULL. */
static struct vfs_mount *mount_deep_at(struct vfs_mount *mnt, const char *text, uint32_t len)
{
    struct vfs_mount *m = mount_named_at(mnt, text, len);

    return m && mount_deep(m) ? m : 0;
}

/**
 * Whether the part of a path classified so far, the `out` bytes of `full`,
 * names a link the walk follows, to its object (`synth_object_link`) or a
 * link made under /dev to its text, and more of the rest, from `p` to `end`,
 * comes after it: VFS_LINK with a new node of the link held in `*node`, 0
 * when it does not, or -ENOMEM. A node of /proc passed on the way whose
 * inode keeps its owner, a process's fd, fdinfo or task directory or one of
 * its links among them, gets its current owner (`procfs_lookup_owner`), as
 * Linux's lookup_fast revalidates each component it reaches (Linux 6.12
 * fs/namei.c:1694-1706), with pid_revalidate (fs/proc/base.c:2053-2066) or
 * tid_fd_revalidate (fs/proc/fd.c:145-161), before it follows a link and
 * even when a later lookup fails.
 */
static int synth_link_part(char *full, uint32_t out, const char *p, const char *end, void **node)
{
    struct vpath vp;

    while (p < end && *p == '/') {
        p++;
    }
    if (p == end) {
        return 0;
    }
    full[out] = '\0';
    synth_classify(full, &vp);
    synth_made(full, &vp);
    procfs_lookup_owner(&vp);
    if (!synth_object_link(&vp) && !synth_self_link(&vp) &&
        !(vp.kind == VNODE_DEV_MADE && devtree_is_dir(vp.syskey) == 0)) {
        return 0;
    }
    return synth_take(&vp, node) != 0 ? -ENOMEM : VFS_LINK;
}

/**
 * The rest of a path through a synthetic namespace whose root directory
 * `prefix` names, "dev", "proc" or "sys", its first `restlen` bytes. A `..`
 * inside it is resolved by the text, since the namespace is a table
 * classified by string. A `..` that would climb above the mount root returns
 * VFS_CLIMB with `*left` at that `..`, which the walk takes at the mount
 * root. A name at the mount root that a nameless mount is entered by, as
 * `shm` in /dev after `pts/..`, returns VFS_ROOT with `*left` at that name;
 * the names of one entered by more than one, as `kernel/debug` in /sys,
 * return VFS_ENTER with that mount in `*node` and `*left` past them.
 * A part that names a link the walk follows to its object and has more of
 * the rest after it returns VFS_LINK (`synth_link_part`), with a new node of
 * the link held and `*left` after its name, so that a `..` after the link
 * climbs from the object. Otherwise the whole path,
 * `prefix` and the rest without its `.` and `..`, is classified
 * (`synth_classify`): 0 with a new node held, -ENOENT, also for a
 * directory of /dev that is absent (`devfs_subdir_present`), -ENOTDIR for a
 * path past a file that is there (`synth_past_present`), -ENAMETOOLONG for
 * a path longer than 255 bytes, or -ENOMEM. The node found last gets its
 * current owner as each one passed on the way does (`synth_link_part`), so
 * that readlink(2) and a stat or open that follows a /proc link update the
 * link's kept owner, as Linux's walk revalidates its last component.
 */
static int synth_rest(struct vfs_mount *mnt, const char *prefix, const char *rest, uint32_t restlen,
                      void **node, const char **left)
{
    char full[256];
    struct vpath vp;
    struct vfs_mount *deep;
    uint32_t plen = (uint32_t)k_strlen(prefix);
    uint32_t out;
    const char *p = rest;
    const char *end = rest + restlen;
    int kind;

    out = plen;
    k_memcpy(full, prefix, out);

    while (p < end) {
        const char *comp;
        uint32_t len;

        while (p < end && *p == '/') {
            p++;
        }
        comp = p;
        while (p < end && *p != '/') {
            p++;
        }
        len = (uint32_t)(p - comp);
        if (len == 0) {
            break;
        }
        if (out == plen && mount_named_at(mnt, comp, len)) {
            *left = comp;
            return VFS_ROOT;
        }
        if (len == 1 && comp[0] == '.') {
            continue;
        }
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            if (out <= plen) {
                *left = comp;
                return VFS_CLIMB;
            }
            out = text_parent(full, out);
            continue;
        }
        if (out + 1 + len + 1 > sizeof(full)) {
            return -ENAMETOOLONG;
        }
        full[out++] = '/';
        k_memcpy(full + out, comp, len);
        out += len;
        deep = mount_deep_at(mnt, full + plen + 1, out - plen - 1);
        if (deep) {
            *node = deep;
            *left = p;
            return VFS_ENTER;
        }
        kind = synth_link_part(full, out, p, end, node);
        if (kind != 0) {
            *left = p;
            return kind;
        }
    }
    full[out] = '\0';

    synth_classify(full, &vp);
    synth_made(full, &vp);
    kind = vp.kind;
    if (kind == VNODE_ENOENT) {
        return -ENOENT;
    }
    if (kind == VNODE_ENOTDIR) {
        return synth_past_present(&vp) ? -ENOTDIR : -ENOENT;
    }
    if (kind == VNODE_DEV_SUBDIR && !devfs_subdir_present(vp.devdir)) {
        return -ENOENT;
    }
    procfs_lookup_owner(&vp);
    return synth_take(&vp, node);
}

static int synth_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st);

/** The path of the root directory of the synthetic file system `mnt` shows,
 * as `synth_classify` takes it: "dev", "dev/pts" for a devpts mount, whose
 * root is that directory, "proc" or "sys". */
static const char *synth_base(struct vfs_mount *mnt)
{
    struct vfs_mount *base = mount_base(mnt);

    if (base->type == &vfs_procfs_type) {
        return "proc";
    }
    if (base->type == &vfs_sysfs_type) {
        return "sys";
    }
    return ((struct synth_node *)base->root)->vp.kind == VNODE_PTS_DIR ? "dev/pts" : "dev";
}

/**
 * Put in `out`, of `cap` bytes, the path `synth_classify` takes of the place
 * `inner` within the synthetic file system of `mnt`, a path as a bind keeps
 * it: `synth_base` and `inner` after it, "/" being nothing. 0, or
 * -ENAMETOOLONG.
 */
static int synth_place(struct vfs_mount *mnt, const char *inner, char *out, uint32_t cap)
{
    const char *base = synth_base(mnt);
    uint32_t bl = (uint32_t)k_strlen(base);

    if (inner[0] == '/' && inner[1] == '\0') {
        inner = "";
    }
    if (bl + k_strlen(inner) + 1 > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(out, base, bl);
    k_strncpy(out + bl, inner, cap - bl);
    return 0;
}

/**
 * Whether the place `inner` within the synthetic file system of `mnt`, as
 * `synth_place` takes it, is classified as the node `node` of that file
 * system is. The walk's record of a place reached through a link to an
 * object, such as /proc/PID/root, names some other place.
 */
static int synth_names(struct vfs_mount *mnt, const void *node, const char *inner)
{
    const struct vpath *a = &((const struct synth_node *)node)->vp;
    char path[256];
    struct vpath b;

    if (synth_place(mnt, inner, path, sizeof(path)) != 0) {
        return 0;
    }
    synth_classify(path, &b);
    return a->kind == b.kind && a->pid == b.pid && a->tid == b.tid && a->procfd == b.procfd &&
           a->procfile == b.procfile && a->pts_index == b.pts_index && a->sysent == b.sysent &&
           a->syskey == b.syskey && streq(a->name, b.name);
}

/**
 * The `lookup_rest` of the synthetic types: `synth_rest` below the root of
 * the mount `mnt`, under the root directory of its file system, or, for a
 * bind of a directory inside it, such as /proc/sys, under the directory the
 * bind shows, at its `bind_root` (`synth_place`). ENOTDIR below a bind of a
 * file, a device of /dev among them, which has nothing below it;
 * ENAMETOOLONG for a directory whose path leaves no room in a classified
 * path.
 */
static int synth_lookup_rest(struct vfs_mount *mnt, const char *rest, uint32_t len, void **node,
                             const char **left)
{
    char prefix[256];
    struct vfs_stat st;
    int rc;

    synth_getattr(mnt, mnt->root, &st);
    if (!VFS_IS_DIR(st.mode)) {
        return -ENOTDIR;
    }
    rc = synth_place(mnt, mnt->bind_of ? mnt->bind_root : "", prefix, sizeof(prefix));
    return rc != 0 ? rc : synth_rest(mnt, prefix, rest, len, node, left);
}

/** Whether the synthetic kind denotes a directory for the resolver. */
static int synth_dir_kind(int kind)
{
    return kind == VNODE_DEV_DIR || kind == VNODE_PTS_DIR || kind == VNODE_PROC_DIR ||
           kind == VNODE_PROC_NET_DIR || kind == VNODE_PROC_PID || kind == VNODE_PROC_FD_DIR ||
           kind == VNODE_PROC_TASK_DIR || kind == VNODE_SYS_DIR || kind == VNODE_SYS_LINK ||
           kind == VNODE_PROC_SYSVIPC_DIR || kind == VNODE_PROC_NET_STAT_DIR ||
           kind == VNODE_PROC_NS_DIR || kind == VNODE_PROC_FDINFO_DIR || kind == VNODE_DEV_SUBDIR ||
           kind == VNODE_PROC_ACPI_DIR || kind == VNODE_PROC_FS_DIR || kind == VNODE_PROC_SYS_DIR;
}

/** The `getattr` of the synthetic types, what the resolver needs of a node:
 * whether it is a directory or a link it follows, /proc/<pid>/exe, cwd and
 * root, /dev/log and a link made under /dev, and the mode the search rule
 * reads (0755 under /dev, 0555 under /proc and /sys, all root's, and an
 * entry's own mode and owner made under /dev, which is -ENOENT once it is
 * removed). /proc/self is a text link to the caller's process directory.
 * A device's link under /sys is classified as the directory it leads to. The
 * system call layer reports the full attributes itself. */
static int synth_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    const struct synth_node *sn = node;
    int kind = sn->vp.kind;

    k_memset(st, 0, sizeof(*st));
    if (kind == VNODE_DEV_MADE) {
        struct devtree_attr a;

        if (devtree_attr(sn->vp.syskey, &a) != 0) {
            return -ENOENT;
        }
        st->mode = a.mode;
        st->nlink = a.nlink;
        st->uid = a.uid;
        st->gid = a.gid;
        return 0;
    }
    if (synth_self_link(&sn->vp)) {
        st->mode = 0120000u | 0777u;
    } else if (synth_dir_kind(kind)) {
        st->mode = VFS_S_IFDIR | (mnt == mounts[1] ? 0755u : 0555u);
    } else if (kind == VNODE_PROC_EXE || kind == VNODE_PROC_CWD || kind == VNODE_PROC_ROOT ||
               kind == VNODE_DEV_LINK) {
        st->mode = 0120000u | 0777u;
    } else {
        st->mode = VFS_S_IFREG | 0444u;
    }
    st->nlink = 1;
    return 0;
}

/** The `readlink` of the synthetic types, the target of a link as text:
 * /proc/self's, the caller's visible pid; /dev/log's, fixed; a link's made
 * under /dev, as it was written;
 * /proc/<pid>/exe's, the path of the program the process
 * runs (`procfs_readlink_exe`); and /proc/<pid>/cwd's and root's, the paths
 * of its directories (`procfs_readlink_dir`). -ENOENT for a process that is
 * gone, -EINVAL for a node that is no link. The resolver goes on from exe's
 * program file itself (`walk_exe`), from the directories of cwd and root
 * themselves (`walk_object`), and from the text of /dev/log, and readlink(2)
 * reads the text through procfs. */
static int synth_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    const struct synth_node *sn = node;
    int pid;

    (void)mnt;
    if (sn->vp.kind == VNODE_DEV_MADE) {
        int n = devtree_readlink(sn->vp.syskey, buf, cap);

        return n > (int)cap ? (int)cap : n;
    }
    if (sn->vp.kind == VNODE_DEV_LINK) {
        uint32_t n = (uint32_t)k_strlen(SYNTH_DEV_LOG_TARGET);

        n = n < cap ? n : cap;
        k_memcpy(buf, SYNTH_DEV_LOG_TARGET, n);
        return (int)n;
    }
    pid = procfs_resolve_id(&sn->vp);
    if (synth_self_link(&sn->vp)) {
        char digits[12];
        uint32_t value = pid < 0 ? 0 : pid_seen((uint32_t)pid);
        uint32_t n = 0, len = 0;

        if (pid < 0) {
            return -ENOENT;
        }
        do {
            digits[n++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value);
        while (n && len < cap) {
            buf[len++] = digits[--n];
        }
        return (int)len;
    }
    if (pid < 0 && (sn->vp.kind == VNODE_PROC_EXE || sn->vp.kind == VNODE_PROC_CWD ||
                    sn->vp.kind == VNODE_PROC_ROOT)) {
        return -ENOENT;
    }
    if (sn->vp.kind == VNODE_PROC_CWD || sn->vp.kind == VNODE_PROC_ROOT) {
        return procfs_readlink_dir(pid, sn->vp.kind == VNODE_PROC_ROOT, buf, (int)cap);
    }
    if (sn->vp.kind != VNODE_PROC_EXE) {
        return -EINVAL;
    }
    return procfs_readlink_exe(pid, buf, (int)cap);
}

/**
 * Go on from the file the /proc/<pid>/exe link `found`, on `cur`'s mount,
 * reaches: the program the process runs, which its address space keeps
 * (`mm_exe_open`), wherever a rename or a relocation has put the file and
 * whether a path reaches it still, as Linux's link reaches the file and not a
 * path. `found` is put. The walk first moves to the root of the file's mount,
 * taking that mount's lock for a locked walk, and reads the file again there:
 * the wait for the lock may let the file move or the process run another
 * program, and nothing sleeps between that read and the walk's hold on the
 * node. 0, or -ENOENT for a process with no program, -EACCES for a caller
 * that may not read its memory (`procfs_exe_file`), or -EBUSY.
 */
static int walk_exe(struct vfs_ref *cur, void *found, int locked, struct vfs_name **entry)
{
    struct vpath vp = ((struct synth_node *)found)->vp;
    struct vfs_ref exe;
    int rc, pid;

    node_put(cur->mnt, found);
    for (;;) {
        pid = procfs_resolve_id(&vp);

        rc = pid < 0 ? -ENOENT : procfs_exe_file(pid, &exe);
        if (rc != 0 || exe.mnt == cur->mnt) {
            break;
        }
        rc = walk_move(cur, exe.mnt, exe.mnt->root, locked);
        if (rc != 0) {
            return rc;
        }
    }
    if (rc == 0) {
        if (entry) {
            *entry = procfs_exe_entry(pid);
        }
        vfs_ref_hold(&exe);
        ref_move_node(cur, exe.node);
    }
    return rc;
}

/**
 * Go on from the object the link `found` of /proc, on `cur`'s mount, refers
 * to (`synth_object_link`), as Linux's walk goes to it and does not read its
 * text: the file or directory a descriptor is open on, or a process's working
 * directory or root (`procfs_link_target`), wherever a rename has put it and
 * whether a path reaches it still. `found` is put. The walk moves to a node
 * as `walk_exe` moves to a program's file: to the root of the node's mount
 * first, taking that mount's lock for a locked walk, and reads the object
 * again there, since the wait may let it move or the descriptor close. A
 * directory of /dev, /proc or /sys, and a working directory with no node, has
 * only a path from the root, which is put in front of `rest`, the part of the
 * path after the link, in `*linkpath`, as an absolute link's target is, with
 * the walk's pages taken at its first link (`walk_pages`). The jump uses up a
 * component of the path, so a chain of them ends, and it is not counted
 * against VFS_LINK_MAX. WALK_AT_OBJECT at the node; 1 with the path in
 * `*linkpath`; or a negative errno: -ENOMEM, -ENAMETOOLONG, -EBUSY, or
 * `procfs_link_target`'s.
 */
static int walk_object(struct vfs_ref *cur, void *found, const char *rest, char **linkpath,
                       char **target, int locked)
{
    struct vpath vp = ((struct synth_node *)found)->vp;
    struct vfs_ref to;
    int rc;

    node_put(cur->mnt, found);
    if (walk_pages(linkpath, target) != 0) {
        return -ENOMEM;
    }
    for (;;) {
        rc = procfs_link_target(&vp, &to, *target, VFS_PATH_MAX);
        if (rc != 0 || to.mnt == cur->mnt) {
            break;
        }
        rc = walk_move(cur, to.mnt, to.mnt->root, locked);
        if (rc != 0) {
            return rc;
        }
    }
    if (rc > 0) {
        while (*rest == '/') {
            rest++;
        }
        rc = walk_splice(*linkpath, *target, rc, rest);
        return rc < 0 ? rc : 1;
    }
    if (rc == 0) {
        vfs_ref_hold(&to);
        ref_move_node(cur, to.node);
        rc = WALK_AT_OBJECT;
    }
    return rc;
}

static const struct vfs_ops devfs_vops = {
    0,
    synth_lookup_rest,
    synth_hold,
    synth_put,
    synth_getattr,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    synth_readlink,
    0,
    0,
    0,
    0,
    0,
    0, /* abort_unmount */
    0, /* symlink */
    0, /* mknod */
    0, /* pin */
    0, /* unpin */
    0, /* remount */
    0, /* fallocate */
    0, /* fdatasync */
    0, /* lock_node */
    0, /* unlock_node */
    0, /* shutdown */
    0, /* xattr */
    0, /* cachestat */
    0, /* put_unless_last */
    0, /* readlink_cached */
};

static const struct vfs_ops procfs_vops = {
    0,
    synth_lookup_rest,
    synth_hold,
    synth_put,
    synth_getattr,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    synth_readlink,
    0,
    0,
    0,
    0,
    0,
    0, /* abort_unmount */
    0, /* symlink */
    0, /* mknod */
    0, /* pin */
    0, /* unpin */
    0, /* remount */
    0, /* fallocate */
    0, /* fdatasync */
    0, /* lock_node */
    0, /* unlock_node */
    0, /* shutdown */
    0, /* xattr */
    0, /* cachestat */
    0, /* put_unless_last */
    0, /* readlink_cached */
};

static const struct vfs_ops sysfs_vops = {
    0,
    synth_lookup_rest,
    synth_hold,
    synth_put,
    synth_getattr,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    synth_readlink,
    0,
    0,
    0,
    0,
    0,
    0, /* abort_unmount */
    0, /* symlink */
    0, /* mknod */
    0, /* pin */
    0, /* unpin */
    0, /* remount */
    0, /* fallocate */
    0, /* fdatasync */
    0, /* lock_node */
    0, /* unlock_node */
    0, /* shutdown */
    0, /* xattr */
    0, /* cachestat */
    0, /* put_unless_last */
    0, /* readlink_cached */
};

/** The constructor of a synthetic mount: its root is a node of the
 * namespace's own directory, whose name `options` has, held for the life of
 * the mount. 0, or -ENOMEM. */
static int synth_mount_root(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                            const char *options)
{
    struct vpath vp;

    (void)dev;
    (void)flags;
    synth_classify(options, &vp);
    return synth_take(&vp, &mnt->root);
}

/** The constructor of /dev, `synth_mount_root` for "dev". */
static int devfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                       const char *options)
{
    (void)options;
    return synth_mount_root(mnt, dev, flags, "dev");
}

/** The constructor of /proc, `synth_mount_root` for "proc". */
static int procfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                        const char *options)
{
    (void)options;
    return synth_mount_root(mnt, dev, flags, "proc");
}

/** The constructor of /sys, `synth_mount_root` for "sys". */
static int sysfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                       const char *options)
{
    (void)options;
    return synth_mount_root(mnt, dev, flags, "sys");
}

const struct vfs_type vfs_devfs_type = {"devfs", devfs_mount, &devfs_vops, 0, 1, 0, 0, 0};
const struct vfs_type vfs_procfs_type = {"proc", procfs_mount, &procfs_vops, 0, 1, 0, 0, 0};
const struct vfs_type vfs_sysfs_type = {"sysfs", sysfs_mount, &sysfs_vops, 0, 1, 0, 0, 0};
const struct vfs_type vfs_debugfs_type = {"debugfs", debugfs_mount, &debugfs_vops, 0, 1, 0, 0, 0};

const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node)
{
    if (mnt->type != &vfs_devfs_type && mnt->type != &vfs_procfs_type &&
        mnt->type != &vfs_sysfs_type) {
        return 0;
    }
    return &((struct synth_node *)node)->vp;
}

/* The permanent entries. */

/** Whether `m` is one of the permanent mounts, which umount2 answers EBUSY
 * for. */
static int mount_permanent(const struct vfs_mount *m)
{
    int i;

    for (i = 0; i < VFS_PERMANENT; i++) {
        if (mounts[i] == m) {
            return 1;
        }
    }
    return 0;
}

/**
 * Drop what a failed mount read of device `dev` from the cache, as an
 * unmount does. The device nodes of /dev read and write a device past the
 * cache, so a block the failed constructor left there would be stale once
 * a program wrote the device, as mkfs does, and the next mount would read
 * it. A NULL `dev` has nothing to drop.
 */
static void mount_refused(block_device_t *dev)
{
    if (dev) {
        (void)bcache_invalidate(dev);
    }
}

/** Construct the permanent mount `idx` of `type` at `path`, under `parent`,
 * NULL for none, with `options` for the constructor, and make it active. A
 * failure panics. */
static void install(int idx, const struct vfs_type *type, const char *path,
                    struct vfs_mount *parent, const char *options)
{
    struct vfs_mount *m = mount_record(idx);

    if (!m) {
        k_panic("vfs: no memory for a mount the kernel needs");
        return;
    }

    m->type = type;
    m->dev = 0;
    m->flags = 0;
    m->parent = parent;
    m->covered = 0;
    m->seq = (uint32_t)idx;
    k_strncpy(m->covered_path, path, sizeof(m->covered_path));
    m->refs = 0;
    m->inprogress = 0;
    mount_lock_init(m);
    m->priv = 0;
    m->opens = 0;
    m->releases = 0;
    m->claim = -1;
    m->source[0] = '\0';
    if (vfs_mount_view_new(m) != 0 || type->mount(m, 0, 0, options) != 0) {
        k_panic("vfs: a permanent mount refused to construct");
    }
    m->state = VFS_ACTIVE;
}

/**
 * The permanent mounts over a root just installed: /dev and /proc in the
 * root directory, and in /dev's root directory the tmpfs /dev/shm, with
 * `nosuid` and `nodev` as Linux systems mount it and mode 1777. A synthetic
 * type cannot host a mount point, so /dev/shm is a nameless mount entered
 * by its name, as /dev and /proc are. Then the tmpfs of memfd_create, with
 * no parent and no path, which nothing enters by name, and whose root, mode
 * 0555, stays empty, since its files have no names; chroot to a
 * /proc/PID/fdinfo makes that root the process's root. Then /sys,
 * read-only, nosuid, nodev and noexec, entered by name as /dev and /proc
 * are, the mqueue file system at /dev/mqueue, which lists the first IPC
 * namespace's POSIX queues and is entered by its name in /dev as /dev/shm
 * is, and last debugfs, nosuid, nodev and noexec, entered from /sys's root
 * directory by the names kernel/debug, where Linux systems mount it.
 */
static void install_permanent(void)
{
    install(1, &vfs_devfs_type, "/dev", mounts[0], 0);
    install(2, &vfs_procfs_type, "/proc", mounts[0], 0);
    install(3, &vfs_tmpfs_type, "/dev/shm", mounts[1], 0);
    mounts[3]->flags = VFS_MS_NOSUID | VFS_MS_NODEV;
    k_strncpy(mounts[3]->source, "tmpfs", sizeof(mounts[3]->source));
    install(4, &vfs_tmpfs_type, "", 0, "mode=0555,size=0");
    mounts[4]->flags = VFS_MS_NOSUID | VFS_MS_NODEV;
    install(5, &vfs_sysfs_type, "/sys", mounts[0], 0);
    mounts[5]->flags = VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC;
    k_strncpy(mounts[5]->source, "sysfs", sizeof(mounts[5]->source));
    install(6, &vfs_mqueue_type, "/dev/mqueue", mounts[1], 0);
    mounts[6]->flags = VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC;
    k_strncpy(mounts[6]->source, "mqueue", sizeof(mounts[6]->source));
    install(7, &vfs_debugfs_type, "/sys/kernel/debug", mounts[5], 0);
    mounts[7]->flags = VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC;
    k_strncpy(mounts[7]->source, "debugfs", sizeof(mounts[7]->source));
}

struct vfs_mount *vfs_memfd_mount(void)
{
    return mounts[4];
}

static int root_installed;

/**
 * The table is read under its lock, since a caller without the big lock,
 * the cached walk and getdents64 among them, may run while a mount grows
 * the array and frees the old one (`mount_new_slot`) or a new root takes
 * entry zero (`vfs_root_over_image`).
 */
int vfs_root_mounted(void)
{
    uint32_t flags = spin_lock_irqsave(&mount_table_lock);
    int active = mounts && mounts[0] && mounts[0]->state == VFS_ACTIVE;

    spin_unlock_irqrestore(&mount_table_lock, flags);
    return active;
}

void vfs_init(void)
{
    if (root_installed) {
        if (mounts[0]->type == &vfs_krffs_type) {
            mounts[0]->root = krffs_fs->node;
        }
        return;
    }
    root_installed = 1;
    mount_table_init();
    install(0, &vfs_krffs_type, "/", 0, 0);
    install_permanent();
}

int vfs_init_root(const struct vfs_type *type, struct block_device *dev, const char *source,
                  int claim)
{
    struct vfs_mount *m;
    int rc;

    if (root_installed) {
        return -EBUSY;
    }
    mount_table_init();
    m = mount_record(0);
    if (!m) {
        return -ENOMEM;
    }
    k_memset(m, 0, sizeof(*m));
    vfs_mount_view_new(m);
    m->type = type;
    m->dev = dev;
    m->state = VFS_MOUNTING;
    mount_lock_init(m);
    m->claim = claim;
    k_strncpy(m->covered_path, "/", sizeof(m->covered_path));
    k_strncpy(m->source, source, sizeof(m->source));
    m->source[sizeof(m->source) - 1] = '\0';
    rc = type->mount(m, dev, 0, 0);
    if (rc != 0) {
        k_memset(m, 0, sizeof(*m));
        mount_refused(dev);
        return rc;
    }
    m->state = VFS_ACTIVE;
    root_installed = 1;
    install_permanent();
    return 0;
}

/*
 * The new root takes entry zero and the old one a slot past the permanent
 * mounts, where an unmount would look for it, with its sequence after the
 * permanent ones so that /proc/PID/mountinfo lists it below the root. No
 * process runs yet, so no process root or descriptor names either record,
 * and the loop device keeps its own pointer to the old one.
 */
int vfs_root_over_image(const struct vfs_type *type, struct block_device *dev, const char *source,
                        const char *place, struct vfs_mount **host)
{
    struct vfs_mount *old, *m;
    uint32_t flags;
    int i, j, rc;

    if (!vfs_root_mounted()) {
        return -ENOENT;
    }
    if (place[0] != '/' || place[1] == '\0' || path_last(place) != place + 1 ||
        k_strlen(place) >= VFS_COVERED_PATH_MAX) {
        return -EINVAL;
    }
    i = mount_new_slot();
    if (i < 0) {
        return -ENOMEM;
    }
    m = mounts[i];
    k_memset(m, 0, sizeof(*m));
    vfs_mount_view_new(m);
    m->type = type;
    m->dev = dev;
    m->state = VFS_MOUNTING;
    mount_lock_init(m);
    m->claim = -1;
    k_strncpy(m->covered_path, "/", sizeof(m->covered_path));
    k_strncpy(m->source, source, sizeof(m->source));
    m->source[sizeof(m->source) - 1] = '\0';
    rc = type->mount(m, dev, 0, 0);
    if (rc != 0) {
        mount_release(m);
        mount_refused(dev);
        return rc;
    }
    m->state = VFS_ACTIVE;
    old = mounts[0];
    flags = spin_lock_irqsave(&mount_table_lock);
    mounts[0] = m;
    mounts[i] = old;
    spin_unlock_irqrestore(&mount_table_lock, flags);
    for (j = 1; j < VFS_PERMANENT; j++) {
        if (mounts[j] && mounts[j]->parent == old) {
            mounts[j]->parent = m;
        }
    }
    old->parent = m;
    old->covered = 0;
    old->seq = ++mount_seq;
    tree_seq++;
    k_strncpy(old->covered_path, place, sizeof(old->covered_path));
    vfs_mount_get(m);
    *host = old;
    return 0;
}

/* mount(2) and umount2(2). */

/* The types mount(2) finds by name, in the order /proc/filesystems lists
 * them. The root's adapter and the synthetic types are not here:
 * `vfs_do_mount` takes the name "proc" itself, and nothing mounts the
 * others by name. */
static const struct vfs_type *const mount_types[] = {
    &vfs_vfat_type,         &vfs_exfat_type,  &vfs_ext2_type,
    &vfs_krffs_volume_type, &vfs_tmpfs_type,  &vfs_mqueue_type,
    &vfs_debugfs_type,      &vfs_pstore_type, 0};

/* The entries of `mount_types`, its terminator apart, counted from the table
 * so that a type added to it is found with no number to change. */
#define VFS_MOUNT_TYPES ((int)(sizeof(mount_types) / sizeof(mount_types[0])) - 1)

/** The type of `mount_types` named `name`, or NULL. */
static const struct vfs_type *type_named(const char *name)
{
    int i;

    for (i = 0; i < VFS_MOUNT_TYPES; i++) {
        if (mount_types[i] && streq(mount_types[i]->name, name)) {
            return mount_types[i];
        }
    }
    return 0;
}

const struct vfs_type *vfs_type_at(int idx)
{
    if (idx < 0 || idx >= VFS_MOUNT_TYPES) {
        return 0;
    }
    return mount_types[idx];
}

/* The flags mount(2) accepts here: honoured or recorded ones. Anything
 * else is EINVAL, MS_REMOUNT, MS_BIND and MS_MOVE first among them. */
#define VFS_MS_KNOWN                                                                     \
    (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC | VFS_MS_SYNCHRONOUS | \
     VFS_MS_DIRSYNC | VFS_MS_NOATIME | VFS_MS_NODIRATIME | VFS_MS_SILENT | VFS_MS_RELATIME)

int vfs_remount(struct vfs_mount *m, uint32_t flags, const char *options)
{
    uint32_t changed = (flags ^ m->flags) & VFS_MS_RDONLY;
    int rc;

    if (flags & ~VFS_MS_KNOWN) {
        return -EINVAL;
    }
    if (m->state != VFS_ACTIVE) {
        return -EBUSY;
    }
    if (vfs_mount_gone(m)) {
        return -EIO;
    }
    if (changed && !m->type->ops->remount) {
        return -EINVAL;
    }
    if (changed && !(flags & VFS_MS_RDONLY) &&
        ((m->dev && (m->dev->flags & BLOCK_READ_ONLY)) || m->write_error)) {
        return -EROFS;
    }
    if (m->type->ops->remount && (changed || (options && *options))) {
        rc = m->type->ops->remount(m, flags, options);
        if (rc != 0) {
            return rc;
        }
    }
    m->flags = flags;
    klog(KLOG_INFO, "vfs", "remounted: %s (%s)", m->covered_path,
         (flags & VFS_MS_RDONLY) ? "read-only" : "writable");
    return 0;
}

/** The wait condition of an unmount: no operation is in progress on the
 * mount `arg`. */
static int drained(void *arg)
{
    struct vfs_mount *mnt = arg;
    return mnt->inprogress == 0;
}

/**
 * The device `source` names for a mount of `t`, into `*dev`, with its claim
 * in `*claim`: a partition window, a stick or a loop device under /dev, its
 * sectors claimed, and `*flags` made read-only for a read-only device
 * (BLOCK_READ_ONLY). A type with no device takes any source string, claims
 * nothing and has a NULL device. Returns 0, or the errno with nothing
 * claimed: that of `devfs_window_of`, or EBUSY for a claimed range or a
 * device with a mount already.
 */
static int mount_source(const struct vfs_type *t, const char *source, block_device_t **dev,
                        uint32_t *flags, int *claim)
{
    block_device_t *backing;
    uint64_t first, count;
    int i, rc;

    *dev = 0;
    *claim = -1;
    if (t->nodev) {
        return 0;
    }
    rc = devfs_window_of(source, dev, &backing, &first, &count);
    if (rc != 0) {
        return rc;
    }
    if ((*dev)->flags & BLOCK_READ_ONLY) {
        *flags |= VFS_MS_RDONLY;
    }
    if (backing) {
        rc = devfs_claim(backing, first, first + count - 1);
        if (rc < 0) {
            return rc;
        }
        *claim = rc;
    }
    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->dev == *dev) {
            devfs_release_claim(*claim);
            *claim = -1;
            return -EBUSY;
        }
    }
    return 0;
}

/** The length of a mount's recorded path that a path below it starts with:
 * 0 for the root's "/", whose children's paths start with their own "/". */
static uint32_t path_below(const struct vfs_mount *m)
{
    return (m->covered_path[0] == '/' && m->covered_path[1] == '\0')
               ? 0
               : (uint32_t)k_strlen(m->covered_path);
}

/**
 * Whether the place `real`, a path from the root of the mount table, is a
 * name in the root directory of the synthetic mount `mnt` that names its
 * node `node` (`synth_names`): a place a nameless mount may be entered by.
 */
static int synth_root_child(struct vfs_mount *mnt, const void *node, const char *real)
{
    const char *in = mnt->bind_of ? mnt->bind_root : "";
    uint32_t ml = path_below(mnt), il;
    char inner[VFS_COVERED_PATH_MAX];
    const char *name, *c;

    if (!real || (ml > 0 && !path_under(real, mnt->covered_path, ml)) || real[ml] != '/') {
        return 0;
    }
    name = real + ml + 1;
    for (c = name; *c && *c != '/'; c++) {
    }
    if (name[0] == '\0' || *c || c - name > VFS_NAME_MAX) {
        return 0;
    }
    if (in[0] == '/' && in[1] == '\0') {
        in = "";
    }
    il = (uint32_t)k_strlen(in);
    if (il + 1 + k_strlen(name) + 1 > sizeof(inner)) {
        return 0;
    }
    k_memcpy(inner, in, il);
    inner[il] = '/';
    k_strncpy(inner + il + 1, name, sizeof(inner) - il - 1);
    return synth_names(mnt, node, inner);
}

/**
 * Check that the node `tref` holds, which `target` named, may take a mount,
 * and put its path from the root of the mount table in `canon`: a name of
 * at most VFS_COVERED_PATH_MAX - 1 bytes, ENAMETOOLONG; not the root of
 * the mount table, EINVAL; a directory when `dir`, and otherwise not one,
 * ENOTDIR, as Linux answers a bind of a file over a directory or the other
 * way round. The root of a placed mount may take one, which goes over it,
 * as on Linux. A file's path is `real`, its path from the root of the mount
 * table as the walk that found it took it (`table_path`), since a type gives
 * the path of a directory only; a file with none, NULL, is ENOTDIR. On an
 * error `tref` is released.
 *
 * In a type that takes the rest of a path in one call, whose nodes are
 * classifications that no lookup finds again, the root of a mount that is
 * not permanent may take a mount, such as the bind of /dev a sandbox makes,
 * and so may a name in the root directory of any mount of one, which `real`
 * gives (`synth_root_child`), as /proc/sys takes the read-only bind that
 * bubblewrap makes of it. The mount over such a name is nameless, and the
 * walk enters it by that name (`named_in_rest`), and `*nameless` is set.
 * A place deeper than that would be passed over by the type's lookup of the
 * rest, and is EINVAL, as is the root of a permanent mount.
 */
static int target_check(struct vfs_ref *tref, const char *target, char *canon, int dir,
                        const char *real, int *nameless)
{
    int at_root = tref->node == tref->mnt->root;
    int synth = tref->mnt->type->ops->lookup_rest != 0;
    struct vfs_stat st;
    int rc = 0;

    *nameless = 0;
    if (k_strlen(target) >= VFS_COVERED_PATH_MAX) {
        rc = -ENAMETOOLONG;
    } else if ((synth && at_root && mount_permanent(tref->mnt)) ||
               (synth && !at_root &&
                !(*nameless = synth_root_child(tref->mnt, tref->node, real))) ||
               (tref->mnt == mounts[0] && at_root)) {
        rc = -EINVAL;
    } else if ((rc = vfs_getattr(tref->mnt, tref->node, &st)) == 0 &&
               !VFS_IS_DIR(st.mode) != !dir) {
        rc = -ENOTDIR;
    }
    if (rc == 0 && at_root) {
        k_strncpy(canon, tref->mnt->covered_path, VFS_COVERED_PATH_MAX);
    } else if (rc == 0 && (!dir || *nameless)) {
        rc = !real ? -ENOTDIR : k_strlen(real) >= VFS_COVERED_PATH_MAX ? -ENAMETOOLONG : 0;
        if (rc == 0) {
            k_strncpy(canon, real, VFS_COVERED_PATH_MAX);
        }
    } else if (rc == 0) {
        rc = vfs_node_path(tref->mnt, tref->node, canon, VFS_COVERED_PATH_MAX);
    }
    if (rc != 0) {
        vfs_ref_release(tref);
    }
    return rc;
}

/** Whether the node `tref` holds is covered already, or reserved by a mount
 * in progress, in the caller's mount namespace. Under the parent's lock. */
static int covered_already(const struct vfs_ref *tref)
{
    int i;

    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->covered == tref->node &&
            mounts[i]->parent == tref->mnt && vfs_mount_seen(mounts[i])) {
            return 1;
        }
    }
    return 0;
}

/**
 * A record in a free slot for a mount of `t` over the node `covered` of
 * `parent`, recorded at `canon` and from `source`, with `flags`, mounting
 * and seen in the caller's mount namespace: the parts every mount has.
 * It counts on its parent and keeps the covered node's hold, which the
 * caller has taken. NULL when no slot can be had. Under the covered node's
 * lock (`vfs_node_lock`), the parent's namespace lock on a type without
 * `file_locked`.
 */
static struct vfs_mount *mount_place(struct vfs_mount *parent, void *covered,
                                     const struct vfs_type *t, const char *canon,
                                     const char *source, uint32_t flags)
{
    int i = mount_new_slot();
    struct vfs_mount *m = i < 0 ? NULL : mounts[i];

    if (!m) {
        return NULL;
    }
    k_memset(m, 0, sizeof(*m));
    if (vfs_mount_view_new(m) != 0) {
        mount_release(m);
        return NULL;
    }
    m->type = t;
    m->flags = flags;
    m->state = VFS_MOUNTING;
    m->parent = parent;
    m->covered = covered;
    mount_lock_init(m);
    m->claim = -1;
    m->seq = ++mount_seq;
    tree_seq++;
    k_strncpy(m->covered_path, canon, sizeof(m->covered_path));
    k_strncpy(m->source, source, sizeof(m->source));
    m->source[sizeof(m->source) - 1] = '\0';
    vfs_mount_get(parent);
    return m;
}

/**
 * The covered check and the reservation of the slot are made under the
 * covered directory's lock (`vfs_node_lock`), so that no namespace change
 * there runs between them: the parent's namespace lock, or on a type that
 * sets `file_locked` the directory's own, under which its `rmdir` looks for
 * a mount on it and a mount looks for its removal (`vfs_cover_lock`).
 * The slot is taken before the constructor runs, because the constructor
 * may sleep on the device and another process could remove or rename the
 * target meanwhile: the slot keeps the covered node's hold, which the walk
 * took, and counts on the parent, and the walk's count of operations in
 * progress is given back. The target is recorded by the path
 * `vfs_node_path` gives rather than by the path that named it, which may
 * pass through a link or start at a root chroot gave the caller.
 *
 * A loop device has no disk under it and claims nothing, since the device
 * itself is what a second mount of it collides with (`mount_source`). A
 * read-only device, a write-protected stick or a loop device attached
 * read-only, makes the mount read-only, and the type learns it from the
 * flags. A mount of type proc over a directory of the permanent /proc
 * changes nothing, since every /proc shows the processes of its reader's
 * PID namespace, as `unshare --mount-proc` would have it; over any other
 * directory, or over the root of a bind of /proc, it places a mount of the
 * same files there, as a sandbox's new root needs. A mount
 * of type devpts is a mount of /dev's type whose root is /dev/pts, so it
 * shows the system's terminals and ptmx, whatever its options ask, since
 * this kernel keeps one set of terminals.
 */
int vfs_do_mount(const char *source, const char *target, const char *type, uint32_t flags,
                 const char *options)
{
    int pts = streq(type, "devpts");
    const struct vfs_type *t = streq(type, "proc") ? &vfs_procfs_type
                               : pts               ? &vfs_devfs_type
                                                   : type_named(type);
    struct vfs_ref tref;
    struct vfs_mount *m = 0;
    block_device_t *dev;
    char canon[VFS_COVERED_PATH_MAX];
    int claim, nameless;
    int rc;

    if (!t) {
        return -ENODEV;
    }
    if (flags & ~VFS_MS_KNOWN) {
        return -EINVAL;
    }
    rc = vfs_resolve(0, target, 0, &tref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    if (t == &vfs_procfs_type && tref.mnt->type == &vfs_procfs_type && mount_permanent(tref.mnt)) {
        vfs_ref_release(&tref);
        return 0;
    }
    rc = target_check(&tref, target, canon, 1, NULL, &nameless);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_cover_lock(&tref);
    if (rc == 0 && covered_already(&tref)) {
        rc = -EBUSY;
    }
    if (rc != 0) {
        vfs_node_unlock(&tref);
        vfs_ref_release(&tref);
        return rc;
    }
    rc = mount_source(t, source, &dev, &flags, &claim);
    if (rc != 0) {
        vfs_node_unlock(&tref);
        vfs_ref_release(&tref);
        return rc;
    }
    m = mount_place(tref.mnt, tref.node, t, canon, source, flags);
    if (!m) {
        devfs_release_claim(claim);
        vfs_node_unlock(&tref);
        vfs_ref_release(&tref);
        return -ENOMEM;
    }
    m->dev = dev;
    m->claim = claim;
    vfs_node_unlock(&tref);
    vfs_leave(tref.mnt);

    vfs_ns_lock(m);
    rc = pts ? synth_mount_root(m, dev, flags, "dev/pts") : t->mount(m, dev, flags, options);
    vfs_ns_unlock(m);
    if (rc != 0) {
        struct vfs_ref cov;

        cov.mnt = m->parent;
        cov.node = m->covered;
        vfs_ref_put(&cov);
        vfs_mount_put(m->parent);
        devfs_release_claim(claim);
        mount_release(m);
        mount_refused(dev);
        return rc;
    }
    m->state = VFS_ACTIVE;
    klog(KLOG_INFO, "vfs", "mounted: %s on %s (%s%s)", source, m->covered_path, t->name,
         (flags & VFS_MS_RDONLY) ? ", read-only" : "");
    return 0;
}

int vfs_mount_in_sys(const struct vfs_type *t, const char *path, const char *source, uint32_t flags)
{
    struct vfs_mount *sys = mounts[5], *m;
    int rc;

    if (!vfs_root_mounted() || !sys || sys->state != VFS_ACTIVE ||
        !path_under(path, sys->covered_path, path_below(sys)) || path[path_below(sys)] != '/' ||
        k_strlen(path) >= VFS_COVERED_PATH_MAX) {
        return -EINVAL;
    }
    m = mount_place(sys, NULL, t, path, source, flags);
    if (!m) {
        return -ENOMEM;
    }
    rc = t->mount(m, 0, flags, 0);
    if (rc != 0) {
        vfs_mount_put(sys);
        mount_release(m);
        return rc;
    }
    m->state = VFS_ACTIVE;
    klog(KLOG_INFO, "vfs", "mounted: %s on %s (%s)", source, m->covered_path, t->name);
    return 0;
}

/** Whether the mount `c` is `top` or below it, by the chain of parents. */
static int mount_within(const struct vfs_mount *c, const struct vfs_mount *top)
{
    for (; c; c = c->parent) {
        if (c == top) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether the node `sref` holds may be a bind's root, with its path from
 * the root of the mount table in `full` and its path within its file system
 * in `inner`, and set `*dir` to whether it is a directory. A file's path is
 * `real`, as `target_check` takes it, and so is the path of a place inside
 * a type that takes the rest of a path in one call, which gives no path of
 * a node; a node with none, NULL, is ENOTDIR or EINVAL. 0, or the error of
 * `getattr`, ENOTDIR, EINVAL for /proc/PID/fd/N, a link the walk stops at,
 * or ENAMETOOLONG.
 *
 * A place inside a synthetic type, such as /proc/sys, which bubblewrap
 * binds read-only over itself, may be one as on Linux: its nodes are
 * classifications of a path, so a lookup below the bind is the type's own
 * of the rest under the bind's `inner` (`synth_lookup_rest`), which is the
 * walk's record of the place with its `.` and `..` resolved; a record that
 * names some other place, as one through /proc/PID/root does, is EINVAL
 * (`synth_names`). A mount below the bind is entered only by name from its
 * root (`target_check`). A bind of KRFFS, whose nodes move, is moved with
 * them by `vfs_repoint`.
 */
static int bind_source(const struct vfs_ref *sref, char *full, char *inner, const char *real,
                       int *dir)
{
    const struct vfs_mount *base = vfs_mount_base(sref->mnt);
    const char *prefix = sref->mnt->bind_of ? sref->mnt->bind_root : "";
    const struct vpath *vp;
    const char *rest;
    struct vfs_stat st;
    int rc = vfs_getattr(sref->mnt, sref->node, &st);

    if (rc != 0) {
        return rc;
    }
    *dir = VFS_IS_DIR(st.mode);
    vp = vfs_synth_vpath(sref->mnt, sref->node);
    if (vp && vp->kind == VNODE_PROC_FD_LINK) {
        return -EINVAL;
    }
    if (sref->node == sref->mnt->root) {
        k_strncpy(full, sref->mnt->covered_path, VFS_COVERED_PATH_MAX);
    } else if (!*dir || base->type->ops->lookup_rest) {
        if (!real) {
            return *dir ? -EINVAL : -ENOTDIR;
        }
        if (k_strlen(real) >= VFS_COVERED_PATH_MAX) {
            return -ENAMETOOLONG;
        }
        k_strncpy(full, real, VFS_COVERED_PATH_MAX);
    } else if ((rc = vfs_node_path(sref->mnt, sref->node, full, VFS_COVERED_PATH_MAX)) != 0) {
        return rc;
    }
    if (path_below(sref->mnt) > k_strlen(full)) {
        return -EINVAL;
    }
    rest = full + path_below(sref->mnt);
    if (prefix[0] == '/' && prefix[1] == '\0') {
        prefix = "";
    }
    if (k_strlen(prefix) + k_strlen(rest) + 2 > VFS_COVERED_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    k_strncpy(inner, prefix, VFS_COVERED_PATH_MAX);
    k_strncpy(inner + k_strlen(inner), rest, VFS_COVERED_PATH_MAX - k_strlen(inner));
    if (inner[0] == '\0') {
        inner[0] = '/';
        inner[1] = '\0';
    }
    if (vp && sref->node != sref->mnt->root && !synth_names(sref->mnt, sref->node, inner)) {
        return -EINVAL;
    }
    return 0;
}

/** Make `m`, placed already, a bind showing the file system of `from` from
 * its node `root`, held for it, at `inner` within that file system. The bind
 * is a reference on the base, so the file system it shows stays mounted
 * under it. */
static void bind_fill(struct vfs_mount *m, struct vfs_mount *from, void *root, const char *inner)
{
    struct vfs_mount *base = mount_base(from);

    m->dev = base->dev;
    m->priv = base->priv;
    m->root = root;
    m->bind_of = base;
    k_strncpy(m->bind_root, inner, sizeof(m->bind_root));
    vfs_mount_get(base);
}

/**
 * A bind of the mount `c` under `parent`, a bind already, at `canon`: its
 * covered node and root held again for the new record, which shows what `c`
 * shows. NULL when no slot can be had. The caller holds no lock; this takes
 * the covered node's (`vfs_node_lock`) around the placing.
 */
static struct vfs_mount *bind_child(struct vfs_mount *c, struct vfs_mount *parent,
                                    const char *canon)
{
    struct vfs_ref cov;
    struct vfs_mount *b;

    cov.mnt = parent;
    cov.node = c->covered;
    vfs_node_lock(&cov);
    b = mount_place(parent, c->covered, c->type, canon, mount_base(c)->source, c->flags);
    if (b) {
        if (c->covered && c->parent->type->ops->hold) {
            c->parent->type->ops->hold(mount_base(c->parent), c->covered);
        }
        if (c->type->ops->hold) {
            c->type->ops->hold(mount_base(c), c->root);
        }
        bind_fill(b, c, c->root, c->bind_of ? c->bind_root : "/");
        b->state = VFS_ACTIVE;
    }
    vfs_node_unlock(&cov);
    return b;
}

/** The bind `made` holds for the parent of the mount `c`, of the `n` mounts
 * in `orig`, or `top` when that parent is `from`, the source's own mount;
 * NULL when the parent was not shown. */
static struct vfs_mount *bind_parent(struct vfs_mount *c, struct vfs_mount **orig,
                                     struct vfs_mount **made, int n, struct vfs_mount *top,
                                     const struct vfs_mount *from)
{
    int j;

    for (j = 0; j < n; j++) {
        if (orig[j] && orig[j] == c->parent && made[j]) {
            return made[j];
        }
    }
    return c->parent == from ? top : NULL;
}

/**
 * For MS_REC: show below the bind `top`, made of the place `full`, each
 * mount the caller sees below that place, in the order of their depth, each
 * as a bind under the bind of its parent (`bind_child`). A mount whose new
 * path would not fit, a KRFFS volume, and a mount whose parent is not shown
 * are left out, with a record in the kernel log, and when the heap has no
 * room for the two arrays nothing is shown below `top`. The mount `skip`
 * and those below it, NULL for none, are left out without a record, as
 * pivot_root leaves out the new root (`vfs_pivot_mounts`).
 */
static void bind_below(struct vfs_mount *top, struct vfs_mount *from, const char *full,
                       const struct vfs_mount *skip)
{
    uint32_t flen = (uint32_t)k_strlen(full), depth;
    int n = mount_slots, i;
    struct vfs_mount **orig = kzalloc(sizeof(*orig) * (uint32_t)n * 2u);
    struct vfs_mount **made = orig ? orig + n : NULL;

    if (!orig) {
        klog(KLOG_WARN, "vfs", "bind: no memory for the mounts below %s", top->covered_path);
        return;
    }
    for (i = 0; i < n; i++) {
        struct vfs_mount *c = mounts[i];

        if (c && c != top && c->state == VFS_ACTIVE && c->parent && vfs_mount_seen(c) &&
            (flen == 1 || path_under(c->covered_path, full, flen)) &&
            !(skip && mount_within(c, skip))) {
            orig[i] = c;
        }
    }
    for (depth = flen + 1; depth < VFS_COVERED_PATH_MAX; depth++) {
        for (i = 0; i < n; i++) {
            struct vfs_mount *c = orig[i], *parent;
            char canon[VFS_COVERED_PATH_MAX];
            const char *tail;

            if (!c || (uint32_t)k_strlen(c->covered_path) != depth) {
                continue;
            }
            parent = bind_parent(c, orig, made, n, top, from);
            tail = c->covered_path + (flen == 1 ? 0 : flen);
            if (!parent || mount_base(c)->type == &vfs_krffs_volume_type ||
                path_below(top) + k_strlen(tail) + 1 > VFS_COVERED_PATH_MAX) {
                klog(KLOG_WARN, "vfs", "bind: %s is not shown below %s", c->covered_path,
                     top->covered_path);
                continue;
            }
            k_strncpy(canon, top->covered_path, sizeof(canon));
            k_strncpy(canon + path_below(top), tail, sizeof(canon) - path_below(top));
            made[i] = bind_child(c, parent, canon);
        }
    }
    kfree(orig);
}

/**
 * Place a bind of the place `sref` holds over the one `tref` holds, `target`
 * naming it, both walks' references taken over: the mounts below the
 * source too with `rec`, but for `skip` and those below it (`bind_below`).
 * Once the bind is placed, the walks' counts of operations in progress are
 * given back; the hold on the covered node stays with the slot, or is put
 * back for a nameless bind, which covers no node (`target_check`), and the
 * hold on the source's node stays with the bind, as its root. On an error
 * both references are released. 0, or the errors `vfs_do_bind` lists.
 */
static int bind_refs(struct vfs_ref *sref, struct vfs_ref *tref, const char *target, int rec,
                     const struct vfs_mount *skip, const char *sreal, const char *treal)
{
    struct vfs_mount *m;
    char full[VFS_COVERED_PATH_MAX], inner[VFS_COVERED_PATH_MAX], canon[VFS_COVERED_PATH_MAX];
    int dir = 1, nameless = 0, busy, gone;
    int rc;

    rc = bind_source(sref, full, inner, sreal, &dir);
    if (rc == 0) {
        rc = target_check(tref, target, canon, dir, treal, &nameless);
    } else {
        vfs_ref_release(tref);
    }
    if (rc != 0) {
        vfs_ref_release(sref);
        return rc;
    }
    gone = vfs_cover_lock(tref);
    busy = nameless ? mount_named(tref->mnt, tref->mnt->root, path_last(canon)) != 0
                    : covered_already(tref);
    gone = nameless ? 0 : gone;
    m = busy || gone ? NULL
                     : mount_place(tref->mnt, nameless ? NULL : tref->node, sref->mnt->type, canon,
                                   vfs_mount_base(sref->mnt)->source, sref->mnt->flags);
    rc = m ? 0 : busy ? -EBUSY : gone ? gone : -ENOMEM;
    if (m) {
        bind_fill(m, sref->mnt, sref->node, inner);
        m->state = VFS_ACTIVE;
    }
    vfs_node_unlock(tref);
    if (rc != 0) {
        vfs_ref_release(tref);
        vfs_ref_release(sref);
        return rc;
    }
    if (nameless) {
        node_put(tref->mnt, tref->node);
    }
    vfs_leave(tref->mnt);
    vfs_leave(sref->mnt);
    if (rec) {
        bind_below(m, sref->mnt, full, skip);
    }
    klog(KLOG_INFO, "vfs", "bound: %s on %s", full, m->covered_path);
    return 0;
}

/**
 * Make `path`, a path from the caller's root as `vfs_resolve_real` gives
 * it, of VFS_PATH_MAX bytes, a path from the root of the mount table, by
 * putting the root's own path in front of it. 0, -ENAMETOOLONG for an
 * empty path, which did not fit the walk's record, or one that does not fit
 * with the root's in front, or the error of `vfs_node_path` for the root.
 */
static int table_path(char *path)
{
    char prefix[VFS_COVERED_PATH_MAX];
    uint32_t pl = 0, tl = (uint32_t)k_strlen(path);
    struct vfs_ref root;
    int rc = 0;

    if (tl == 0) {
        return -ENAMETOOLONG;
    }
    walk_root(&root);
    if (root.node == root.mnt->root) {
        k_strncpy(prefix, root.mnt->covered_path, sizeof(prefix));
    } else if (vfs_enter(root.mnt) != 0) {
        return -EBUSY;
    } else {
        vfs_ref_hold(&root);
        rc = vfs_node_path(root.mnt, root.node, prefix, sizeof(prefix));
        vfs_ref_release(&root);
    }
    if (rc != 0) {
        return rc;
    }
    pl = (uint32_t)k_strlen(prefix);
    if (pl == 1) {
        return 0;
    }
    if (tl == 1) {
        tl = 0;
    }
    if (pl + tl + 1 > VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    k_memmove(path + pl, path, tl + 1);
    k_memcpy(path, prefix, pl);
    path[pl + tl] = '\0';
    return 0;
}

int vfs_do_bind(const char *source, const char *target, int rec)
{
    struct vfs_ref sref, tref;
    char full[VFS_COVERED_PATH_MAX], inner[VFS_COVERED_PATH_MAX];
    char *sreal = kmalloc(VFS_PATH_MAX), *treal = kmalloc(VFS_PATH_MAX);
    int dir, rc;

    rc = sreal && treal ? vfs_resolve_real(source, 0, &sref, sreal) : -ENOMEM;
    if (rc == 0) {
        rc = table_path(sreal);
        if (rc == 0) {
            rc = bind_source(&sref, full, inner, sreal, &dir);
        }
        if (rc == 0) {
            rc = vfs_resolve_real(target, 0, &tref, treal);
        }
        if (rc == 0 && (rc = table_path(treal)) != 0) {
            vfs_ref_release(&tref);
        }
        if (rc != 0) {
            vfs_ref_release(&sref);
        } else {
            rc = bind_refs(&sref, &tref, target, rec, NULL, sreal, treal);
        }
    }
    kfree(sreal);
    kfree(treal);
    return rc;
}

int vfs_remount_bind(struct vfs_mount *m, uint32_t flags)
{
    if (flags & ~VFS_MS_KNOWN) {
        return -EINVAL;
    }
    if (m->state != VFS_ACTIVE) {
        return -EBUSY;
    }
    if (!(flags & VFS_MS_RDONLY) && (mount_base(m)->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    m->flags = flags;
    klog(KLOG_INFO, "vfs", "remounted: %s (%s)", m->covered_path,
         (flags & VFS_MS_RDONLY) ? "read-only" : "writable");
    return 0;
}

/**
 * Return `m` to its type after an unmount that passed `prepare_unmount` and
 * then failed, so that the mount stays: the type wrote its last metadata for
 * a volume that was going away, and its `abort_unmount` puts back what that
 * changed. A type without one is left as its preparation left it, and a
 * mount whose device has gone, which was not prepared and takes no write, is
 * left alone.
 */
static void unmount_abandoned(struct vfs_mount *m)
{
    if (vfs_mount_gone(m)) {
        return;
    }
    if (m->type->ops->abort_unmount) {
        m->type->ops->abort_unmount(m);
    }
}

/**
 * `target`, a path from the calling process's root, as the path from the
 * root of the mount table that a mount records, into `out` of
 * VFS_COVERED_PATH_MAX bytes: the root's own path goes in front of it, so
 * the answer names a place at or below the caller's root. The root is held
 * while its path is rebuilt, since a thread that shares it may chroot
 * meanwhile. Returns 0; -EBUSY for a root on a mount being unmounted; the
 * error `vfs_node_path` gives; or -ENAMETOOLONG for a path longer than a
 * recorded one can be.
 */
static int unmount_target(const char *target, char *out)
{
    uint32_t tl = (uint32_t)k_strlen(target);
    struct vfs_ref root;
    struct vfs_mount *mnt;
    uint32_t rl = 0;
    int rc;

    if (current_proc && current_proc->fs->root_mnt) {
        mnt = current_proc->fs->root_mnt;
        root.mnt = mnt;
        root.node = current_proc->fs->root_node;
        if (vfs_enter(mnt) != 0) {
            return -EBUSY;
        }
        vfs_ref_hold(&root);
        rc = vfs_node_path(mnt, root.node, out, VFS_COVERED_PATH_MAX);
        vfs_ref_release(&root);
        if (rc != 0) {
            return rc;
        }
        rl = (uint32_t)k_strlen(out);
        if (tl == 1) {
            return 0;
        }
    }
    if (rl + tl + 1 > VFS_COVERED_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    k_memcpy(out + rl, target, tl + 1);
    return 0;
}

/**
 * The mount `target` names by its recorded path, for an unmount whose walk
 * met a removed device on the way: the one active mount with that path that
 * has an ancestor whose device has gone, or NULL when there is none or more
 * than one.
 *
 * Without it, a child mount's reference on a parent whose device has gone
 * could never be dropped, since no lookup on the missing medium is possible.
 * The caller names the target from its own root, and the name is put in the
 * mount table's terms before it is compared (`unmount_target`), so only a
 * mount at or below that root can match; a recorded path has no `.` or
 * `..`, so a target that climbs matches none. A mount whose ancestors are
 * all present is found only by the walk.
 */
static struct vfs_mount *unmount_below_gone(const char *target)
{
    char want[VFS_COVERED_PATH_MAX];
    struct vfs_mount *found = NULL;
    struct vfs_mount *m;
    struct vfs_mount *parent;
    int i;

    if (unmount_target(target, want) != 0) {
        return NULL;
    }
    for (i = VFS_PERMANENT; i < mount_slots; i++) {
        m = mounts[i];
        if (!m || m->state != VFS_ACTIVE || !streq(m->covered_path, want)) {
            continue;
        }
        for (parent = m->parent; parent; parent = parent->parent) {
            if (vfs_mount_gone(parent)) {
                if (found) {
                    return NULL;
                }
                found = m;
                break;
            }
        }
    }
    return found;
}

/**
 * The mount an unmount's walk found at `tref`, in `*out`: 0, -ENOTDIR for a
 * target that named a directory by its end, `dir`, and reached anything
 * else, -EINVAL for a node that is no mount's root, or -EBUSY for a
 * permanent mount. The reference is released.
 *
 * A mount's root is a directory, so a target that reached one needs no
 * attribute read, which is EIO on a mount whose device has gone.
 */
static int unmount_found(struct vfs_ref *tref, int dir, struct vfs_mount **out)
{
    struct vfs_stat st;
    int rc = 0;

    if (dir && tref->node != tref->mnt->root) {
        rc = vfs_getattr(tref->mnt, tref->node, &st);
        if (rc == 0 && !VFS_IS_DIR(st.mode)) {
            rc = -ENOTDIR;
        }
    }
    if (rc == 0 && tref->node != tref->mnt->root) {
        rc = -EINVAL;
    } else if (rc == 0 && mount_permanent(tref->mnt)) {
        rc = -EBUSY;
    }
    *out = tref->mnt;
    vfs_ref_release(tref);
    return rc;
}

static int unmount_mount(struct vfs_mount *m);

/**
 * A target that named a directory by its end is checked by the walk that
 * finds the mount, before the EINVAL of a name that is no mount point, as
 * on Linux. The node checked is the node unmounted, so a symbolic link
 * replaced between two walks cannot change the answer. A target found by
 * its recorded path below a removed device is a mount's root, a directory.
 */
int vfs_do_umount(const char *target, int dir)
{
    struct vfs_ref tref;
    struct vfs_mount *m;
    int rc;

    rc = resolve_path(0, target, WALK_UNMOUNT, &tref, 0, 0, 0);
    m = rc == -EIO ? unmount_below_gone(target) : NULL;
    if (rc != 0 && !m) {
        return rc;
    }
    if (rc == 0) {
        rc = unmount_found(&tref, dir, &m);
        if (rc != 0) {
            return rc;
        }
    }
    if (m->state != VFS_ACTIVE) {
        return -EBUSY;
    }
    if (vfs_mount_shared(m)) {
        vfs_mount_hide(m);
        return 0;
    }
    return unmount_mount(m);
}

/** The mount the calling process's root is on: the root of the mount table
 * for a process chroot has not moved. */
static struct vfs_mount *root_mount_of_caller(void)
{
    return current_proc && current_proc->fs->root_mnt ? current_proc->fs->root_mnt : mounts[0];
}

/**
 * Whether a lazy unmount of `m` takes `c` from the caller's view: whether
 * `c` is `m` or below it, by the chain of parents, and that chain meets `m`
 * before it meets `root`, the mount of the caller's root. The root and what
 * is below it stay, as after pivot_root they are no longer below the old
 * root that is detached.
 */
static int detached_by(const struct vfs_mount *c, const struct vfs_mount *m,
                       const struct vfs_mount *root)
{
    for (; c; c = c->parent) {
        if (c == root) {
            return 0;
        }
        if (c == m) {
            return 1;
        }
    }
    return 0;
}

int vfs_do_detach(const char *target, int dir)
{
    struct vfs_ref tref;
    struct vfs_mount *m, *root = root_mount_of_caller();
    int i, rc;

    rc = resolve_path(0, target, WALK_UNMOUNT, &tref, 0, 0, 0);
    m = rc == -EIO ? unmount_below_gone(target) : NULL;
    if (rc != 0 && !m) {
        return rc;
    }
    if (rc == 0) {
        rc = unmount_found(&tref, dir, &m);
        if (rc == -EBUSY && m != mounts[0]) {
            rc = 0;
        }
        if (rc != 0) {
            return rc;
        }
    }
    if (m == mounts[0] || m == vfs_memfd_mount()) {
        return -EINVAL;
    }
    if (m->state != VFS_ACTIVE) {
        return -EBUSY;
    }
    for (i = 0; i < mount_slots; i++) {
        struct vfs_mount *c = mounts[i];

        if (c && c->state == VFS_ACTIVE && vfs_mount_seen(c) && detached_by(c, m, root)) {
            vfs_mount_hide(c);
        }
    }
    klog(KLOG_INFO, "vfs", "detached: %s", m->covered_path);
    vfs_reap_detached();
    return 0;
}

/**
 * Each candidate's view is read again as the loop reaches it, since an
 * unmount may sleep: meanwhile a namespace can be made and a mount made in
 * it. The read and the test run under the big lock with no sleep between
 * them, and `unmount_mount` sets VFS_UNMOUNTING before it first sleeps; a
 * namespace made after that copies its parent's view, from which the mount
 * is hidden, so nothing comes to see it.
 */
void vfs_reap_detached(void)
{
    int i, progress = 1;

    while (progress) {
        progress = 0;
        for (i = VFS_PERMANENT; i < mount_slots; i++) {
            struct vfs_mount *m = mounts[i];

            if (m && m->state == VFS_ACTIVE && vfs_mount_unseen(m) && m->refs == 0 &&
                m->inprogress == 0 && unmount_mount(m) == 0) {
                progress = 1;
            }
        }
    }
}

/**
 * Reap the detached mounts when `mnt`, which a hold or a description just
 * let go of with the put that left `left` references, is one no live
 * namespace sees and nothing holds any more. The decision is the put's
 * count, never a read of `mnt`: once the count is 0 an unmount or another
 * closer's reap may free the record (`mount_release`), so `mnt` is read only
 * under the big lock, which those take, and only once the table still has it
 * (`vfs_mount_index`), as Linux's `mntput_no_expire` drops the count under
 * RCU and decides under `lock_mount_hash` (fs/namespace.c:1405-1430). A
 * record freed and made again at the same address is a live mount, judged
 * on its own fields.
 */
static void reap_if_detached(const struct vfs_mount *mnt, uint32_t left)
{
    int big;

    if (left != 0) {
        return;
    }
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    if (vfs_mount_index(mnt) >= 0 && mnt->refs == 0 && mnt->state == VFS_ACTIVE &&
        !mount_permanent(mnt) && vfs_mount_unseen(mnt)) {
        vfs_reap_detached();
    }
    if (big) {
        bkl_exit();
    }
}

/**
 * The checks of pivot_root on the new root `nref`, the place `oref` the old
 * root goes to, and the caller's root, the root of `rmnt` at `rnode`, in
 * Linux's order; `*same` says whether `oref` is the new root itself. 0,
 * -EINVAL, -ENOTDIR, -EBUSY or the error of `vfs_node_path`.
 */
static int pivot_check(const struct vfs_ref *nref, const struct vfs_ref *oref,
                       const struct vfs_mount *rmnt, const void *rnode, int *same)
{
    struct vfs_stat st;
    char opath[VFS_COVERED_PATH_MAX];
    int rc = vfs_getattr(oref->mnt, oref->node, &st);

    if (rc != 0) {
        return rc;
    }
    if (!VFS_IS_DIR(st.mode)) {
        return -ENOTDIR;
    }
    if (nref->node != nref->mnt->root || nref->mnt->type->ops->lookup_rest ||
        mount_permanent(nref->mnt) || rnode != rmnt->root) {
        return -EINVAL;
    }
    if (nref->mnt == rmnt || oref->mnt == rmnt) {
        return -EBUSY;
    }
    *same = oref->mnt == nref->mnt && oref->node == nref->node;
    if (*same) {
        return 0;
    }
    rc = vfs_node_path(oref->mnt, oref->node, opath, sizeof(opath));
    if (rc != 0) {
        return rc;
    }
    return path_under(opath, nref->mnt->covered_path, (uint32_t)k_strlen(nref->mnt->covered_path))
               ? 0
               : -EINVAL;
}

int vfs_pivot_mounts(const char *new_root, const char *put_old, struct vfs_ref *out)
{
    struct vfs_ref nref, oref, sref;
    struct vfs_mount *rmnt = root_mount_of_caller();
    void *rnode =
        current_proc && current_proc->fs->root_mnt ? current_proc->fs->root_node : mounts[0]->root;
    int i, rc, same = 0;

    tree_seq++;
    rc = vfs_resolve(0, new_root, 0, &nref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_resolve(0, put_old, 0, &oref, 0, 0);
    if (rc != 0) {
        vfs_ref_release(&nref);
        return rc;
    }
    rc = pivot_check(&nref, &oref, rmnt, rnode, &same);
    if (rc == 0 && !same) {
        sref.mnt = rmnt;
        sref.node = rnode;
        rc = vfs_enter(rmnt);
        if (rc == 0) {
            vfs_ref_hold(&sref);
            rc = bind_refs(&sref, &oref, put_old, 1, nref.mnt, NULL, NULL);
        } else {
            vfs_ref_release(&oref);
        }
        if (rc != 0) {
            vfs_ref_release(&nref);
            return rc;
        }
        for (i = VFS_PERMANENT; i < mount_slots; i++) {
            struct vfs_mount *c = mounts[i];

            if (c && c->state == VFS_ACTIVE && vfs_mount_seen(c) && mount_within(c, rmnt) &&
                !mount_within(c, nref.mnt) && !mount_within(nref.mnt, c)) {
                vfs_mount_hide(c);
            }
        }
    } else {
        vfs_ref_release(&oref);
    }
    if (rc != 0) {
        vfs_ref_release(&nref);
        return rc;
    }
    klog(KLOG_INFO, "vfs", "pivoted: %s", nref.mnt->covered_path);
    *out = nref;
    vfs_reap_detached();
    return 0;
}

/**
 * The end of a bind's unmount, after its operations drained and nothing
 * holds it: its root and covered node are put back and its parent and base
 * let go; the file system it showed is not written, since its base still
 * shows it.
 */
static int unmount_bind(struct vfs_mount *m)
{
    struct vfs_ref cov;

    vfs_ns_lock(m);
    if (m->type->ops->put) {
        m->type->ops->put(mount_base(m), m->root);
    }
    inotify_unmount(m);
    vfs_ns_unlock(m);
    klog(KLOG_INFO, "vfs", "unmounted %s: %d opens, %d releases", m->covered_path, (int)m->opens,
         (int)m->releases);
    cov.mnt = m->parent;
    cov.node = m->covered;
    if (cov.node) {
        vfs_ref_put(&cov);
    }
    vfs_mount_put(m->parent);
    vfs_mount_put(m->bind_of);
    mount_release(m);
    return 0;
}

/**
 * Unmount `m`, an active mount no other mount namespace sees, as
 * `vfs_do_umount` describes. 0, or the error that left it mounted.
 *
 * A device that has gone takes nothing, so the sync, the preparation, the
 * writeback and the flush are skipped for it: each would fail, and an
 * unmount that failed on a stick that is no longer there would keep the
 * mount for good. Nothing irreversible happens before the cache drops the
 * device's buffers, so a failure up to there returns the mount to
 * VFS_ACTIVE, with the type's `abort_unmount` (`unmount_abandoned`) once
 * `prepare_unmount` has run.
 */
static int unmount_mount(struct vfs_mount *m)
{
    int gone;
    struct vfs_ref cov;
    int rc;
    block_device_t *dev = m->dev;

    arch_atomic_xchg((volatile unsigned int *)(void *)&m->state, (unsigned int)VFS_UNMOUNTING);
    tree_seq++;
    while (*(volatile uint32_t *)&m->inprogress > 0) {
        wq_wait_event_nointr(&m->drain_wq, drained, m);
    }
    if (m->refs > 0) {
        m->state = VFS_ACTIVE;
        return -EBUSY;
    }
    if (m->bind_of) {
        return unmount_bind(m);
    }
    dcache_forget_mount(m);
    vfs_ns_lock(m);
    gone = vfs_mount_gone(m);
    if (!gone && m->type->ops->sync) {
        rc = m->type->ops->sync(m);
        if (rc != 0) {
            vfs_ns_unlock(m);
            m->state = VFS_ACTIVE;
            return rc;
        }
    }
    if (!gone && m->type->ops->prepare_unmount) {
        rc = m->type->ops->prepare_unmount(m);
        if (rc != 0) {
            vfs_ns_unlock(m);
            m->state = VFS_ACTIVE;
            return rc;
        }
    }
    if (!gone && m->dev && bcache_sync(m->dev) != 0) {
        unmount_abandoned(m);
        vfs_ns_unlock(m);
        m->state = VFS_ACTIVE;
        return -EIO;
    }
    /* A disk's own write cache reaches the medium only at a flush. */
    rc = gone || !m->dev ? 0 : block_flush(m->dev);
    if (rc != 0) {
        vfs_device_failed(m->dev, 0, rc);
        unmount_abandoned(m);
        vfs_ns_unlock(m);
        m->state = VFS_ACTIVE;
        return -EIO;
    }
    if (m->dev && bcache_invalidate(m->dev) != 0) {
        unmount_abandoned(m);
        vfs_ns_unlock(m);
        m->state = VFS_ACTIVE;
        return -EBUSY;
    }
    klog(KLOG_INFO, "vfs", "unmounted %s: %d opens, %d releases", m->covered_path, (int)m->opens,
         (int)m->releases);
    if (m->type->ops->put) {
        m->type->ops->put(m, m->root);
    }
    inotify_unmount(m);
    filecache_forget_mount(m);
    if (m->type->ops->unmount) {
        m->type->ops->unmount(m);
    }
    vfs_ns_unlock(m);
    cov.mnt = m->parent;
    cov.node = m->covered;
    vfs_ref_put(&cov);
    vfs_mount_put(m->parent);
    devfs_release_claim(m->claim);
    mount_release(m);
    loop_unmounted(dev);
    return 0;
}

int vfs_unmount_record(struct vfs_mount *m)
{
    return unmount_mount(m);
}

/* The working directory's and the root's holds. */

/**
 * Let go of a working directory's or a root's hold on `node` of `mnt`: the
 * put, which may sleep, and then the mount's count, which keeps the mount
 * until the put is done. A NULL `mnt` holds nothing.
 */
static void hold_release(struct vfs_mount *mnt, void *node)
{
    struct vfs_ref r;

    if (!mnt) {
        return;
    }
    r.mnt = mnt;
    r.node = node;
    vfs_ref_put(&r);
    reap_if_detached(mnt, vfs_mount_put(mnt));
}

/**
 * The new directory is stored before the old one is let go of, since the
 * put may sleep: a thread that shares the working directory (CLONE_FS) and
 * changes it meanwhile replaces a hold that is in the record, and the pair
 * in the record stays the directory its path names. KRFFS, the boot root
 * and a mounted volume, has no `hold`: the working directory takes only the
 * mount's count, and the record keeps its node for `rmdir`, which answers
 * EBUSY for a directory that is a working directory.
 */
void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    struct vfs_mount *was = p->fs->cwd_mnt;
    void *was_node = p->fs->cwd_node;
    uint32_t flags;

    if (mnt && (mnt == mounts[1] || mnt == mounts[2])) {
        mnt = 0;
    }
    if (mnt) {
        if (mnt->type->ops->hold) {
            mnt->type->ops->hold(mount_base(mnt), node);
        }
        vfs_mount_get(mnt);
    }
    flags = spin_lock_irqsave(&p->fs->lock);
    p->fs->cwd_mnt = mnt;
    p->fs->cwd_node = mnt ? node : 0;
    spin_unlock_irqrestore(&p->fs->lock, flags);
    hold_release(was, was_node);
}

/**
 * The new root is stored before the old one is let go of, since the put may
 * sleep: a thread that shares the root (CLONE_FS) and resolves a path
 * meanwhile finds one of the two roots in the record, never none, which
 * would be the root of the mount table.
 */
void vfs_root_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    struct vfs_mount *was = p->fs->root_mnt;
    void *was_node = p->fs->root_node;
    uint32_t flags;

    if (mnt && mnt == mounts[0] && node == mnt->root) {
        mnt = 0;
    }
    if (mnt) {
        if (mnt->type->ops->hold) {
            mnt->type->ops->hold(mount_base(mnt), node);
        }
        vfs_mount_get(mnt);
    }
    flags = spin_lock_irqsave(&p->fs->lock);
    p->fs->root_mnt = mnt;
    p->fs->root_node = mnt ? node : 0;
    spin_unlock_irqrestore(&p->fs->lock, flags);
    hold_release(was, was_node);
}

void vfs_root_copy(struct process *child, const struct process *parent)
{
    vfs_root_set(child, parent->fs->root_mnt, parent->fs->root_node);
}

void vfs_root_clear(struct process *p)
{
    vfs_root_set(p, 0, 0);
}

void vfs_cwd_clear(struct process *p)
{
    vfs_cwd_set(p, 0, 0);
}

int vfs_xattr(struct vfs_mount *mnt, void *node, struct vfs_xattr *arg)
{
    struct vfs_stat st;
    int rc;

    if (vfs_mount_gone(mnt))
        return -EIO;
    if (!mnt->type->ops->xattr)
        return arg->op == VFS_XATTR_LIST ? 0 : -EOPNOTSUPP;
    if (arg->op >= VFS_XATTR_SET) {
        if (mount_rdonly(mnt))
            return -EROFS;
        if (guard_node(mnt, node))
            return -EPERM;
    }
    rc = mnt->type->ops->xattr(mount_base(mnt), node, arg);
    if (rc == 0 && arg->op >= VFS_XATTR_SET && inotify_watches && watched_node(mnt, node, &st) == 0)
        inotify_file(mnt, st.ino, IN_ATTRIB);
    return rc;
}
