/* The checked ext2 attribute block codec; buffers belong to its caller. */
#ifndef _EXT2XATTR_H_
#define _EXT2XATTR_H_

#include "vfs.h"

/** Check one complete block of `bytes` bytes; 0 or -EIO for a bad layout. */
int ext2_xattr_check(const uint8_t *block, uint32_t bytes);
/* The name indexes of the entries the driver reads and writes: a user
 * attribute's name follows `user.`, and a POSIX ACL's name is empty. */
#define EXT2_XATTR_USER        1
#define EXT2_XATTR_ACL_ACCESS  2
#define EXT2_XATTR_ACL_DEFAULT 3

/** Read the value of the entry `name` of index `index` into a kernel
 * buffer; return its bytes or -errno. `size` zero asks only for the size,
 * and a buffer too small is -ERANGE. A NULL block is an inode without
 * attributes, whose every name is -ENODATA. */
int ext2_xattr_read(const uint8_t *block, uint32_t bytes, int index, const char *name, void *value,
                    uint32_t size);
/** List the user attributes, and with `acl` the POSIX ACLs, as Linux's
 * listxattr gives them: each full name with its NUL, in the block's order.
 * Return the bytes or -errno; `size` zero asks only for the size, and a
 * buffer too small is -ERANGE. A NULL block lists nothing. */
int ext2_xattr_list(const uint8_t *block, uint32_t bytes, int acl, void *value, uint32_t size);
/** Repack a block into distinct `out`, preserving the other entries.
 * `arg->name` excludes the prefix of `index` and may be empty only for an
 * index other than EXT2_XATTR_USER; REMOVE removes it, flags are CREATE=1
 * and REPLACE=2. Returns 1 for a nonempty block, 0 for empty, or -errno.
 * `block` may be NULL; both buffers otherwise span `bytes`. */
int ext2_xattr_edit(const uint8_t *block, uint8_t *out, uint32_t bytes, int index,
                    const struct vfs_xattr *arg);
#endif
