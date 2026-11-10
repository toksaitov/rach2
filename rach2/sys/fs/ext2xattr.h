/* The checked ext2 attribute block codec; buffers belong to its caller. */
#ifndef _EXT2XATTR_H_
#define _EXT2XATTR_H_

#include "vfs.h"

/** Check one complete block of `bytes` bytes; 0 or -EIO for a bad layout. */
int ext2_xattr_check(const uint8_t *block, uint32_t bytes);
/** Read or list user attributes into a kernel buffer; return bytes or -errno.
 * `name` is NULL for a list; `size` zero asks only for the required size.
 * A NULL block is an inode without attributes. */
int ext2_xattr_read(const uint8_t *block, uint32_t bytes, const char *name, void *value,
                    uint32_t size);
/** Repack a block into distinct `out`, preserving other namespaces.
 * `arg->name` excludes user.; REMOVE removes it, flags are CREATE=1 and
 * REPLACE=2. Returns 1 for a nonempty block, 0 for empty, or -errno.
 * `block` may be NULL; both buffers otherwise span `bytes`. */
int ext2_xattr_edit(const uint8_t *block, uint8_t *out, uint32_t bytes,
                    const struct vfs_xattr *arg);
#endif
