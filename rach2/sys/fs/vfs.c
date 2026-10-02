/*
 * vfs.c: the mount table and the resolver. The contract is in vfs.h; what
 * is here is the walk, one component at a time, the reference and transient
 * counts, the per-mount namespace lock, the default-answer wrappers, the
 * glue that makes KRFFS and the two synthetic namespaces answer the
 * operation interface, and mount(2) and umount2(2).
 */

#include "../mm/kheap.h"
#include "vfs.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/pagecache.h"
#include "../panic.h"
#include "../proc/process.h"
#include "bcache.h"
#include "devfs.h"
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
#include "../io/inotify.h"

#define VFS_S_IFMT       0170000u
#define VFS_S_IFDIR      0040000u
#define VFS_S_IFREG      0100000u
#define VFS_IS_DIR(mode) (((mode) & VFS_S_IFMT) == VFS_S_IFDIR)

/*
 * The mounts, by number: an array of pointers that grows, each record
 * allocated from the heap when the mount is made and freed when it is
 * unmounted; a NULL slot is free. The root is 0, /dev 1 and /proc 2, for
 * the life of the kernel.
 */
static struct vfs_mount **mounts;
static int mount_slots;

int vfs_mount_slots(void)
{
    return mount_slots;
}

/* The array of eight slots, made once: the boot's root installers both
 * need it, and either can run first. */
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

/* A record in slot `idx`, allocated if the slot is empty; NULL when the
 * heap has none. */
static struct vfs_mount *mount_record(int idx)
{
    if (!mounts[idx]) {
        mounts[idx] = kzalloc(sizeof(struct vfs_mount));
    }
    return mounts[idx];
}

/* The first free slot past the permanent ones, the array grown by doubling
 * when every slot is taken, with a record in it; -1 when the heap has
 * none. */
static int mount_new_slot(void)
{
    int i;

    for (i = VFS_PERMANENT; i < mount_slots; i++) {
        if (!mounts[i]) {
            return mount_record(i) ? i : -1;
        }
    }
    {
        int more = mount_slots * 2;
        struct vfs_mount **grown = kzalloc(sizeof(*grown) * more);

        if (!grown) {
            return -1;
        }
        k_memcpy(grown, mounts, sizeof(*grown) * mount_slots);
        kfree(mounts);
        mounts = grown;
        i = mount_slots;
        mount_slots = more;
    }
    return mount_record(i) ? i : -1;
}

/* Give a user mount's record back; its slot is free for the next. */
static void mount_release(struct vfs_mount *m)
{
    int i;

    for (i = VFS_PERMANENT; i < mount_slots; i++) {
        if (mounts[i] == m) {
            mounts[i] = NULL;
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

struct vfs_mount *vfs_root_mount(void)
{
    return mounts[0];
}

/**
 * Test both the mounted view and its backing device for removal.
 * Partition views can be invalidated before the physical record is released.
 */
int vfs_mount_gone(const struct vfs_mount *mnt)
{
    return mnt->dev && ((mnt->dev->flags | block_flags(mnt->dev)) & BLOCK_GONE);
}

/* Reference and transient counts. */

void vfs_ref_hold(const struct vfs_ref *r)
{
    if (r->mnt->type->ops->hold) {
        r->mnt->type->ops->hold(r->mnt, r->node);
    }
}

void vfs_ref_put(struct vfs_ref *r)
{
    if (r->mnt && r->mnt->type->ops->put) {
        r->mnt->type->ops->put(r->mnt, r->node);
    }
    r->mnt = 0;
    r->node = 0;
}

int vfs_enter(struct vfs_mount *mnt)
{
    if (mnt->state != VFS_ACTIVE) {
        return -EBUSY;
    }
    mnt->inprogress++;
    return 0;
}

void vfs_leave(struct vfs_mount *mnt)
{
    if (mnt->inprogress == 0) {
        k_panic("vfs: an operation left a mount it never entered");
    }
    mnt->inprogress--;
    if (mnt->inprogress == 0 && mnt->state == VFS_UNMOUNTING) {
        wq_wake_all(&mnt->ns_wq); /* the unmount drains on this queue */
    }
}

/* The namespace lock. */

struct ns_turn {
    struct vfs_mount *mnt;
    uint32_t place;
};

static int ns_my_turn(void *arg)
{
    const struct ns_turn *t = arg;
    return t->mnt->ns_holder == -1 && t->mnt->ns_serving == t->place;
}

int vfs_ns_held_by_caller(void)
{
    int me = current_proc ? (int)current_proc->pid : 0;
    int i;

    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->ns_holder == me &&
            mounts[i]->ns_depth > 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Take the mount's lock, in the order the callers asked for it.
 *
 * A caller takes a place in the line and waits until the lock is free and
 * its place is served, so a process that releases the lock and asks again
 * waits behind everyone who asked meanwhile: a writer that locks for every
 * call cannot take the lock back before a waiter has had it, and so cannot
 * starve the waiter. The order costs no switch of its own; releasing the
 * lock does not yield. A caller that holds the lock takes it again without a
 * place. The wait sleeps and a signal does not end it.
 */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    int me = current_proc ? (int)current_proc->pid : 0;
    struct ns_turn turn;

    if (mnt->ns_holder == me && mnt->ns_depth > 0) {
        mnt->ns_depth++;
        return;
    }
    turn.mnt = mnt;
    turn.place = mnt->ns_next++;
    while (!ns_my_turn(&turn)) {
        wq_wait_event_nointr(&mnt->ns_wq, ns_my_turn, &turn);
    }
    mnt->ns_holder = me;
    mnt->ns_depth = 1;
}

/** Release the lock and serve the next place in the line. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    if (mnt->ns_depth > 1) {
        mnt->ns_depth--;
        return;
    }
    mnt->ns_depth = 0;
    mnt->ns_holder = -1;
    mnt->ns_serving++;
    wq_wake_all(&mnt->ns_wq);
}

/* The permission the resolver checks. */

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

/*
 * The classic nine-bit rule over the attributes the file system reports:
 * one identity class is chosen and judged alone, and a uid of 0 passes
 * unless the thread gave up its capabilities (`nocaps`). `use_real` judges
 * by the real ids, for access(2), the one call that asks who the process
 * really is. The search bit is what walking a directory costs.
 */
static int may_search(struct vfs_mount *mnt, void *node, int use_real)
{
    struct vfs_stat st;
    uint32_t uid, bits;
    int rc;

    if (!mnt->type->ops->getattr) {
        return 0; /* a flat synthetic namespace is searchable */
    }
    rc = mnt->type->ops->getattr(mnt, node, &st);
    if (rc != 0) {
        return rc;
    }
    if (!VFS_IS_DIR(st.mode)) {
        return -ENOTDIR;
    }
    if (!current_proc) {
        return 0;
    }
    uid = use_real ? current_proc->ruid : current_proc->euid;
    if (uid == 0 && !current_proc->nocaps) {
        return 0;
    }
    if (uid == st.uid) {
        bits = st.mode >> 6;
    } else if ((use_real ? current_proc->rgid : current_proc->egid) == st.gid ||
               in_groups(st.gid)) {
        bits = st.mode >> 3;
    } else {
        bits = st.mode;
    }
    return (bits & 1u) ? 0 : -EACCES;
}

/* The resolver. */

/* The mount whose root replaces `node` when the walk arrives at it: a child
 * of `mnt` covering that node. The two synthetic mounts cover no node; they
 * are entered by name from the root directory, which `mount_named`
 * matches. */
static struct vfs_mount *mount_covering(struct vfs_mount *mnt, void *node)
{
    int i;

    for (i = 1; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->parent == mnt &&
            mounts[i]->covered && mounts[i]->covered == node) {
            return mounts[i]; /* a transitional mount answers EBUSY on entry */
        }
    }
    return 0;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The last component of a mount's recorded path: the name a nameless mount
 * stands at in its parent's root directory. */
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

/* The nameless mount standing at the name comp[0..len) in the root
 * directory of `mnt`: /dev and /proc in the root's, /dev/shm in /dev's; or
 * NULL. */
static struct vfs_mount *mount_named_at(struct vfs_mount *mnt, const char *comp, uint32_t len)
{
    int i;

    for (i = 1; i < mount_slots; i++) {
        struct vfs_mount *m = mounts[i];
        const char *name;
        uint32_t n;

        if (!m || m->state != VFS_ACTIVE || m->parent != mnt || m->covered) {
            continue;
        }
        name = path_last(m->covered_path);
        for (n = 0; n < len && name[n] == comp[n]; n++) {
        }
        if (n == len && name[n] == '\0') {
            return m;
        }
    }
    return 0;
}

/* The nameless mount standing at `name` in the root directory `node` of
 * `mnt`, or NULL. */
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

        if (!m || m->state != VFS_ACTIVE || m->parent != parent || m->covered || n-- > 0) {
            continue;
        }
        *name = path_last(m->covered_path);
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
    if (!mnt->type->ops->lookup || mnt->type->ops->lookup(mnt, node, name, &found) != 0) {
        return 0;
    }
    /* A mount still being made covers the node as well as an active one. */
    for (i = 1; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->parent == mnt &&
            mounts[i]->covered == found) {
            m = mounts[i];
        }
    }
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mnt, found);
    }
    return m;
}

/** Put a node a lookup returned, on a type that counts holds. */
static void node_put(struct vfs_mount *mnt, void *node)
{
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mnt, node);
    }
}

int vfs_leaf_root(struct vfs_mount *mnt, void *node, const char *name)
{
    void *found;
    process_t *pr;
    int is = 0;

    for (pr = proc_list; pr && !(pr->fs && pr->fs->root_mnt == mnt); pr = pr->next) {
    }
    if (!pr || !mnt->type->ops->lookup || mnt->type->ops->lookup(mnt, node, name, &found) != 0) {
        return 0;
    }
    for (pr = proc_list; pr && !is; pr = pr->next) {
        is = pr->fs && pr->fs->root_mnt == mnt && pr->fs->root_node == found;
    }
    node_put(mnt, found);
    return is;
}

int vfs_node_path(struct vfs_mount *mnt, void *node, char *out, uint32_t cap)
{
    uint32_t ml = k_strlen(mnt->covered_path);
    int r;

    if (mnt == mounts[0] && mnt->type == &vfs_krffs_type) {
        return krffs_node_path((krffs_node_t *)node, out, cap) == 0 ? 0 : -ENAMETOOLONG;
    }
    if (!mnt->type->ops->path) {
        return -ENOENT;
    }
    if (ml <= 1) {
        ml = 0; /* the root mount: the type's path is the whole */
    }
    if (ml + 2 > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(out, mnt->covered_path, ml);
    r = mnt->type->ops->path(mnt, node, out + ml, cap - ml);
    if (r != 0) {
        return r;
    }
    if (ml > 0 && out[ml + 1] == '\0') {
        out[ml] = '\0'; /* the mount's root is the directory it covers */
    }
    return 0;
}

/*
 * A locked walk takes a mount's namespace lock only where the type asks for
 * it. KRFFS moves a file's bytes while they are open and ext2 holds a batch
 * of dirty blocks between calls, so an open of either must register its
 * description under the lock or it can land on an offset another file has
 * taken. A type with neither property needs nothing here, and locking it
 * would serialise lookups that are meant to run together: two processes
 * opening one cold file on a FAT volume must both reach the buffer cache,
 * which is what `tests/run mount` counts.
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
 * Move the held node and transient reference to the next mount.
 *
 * A locked walk releases the old mount before waiting for the new lock.
 * Mount crossings use a held covered directory or a mount's root.
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
    /* The put, which may sleep, before the mount is left; the put clears
     * the reference, so the mount is remembered first. */
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

/* Where an absolute path starts: the calling process's root, which chroot
 * set, or the root of the root mount. */
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

/* Whether `..` at `cur` stays there: at the walk's root, and at the root of
 * the root mount, above which there is nothing, whatever root the walk has. */
static int dotdot_stays(const struct vfs_ref *cur, const struct vfs_ref *root)
{
    return (cur->mnt == root->mnt && cur->node == root->node) ||
           (cur->mnt == mounts[0] && cur->node == mounts[0]->root);
}

/* Replace the node `cur` holds with `node`, held already, on the same mount:
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

/** Copy the unwalked final component and locate it within a parent path. */
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

/*
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
    uint32_t rlen = (uint32_t)k_strlen(rest);
    uint32_t join = rlen != 0 ? 1u : 0u;
    int tlen;

    if (++*links > VFS_LINK_MAX) {
        node_put(mnt, found);
        return -ELOOP;
    }
    if (!*linkpath) {
        *linkpath = path_buf_get();
        *target = path_buf_get();
        if (!*linkpath || !*target) {
            node_put(mnt, found);
            return -ENOMEM;
        }
    }
    tlen = mnt->type->ops->readlink(mnt, found, *target, VFS_PATH_MAX);
    node_put(mnt, found);
    if (tlen < 0) {
        return tlen;
    }
    if (tlen == 0) {
        return -ENOENT; /* an empty target names nothing */
    }
    if ((uint32_t)tlen + join + rlen + 1 > VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    /* The rest may already be inside `linkpath`, from a link followed before
     * this one, so it moves before the target is written over it. */
    k_memmove(*linkpath + tlen + join, rest, rlen + 1);
    k_memcpy(*linkpath, *target, (uint32_t)tlen);
    if (join) {
        (*linkpath)[tlen] = '/';
    }
    return (*target)[0] == '/';
}

static int walk_exe(struct vfs_ref *cur, void *found, int locked);

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
           mnt->type->ops->getattr(mnt, node, &st) == 0 && (st.mode & 0170000u) == 0120000u;
}

/**
 * Resolve one path. See vfs.h for the whole contract. The loop takes one
 * component at a time and handles five cases: `.`, `..`, a mount whose type
 * takes the rest of the path in one call, a nameless mount entered by name
 * from the root directory, and the ordinary lookup of one name, after which
 * a symbolic link is followed and a covering mount is crossed.
 *
 * On a type with symbolic links, a component that is one is followed when
 * more of the path comes after it, and at the last component unless the
 * caller passed VFS_WALK_NOFOLLOW. Its target replaces the component, an
 * absolute target restarts at the root, and the walk goes on from the
 * directory the link was found in. A mount covers a directory, so a link is
 * never a covered node. /proc/<pid>/exe is followed to the program's file
 * itself (`walk_exe`), and no text.
 *
 * The target and the path it is joined to are in two path pages, taken at
 * the first link and given back on every return; a walk that meets no link
 * takes none.
 *
 * `..` at the root of a mount over a directory runs again at the covered
 * directory, in the parent mount, since the mount stands in for it. A
 * nameless mount stands by name in its parent's root directory, as /dev/shm
 * does in /dev's, so `..` at its root is that directory and is used up
 * there: run again, the synthetic type would climb once more.
 *
 * The walk keeps the root it starts from: `..` stops there, and at the root
 * of the root mount whatever the root, and an absolute link's target starts
 * there. A thread that shares the root may chroot while the walk sleeps; at
 * its next `..` or absolute link the walk sees that (`root_current`), lets
 * go of where it is and starts over from the new root, so no walk mixes two.
 *
 * VFS_WALK_LOCK holds one namespace lock at a time and returns with the
 * final mount locked when its type asks for the lock, so an open can
 * register before relocation. An unmount may resolve a removed mount at the
 * final component only, so it can release that mount without permitting
 * lookup inside it.
 */
static int resolve_path(const struct vfs_ref *start, const char *path, int flags,
                        struct vfs_ref *out, char *leaf, uint32_t leafcap, int unmount)
{
    struct vfs_ref cur;
    struct vfs_ref root;
    const char *p;
    const char *leaf_comp = 0;
    const char *last_comp;
    int use_real = (flags & VFS_WALK_REAL_IDS) ? 1 : 0;
    int links;
    int locked = (flags & VFS_WALK_LOCK) != 0;
    struct vfs_mount *was;
    char *linkpath = 0;
    char *target = 0;
    int rc;

    if (!path || !out) {
        return -EINVAL;
    }
    if (mounts[0]->state != VFS_ACTIVE) {
        return -ENOENT;
    }

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
    walk_root(&root);
    cur = (p[0] == '/' || !start) ? root : *start;
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
        if (*p == '\0' || (last_comp && p >= last_comp)) {
            break;
        }
        if (vfs_mount_gone(cur.mnt)) {
            rc = -EIO;
            goto out_err;
        }

        /* The rest-taking mounts: the whole remainder in one call, with a
         * climb above their root returned here to continue at the parent. */
        if (cur.mnt->type->ops->lookup_rest && !named_in_rest(&cur, p)) {
            const char *left = 0;
            char restbuf[VFS_NAME_MAX + 1];
            const char *rest = p;

            /* With WALK_PARENT only the part before the leaf is walked. */
            if (last_comp) {
                uint32_t rlen = (uint32_t)(last_comp - p);

                while (rlen > 0 && p[rlen - 1] == '/') {
                    rlen--;
                }
                if (rlen == 0) {
                    break; /* the parent is this mount's root */
                }
                if (rlen > VFS_NAME_MAX) {
                    rc = -ENAMETOOLONG;
                    goto out_err;
                }
                k_memcpy(restbuf, p, rlen);
                restbuf[rlen] = '\0';
                rest = restbuf;
            }
            rc = cur.mnt->type->ops->lookup_rest(cur.mnt, rest, &found, &left);
            if (rc == VFS_CLIMB || rc == VFS_ROOT) {
                struct vfs_mount *pm = cur.mnt->parent ? cur.mnt->parent : mounts[0];
                void *pnode = cur.mnt->covered ? cur.mnt->covered : mounts[0]->root;
                if (rest == restbuf) {
                    /* The type used part of a bounded copy; continue at the
                     * same offset of the real path. */
                    p += left - restbuf;
                } else {
                    p = left;
                }
                if (rc == VFS_ROOT) {
                    continue;
                }
                rc = walk_move(&cur, pm, pnode, locked);
                if (rc != 0) {
                    goto out_err;
                }
                continue;
            }
            if (rc != 0) {
                goto out_err;
            }
            if (!last_comp && !(flags & VFS_WALK_NOFOLLOW) && walk_is_link(cur.mnt, found)) {
                if (vfs_synth_vpath(cur.mnt, found)->kind == VNODE_PROC_EXE) {
                    rc = walk_exe(&cur, found, locked);
                    if (rc != 0) {
                        goto out_err;
                    }
                    break;
                }
                rc = walk_link(cur.mnt, found, "", &linkpath, &target, &links);
                if (rc < 0) {
                    goto out_err;
                }
                p = linkpath;
                if (rc == 1) {
                    if (!root_current(&root)) {
                        goto restart;
                    }
                    rc = walk_move(&cur, root.mnt, root.node, locked);
                    if (rc != 0) {
                        goto out_err;
                    }
                }
                continue;
            }
            ref_move_node(&cur, found);
            break;
        }

        rc = may_search(cur.mnt, cur.node, use_real);
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
            if (!root_current(&root)) {
                goto restart;
            }
            if (dotdot_stays(&cur, &root)) {
                continue; /* a root is its own parent */
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
                rc = walk_move(&cur, cur.mnt->parent, cur.mnt->parent->root, locked);
                if (rc != 0) {
                    goto out_err;
                }
                continue;
            }
        }

        /* The nameless mounts, entered by name from the root directory. */
        m = mount_named(cur.mnt, cur.node, comp);
        if (m) {
            rc = walk_move(&cur, m, m->root, locked);
            if (rc != 0) {
                goto out_err;
            }
            continue;
        }

        rc = cur.mnt->type->ops->lookup(cur.mnt, cur.node, comp, &found);
        if (rc != 0) {
            goto out_err;
        }
        if (cur.mnt->type->ops->readlink && cur.mnt->type->ops->getattr) {
            struct vfs_stat lst;
            const char *rest = p;
            int more;

            while (*rest == '/') {
                rest++;
            }
            more = *rest != '\0';
            rc = cur.mnt->type->ops->getattr(cur.mnt, found, &lst);
            if (rc != 0) {
                node_put(cur.mnt, found);
                goto out_err;
            }
            if ((lst.mode & 0170000u) == 0120000u && (more || !(flags & VFS_WALK_NOFOLLOW))) {
                int absolute = walk_link(cur.mnt, found, rest, &linkpath, &target, &links);

                if (absolute < 0) {
                    rc = absolute;
                    goto out_err;
                }
                if (last_comp) {
                    last_comp = walk_last_comp(linkpath);
                }
                p = linkpath;
                if (absolute) {
                    if (!root_current(&root)) {
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
        ref_move_node(&cur, found);

        /* A directory covered by a mount is replaced by that mount's root
         * before the walk continues. */
        m = mount_covering(cur.mnt, cur.node);
        if (m) {
            rc = walk_move(&cur, m, m->root, locked);
            if (rc != 0) {
                goto out_err;
            }
        }
    }

    if (vfs_mount_gone(cur.mnt) && !unmount) {
        rc = -EIO;
        goto out_err;
    }
    if (flags & VFS_WALK_PARENT) {
        rc = may_search(cur.mnt, cur.node, use_real);
        if (rc != 0) {
            goto out_err;
        }
    }
    path_buf_put(linkpath);
    path_buf_put(target);
    *out = cur;
    return 0;

restart:
    was = cur.mnt;
    vfs_ref_put(&cur);
    walk_unlock(was, locked);
    vfs_leave(was);
    goto begin;

out_err:
    path_buf_put(linkpath);
    path_buf_put(target);
    was = cur.mnt;
    vfs_ref_put(&cur);
    walk_unlock(was, locked);
    vfs_leave(was);
    return rc;
}

/** Resolve a path only while its mount's device is present. */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t leafcap)
{
    return resolve_path(start, path, flags, out, leaf, leafcap, 0);
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
    mnt->refs++;
}

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
    mnt->refs--;
}

/* Repointing, for a KRFFS relocation. */

void vfs_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    process_t *pr;
    int i, j;

    for (pr = proc_list; pr; pr = pr->next) {
        if (mnt == mounts[0]) {
            for (j = 0; j < fd_table_cap(pr->fds); j++) {
                fd_entry_t *e = &pr->fds->entries[j];

                if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) &&
                    (void *)e->node == from) {
                    e->node = to;
                }
            }
        }
        if (pr->fs && pr->fs->cwd_mnt == mnt && pr->fs->cwd_node == from) {
            pr->fs->cwd_node = to;
        }
        if (pr->fs && pr->fs->root_mnt == mnt && pr->fs->root_node == from) {
            pr->fs->root_node = to;
        }
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
    fd_ofd_repoint(mnt, from, to);
    loop_repoint(mnt, from, to);
    lock_repoint(mnt, (uint64_t)(uintptr_t)from, (uint64_t)(uintptr_t)to);
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
    return mnt->type->ops->read(mnt, handle, buf, count, offset);
}

int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->write) {
        return -EROFS;
    }
    return mnt->type->ops->write(mnt, handle, buf, count, offset);
}

/**
 * Truncate through an open handle unless a loop device holds the file.
 *
 * The KRFFS root uses its node as a handle, including for `O_TRUNC`.
 * Mounted types receive the handle returned by their open operation.
 */
int vfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (loop_backs(mnt, handle)) {
        return -EBUSY;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->truncate) {
        return -EROFS;
    }
    return mnt->type->ops->truncate(mnt, handle, size);
}

int vfs_fsync(struct vfs_mount *mnt, void *handle)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (!mnt->type->ops->fsync) {
        rc = mnt->dev ? bcache_sync(mnt->dev) : 0;
    } else {
        rc = mnt->type->ops->fsync(mnt, handle);
    }
    if (rc == 0 && mnt->dev) {
        rc = block_flush(mnt->dev);
        if (rc != 0) {
            vfs_device_failed(mnt->dev, 0, rc);
        }
    }
    return rc;
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
        /* The mount itself is one hold, and each description, working
         * directory, covering mount and operation in flight on it is
         * another. An unmount waits out the operations in flight and refuses
         * with EBUSY while anything else still holds it. */
        held += 1 + (int)mounts[i]->refs + (int)mounts[i]->inprogress;
    }
    return held;
}

int vfs_sync_all(void)
{
    int i;

    for (i = 0; i < mount_slots; i++) {
        struct vfs_mount *m = mounts[i];

        if (!m || m->state != VFS_ACTIVE || !m->dev || vfs_mount_gone(m) || vfs_enter(m) != 0) {
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
    return mnt->type->ops->getattr(mnt, node, st);
}

/* inotify's view of the namespace operations: each asks what it needs only
 * while a watch exists (`inotify_watches`). */

/** The attributes of `node`, into `st`; 0, or -1 when the type has none. */
static int watched_node(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    return mnt->type->ops->getattr && mnt->type->ops->getattr(mnt, node, st) == 0 ? 0 : -1;
}

/** The attributes of the entry `name` of `dir`, into `st`; 0, or -1 when
 * there is none. */
static int watched_entry(struct vfs_mount *mnt, void *dir, const char *name, struct vfs_stat *st)
{
    void *found;
    int rc;

    if (!mnt->type->ops->lookup || mnt->type->ops->lookup(mnt, dir, name, &found) != 0) {
        return -1;
    }
    rc = watched_node(mnt, found, st);
    node_put(mnt, found);
    return rc;
}

static uint32_t watched_isdir(const struct vfs_stat *st)
{
    return (st->mode & 0170000u) == 0040000u ? IN_ISDIR : 0u;
}

/** The entry `name` was made in `dir`, a directory when `isdir`. */
static void watch_made(struct vfs_mount *mnt, void *dir, const char *name, uint32_t isdir)
{
    struct vfs_stat st;

    if (inotify_watches && watched_node(mnt, dir, &st) == 0) {
        inotify_child(mnt, st.ino, name, IN_CREATE | isdir, 0);
    }
}

/**
 * The entry `name` of the directory `dir_ino` is gone, and `was` is what it
 * named: a file's links changed, as Linux reports with IN_ATTRIB, and a
 * directory, or a file with no link left, is gone itself. Linux reports
 * that of a file at its last close; here it is at its last link's removal.
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

/** The events of a setattr of `what`, by Linux's `fsnotify_change`: the
 * size and the modification time alone are a change of the data, and both
 * times together are utime's change of the attributes. */
static uint32_t setattr_events(int what)
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
    /* The length of a file that backs a loop device is the device's sector
     * count: nothing may change it while the device is attached. */
    if ((what & VFS_SET_SIZE) && loop_backs(mnt, node)) {
        return -EBUSY;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->setattr) {
        return -EROFS;
    }
    rc = mnt->type->ops->setattr(mnt, node, what, st);
    if (rc == 0 && inotify_watches && watched_node(mnt, node, &now) == 0) {
        inotify_file(mnt, now.ino, setattr_events(what));
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
        rc = mnt->type->ops->open(mnt, node, flags, handle);
        if (rc != 0) {
            return rc;
        }
    } else {
        *handle = node; /* a type that needs no per-open object */
    }
    mnt->opens++;
    return 0;
}

void vfs_release(struct vfs_mount *mnt, void *handle)
{
    if (mnt->type->ops->release) {
        mnt->type->ops->release(mnt, handle);
    }
    mnt->releases++;
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
    return mnt->type->ops->readdir(mnt, handle, cursor, ent);
}

int vfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->create) {
        return -EROFS;
    }
    rc = mnt->type->ops->create(mnt, dir, name, mode, node);
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
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->unlink) {
        return -EROFS;
    }
    watched = inotify_watches && watched_entry(mnt, dir, name, &was) == 0 &&
              watched_node(mnt, dir, &parent) == 0;
    rc = mnt->type->ops->unlink(mnt, dir, name);
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
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->mkdir) {
        return -EROFS;
    }
    rc = mnt->type->ops->mkdir(mnt, dir, name, mode);
    if (rc == 0) {
        watch_made(mnt, dir, name, IN_ISDIR);
    }
    return rc;
}

int vfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct vfs_stat was, parent;
    int watched;
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->rmdir) {
        return -EROFS;
    }
    watched = inotify_watches && watched_entry(mnt, dir, name, &was) == 0 &&
              watched_node(mnt, dir, &parent) == 0;
    rc = mnt->type->ops->rmdir(mnt, dir, name);
    if (rc == 0 && watched) {
        watch_removed(mnt, parent.ino, name, &was);
    }
    return rc;
}

/**
 * Rename, and tell inotify: IN_MOVED_FROM and IN_MOVED_TO with one cookie
 * to the two directories, IN_MOVE_SELF to the file moved, and to a file the
 * new name replaced what its removal would have told.
 */
int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
               const char *newname)
{
    struct vfs_stat moved, replaced, from, to;
    int watched, had_target = 0;
    int rc;

    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->rename) {
        return -EROFS;
    }
    watched = inotify_watches && watched_entry(mnt, olddir, oldname, &moved) == 0 &&
              watched_node(mnt, olddir, &from) == 0 && watched_node(mnt, newdir, &to) == 0;
    if (watched) {
        had_target =
            watched_entry(mnt, newdir, newname, &replaced) == 0 && replaced.ino != moved.ino;
    }
    rc = mnt->type->ops->rename(mnt, olddir, oldname, newdir, newname);
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
    if (mnt->flags & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (!mnt->type->ops->link) {
        return -EPERM; /* a file system without links */
    }
    rc = mnt->type->ops->link(mnt, dir, name, node);
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
    if (mnt->flags & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (!mnt->type->ops->symlink) {
        return -EPERM;
    }
    rc = mnt->type->ops->symlink(mnt, dir, name, target);
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
    if (mnt->flags & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (!mnt->type->ops->mknod) {
        return -EPERM; /* a file system with no such nodes */
    }
    rc = mnt->type->ops->mknod(mnt, dir, name, mode, rdev, node);
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
    return mnt->type->ops->statfs(mnt, st);
}

/* The KRFFS type: the root, adapted onto the interface. */

/* A node handle is the node's place in the mapped image; the root is
 * `krffs_fs->node`. Hold and put are NULL because a place is not a
 * resource. */

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

static int kfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    krffs_node_t *n = node;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    if (n == krffs_fs->node) {
        /* The root's stored mode has the dirty flag in it; it reads as the
         * fixed identity stat reports. */
        st->mode = VFS_S_IFDIR | 0755u;
        st->ino = 1;
        return 0;
    }
    st->mode = krffs_mode_type(n->mode) | (uint32_t)(n->mode & 07777u);
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

static int kfs_sync(struct vfs_mount *mnt)
{
    (void)mnt;
    return pc_sync() == 0 ? 0 : -EIO;
}

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

/* A grow can relocate the node; every holder, this writer's description
 * included, is repointed before the byte count returns. */
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

/**
 * Truncate the held node and repoint its holders if it moves.
 */
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

static int kfs_fsync_locked(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    return pc_sync() == 0 ? 0 : -EIO; /* the window flushes whole */
}

/* The cursor is the entry node's byte offset in the image plus one, so a
 * deletion between calls never renumbers the survivors; the traversal
 * visits nodes in ascending offsets, so the first match at or past the
 * cursor is the next entry. */
static int kfs_readdir_locked(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                              struct vfs_dirent *ent)
{
    krffs_node_t *dirn = handle;
    uint64_t dir_id = (dirn == krffs_fs->node) ? (uint64_t)KRFFS_ROOT_ID : dirn->id;
    krffs_node_t *node = krffs_fs->node;

    (void)mnt;
    do {
        uint32_t node_off = (uint32_t)((uintptr_t)node - (uintptr_t)krffs_fs->node);

        /* An orphan keeps its parent id but has no name; a caller that
         * cannot reach it by path must not see it in a listing either. */
        if (node->type == KRFFS_RESERVED_NODE && node->parent_id == dir_id &&
            node->name[0] != '\0' && node_off >= *cursor) {
            uint32_t nlen = k_strlen((const char *)node->name);

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

/* The namespace operations go through the parent's canonical path, since
 * the namespace half of KRFFS takes paths; a name is joined to the held
 * directory and handed to the path call. */
static int kfs_join(void *dir, const char *name, char *path, uint32_t cap)
{
    krffs_node_t *d = dir ? dir : krffs_fs->node;
    uint32_t dlen, nlen;

    if (krffs_node_path(d, path, cap) != 0) {
        return -ENAMETOOLONG;
    }
    dlen = k_strlen(path);
    nlen = k_strlen(name);
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
 * Make the directory `name` in `dir`. The parent is resolved and held, so
 * the core refuses one of two things: a name whose directory identifier
 * another directory has (`KRFFS_ETAKEN`), which is EINVAL, the answer for a
 * name the file system cannot hold, and a root with no room, ENOSPC.
 */
static int kfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    char path[256];
    int rc = kfs_join(dir, name, path, sizeof(path));

    (void)mnt;
    (void)mode; /* the caller stamps mode and owner under its own rules */
    if (rc != 0) {
        return rc;
    }
    rc = krffs_mkdir(path);
    if (rc == KRFFS_ETAKEN) {
        return -EINVAL;
    }
    return rc == 0 ? 0 : -ENOSPC;
}

static int kfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    char path[256];
    int rc = kfs_join(dir, name, path, sizeof(path));

    (void)mnt;
    if (rc != 0) {
        return rc;
    }
    /* The target is resolved and a directory, so what remains is emptiness. */
    return krffs_rmdir(path) == 0 ? 0 : -ENOTEMPTY;
}

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

/* Creation goes through the parent's canonical path like the other
 * namespace operations; the node comes back resolved. */
static int kfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                      void **node)
{
    char path[256];
    krffs_node_t *n;
    int rc;

    (void)mnt;
    (void)mode; /* the caller stamps mode and owner under its own rules */
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
 * A FIFO or a socket, made as `kfs_create` makes a file, with its type bits
 * on the medium before its name. A device node has a number KRFFS has no
 * field for, and is refused with EPERM. A name already there is EEXIST: the
 * core's create answers success for one, which would make this somebody
 * else's node.
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
 * The data operations, under the mount's lock. The namespace operations
 * arrive locked from the common layer; these take the lock themselves. The
 * disk driver sleeps for its commands inside a write's sync points, and a
 * second process that grows a file of its own meanwhile moves nodes under
 * the first: without the lock, a file whose node moved would be written at
 * the old place, over the header of the node after it. The lock is the same
 * one the namespace operations hold, so a create and a write cannot
 * interleave either.
 */
static int32_t kfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                        uint64_t offset)
{
    int32_t rc;

    vfs_ns_lock(mnt);
    rc = kfs_read_locked(mnt, handle, buf, count, offset);
    vfs_ns_unlock(mnt);
    return rc;
}

static int32_t kfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                         uint64_t offset)
{
    int32_t rc;

    vfs_ns_lock(mnt);
    rc = kfs_write_locked(mnt, handle, buf, count, offset);
    vfs_ns_unlock(mnt);
    return rc;
}

static int kfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    int rc;

    vfs_ns_lock(mnt);
    rc = kfs_truncate_locked(mnt, handle, size);
    vfs_ns_unlock(mnt);
    return rc;
}

static int kfs_fsync(struct vfs_mount *mnt, void *handle)
{
    int rc;

    vfs_ns_lock(mnt);
    rc = kfs_fsync_locked(mnt, handle);
    vfs_ns_unlock(mnt);
    return rc;
}

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
    0,                           /* path: KRFFS directories do not move */
    0,                           /* symlink */
    kfs_mknod,   0,              /* pin: the syscall layer's holders keep a node */
    0,                           /* unpin */
    0,                           /* remount */
};

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

const struct vfs_type vfs_krffs_type = {"krffs", kfs_mount, &krffs_ops, 1, 0};

/* The two synthetic types. */

/*
 * A synthetic node is a classified path. The classification has a device
 * leaf name in it, which a value packed into the pointer cannot, so the
 * nodes are records from the heap and hold/put count references to them.
 * A synthetic node lives for the length of one syscall (the open that
 * follows a resolution binds a device's own ops onto the descriptor); a
 * record the heap cannot give answers ENOMEM.
 */
struct synth_node {
    struct vpath vp;
    int refs;
};

/* A synthetic node is allocated from the heap when a path is resolved to
 * it and freed when its last holder puts it. */
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

static void synth_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct synth_node *)node)->refs++;
}

static void synth_put(struct vfs_mount *mnt, void *node)
{
    struct synth_node *n = node;

    (void)mnt;
    if (n->refs > 0 && --n->refs == 0) {
        kfree(n);
    }
}

/**
 * The rest of a path through a flat synthetic namespace, `..` inside it
 * resolved lexically because the namespace is a table of spellings, not a
 * tree. A `..` that would climb above the mount root returns `VFS_CLIMB`
 * with `*left` at the remainder, and the resolver continues at the covered
 * node. A name at the mount root that a nameless mount stands at, as `shm`
 * in /dev after `pts/..`, returns `VFS_ROOT` with `*left` at that name, and
 * the resolver enters the mount from the root directory. The full spelling
 * (`prefix` + the canonical rest) goes through `synth_classify`.
 */
static int synth_rest(struct vfs_mount *mnt, const char *prefix, const char *rest, void **node,
                      const char **left)
{
    char full[256];
    struct vpath vp;
    uint32_t plen = (uint32_t)k_strlen(prefix);
    uint32_t out;
    const char *p = rest;
    int kind;

    out = plen;
    k_memcpy(full, prefix, out);

    while (*p) {
        const char *comp;
        uint32_t len = 0;

        while (*p == '/') {
            p++;
        }
        comp = p;
        while (*p != '\0' && *p != '/') {
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
                while (*p == '/') {
                    p++;
                }
                *left = p;
                return VFS_CLIMB;
            }
            while (out > 0 && full[out - 1] != '/') {
                out--;
            }
            if (out > 0) {
                out--;
            }
            continue;
        }
        if (out + 1 + len + 1 > sizeof(full)) {
            return -ENAMETOOLONG;
        }
        full[out++] = '/';
        k_memcpy(full + out, comp, len);
        out += len;
    }
    full[out] = '\0';

    kind = synth_classify(full, &vp);
    if (kind == VNODE_ENOENT) {
        return -ENOENT;
    }
    if (kind == VNODE_ENOTDIR) {
        return -ENOTDIR;
    }
    return synth_take(&vp, node);
}

static int devfs_rest(struct vfs_mount *mnt, const char *rest, void **node, const char **left)
{
    return synth_rest(mnt, "dev", rest, node, left);
}

static int procfs_rest(struct vfs_mount *mnt, const char *rest, void **node, const char **left)
{
    return synth_rest(mnt, "proc", rest, node, left);
}

static int sysfs_rest(struct vfs_mount *mnt, const char *rest, void **node, const char **left)
{
    return synth_rest(mnt, "sys", rest, node, left);
}

/* What the resolver needs of a synthetic node: whether it is a directory or
 * a link it follows, /proc/<pid>/exe, and the mode the search rule reads
 * (0755 under /dev, 0555 under /proc and /sys, all root's). A device's link
 * under /sys is classified, not resolved, as /proc/self is, so to the
 * resolver it is the directory it leads to. The syscall layer reports the
 * full attributes itself. */
static int synth_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    const struct synth_node *sn = node;
    int kind = sn->vp.kind;
    int is_dir = kind == VNODE_DEV_DIR || kind == VNODE_PTS_DIR || kind == VNODE_PROC_DIR ||
                 kind == VNODE_PROC_NET_DIR || kind == VNODE_PROC_PID ||
                 kind == VNODE_PROC_FD_DIR || kind == VNODE_PROC_TASK_DIR ||
                 kind == VNODE_SYS_DIR || kind == VNODE_SYS_LINK ||
                 kind == VNODE_PROC_SYSVIPC_DIR || kind == VNODE_PROC_NET_STAT_DIR;

    k_memset(st, 0, sizeof(*st));
    if (is_dir) {
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

/* The target of a synthetic link as text: /dev/log's, fixed; /proc/<pid>/
 * exe's, the path of the program the process runs (`procfs_readlink_exe`);
 * and /proc/<pid>/cwd's and root's, the paths of its directories
 * (`procfs_readlink_dir`). The resolver goes on from exe's program file
 * itself (`walk_exe`), and from the text of the others, and readlink(2)
 * reads the text through procfs. */
static int synth_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    const struct synth_node *sn = node;
    int pid;

    (void)mnt;
    if (sn->vp.kind == VNODE_DEV_LINK) {
        uint32_t n = (uint32_t)k_strlen(SYNTH_DEV_LOG_TARGET);

        n = n < cap ? n : cap;
        k_memcpy(buf, SYNTH_DEV_LOG_TARGET, n);
        return (int)n;
    }
    pid = procfs_resolve_id(&sn->vp);
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
static int walk_exe(struct vfs_ref *cur, void *found, int locked)
{
    struct vpath vp = ((struct synth_node *)found)->vp;
    struct vfs_ref exe;
    int rc;

    node_put(cur->mnt, found);
    for (;;) {
        int pid = procfs_resolve_id(&vp);

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
        vfs_ref_hold(&exe);
        ref_move_node(cur, exe.node);
    }
    return rc;
}

static const struct vfs_ops devfs_vops = {
    0, devfs_rest, synth_hold, synth_put, synth_getattr,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,          0,          0,         synth_readlink, 0, 0, 0, 0, 0, 0, /* abort_unmount */
    0,                                                                      /* symlink */
    0,                                                                      /* mknod */
    0,                                                                      /* pin */
    0,                                                                      /* unpin */
    0,                                                                      /* remount */
};

static const struct vfs_ops procfs_vops = {
    0, procfs_rest, synth_hold, synth_put, synth_getattr,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,           0,          0,         synth_readlink, 0, 0, 0, 0, 0, 0, /* abort_unmount */
    0,                                                                       /* symlink */
    0,                                                                       /* mknod */
    0,                                                                       /* pin */
    0,                                                                       /* unpin */
    0,                                                                       /* remount */
};

static const struct vfs_ops sysfs_vops = {
    0, sysfs_rest, synth_hold, synth_put, synth_getattr,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,          0,          0,         synth_readlink, 0, 0, 0, 0, 0, 0, /* abort_unmount */
    0,                                                                      /* symlink */
    0,                                                                      /* mknod */
    0,                                                                      /* pin */
    0,                                                                      /* unpin */
    0,                                                                      /* remount */
};

static int synth_mount_root(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                            const char *options)
{
    struct vpath vp;

    (void)dev;
    (void)flags;
    /* The mount root is the namespace's own directory, held for the life of
     * the mount. */
    synth_classify(options, &vp);
    return synth_take(&vp, &mnt->root);
}

static int devfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                       const char *options)
{
    (void)options;
    return synth_mount_root(mnt, dev, flags, "dev");
}

static int procfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                        const char *options)
{
    (void)options;
    return synth_mount_root(mnt, dev, flags, "proc");
}

static int sysfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                       const char *options)
{
    (void)options;
    return synth_mount_root(mnt, dev, flags, "sys");
}

const struct vfs_type vfs_devfs_type = {"devfs", devfs_mount, &devfs_vops, 0, 1};
const struct vfs_type vfs_procfs_type = {"proc", procfs_mount, &procfs_vops, 0, 1};
const struct vfs_type vfs_sysfs_type = {"sysfs", sysfs_mount, &sysfs_vops, 0, 1};

/** The classification of a synthetic node, or NULL when `mnt` is not one
 * of the three synthetic mounts. The syscall layer's glue reads it. */
const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node)
{
    if (mnt->type != &vfs_devfs_type && mnt->type != &vfs_procfs_type &&
        mnt->type != &vfs_sysfs_type) {
        return 0;
    }
    return &((struct synth_node *)node)->vp;
}

/* The permanent entries. */

/** Whether `m` is one of the permanent mounts, which umount2 refuses. */
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

/** Construct permanent mount `idx` of `type` at `path`; a failure panics. */
static void install(int idx, const struct vfs_type *type, const char *path,
                    struct vfs_mount *parent)
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
    k_strncpy(m->covered_path, path, sizeof(m->covered_path));
    m->refs = 0;
    m->inprogress = 0;
    m->ns_holder = -1;
    m->ns_depth = 0;
    m->ns_next = 0;
    m->ns_serving = 0;
    wait_queue_init(&m->ns_wq);
    m->priv = 0;
    m->opens = 0;
    m->releases = 0;
    m->claim = -1;
    m->source[0] = '\0';
    if (type->mount(m, 0, 0, 0) != 0) {
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
 * no parent and no path, which nothing enters by name, and last /sys,
 * read-only, as /dev and /proc are entered.
 */
static void install_permanent(void)
{
    install(1, &vfs_devfs_type, "/dev", mounts[0]);
    install(2, &vfs_procfs_type, "/proc", mounts[0]);
    install(3, &vfs_tmpfs_type, "/dev/shm", mounts[1]);
    mounts[3]->flags = VFS_MS_NOSUID | VFS_MS_NODEV;
    k_strncpy(mounts[3]->source, "tmpfs", sizeof(mounts[3]->source));
    install(4, &vfs_tmpfs_type, "", 0);
    mounts[4]->flags = VFS_MS_NOSUID | VFS_MS_NODEV;
    install(5, &vfs_sysfs_type, "/sys", mounts[0]);
    mounts[5]->flags = VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC;
    k_strncpy(mounts[5]->source, "sysfs", sizeof(mounts[5]->source));
}

struct vfs_mount *vfs_memfd_mount(void)
{
    return mounts[4];
}

static int root_installed;

int vfs_root_mounted(void)
{
    return mounts && mounts[0] && mounts[0]->state == VFS_ACTIVE;
}

void vfs_init(void)
{
    if (root_installed) {
        /* A re-mount of the root (the debug shell can remount from the
         * disk) replaces the image; the entry keeps its slot and takes the
         * new root node. An ext2 root is not replaced. */
        if (mounts[0]->type == &vfs_krffs_type) {
            mounts[0]->root = krffs_fs->node;
        }
        return;
    }
    root_installed = 1;
    mount_table_init();
    install(0, &vfs_krffs_type, "/", 0);
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
    m->type = type;
    m->dev = dev;
    m->state = VFS_MOUNTING;
    m->ns_holder = -1;
    wait_queue_init(&m->ns_wq);
    m->claim = claim;
    k_strncpy(m->covered_path, "/", sizeof(m->covered_path));
    k_strncpy(m->source, source, sizeof(m->source));
    m->source[sizeof(m->source) - 1] = '\0';
    rc = type->mount(m, dev, 0, 0);
    if (rc != 0) {
        k_memset(m, 0, sizeof(*m));
        return rc;
    }
    m->state = VFS_ACTIVE;
    root_installed = 1;
    install_permanent();
    return 0;
}

/* mount(2) and umount2(2). */

/* The types mount(2) accepts, by name; the root's adapter and the two
 * synthetic types are not here because nothing mounts them by name. */
static const struct vfs_type *const mount_types[] = {
    &vfs_vfat_type, &vfs_exfat_type, &vfs_ext2_type, &vfs_krffs_volume_type, &vfs_tmpfs_type, 0};

/* The table's entries, its terminator apart. Counted, not written down twice:
 * a fifth type added to the list above and not to a number here is a type
 * mount(2) would not find. */
#define VFS_MOUNT_TYPES ((int)(sizeof(mount_types) / sizeof(mount_types[0])) - 1)

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

int vfs_remount(struct vfs_mount *m, uint32_t flags)
{
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
    if ((flags ^ m->flags) & VFS_MS_RDONLY) {
        if (!m->type->ops->remount) {
            return -EINVAL;
        }
        if (!(flags & VFS_MS_RDONLY) &&
            ((m->dev && (m->dev->flags & BLOCK_READ_ONLY)) || m->write_error)) {
            return -EROFS;
        }
        rc = m->type->ops->remount(m, flags);
        if (rc != 0) {
            return rc;
        }
    }
    m->flags = flags;
    klog(KLOG_INFO, "vfs", "remounted: %s (%s)", m->covered_path,
         (flags & VFS_MS_RDONLY) ? "read-only" : "writable");
    return 0;
}

/* Wait predicate: no operation is in progress on the mount. */
static int drained(void *arg)
{
    struct vfs_mount *mnt = arg;
    return mnt->inprogress == 0;
}

/**
 * The device `source` names for a mount of `t`, into `*dev`, with its claim
 * in `*claim`: a partition window, a stick or a loop device under /dev, its
 * sectors claimed, and `*flags` made read-only for a device that refuses
 * writes. A type with no device takes any source string, claims nothing and
 * has a NULL device. Returns 0, or the errno with nothing claimed: a
 * device's own, EBUSY for a claimed range or a device with a mount already.
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
            return -EBUSY; /* the device already has a mount */
        }
    }
    return 0;
}

/**
 * mount(2): the order of work is resolve and hold the target, check its
 * path length and what it is, claim the source's range, take a slot as
 * `mounting` under the parent's namespace lock so no namespace change runs
 * between the covered check and the reservation, run the constructor,
 * publish. Every failure after the first step releases what the steps
 * before it took. The target is recorded by its path from the root of the
 * mount table (`vfs_node_path`), not by the path that named it, which may
 * pass through a link or start at a root chroot gave the caller.
 *
 * The source is a partition window under /dev, claimed as a sector range on
 * the backing disk; a loop device has no disk under it and claims nothing,
 * since the device itself is what a second mount of it collides with, and a
 * type with no device, tmpfs, takes any source string (`mount_source`). A
 * device that refuses writes, a write-protected stick or a loop device
 * attached read-only, makes the mount read-only whatever the caller asked
 * for, and the type learns it from the flags. The slot is taken before the
 * constructor runs, because the constructor may sleep on the device and
 * another process could remove or rename the target meanwhile; the covered
 * node stays held and the parent counted.
 */
int vfs_do_mount(const char *source, const char *target, const char *type, uint32_t flags,
                 const char *options)
{
    const struct vfs_type *t = type_named(type);
    struct vfs_ref tref;
    struct vfs_stat st;
    struct vfs_mount *m = 0;
    block_device_t *dev;
    char canon[VFS_COVERED_PATH_MAX];
    int claim;
    int i;
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
    if (k_strlen(target) >= VFS_COVERED_PATH_MAX) {
        vfs_ref_release(&tref);
        return -ENAMETOOLONG;
    }
    if (tref.mnt->type->ops->lookup_rest) {
        vfs_ref_release(&tref);
        return -EINVAL;
    }
    if (tref.mnt == mounts[0] && tref.node == mounts[0]->root) {
        vfs_ref_release(&tref);
        return -EINVAL;
    }
    if (tref.node == tref.mnt->root) {
        vfs_ref_release(&tref);
        return -EBUSY; /* the root of a placed mount: a mount point already */
    }
    rc = vfs_getattr(tref.mnt, tref.node, &st);
    if (rc != 0) {
        vfs_ref_release(&tref);
        return rc;
    }
    if (!VFS_IS_DIR(st.mode)) {
        vfs_ref_release(&tref);
        return -ENOTDIR;
    }
    rc = vfs_node_path(tref.mnt, tref.node, canon, sizeof(canon));
    if (rc != 0) {
        vfs_ref_release(&tref);
        return rc;
    }
    vfs_ns_lock(tref.mnt);
    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->covered == tref.node &&
            mounts[i]->parent == tref.mnt) {
            vfs_ns_unlock(tref.mnt);
            vfs_ref_release(&tref);
            return -EBUSY; /* covered, or reserved by a mount in progress */
        }
    }
    rc = mount_source(t, source, &dev, &flags, &claim);
    if (rc != 0) {
        vfs_ns_unlock(tref.mnt);
        vfs_ref_release(&tref);
        return rc;
    }
    i = mount_new_slot();
    m = i < 0 ? NULL : mounts[i];
    if (!m) {
        devfs_release_claim(claim);
        vfs_ns_unlock(tref.mnt);
        vfs_ref_release(&tref);
        return -ENOMEM;
    }
    k_memset(m, 0, sizeof(*m));
    m->type = t;
    m->dev = dev;
    m->flags = flags;
    m->state = VFS_MOUNTING;
    m->parent = tref.mnt;
    m->covered = tref.node;
    m->ns_holder = -1;
    m->ns_depth = 0;
    m->ns_next = 0;
    m->ns_serving = 0;
    wait_queue_init(&m->ns_wq);
    m->claim = claim;
    k_strncpy(m->covered_path, canon, sizeof(m->covered_path));
    k_strncpy(m->source, source, sizeof(m->source));
    m->source[sizeof(m->source) - 1] = '\0';
    tref.mnt->refs++; /* a child mount is a reference on its parent */
    vfs_ns_unlock(tref.mnt);
    vfs_leave(tref.mnt); /* the walk's count; the hold stays with the slot */

    vfs_ns_lock(m);
    rc = t->mount(m, dev, flags, options);
    vfs_ns_unlock(m);
    if (rc != 0) {
        struct vfs_ref cov;

        cov.mnt = m->parent;
        cov.node = m->covered;
        vfs_ref_put(&cov);
        m->parent->refs--;
        devfs_release_claim(claim);
        mount_release(m);
        return rc;
    }
    m->state = VFS_ACTIVE;
    klog(KLOG_INFO, "vfs", "mounted: %s on %s (%s%s)", source, m->covered_path, t->name,
         (flags & VFS_MS_RDONLY) ? ", read-only" : "");
    return 0;
}

/**
 * An unmount that prepared and could not be finished, so the mount stays.
 *
 * The type wrote its last metadata for a volume that is going away; the
 * volume is not going away, and what it wrote says otherwise. A type that
 * cannot put it back leaves a mount nobody may write, which is the answer
 * that cannot be wrong.
 */
static void unmount_abandoned(struct vfs_mount *m)
{
    if (vfs_mount_gone(m)) {
        return; /* nothing was written and nothing can be */
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
 * Find a mount by its recorded target when a removed ancestor blocks lookup.
 *
 * Removal must not make a child mount's reference on its parent permanent.
 * The recorded spelling is the recovery name; no lookup on the missing
 * medium is needed. The caller names the target from its own root, and the
 * name is put in the mount table's terms before it is compared
 * (`unmount_target`), so only a mount at or below that root can match; a
 * recorded path has no `.` or `..`, so a target that climbs matches none.
 * Only a unique active mount below a gone ancestor is eligible. Ordinary
 * lookup errors and mounts on present ancestors retain their usual pathname
 * rules.
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
 * attribute read, which the file system of a device that has gone refuses.
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
        rc = -EINVAL; /* not a mount point */
    } else if (rc == 0 && mount_permanent(tref->mnt)) {
        rc = -EBUSY;
    }
    *out = tref->mnt;
    vfs_ref_release(tref);
    return rc;
}

/**
 * umount2(2): the mount goes to `unmounting`, which refuses new work; the
 * in-progress count drains; the reference count is checked; sync and
 * prepare_unmount; the cache's dirty buffers are written and the device's
 * own cache is flushed, a failure here returning the mount to `active`; the
 * cache drops the device's buffers; unmount; the covered node is released;
 * the claim is released; the entry clears.
 *
 * A device that has gone takes nothing, so the sync, prepare_unmount, the
 * writeback and the flush are skipped for it: each would fail, and an
 * unmount that failed on a stick that is no longer there would keep the
 * mount for good.
 *
 * A target that named a directory by its end, `dir`, must reach one, and
 * the walk that finds the mount answers for it: anything else is -ENOTDIR,
 * as on Linux, before the -EINVAL of a name that is no mount point. The node
 * checked is the node unmounted, so a symbolic link replaced between two
 * walks cannot change the answer. A target found by its recorded name below
 * a removed device is a mount's root, a directory.
 */
int vfs_do_umount(const char *target, int dir)
{
    int gone;
    struct vfs_ref tref;
    struct vfs_mount *m;
    struct vfs_ref cov;
    int rc;

    rc = resolve_path(0, target, 0, &tref, 0, 0, 1);
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
    m->state = VFS_UNMOUNTING;
    while (m->inprogress > 0) {
        wq_wait_event_nointr(&m->ns_wq, drained, m);
    }
    if (m->refs > 0) {
        m->state = VFS_ACTIVE;
        return -EBUSY;
    }
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
        return -EIO; /* nothing irreversible has happened yet */
    }
    /* A stick's own cache is not on the medium until the stick is told. */
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
        return -EBUSY; /* a buffer of the device is still held or dirty */
    }
    klog(KLOG_INFO, "vfs", "unmounted %s: %d opens, %d releases", m->covered_path, (int)m->opens,
         (int)m->releases);
    if (m->type->ops->put) {
        m->type->ops->put(m, m->root);
    }
    inotify_unmount(m);
    m->type->ops->unmount(m);
    vfs_ns_unlock(m);
    cov.mnt = m->parent;
    cov.node = m->covered;
    vfs_ref_put(&cov);
    m->parent->refs--;
    devfs_release_claim(m->claim);
    mount_release(m);
    return 0;
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
    mnt->refs--;
}

/**
 * Hold the new working directory, store it, and only then let go of the old
 * one, whose put may sleep: a thread that shares the working directory
 * (CLONE_FS) and changes it meanwhile replaces a hold that is in the record,
 * and the pair in the record stays the directory its path names. /dev and
 * /proc need no hold and take none. KRFFS, the boot root and a mounted
 * volume, has no `hold`: the working directory takes only the mount's
 * count, and the record keeps its node for `rmdir`, which refuses a
 * directory that is a working directory.
 */
void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    struct vfs_mount *was = p->fs->cwd_mnt;
    void *was_node = p->fs->cwd_node;

    if (mnt && (mnt == mounts[1] || mnt == mounts[2])) {
        mnt = 0;
    }
    if (mnt) {
        if (mnt->type->ops->hold) {
            mnt->type->ops->hold(mnt, node);
        }
        mnt->refs++;
    }
    p->fs->cwd_mnt = mnt;
    p->fs->cwd_node = mnt ? node : 0;
    hold_release(was, was_node);
}

/**
 * Hold the new root, store it, and only then let go of the old one. The put
 * may sleep, and a thread that shares the root (CLONE_FS) and resolves a
 * path meanwhile finds one of the two roots in the record, never none,
 * which would be the root of the mount table.
 */
void vfs_root_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    struct vfs_mount *was = p->fs->root_mnt;
    void *was_node = p->fs->root_node;

    if (mnt && mnt == mounts[0] && node == mnt->root) {
        mnt = 0;
    }
    if (mnt) {
        if (mnt->type->ops->hold) {
            mnt->type->ops->hold(mnt, node);
        }
        mnt->refs++;
    }
    p->fs->root_mnt = mnt;
    p->fs->root_node = mnt ? node : 0;
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
