/* Models with no captured entry must not silently accept a real record. */
#include "fs/name.h"

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
