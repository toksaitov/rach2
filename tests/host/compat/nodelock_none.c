/* The node locks for models with no file_locked type: every lock is the
 * mount's namespace lock, which the model defines, as nodelock.c takes it
 * for such a type. */
#include "fs/vfs.h"

int vfs_file_locked(const struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

void vfs_node_lock(const struct vfs_ref *ref)
{
    vfs_ns_lock(ref->mnt);
}

void vfs_node_unlock(const struct vfs_ref *ref)
{
    vfs_ns_unlock(ref->mnt);
}

int vfs_child_lock(const struct vfs_ref *dir, const char *name, struct vfs_ref *child)
{
    (void)name;
    child->mnt = dir->mnt;
    child->node = 0;
    return 0;
}

void vfs_child_unlock(struct vfs_ref *child)
{
    child->node = 0;
}

int vfs_rename_lock(const struct vfs_ref *opar, const char *oleaf, const struct vfs_ref *npar,
                    const char *nleaf, struct vfs_rename_hold *h)
{
    (void)oleaf;
    (void)npar;
    (void)nleaf;
    h->renaming = 0;
    h->dir[0] = *opar;
    h->dir[1].mnt = 0;
    h->dir[1].node = 0;
    h->child[0] = h->dir[1];
    h->child[1] = h->dir[1];
    vfs_ns_lock(opar->mnt);
    return 0;
}

void vfs_rename_unlock(struct vfs_rename_hold *h)
{
    vfs_ns_unlock(h->dir[0].mnt);
}

int vfs_cover_lock(const struct vfs_ref *tref)
{
    vfs_ns_lock(tref->mnt);
    return 0;
}
