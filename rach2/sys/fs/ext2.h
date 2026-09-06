#ifndef _EXT2_H_
#define _EXT2_H_

#include "vfs.h"

/* Held nodes per mount, a pool like FAT32's; `ENOMEM` beyond. */
#define EXT2_GROUPS 128 /* 16 GiB at 4 KiB blocks; a larger volume is refused */

extern const struct vfs_type vfs_ext2_type;

#endif /* _EXT2_H_ */
