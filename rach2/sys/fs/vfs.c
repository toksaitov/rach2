/*
 * vfs.c: the mount table and the resolver. The contract is in vfs.h; what
 * is here is the walk, one component at a time, the reference and transient
 * counts, the per-mount namespace lock, the default-answer wrappers, and
 * the glue that makes KRFFS and the two synthetic namespaces answer the
 * operation interface.
 */

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
#include "krffs.h"
#include "synthfs.h"

#define VFS_S_IFMT 0170000u
#define VFS_S_IFDIR 0040000u
#define VFS_S_IFREG 0100000u
#define VFS_IS_DIR(mode) (((mode) & VFS_S_IFMT) == VFS_S_IFDIR)

static struct vfs_mount mounts[VFS_MOUNTS];

struct vfs_mount *vfs_mount_at(int idx)
{
    if (idx < 0 || idx >= VFS_MOUNTS || mounts[idx].state == VFS_FREE) {
        return 0;
    }
    return &mounts[idx];
}

struct vfs_mount *vfs_root_mount(void)
{
    return &mounts[0];
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

void vfs_ns_lock(struct vfs_mount *mnt)
{
    while (mnt->ns_holder != -1) {
        wq_wait_event(&mnt->ns_wq, ns_free, mnt); /* EINTR: still wanted */
    }
    mnt->ns_holder = current_proc ? (int)current_proc->pid : 0;
}

void vfs_ns_unlock(struct vfs_mount *mnt)
{
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

    for (i = 1; i < VFS_MOUNTS; i++) {
        if (mounts[i].state == VFS_ACTIVE && mounts[i].parent == mnt &&
            mounts[i].covered && mounts[i].covered == node) {
            return &mounts[i];
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

static struct vfs_mount *mount_named(struct vfs_mount *mnt, void *node,
                                     const char *name)
{
    int i;

    for (i = 1; i < VFS_MOUNTS; i++) {
        if (mounts[i].state == VFS_ACTIVE && mounts[i].parent == mnt &&
            !mounts[i].covered && node == mnt->root &&
            streq(mounts[i].covered_path + 1, name)) {
            return &mounts[i];
        }
    }
    return 0;
}

struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node,
                                 const char *name)
{
    void *found;
    struct vfs_mount *m = mount_named(mnt, node, name);
    int i;

    if (m) {
        return m;
    }
    if (!mnt->type->ops->lookup ||
        mnt->type->ops->lookup(mnt, node, name, &found) != 0) {
        return 0;
    }
    /* Any slot that holds the node covers it for a namespace change: one
     * still mounting, whose constructor may be asleep, as much as an
     * active one. The resolver crosses only into active mounts. */
    for (i = 1; i < VFS_MOUNTS; i++) {
        if (mounts[i].state != VFS_FREE && mounts[i].parent == mnt &&
            mounts[i].covered == found) {
            m = &mounts[i];
        }
    }
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mnt, found);
    }
    return m;
}

/* Replace the current place with another, moving the hold and the transient
 * count. Fails only when the new mount refuses entry. */
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
    if (mnt != cur->mnt) {
        vfs_leave(cur->mnt);
    }
    vfs_ref_put(cur);
    *cur = next;
    return 0;
}

/**
 * Resolve one path. See vfs.h for the whole contract; the shape here is a
 * loop over components with four arms: `.`, `..`, a mount whose type takes
 * the rest of the path in one call, and the ordinary lookup of one name,
 * followed by the crossing onto any mount covering the result.
 */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags,
                struct vfs_ref *out, char *leaf, uint32_t leafcap)
{
    struct vfs_ref cur;
    const char *p = path;
    const char *last_comp = 0;
    int use_real = (flags & VFS_WALK_REAL_IDS) ? 1 : 0;
    int rc;

    if (!path || !out) {
        return -EINVAL;
    }
    if (mounts[0].state != VFS_ACTIVE) {
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
        cur.mnt = &mounts[0];
        cur.node = mounts[0].root;
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
                struct vfs_mount *pm = cur.mnt->parent ? cur.mnt->parent
                                                       : &mounts[0];
                void *pnode = cur.mnt->covered ? cur.mnt->covered
                                               : mounts[0].root;
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
                if (cur.mnt == &mounts[0]) {
                    continue; /* the root of the root is its own parent */
                }
                /* At a mount root: continue at the covered node (the two
                 * nameless mounts hang off the root directory itself), and
                 * re-run this same `..` there. */
                p -= 2;
                rc = walk_move(&cur, cur.mnt->parent,
                               cur.mnt->covered ? cur.mnt->covered
                                                : cur.mnt->parent->root);
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

    if (flags & VFS_WALK_PARENT) {
        rc = may_search(cur.mnt, cur.node, use_real);
        if (rc != 0) {
            goto out_err;
        }
    }
    *out = cur;
    return 0;

out_err:
    vfs_leave(cur.mnt);
    vfs_ref_put(&cur);
    return rc;
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
    int i, j;

    for (i = 0; i < MAX_PROCS; i++) {
        process_t *pr = &proc_table[i];

        if (pr->state == PROC_UNUSED) {
            continue;
        }
        for (j = 0; j < MAX_FDS; j++) {
            fd_entry_t *e = &pr->fds.entries[j];

            if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) &&
                (void *)e->node == from) {
                e->node = to;
            }
        }
    }
    for (i = 0; i < VFS_MOUNTS; i++) {
        if (mounts[i].state == VFS_FREE) {
            continue;
        }
        if (mounts[i].parent == mnt && mounts[i].covered == from) {
            mounts[i].covered = to;
        }
    }
    if (mnt->root == from) {
        mnt->root = to;
    }
    fd_ofd_repoint(from, to);
}

/* ---- the default-answer wrappers ---- */

int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf,
                 uint32_t count, uint64_t offset)
{
    if (!mnt->type->ops->read) {
        return -EINVAL;
    }
    return mnt->type->ops->read(mnt, handle, buf, count, offset);
}

int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf,
                  uint32_t count, uint64_t offset)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->write) {
        return -EROFS;
    }
    return mnt->type->ops->write(mnt, handle, buf, count, offset);
}

int vfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->truncate) {
        return -EROFS;
    }
    return mnt->type->ops->truncate(mnt, handle, size);
}

int vfs_fsync(struct vfs_mount *mnt, void *handle)
{
    if (!mnt->type->ops->fsync) {
        return 0; /* nothing buffered is already stable */
    }
    return mnt->type->ops->fsync(mnt, handle);
}

int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    if (!mnt->type->ops->getattr) {
        return -EINVAL;
    }
    return mnt->type->ops->getattr(mnt, node, st);
}

int vfs_setattr(struct vfs_mount *mnt, void *node, int what,
                const struct vfs_stat *st)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->setattr) {
        return -EROFS;
    }
    return mnt->type->ops->setattr(mnt, node, what, st);
}

int vfs_open_node(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    int rc;

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

int vfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                struct vfs_dirent *ent)
{
    if (!mnt->type->ops->readdir) {
        return -EINVAL;
    }
    return mnt->type->ops->readdir(mnt, handle, cursor, ent);
}

int vfs_create(struct vfs_mount *mnt, void *dir, const char *name,
               uint32_t mode, void **node)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->create) {
        return -EROFS;
    }
    return mnt->type->ops->create(mnt, dir, name, mode, node);
}

int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->unlink) {
        return -EROFS;
    }
    return mnt->type->ops->unlink(mnt, dir, name);
}

int vfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name,
              uint32_t mode)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->mkdir) {
        return -EROFS;
    }
    return mnt->type->ops->mkdir(mnt, dir, name, mode);
}

int vfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->rmdir) {
        return -EROFS;
    }
    return mnt->type->ops->rmdir(mnt, dir, name);
}

int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname,
               void *newdir, const char *newname)
{
    if ((mnt->flags & VFS_MS_RDONLY) || !mnt->type->ops->rename) {
        return -EROFS;
    }
    return mnt->type->ops->rename(mnt, olddir, oldname, newdir, newname);
}

int vfs_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    if (mnt->flags & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (!mnt->type->ops->link) {
        return -EPERM; /* a file system without links */
    }
    return mnt->type->ops->link(mnt, dir, name, node);
}

int vfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    if (!mnt->type->ops->statfs) {
        return -EINVAL;
    }
    return mnt->type->ops->statfs(mnt, st);
}

/* ==== the KRFFS type: the root, adapted onto the interface ==== */

/* A node handle is the node's place in the mapped image; the root is
 * `krffs_fs->node`. Hold and put are empty because a place is not a
 * resource, which is the reason the type may leave them NULL. */

static int kfs_lookup(struct vfs_mount *mnt, void *dir, const char *name,
                      void **node)
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
    st->mode = KRFFS_IS_DIR(n->mode)
                   ? (VFS_S_IFDIR | (uint32_t)(n->mode & 07777u))
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

static int32_t kfs_read(struct vfs_mount *mnt, void *handle, void *buf,
                        uint32_t count, uint64_t offset)
{
    int n;

    (void)mnt;
    n = krffs_read_node(handle, buf, count, (uint32_t)offset);
    return n < 0 ? -EIO : n;
}

/* A grow can relocate the node; every holder, this writer's description
 * included, is repointed before the byte count returns. */
static int32_t kfs_write(struct vfs_mount *mnt, void *handle, const void *buf,
                         uint32_t count, uint64_t offset)
{
    krffs_node_t *n = handle;
    int written;

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

static int kfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    char path[256];

    (void)mnt;
    if (krffs_node_path(handle, path, sizeof(path)) != 0) {
        return -EIO;
    }
    if (krffs_truncate(path + (path[0] == '/'), (uint32_t)size) != 0) {
        return -EIO;
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
    uint64_t dir_id =
        (dirn == krffs_fs->node) ? (uint64_t)KRFFS_ROOT_ID : dirn->id;
    krffs_node_t *node = krffs_fs->node;

    (void)mnt;
    do {
        uint32_t node_off =
            (uint32_t)((uintptr_t)node - (uintptr_t)krffs_fs->node);

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

/* The namespace operations go through the parent's canonical path, which
 * is how the namespace half of KRFFS speaks today; a name is joined to the
 * held directory and handed to the path call. */
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

static int kfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name,
                     uint32_t mode)
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

static int kfs_rename(struct vfs_mount *mnt, void *olddir,
                      const char *oldname, void *newdir, const char *newname)
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
    return krffs_rename(oldp + (oldp[0] == '/'), newp + (newp[0] == '/')) == 0
               ? 0
               : -EIO;
}

/* Creation goes through the parent's canonical path, which is how the
 * namespace half of KRFFS speaks today; the node comes back resolved. */
static int kfs_create(struct vfs_mount *mnt, void *dir, const char *name,
                      uint32_t mode, void **node)
{
    char path[256];
    krffs_node_t *d = dir ? dir : krffs_fs->node;
    krffs_node_t *n;
    uint32_t dlen, nlen;

    (void)mnt;
    (void)mode; /* the caller stamps mode and owner under its own rules */
    if (krffs_node_path(d, path, sizeof(path)) != 0) {
        return -ENAMETOOLONG;
    }
    dlen = k_strlen(path);
    nlen = k_strlen(name);
    if (dlen == 0 || path[dlen - 1] != '/') {
        if (dlen + 1 >= sizeof(path)) {
            return -ENAMETOOLONG;
        }
        path[dlen++] = '/';
    }
    if (dlen + nlen + 1 > sizeof(path)) {
        return -ENAMETOOLONG;
    }
    k_memcpy(path + dlen, name, nlen + 1);
    if (krffs_create(path + (path[0] == '/')) != 0) {
        return -ENOSPC;
    }
    n = krffs_child(d, name);
    if (!n) {
        return -EIO;
    }
    *node = n;
    return 0;
}

static const struct vfs_ops krffs_ops = {
    kfs_lookup,
    0, /* lookup_rest */
    0, /* hold */
    0, /* put */
    kfs_getattr,
    0, /* setattr: the migration moves it here family by family */
    0, /* open: the node is the handle */
    0, /* release */
    kfs_read,
    kfs_write,
    kfs_truncate,
    kfs_fsync,
    kfs_readdir,
    kfs_create,
    kfs_unlink,
    kfs_mkdir,
    kfs_rmdir,
    kfs_rename,
    0, /* link: KRFFS has none */
    0, /* readlink: KRFFS has none */
    kfs_statfs,
    kfs_sync,
    0, /* prepare_unmount: the root is permanent */
    0, /* unmount */
};

static int kfs_mount(struct vfs_mount *mnt, block_device_t *dev,
                     uint32_t flags, const char *options)
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
 * A synthetic node is a classified path. The classification carries a
 * device leaf name, which a value packed into the pointer cannot, so the
 * nodes live in a small pool and hold/put count references into it. A
 * synthetic node lives for the length of one syscall (the open that
 * follows a resolution binds a device's own ops onto the descriptor), so
 * the pool's sixteen slots are a comfortable roof, and an exhausted pool
 * answers ENOMEM rather than corrupting anything.
 */
struct synth_node {
    struct vpath vp;
    int refs;
};

static struct synth_node synth_pool[16];

static int synth_take(const struct vpath *vp, void **node)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (synth_pool[i].refs == 0) {
            synth_pool[i].vp = *vp;
            synth_pool[i].refs = 1;
            *node = &synth_pool[i];
            return 0;
        }
    }
    klog(KLOG_WARN, "vfs", "synthetic node pool exhausted");
    return -ENOMEM;
}

static void synth_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct synth_node *)node)->refs++;
}

static void synth_put(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct synth_node *)node)->refs--;
}

/**
 * The rest of a path through a flat synthetic namespace, `..` inside it
 * resolved lexically because the namespace is a table of spellings, not a
 * tree. A `..` that would climb above the mount root returns `VFS_CLIMB`
 * with `*left` at the remainder, and the resolver continues at the covered
 * node. The full spelling (`prefix` + the canonical rest) goes through
 * `synth_classify`, the same classification the namespaces always had.
 */
static int synth_rest(struct vfs_mount *mnt, const char *prefix,
                      const char *rest, void **node, const char **left)
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

static int devfs_rest(struct vfs_mount *mnt, const char *rest, void **node,
                      const char **left)
{
    return synth_rest(mnt, "dev", rest, node, left);
}

static int procfs_rest(struct vfs_mount *mnt, const char *rest, void **node,
                       const char **left)
{
    return synth_rest(mnt, "proc", rest, node, left);
}

static const struct vfs_ops devfs_vops = {
    0,          devfs_rest, synth_hold, synth_put, 0, 0, 0, 0, 0, 0, 0, 0,
    0,          0,          0,          0,         0, 0, 0, 0, 0, 0, 0, 0,
};

static const struct vfs_ops procfs_vops = {
    0,           procfs_rest, synth_hold, synth_put, 0, 0, 0, 0, 0, 0, 0, 0,
    0,           0,           0,          0,         0, 0, 0, 0, 0, 0, 0, 0,
};

static int synth_mount_root(struct vfs_mount *mnt, block_device_t *dev,
                            uint32_t flags, const char *options)
{
    struct vpath vp;

    (void)dev;
    (void)flags;
    /* The mount root is the namespace's own directory, held for the life of
     * the mount. */
    synth_classify(options, &vp);
    return synth_take(&vp, &mnt->root);
}

static int devfs_mount(struct vfs_mount *mnt, block_device_t *dev,
                       uint32_t flags, const char *options)
{
    (void)options;
    return synth_mount_root(mnt, dev, flags, "dev");
}

static int procfs_mount(struct vfs_mount *mnt, block_device_t *dev,
                        uint32_t flags, const char *options)
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
    struct vfs_mount *m = &mounts[idx];

    m->type = type;
    m->dev = 0;
    m->flags = 0;
    m->parent = parent;
    m->covered = 0;
    k_strncpy(m->covered_path, path, sizeof(m->covered_path));
    m->refs = 0;
    m->inprogress = 0;
    m->ns_holder = -1;
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

void vfs_init(void)
{
    static int installed;

    if (installed) {
        /* A re-mount of the root (the debug shell can remount from the
         * disk) replaces the image; the entry keeps its slot and takes the
         * new root node. */
        mounts[0].root = krffs_fs->node;
        return;
    }
    installed = 1;
    install(0, &vfs_krffs_type, "/", 0);
    install(1, &vfs_devfs_type, "/dev", &mounts[0]);
    install(2, &vfs_procfs_type, "/proc", &mounts[0]);
}

/* ==== mount(2) and umount2(2) ==== */

/* The types mount(2) accepts, by the name it spells; the permanent three
 * are not here because nothing mounts them by name. */
static const struct vfs_type *const mount_types[] = {&vfs_vfat_type, 0, 0, 0};

static const struct vfs_type *type_named(const char *name)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (mount_types[i] && streq(mount_types[i]->name, name)) {
            return mount_types[i];
        }
    }
    return 0;
}

const struct vfs_type *vfs_type_at(int idx)
{
    if (idx < 0 || idx >= 4) {
        return 0;
    }
    return mount_types[idx];
}

/* The flags mount(2) may carry here: honoured or recorded ones. Anything
 * else is EINVAL, MS_REMOUNT, MS_BIND and MS_MOVE first among them. */
#define VFS_MS_KNOWN                                                          \
    (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC |            \
     VFS_MS_NOATIME | VFS_MS_NODIRATIME | VFS_MS_SILENT | VFS_MS_RELATIME)

static int drained(void *arg)
{
    struct vfs_mount *mnt = arg;
    return mnt->inprogress == 0;
}

/**
 * mount(2): the order of work is resolve and hold the target, check its
 * path length, claim the source's range, take a slot as `mounting`, run
 * the constructor, publish. Every failure after the first step releases
 * what the steps before it took.
 */
int vfs_do_mount(const char *source, const char *target, const char *type,
                 uint32_t flags, const char *options)
{
    const struct vfs_type *t = type_named(type);
    struct vfs_ref tref;
    struct vfs_stat st;
    struct vfs_mount *m = 0;
    block_device_t *dev, *backing;
    uint64_t first, count;
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
    /* The target: a directory on a mount that can host one, not the root
     * of the root, not already covered. */
    if (tref.mnt->type->ops->lookup_rest) {
        vfs_ref_release(&tref);
        return -EINVAL;
    }
    if (tref.mnt == &mounts[0] && tref.node == mounts[0].root) {
        vfs_ref_release(&tref);
        return -EINVAL;
    }
    if (vfs_getattr(tref.mnt, tref.node, &st) != 0 || !VFS_IS_DIR(st.mode)) {
        vfs_ref_release(&tref);
        return -ENOTDIR;
    }
    if (k_strlen(target) >= VFS_COVERED_PATH_MAX) {
        vfs_ref_release(&tref);
        return -ENAMETOOLONG;
    }
    for (i = 0; i < VFS_MOUNTS; i++) {
        if (mounts[i].state != VFS_FREE && mounts[i].covered == tref.node &&
            mounts[i].parent == tref.mnt) {
            vfs_ref_release(&tref);
            return -EBUSY; /* covered, or reserved by a mount in progress */
        }
    }
    /* The source: a partition window under /dev, claimed as a sector range
     * on the backing disk. */
    rc = devfs_window_of(source, &dev, &backing, &first, &count);
    if (rc != 0) {
        vfs_ref_release(&tref);
        return rc;
    }
    claim = devfs_claim(backing, first, first + count - 1);
    if (claim < 0) {
        vfs_ref_release(&tref);
        return claim;
    }
    for (i = 3; i < VFS_MOUNTS; i++) {
        if (mounts[i].state == VFS_FREE) {
            m = &mounts[i];
            break;
        }
    }
    if (!m) {
        devfs_release_claim(claim);
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
    wait_queue_init(&m->ns_wq);
    m->claim = claim;
    k_strncpy(m->covered_path, target, sizeof(m->covered_path));
    k_strncpy(m->source, source, sizeof(m->source));
    m->source[sizeof(m->source) - 1] = '\0';
    tref.mnt->refs++; /* a child mount is a reference on its parent */
    vfs_leave(tref.mnt); /* the walk's count; the hold stays with the slot */

    vfs_ns_lock(m);
    rc = t->mount(m, dev, flags, options);
    vfs_ns_unlock(m);
    if (rc != 0) {
        struct vfs_ref cov;

        cov.mnt = m->parent;
        cov.node = m->covered;
        m->parent->refs--;
        vfs_ref_put(&cov);
        devfs_release_claim(claim);
        m->state = VFS_FREE;
        return rc;
    }
    m->state = VFS_ACTIVE;
    klog(KLOG_INFO, "vfs", "mounted %s on %s (%s%s)", source, target, t->name,
         (flags & VFS_MS_RDONLY) ? ", read-only" : "");
    return 0;
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
    struct vfs_ref tref;
    struct vfs_mount *m;
    struct vfs_ref cov;
    int rc;

    rc = vfs_resolve(0, target, 0, &tref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    m = tref.mnt;
    if (tref.node != m->root) {
        vfs_ref_release(&tref);
        return -EINVAL; /* not a mount point */
    }
    if (m == &mounts[0] || m == &mounts[1] || m == &mounts[2]) {
        vfs_ref_release(&tref);
        return -EBUSY; /* the permanent three */
    }
    vfs_ref_release(&tref);
    if (m->state != VFS_ACTIVE) {
        return -EBUSY;
    }
    m->state = VFS_UNMOUNTING;
    while (m->inprogress > 0) {
        wq_wait_event(&m->ns_wq, drained, m); /* EINTR: keep draining */
    }
    if (m->refs > 0) {
        m->state = VFS_ACTIVE;
        return -EBUSY;
    }
    vfs_ns_lock(m);
    if (m->type->ops->sync) {
        m->type->ops->sync(m);
    }
    if (m->type->ops->prepare_unmount) {
        rc = m->type->ops->prepare_unmount(m);
        if (rc != 0) {
            vfs_ns_unlock(m);
            m->state = VFS_ACTIVE;
            return rc;
        }
    }
    if (bcache_sync(m->dev) != 0) {
        vfs_ns_unlock(m);
        m->state = VFS_ACTIVE;
        return -EIO; /* nothing irreversible has happened yet */
    }
    bcache_invalidate(m->dev);
    klog(KLOG_INFO, "vfs", "unmounted %s: %d opens, %d releases",
         m->covered_path, (int)m->opens, (int)m->releases);
    if (m->type->ops->put) {
        m->type->ops->put(m, m->root);
    }
    m->type->ops->unmount(m);
    vfs_ns_unlock(m);
    cov.mnt = m->parent;
    cov.node = m->covered;
    m->parent->refs--;
    vfs_ref_put(&cov);
    devfs_release_claim(m->claim);
    m->state = VFS_FREE;
    return 0;
}

/* ---- the working directory's hold on its mount ---- */

void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    vfs_cwd_clear(p);
    if (!mnt || mnt == &mounts[0] || mnt == &mounts[1] || mnt == &mounts[2]) {
        return; /* the permanent mounts need no pin */
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

    if (!p->cwd_mnt) {
        return;
    }
    r.mnt = p->cwd_mnt;
    r.node = p->cwd_node;
    p->cwd_mnt->refs--;
    p->cwd_mnt = 0;
    p->cwd_node = 0;
    vfs_ref_put(&r);
}
