/* The locks of a file system that locks each file on its own
 * (`vfs_type.file_locked`): a node's, a rename's and a mount's cover. */

#include "vfs.h"
#include "dcache.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/process.h"

/* The record of every mount's rename lock (sleeplock.h). */
static struct sleep_lock_record rename_lock_record = SLEEP_LOCK_RECORD("rename");

/* The most `..` a rename's climb takes before it calls the tree a loop. */
#define RENAME_CLIMB_MAX 4096

/** The mount whose file system `m` shows, as `vfs_mount_base` gives it, for
 * the callers that change it. */
static struct vfs_mount *base_of(struct vfs_mount *m)
{
    return m->bind_of ? m->bind_of : m;
}

/** Put a node a lookup returned, on a type that counts holds. */
static void put_node(struct vfs_mount *mnt, void *node)
{
    if (mnt->type->ops->put) {
        mnt->type->ops->put(base_of(mnt), node);
    }
}

void vfs_rename_lock_init(struct vfs_mount *m)
{
    sleep_lock_init(&m->rename, &rename_lock_record, 0);
}

int vfs_file_locked(const struct vfs_mount *mnt)
{
    return vfs_mount_base(mnt)->type->file_locked;
}

/** The node `ref` names, its mount's root for a NULL node. */
static void *ref_node(const struct vfs_ref *ref)
{
    return ref->node ? ref->node : ref->mnt->root;
}

void vfs_node_lock(const struct vfs_ref *ref)
{
    struct vfs_mount *base = base_of(ref->mnt);

    if (base->type->file_locked) {
        base->type->ops->lock_node(base, ref_node(ref));
    } else {
        vfs_ns_lock(ref->mnt);
    }
}

/** The name cache puts what it was given back under a file lock once the
 * caller holds none (`dcache_drain`), so that a removed file whose last
 * hold was a name's is freed at the end of the call that removed it. */
void vfs_node_unlock(const struct vfs_ref *ref)
{
    struct vfs_mount *base = base_of(ref->mnt);

    if (base->type->file_locked) {
        base->type->ops->unlock_node(base, ref_node(ref));
        if (current_proc && current_proc->file_locks == 0) {
            dcache_drain();
        }
    } else {
        vfs_ns_unlock(ref->mnt);
    }
}

int vfs_child_lock(const struct vfs_ref *dir, const char *name, struct vfs_ref *child)
{
    struct vfs_mount *base = base_of(dir->mnt);
    void *found;
    int rc;

    child->mnt = dir->mnt;
    child->node = NULL;
    if (!base->type->file_locked) {
        return 0;
    }
    rc = base->type->ops->lookup(base, ref_node(dir), name, &found);
    if (rc != 0) {
        return rc == -ENOENT ? 0 : rc;
    }
    child->node = found;
    vfs_node_lock(child);
    return 0;
}

void vfs_child_unlock(struct vfs_ref *child)
{
    if (child->node) {
        vfs_node_unlock(child);
        put_node(child->mnt, child->node);
        child->node = NULL;
    }
}

/** The inode number of `node` of the file system `base`, into `*ino`: 0 or
 * the type's error. */
static int node_ino(struct vfs_mount *base, void *node, uint64_t *ino)
{
    struct vfs_stat st;
    int rc = base->type->ops->getattr(base, node, &st);

    if (rc == 0) {
        *ino = st.ino;
    }
    return rc;
}

/**
 * Climb from the directory `from` of `base` through `..` to the root, and
 * set in `*met` bit 1 when it passes inode `a` and bit 2 when it passes `b`,
 * `from` itself included; 0 for neither, which a number of 0 never matches.
 * Holds nothing of its own while it asks, so that only the caller's rename
 * lock keeps the tree's shape. 0, -ELOOP past RENAME_CLIMB_MAX steps, or
 * the type's error.
 */
static int climb_meets(struct vfs_mount *base, void *from, uint64_t a, uint64_t b, int *met)
{
    void *node = from;
    void *up;
    uint64_t ino = 0, upino = 0;
    int held = 0;
    int n;
    int rc = node_ino(base, node, &ino);

    *met = 0;
    for (n = 0; rc == 0; n++) {
        if (n == RENAME_CLIMB_MAX) {
            rc = -ELOOP;
            break;
        }
        *met |= (a && ino == a ? 1 : 0) | (b && ino == b ? 2 : 0);
        rc = base->type->ops->lookup(base, node, "..", &up);
        if (rc != 0) {
            break;
        }
        if (held) {
            put_node(base, node);
        }
        node = up;
        held = 1;
        rc = node_ino(base, node, &upino);
        if (rc == 0 && upino == ino) {
            break; /* the root is its own parent */
        }
        ino = upino;
    }
    if (held) {
        put_node(base, node);
    }
    return rc;
}

/** The inode number of `name` in the directory `dir` of `base`, or 0 for a
 * name it does not have or cannot look up. */
static uint64_t name_ino(struct vfs_mount *base, void *dir, const char *name)
{
    void *found;
    uint64_t ino = 0;

    if (base->type->ops->lookup(base, dir, name, &found) == 0) {
        if (node_ino(base, found, &ino) != 0) {
            ino = 0;
        }
        put_node(base, found);
    }
    return ino;
}

/**
 * The order in which the rename's two directories, inodes `oino` and
 * `nino`, lock, into `h->dir`, under the rename lock of `base`, and which
 * names' nodes stay unlocked, `*skip` bit 1 for the source and 2 for the
 * target, with the inode numbers of the names that decision was made on,
 * `*cino` and `*tino`, 0 for a name not there. 0 or the climb's error.
 */
static int rename_order(struct vfs_mount *base, const struct vfs_ref *opar, const char *oleaf,
                        const struct vfs_ref *npar, const char *nleaf, uint64_t oino, uint64_t nino,
                        struct vfs_rename_hold *h, int *skip, uint64_t *cino, uint64_t *tino)
{
    int nup, oup;
    int rc;

    *cino = name_ino(base, ref_node(opar), oleaf);
    *tino = name_ino(base, ref_node(npar), nleaf);
    rc = climb_meets(base, ref_node(npar), oino, *cino, &nup);
    if (rc == 0) {
        rc = climb_meets(base, ref_node(opar), nino, *tino, &oup);
    }
    if (rc != 0) {
        return rc;
    }
    *skip = ((nup & 2) ? 1 : 0) | ((oup & 2) ? 2 : 0);
    if ((nup & 1) || (!(oup & 1) && oino < nino)) {
        h->dir[0] = *opar;
        h->dir[1] = *npar;
    } else {
        h->dir[0] = *npar;
        h->dir[1] = *opar;
    }
    return 0;
}

/** Look `name` up in the locked directory `dir` and hold the node into
 * `out`, with its inode number in `*ino` and whether it is a directory in
 * `*isdir`; `out->node` NULL and `*ino` 0 for a name that is not there. */
static void rename_child(struct vfs_mount *base, const struct vfs_ref *dir, const char *name,
                         struct vfs_ref *out, uint64_t *ino, int *isdir)
{
    struct vfs_stat st;
    void *found;

    out->mnt = dir->mnt;
    out->node = NULL;
    *ino = 0;
    *isdir = 0;
    if (base->type->ops->lookup(base, ref_node(dir), name, &found) != 0) {
        return;
    }
    if (base->type->ops->getattr(base, found, &st) != 0) {
        put_node(base, found);
        return;
    }
    *ino = st.ino;
    *isdir = (st.mode & 0170000u) == 0040000u;
    out->node = found;
}

/**
 * Under the rename's directory locks, lock the nodes of its names that
 * `skip` does not leave out, each once, into `h->child`: a directory before
 * a file, as every other call that holds a directory and a file takes them,
 * else by inode number.
 * Returns 0, or, for a rename between two directories, 1 with nothing
 * locked here when a name's node is not the one, `cino` or `tino`, the
 * order was decided on, as when another process renamed a directory above
 * the other one to that name within its own directory meanwhile.
 */
static int rename_children(struct vfs_mount *base, const struct vfs_ref *opar, const char *oleaf,
                           const struct vfs_ref *npar, const char *nleaf, struct vfs_rename_hold *h,
                           int skip, uint64_t cino, uint64_t tino)
{
    struct vfs_ref c, t, tmp;
    uint64_t c2, t2;
    int cdir, tdir;

    rename_child(base, opar, oleaf, &c, &c2, &cdir);
    rename_child(base, npar, nleaf, &t, &t2, &tdir);
    if (h->renaming && (c2 != cino || t2 != tino)) {
        if (c.node) {
            put_node(base, c.node);
        }
        if (t.node) {
            put_node(base, t.node);
        }
        return 1;
    }
    if ((skip & 1) && c.node) {
        put_node(base, c.node);
        c.node = NULL;
    }
    if (((skip & 2) || (c.node && t2 == c2)) && t.node) {
        put_node(base, t.node);
        t.node = NULL;
    }
    if (c.node && t.node && (tdir != cdir ? tdir : t2 < c2)) {
        tmp = c;
        c = t;
        t = tmp;
    }
    h->child[0] = c.node ? c : t;
    h->child[1] = c.node ? t : c;
    if (h->child[0].node) {
        vfs_node_lock(&h->child[0]);
    }
    if (h->child[1].node) {
        vfs_node_lock(&h->child[1]);
    }
    return 0;
}

int vfs_rename_lock(const struct vfs_ref *opar, const char *oleaf, const struct vfs_ref *npar,
                    const char *nleaf, struct vfs_rename_hold *h)
{
    struct vfs_mount *base = base_of(opar->mnt);
    uint64_t oino = 0, nino = 0, cino = 0, tino = 0;
    int skip;
    int rc;

    for (;;) {
        k_memset(h, 0, sizeof(*h));
        h->dir[0] = *opar;
        if (!base->type->file_locked) {
            vfs_ns_lock(opar->mnt);
            return 0;
        }
        rc = node_ino(base, ref_node(opar), &oino);
        if (rc == 0) {
            rc = node_ino(base, ref_node(npar), &nino);
        }
        if (rc != 0) {
            return rc;
        }
        skip = 0;
        if (oino != nino) {
            sleep_lock_take(&base->rename);
            h->renaming = base;
            rc = rename_order(base, opar, oleaf, npar, nleaf, oino, nino, h, &skip, &cino, &tino);
            if (rc != 0) {
                sleep_lock_give(&base->rename);
                return rc;
            }
        }
        vfs_node_lock(&h->dir[0]);
        if (h->dir[1].mnt) {
            vfs_node_lock(&h->dir[1]);
        }
        if (rename_children(base, opar, oleaf, npar, nleaf, h, skip, cino, tino) == 0) {
            return 0;
        }
        vfs_rename_unlock(h);
    }
}

void vfs_rename_unlock(struct vfs_rename_hold *h)
{
    int i;

    for (i = 1; i >= 0; i--) {
        vfs_child_unlock(&h->child[i]);
    }
    if (!vfs_file_locked(h->dir[0].mnt)) {
        vfs_ns_unlock(h->dir[0].mnt);
    } else {
        for (i = 1; i >= 0; i--) {
            if (h->dir[i].mnt) {
                vfs_node_unlock(&h->dir[i]);
            }
        }
    }
    if (h->renaming) {
        sleep_lock_give(&h->renaming->rename);
    }
    k_memset(h, 0, sizeof(*h));
}

int vfs_cover_lock(const struct vfs_ref *tref)
{
    struct vfs_mount *base = base_of(tref->mnt);
    struct vfs_stat st;

    vfs_node_lock(tref);
    if (!tref->node || !base->type->file_locked) {
        return 0;
    }
    return base->type->ops->getattr(base, tref->node, &st) == 0 && st.nlink == 0 ? -ENOENT : 0;
}
