/* Models with no captured entry must not silently accept a real record. */
#include "fs/name.h"

/** Admit a change in a model that has no name walks to exclude. */
void vfs_name_lock_changes(struct vfs_mount *mnt)
{
    (void)mnt;
}

/** End the model's change of names. */
void vfs_name_unlock_changes(struct vfs_mount *mnt)
{
    (void)mnt;
}

void vfs_name_put(struct vfs_name *name)
{
    if (name) {
        __builtin_trap();
    }
}

struct vfs_name *vfs_name_ref(struct vfs_name *name)
{
    if (name) {
        __builtin_trap();
    }
    return name;
}

int vfs_name_capture(const struct vfs_ref *parent, const char *leaf, struct vfs_name **out)
{
    (void)parent;
    (void)leaf;
    *out = 0;
    return 0;
}
