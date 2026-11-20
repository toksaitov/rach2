/* vfat's attribute requests (fs/fat32.h) for a host test that compiles
 * syscall_fs.c and mounts no vfat volume: no row reaches them. */
#include "../../../rach2/sys/fs/fat32.h"
#include "../../../rach2/sys/lib/kerrno.h"

/** No attributes: no row asks a vfat node for them. */
uint32_t fat_attr_get(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return 0;
}

/** Refuse, as for a node that is not on vfat: no row sets attributes. */
int fat_attr_set(struct vfs_mount *mnt, void *node, uint32_t attr, int rights)
{
    (void)mnt;
    (void)node;
    (void)attr;
    (void)rights;
    return -ENOTTY;
}
