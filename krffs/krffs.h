#ifndef KRFFS_H
#define KRFFS_H

#include "krffs_allocator.h"
#include "krffs_file_system.h"
#include "krffs_fuse_operations.h"
#include "krffs_node.h"
#include "krffs_platform.h"
#include "krffs_utilities.h"

/** The currently opened KRFFS file system, backed by `File_System_Store`. */
struct krffs_file_system *KRFFS_File_System = NULL;
static struct krffs_file_system File_System_Store;

#endif
