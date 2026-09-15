/*
 * vfs.c: the mount table and the resolver. The contract is in vfs.h; what
 * is here is the walk, one component at a time, the reference and transient
 * counts, the per-mount namespace lock, the default-answer wrappers, and
 * the glue that makes KRFFS and the two synthetic namespaces answer the
 * operation interface.
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
#include "synthfs.h"

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

/* The first free slot from 3 on, the array grown by doubling when every
 * slot is taken, with a record in it; -1 when the heap has none. */
static int mount_new_slot(void)
{
    int i;

    for (i = 3; i < mount_slots; i++) {
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

    for (i = 3; i < mount_slots; i++) {
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

/* ---- reference and transient counts ---- */

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

/* ---- the namespace lock ---- */

static int ns_free(void *arg)
{
    struct vfs_mount *mnt = arg;
    return mnt->ns_holder == -1;
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

void vfs_ns_lock(struct vfs_mount *mnt)
{
    int me = current_proc ? (int)current_proc->pid : 0;

    if (mnt->ns_holder == me && mnt->ns_depth > 0) {
        mnt->ns_depth++;
        return;
    }
    while (mnt->ns_holder != -1) {
        wq_wait_event_nointr(&mnt->ns_wq, ns_free, mnt);
    }
    mnt->ns_holder = me;
    mnt->ns_depth = 1;
}

void vfs_ns_unlock(struct vfs_mount *mnt)
{
    if (mnt->ns_depth > 1) {
        mnt->ns_depth--;
        return;
    }
    mnt->ns_depth = 0;
    mnt->ns_holder = -1;
    wq_wake_all(&mnt->ns_wq);
}

/* ---- the permission the resolver charges ---- */

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
 * one identity class is chosen and judged alone. `use_real` judges by the
 * real ids, for access(2), the one call that asks who the process really
 * is. The search bit is what walking a directory costs.
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
    if (uid == 0) {
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

/* ---- the resolver ---- */

/* The mount whose root replaces `node` when the walk arrives at it: a child
 * of `mnt` covering that node. The two synthetic mounts cover no node; they
 * are entered by name from the root directory, which is the second match
 * arm. */
static struct vfs_mount *mount_covering(struct vfs_mount *mnt, void *node)
{
    int i;

    for (i = 1; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->parent == mnt && mounts[i]->covered &&
            mounts[i]->covered == node) {
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

static struct vfs_mount *mount_named(struct vfs_mount *mnt, void *node, const char *name)
{
    int i;

    for (i = 1; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state == VFS_ACTIVE && mounts[i]->parent == mnt && !mounts[i]->covered &&
            node == mnt->root && streq(mounts[i]->covered_path + 1, name)) {
            return mounts[i];
        }
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
    /* Any slot that holds the node covers it for a namespace change: one
     * still mounting, whose constructor may be asleep, as much as an
     * active one. The resolver crosses only into active mounts. */
    for (i = 1; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->parent == mnt && mounts[i]->covered == found) {
            m = mounts[i];
        }
    }
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mnt, found);
    }
    return m;
}

/* Replace the current place with another, moving the hold and the transient
 * count. Fails only when the new mount refuses entry. */
/* Put a node a lookup returned, on a type that counts holds. */
static void node_put(struct vfs_mount *mnt, void *node)
{
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mnt, node);
    }
}

static int walk_move(struct vfs_ref *cur, struct vfs_mount *mnt, void *node)
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
            vfs_leave(was);
        }
    }
    *cur = next;
    return 0;
}

/**
 * Resolve one path. See vfs.h for the whole contract. The loop takes one
 * component at a time and handles four cases: `.`, `..`, a mount whose type
 * takes the rest of the path in one call, and the ordinary lookup of one
 * name, after which a symbolic link is followed and a covering mount is
 * crossed. An unmount may resolve a removed mount at the final component
 * only, so it can release that mount without permitting lookup inside it.
 */
static int resolve_path(const struct vfs_ref *start, const char *path, int flags,
                        struct vfs_ref *out, char *leaf, uint32_t leafcap, int unmount)
{
    struct vfs_ref cur;
    const char *p = path;
    const char *last_comp = 0;
    int use_real = (flags & VFS_WALK_REAL_IDS) ? 1 : 0;
    int links = 0;
    char linkpath[VFS_WALK_PATH_MAX];
    int rc;

    if (!path || !out) {
        return -EINVAL;
    }
    if (mounts[0]->state != VFS_ACTIVE) {
        return -ENOENT;
    }

    /* With WALK_PARENT the final component is the caller's, not walked. */
    if (flags & VFS_WALK_PARENT) {
        const char *end;

        end = path + k_strlen(path);
        while (end > path && end[-1] == '/') {
            end--;
        }
        if (end == path) {
            return -EINVAL; /* the root has no parent to hand out */
        }
        last_comp = end;
        while (last_comp > path && last_comp[-1] != '/') {
            last_comp--;
        }
        if ((uint32_t)(end - last_comp) + 1 > leafcap) {
            return -ENAMETOOLONG;
        }
        k_memcpy(leaf, last_comp, (uint32_t)(end - last_comp));
        leaf[end - last_comp] = '\0';
    }

    if (p[0] == '/' || !start) {
        cur.mnt = mounts[0];
        cur.node = mounts[0]->root;
    } else {
        cur = *start;
    }
    if (vfs_enter(cur.mnt) != 0) {
        return -EBUSY;
    }
    vfs_ref_hold(&cur);

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
        if (cur.mnt->type->ops->lookup_rest) {
            const char *left = 0;
            char restbuf[VFS_NAME_MAX + 1];
            const char *rest = p;
            struct vfs_mount *stay = cur.mnt;

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
            if (rc == VFS_CLIMB) {
                struct vfs_mount *pm = cur.mnt->parent ? cur.mnt->parent : mounts[0];
                void *pnode = cur.mnt->covered ? cur.mnt->covered : mounts[0]->root;
                if (rest == restbuf) {
                    /* The climb consumed part of a bounded copy; continue
                     * at the same offset of the real path. */
                    p += left - restbuf;
                } else {
                    p = left;
                }
                rc = walk_move(&cur, pm, pnode);
                if (rc != 0) {
                    goto out_err;
                }
                continue;
            }
            if (rc != 0) {
                goto out_err;
            }
            vfs_ref_put(&cur);
            cur.mnt = stay; /* the put clears the ref; the mount stays */
            cur.node = found;
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
            if (cur.node == cur.mnt->root) {
                if (cur.mnt == mounts[0]) {
                    continue; /* the root of the root is its own parent */
                }
                /* At a mount root: continue at the covered node (the two
                 * nameless mounts hang off the root directory itself), and
                 * re-run this same `..` there. */
                p -= 2;
                rc = walk_move(&cur, cur.mnt->parent,
                               cur.mnt->covered ? cur.mnt->covered : cur.mnt->parent->root);
                if (rc != 0) {
                    goto out_err;
                }
                continue;
            }
        }

        /* The nameless mounts, entered by name from the root directory. */
        m = mount_named(cur.mnt, cur.node, comp);
        if (m) {
            rc = walk_move(&cur, m, m->root);
            if (rc != 0) {
                goto out_err;
            }
            continue;
        }

        rc = cur.mnt->type->ops->lookup(cur.mnt, cur.node, comp, &found);
        if (rc != 0) {
            goto out_err;
        }
        /* On a type with symbolic links, a component that is one is
         * followed when more of the path comes after it, and at the last
         * component unless the caller passed VFS_WALK_NOFOLLOW. Its target
         * replaces the component, an absolute target restarts at the root,
         * and the walk goes on from the directory the link was found in. A
         * mount covers a directory, so a link is never a covered node. */
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
                char target[VFS_WALK_PATH_MAX];
                int tlen;
                uint32_t rlen = (uint32_t)k_strlen(rest);
                uint32_t join = rlen != 0 ? 1u : 0u;

                if (++links > VFS_LINK_MAX) {
                    node_put(cur.mnt, found);
                    rc = -ELOOP;
                    goto out_err;
                }
                tlen = cur.mnt->type->ops->readlink(cur.mnt, found, target, sizeof(target));
                node_put(cur.mnt, found);
                if (tlen < 0) {
                    rc = tlen;
                    goto out_err;
                }
                if (tlen == 0) {
                    rc = -ENOENT; /* an empty target names nothing */
                    goto out_err;
                }
                if ((uint32_t)tlen + join + rlen + 1 > sizeof(linkpath)) {
                    rc = -ENAMETOOLONG;
                    goto out_err;
                }
                /* The rest may already be inside `linkpath`, from a link
                 * followed before this one, so it moves before the target
                 * is written over it. */
                k_memmove(linkpath + tlen + join, rest, rlen + 1);
                k_memcpy(linkpath, target, (uint32_t)tlen);
                if (join) {
                    linkpath[tlen] = '/';
                }
                if (last_comp) {
                    /* The leaf returned to the caller is the last component
                     * of the spliced path. */
                    const char *end = linkpath + k_strlen(linkpath);

                    while (end > linkpath && end[-1] == '/') {
                        end--;
                    }
                    last_comp = end;
                    while (last_comp > linkpath && last_comp[-1] != '/') {
                        last_comp--;
                    }
                }
                p = linkpath;
                if (target[0] == '/') {
                    rc = walk_move(&cur, mounts[0], mounts[0]->root);
                    if (rc != 0) {
                        goto out_err;
                    }
                }
                continue;
            }
        }
        {
            struct vfs_mount *stay = cur.mnt;

            vfs_ref_put(&cur);
            cur.mnt = stay; /* the put clears the ref; the mount stays */
            cur.node = found;
        }

        /* A directory covered by a mount is replaced by that mount's root
         * before the walk continues. */
        m = mount_covering(cur.mnt, cur.node);
        if (m) {
            rc = walk_move(&cur, m, m->root);
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
    *out = cur;
    return 0;

out_err:
    {
        struct vfs_mount *was = cur.mnt;

        vfs_ref_put(&cur);
        vfs_leave(was);
    }
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

    vfs_release(mnt, handle);
    r.mnt = mnt;
    r.node = node;
    vfs_ref_put(&r);
    if (mnt->refs == 0) {
        k_panic("vfs: a description closed on a mount that had none");
    }
    mnt->refs--;
}

/* ---- repointing, for a KRFFS relocation ---- */

void vfs_repoint(struct vfs_mount *mnt, void *from, void *to)
{
    process_t *pr;
    int i, j;

    for (pr = proc_list; pr; pr = pr->next) {
        if (mnt == mounts[0]) {
            for (j = 0; j < fd_table_cap(&pr->fds); j++) {
                fd_entry_t *e = &pr->fds.entries[j];

                if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) &&
                    (void *)e->node == from) {
                    e->node = to;
                }
            }
        }
        if (pr->cwd_mnt == mnt && pr->cwd_node == from) {
            pr->cwd_node = to;
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
}

/* ---- the default-answer wrappers ---- */

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

int vfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    /* The length of a file that backs a loop device is that device's sector
     * count, so nothing changes it here either: `open(O_TRUNC)` reaches this
     * with the node as its handle, and a descriptor's `ftruncate` reaches it
     * with the handle its open was given. */
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
    }
    return rc;
}

void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    int i;

    for (i = 0; i < mount_slots; i++) {
        if (!mounts[i] || mounts[i]->state == VFS_FREE || mounts[i]->dev != dev ||
            (mounts[i]->flags & VFS_MS_RDONLY)) {
            continue;
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
         * another; the last of them is what an unmount waits for. */
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
            block_flush(m->dev);
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

int vfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
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
    return mnt->type->ops->setattr(mnt, node, what, st);
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
    return mnt->type->ops->readdir(mnt, handle, cursor, ent);
}

int vfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->create) {
        return -EROFS;
    }
    return mnt->type->ops->create(mnt, dir, name, mode, node);
}

int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->unlink) {
        return -EROFS;
    }
    return mnt->type->ops->unlink(mnt, dir, name);
}

int vfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->mkdir) {
        return -EROFS;
    }
    return mnt->type->ops->mkdir(mnt, dir, name, mode);
}

int vfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->rmdir) {
        return -EROFS;
    }
    return mnt->type->ops->rmdir(mnt, dir, name);
}

int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
               const char *newname)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->rename) {
        return -EROFS;
    }
    return mnt->type->ops->rename(mnt, olddir, oldname, newdir, newname);
}

int vfs_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mnt->flags & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (!mnt->type->ops->link) {
        return -EPERM; /* a file system without links */
    }
    return mnt->type->ops->link(mnt, dir, name, node);
}

int vfs_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (mnt->flags & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (!mnt->type->ops->symlink) {
        return -EPERM;
    }
    return mnt->type->ops->symlink(mnt, dir, name, target);
}

int vfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    if (vfs_mount_gone(mnt)) {
        return -EIO;
    }
    if (!mnt->type->ops->statfs) {
        return -EINVAL;
    }
    return mnt->type->ops->statfs(mnt, st);
}

/* ==== the KRFFS type: the root, adapted onto the interface ==== */

/* A node handle is the node's place in the mapped image; the root is
 * `krffs_fs->node`. Hold and put are empty because a place is not a
 * resource, which is the reason the type may leave them NULL. */

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
        /* The root's stored mode carries the dirty flag; it reads as the
         * fixed identity stat reports. */
        st->mode = VFS_S_IFDIR | 0755u;
        st->ino = 1;
        return 0;
    }
    st->mode = KRFFS_IS_DIR(n->mode) ? (VFS_S_IFDIR | (uint32_t)(n->mode & 07777u))
                                     : (VFS_S_IFREG | (uint32_t)(n->mode & 07777u));
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = 1;
    st->ino = n->id;
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

static int32_t kfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
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
static int32_t kfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                         uint64_t offset)
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

/* A grow can relocate the node, as in `kfs_write`; the path call reports
 * only status, so the node is looked up again and every holder repointed. */
static int kfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    char path[256];
    krffs_node_t *n;

    if (size > 0xFFFFFFFFu) {
        return -EFBIG;
    }
    if (krffs_node_path(handle, path, sizeof(path)) != 0) {
        return -EIO;
    }
    if (krffs_truncate(path + (path[0] == '/'), (uint32_t)size) != 0) {
        return -EIO;
    }
    n = krffs_resolve(path + (path[0] == '/'));
    if (n && n != (krffs_node_t *)handle) {
        vfs_repoint(mnt, handle, n);
    }
    return 0;
}

static int kfs_fsync(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    return pc_sync() == 0 ? 0 : -EIO; /* the window flushes whole */
}

/* The cursor is the entry node's byte offset in the image plus one, so a
 * deletion between calls never renumbers the survivors; the traversal
 * visits nodes in ascending offsets, so the first match at or past the
 * cursor is the next entry. */
static int kfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
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
            ent->ino = node->id;
            ent->is_dir = KRFFS_IS_DIR(node->mode) ? 1 : 0;
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

static int kfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    char path[256];
    int rc = kfs_join(dir, name, path, sizeof(path));

    (void)mnt;
    (void)mode; /* the caller stamps mode and owner under its own rules */
    if (rc != 0) {
        return rc;
    }
    /* The parent is resolved and held, so the one failure left is space. */
    return krffs_mkdir(path) == 0 ? 0 : -ENOSPC;
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

static const struct vfs_ops krffs_ops = {
    kfs_lookup,  0, /* lookup_rest */
    0,              /* hold */
    0,              /* put */
    kfs_getattr, 0, /* setattr: the migration moves it here family by family */
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
    0, /* symlink */
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

const struct vfs_type vfs_krffs_type = {"krffs", kfs_mount, &krffs_ops};

/* ==== the two synthetic types ==== */

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
 * node. The full spelling (`prefix` + the canonical rest) goes through
 * `synth_classify`, the same classification the namespaces always had.
 */
static int synth_rest(struct vfs_mount *mnt, const char *prefix, const char *rest, void **node,
                      const char **left)
{
    char full[256];
    struct vpath vp;
    uint32_t out;
    const char *p = rest;
    int kind;

    out = k_strlen(prefix);
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
        if (len == 1 && comp[0] == '.') {
            continue;
        }
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            if (out <= k_strlen(prefix)) {
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
    (void)mnt;
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

/* What the resolver needs of a synthetic node: whether it is a directory,
 * and the mode the search rule reads (0755 under /dev, 0555 under /proc,
 * both root's). The syscall layer reports the full attributes itself. */
static int synth_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    const struct synth_node *sn = node;
    int kind = sn->vp.kind;
    int is_dir = kind == VNODE_DEV_DIR || kind == VNODE_PTS_DIR || kind == VNODE_PROC_DIR ||
                 kind == VNODE_PROC_NET_DIR || kind == VNODE_PROC_PID || kind == VNODE_PROC_FD_DIR;

    k_memset(st, 0, sizeof(*st));
    if (is_dir) {
        st->mode = VFS_S_IFDIR | (mnt == mounts[1] ? 0755u : 0555u);
    } else {
        st->mode = VFS_S_IFREG | 0444u;
    }
    st->nlink = 1;
    return 0;
}

static const struct vfs_ops devfs_vops = {
    0, devfs_rest, synth_hold, synth_put, synth_getattr,
    0, 0,          0,          0,         0,
    0, 0,          0,          0,         0,
    0, 0,          0,          0,         0,
    0, 0,          0,          0,         0,
    0, /* abort_unmount */
    0, /* symlink */
};

static const struct vfs_ops procfs_vops = {
    0, procfs_rest, synth_hold, synth_put, synth_getattr,
    0, 0,           0,          0,         0,
    0, 0,           0,          0,         0,
    0, 0,           0,          0,         0,
    0, 0,           0,          0,         0,
    0, /* abort_unmount */
    0, /* symlink */
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

const struct vfs_type vfs_devfs_type = {"devfs", devfs_mount, &devfs_vops};
const struct vfs_type vfs_procfs_type = {"proc", procfs_mount, &procfs_vops};

/** The classification a synthetic node carries, or NULL when `mnt` is not
 * one of the two synthetic mounts. The syscall layer's glue reads it. */
const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node)
{
    if (mnt->type != &vfs_devfs_type && mnt->type != &vfs_procfs_type) {
        return 0;
    }
    return &((struct synth_node *)node)->vp;
}

/* ---- the permanent entries ---- */

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
    install(1, &vfs_devfs_type, "/dev", mounts[0]);
    install(2, &vfs_procfs_type, "/proc", mounts[0]);
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
    install(1, &vfs_devfs_type, "/dev", mounts[0]);
    install(2, &vfs_procfs_type, "/proc", mounts[0]);
    return 0;
}

/* ==== mount(2) and umount2(2) ==== */

/* The types mount(2) accepts, by the name it spells; the permanent three
 * are not here because nothing mounts them by name. */
static const struct vfs_type *const mount_types[] = {&vfs_vfat_type, &vfs_exfat_type, &vfs_ext2_type,
                                                    &vfs_krffs_volume_type, 0};

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

/* The flags mount(2) may carry here: honoured or recorded ones. Anything
 * else is EINVAL, MS_REMOUNT, MS_BIND and MS_MOVE first among them. */
#define VFS_MS_KNOWN                                                                 \
    (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC | VFS_MS_NOATIME | \
     VFS_MS_NODIRATIME | VFS_MS_SILENT | VFS_MS_RELATIME)

static int drained(void *arg)
{
    struct vfs_mount *mnt = arg;
    return mnt->inprogress == 0;
}

/**
 * mount(2): the order of work is resolve and hold the target, check its
 * path length and what it is, claim the source's range, take a slot as
 * `mounting` under the parent's namespace lock so no namespace change runs
 * between the covered check and the reservation, run the constructor,
 * publish. Every failure after the first step releases what the steps
 * before it took. A target on the root is recorded by its canonical path.
 */
int vfs_do_mount(const char *source, const char *target, const char *type, uint32_t flags,
                 const char *options)
{
    const struct vfs_type *t = type_named(type);
    struct vfs_ref tref;
    struct vfs_stat st;
    struct vfs_mount *m = 0;
    block_device_t *dev, *backing;
    uint64_t first, count;
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
    if (tref.mnt == mounts[0] && mounts[0]->type == &vfs_krffs_type &&
        krffs_node_path(tref.node, canon, sizeof(canon)) != 0) {
        vfs_ref_release(&tref);
        return -ENAMETOOLONG;
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
    /* The source: a partition window under /dev, claimed as a sector range
     * on the backing disk. */
    rc = devfs_window_of(source, &dev, &backing, &first, &count);
    if (rc != 0) {
        vfs_ns_unlock(tref.mnt);
        vfs_ref_release(&tref);
        return rc;
    }
    /* A medium that refuses writes makes every mount of it read-only,
     * whatever the caller asked for: a write-protected stick, or a loop
     * device attached to be read and not written. The types are told through
     * the flags, which is the one place that has to know. */
    if (dev->flags & BLOCK_READ_ONLY) {
        flags |= VFS_MS_RDONLY;
    }

    /* A source with no disk under it, which is what a loop device is: the
     * device itself is the identity a second mount collides with, and the
     * table below refuses that on its own. */
    claim = -1;
    if (backing) {
        claim = devfs_claim(backing, first, first + count - 1);
        if (claim < 0) {
            vfs_ns_unlock(tref.mnt);
            vfs_ref_release(&tref);
            return claim;
        }
    }
    for (i = 0; i < mount_slots; i++) {
        if (mounts[i] && mounts[i]->state != VFS_FREE && mounts[i]->dev == dev) {
            devfs_release_claim(claim);
            vfs_ns_unlock(tref.mnt);
            vfs_ref_release(&tref);
            return -EBUSY; /* the device already carries a mount */
        }
    }
    i = mount_new_slot();
    m = i < 0 ? NULL : mounts[i];
    if (!m) {
        devfs_release_claim(claim);
        vfs_ns_unlock(tref.mnt);
        vfs_ref_release(&tref);
        return -ENOMEM;
    }
    /* The slot is taken before the constructor runs, because with a
     * sleeping device the constructor sleeps and another process could
     * remove or rename the target under it; the covered node is held and
     * the parent counted. */
    k_memset(m, 0, sizeof(*m));
    m->type = t;
    m->dev = dev;
    m->flags = flags;
    m->state = VFS_MOUNTING;
    m->parent = tref.mnt;
    m->covered = tref.node;
    m->ns_holder = -1;
    m->ns_depth = 0;
    wait_queue_init(&m->ns_wq);
    m->claim = claim;
    k_strncpy(m->covered_path,
              tref.mnt == mounts[0] && mounts[0]->type == &vfs_krffs_type ? canon : target,
              sizeof(m->covered_path));
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
 * Find a mount by its recorded target when a removed ancestor blocks lookup.
 *
 * Removal must not make a child mount's reference on its parent permanent.
 * The recorded spelling is the recovery name; no lookup on the missing
 * medium is needed. Only a unique active mount below a gone ancestor is
 * eligible. Ordinary lookup errors and mounts on present ancestors retain
 * their usual pathname rules.
 */
static struct vfs_mount *unmount_below_gone(const char *target)
{
    struct vfs_mount *found = NULL;
    struct vfs_mount *m;
    struct vfs_mount *parent;
    int i;

    for (i = 3; i < mount_slots; i++) {
        m = mounts[i];
        if (!m || m->state != VFS_ACTIVE || !streq(m->covered_path, target)) {
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
 * umount2(2): the mount goes to `unmounting`, which refuses new work; the
 * in-progress count drains; the reference count is checked; sync and
 * prepare_unmount; the cache's dirty buffers are written, a failure here
 * returning the mount to `active`; the cache drops the device's buffers;
 * unmount; the covered node is released; the claim is released; the entry
 * clears.
 */
int vfs_do_umount(const char *target)
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
        m = tref.mnt;
        if (tref.node != m->root) {
            vfs_ref_release(&tref);
            return -EINVAL; /* not a mount point */
        }
        if (m == mounts[0] || m == mounts[1] || m == mounts[2]) {
            vfs_ref_release(&tref);
            return -EBUSY; /* the permanent three */
        }
        vfs_ref_release(&tref);
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
    /* A device that has gone takes nothing, so the three writes below are
     * not attempted: each would fail, and an unmount that fails on a stick
     * that is no longer there would leave the mount for good. What it held
     * is let go of, and that is the whole of the unmount. */
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
    if (!gone && bcache_sync(m->dev) != 0) {
        unmount_abandoned(m);
        vfs_ns_unlock(m);
        m->state = VFS_ACTIVE;
        return -EIO; /* nothing irreversible has happened yet */
    }
    /* The buffers are on the wire; a stick keeps a cache of its own, and
     * what is in it is not on the medium until it is told. */
    if (!gone && block_flush(m->dev) != 0) {
        unmount_abandoned(m);
        vfs_ns_unlock(m);
        m->state = VFS_ACTIVE;
        return -EIO;
    }
    if (bcache_invalidate(m->dev) != 0) {
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

/* ---- the working directory's hold on its mount ---- */

void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    vfs_cwd_clear(p);
    if (!mnt || mnt == mounts[1] || mnt == mounts[2] ||
        (mnt == mounts[0] && mnt->type == &vfs_krffs_type)) {
        return; /* the permanent mounts need no pin, but a root of another type does */
    }
    p->cwd_mnt = mnt;
    p->cwd_node = node;
    if (mnt->type->ops->hold) {
        mnt->type->ops->hold(mnt, node);
    }
    mnt->refs++;
}

void vfs_cwd_clear(struct process *p)
{
    struct vfs_ref r;
    struct vfs_mount *was;

    if (!p->cwd_mnt) {
        return;
    }
    was = p->cwd_mnt;

    r.mnt = was;
    r.node = p->cwd_node;
    p->cwd_mnt = 0;
    p->cwd_node = 0;
    /* The put may sleep; the mount's count keeps it until the put is done. */
    vfs_ref_put(&r);
    was->refs--;
}
