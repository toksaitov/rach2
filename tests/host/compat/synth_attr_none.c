/* The kept attributes of /proc, /sys and /dev nodes (fs/synth_attr.h) for
 * a host test whose rows change none: nothing is kept. */
#include "../../../rach2/sys/fs/synth_attr.h"
#include "../../../rach2/sys/fs/vfs.h"

/** Nothing: no node has kept attributes. */
void synth_attr_apply(uint64_t dev, uint64_t ino, struct vfs_stat *st)
{
    (void)dev;
    (void)ino;
    (void)st;
}

/** Keep nothing, and succeed: no row reads a change back. */
int32_t synth_attr_store(uint64_t dev, uint64_t ino, int what, const struct vfs_stat *st)
{
    (void)dev;
    (void)ino;
    (void)what;
    (void)st;
    return 0;
}
