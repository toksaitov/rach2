#ifndef KRFFS_FILE_SYSTEM_H
#define KRFFS_FILE_SYSTEM_H

#include <stdint.h>
#include <sys/types.h>

struct krffs_node;

/**
 * Magic value at the start of each metadata node, used to identify the file
 * system and check its consistency. The bytes are the ASCII codes of `K` and
 * `R`.
 */
static const uint16_t KRFFS_File_System_Magic = 0x4B52;

/** Name of the file system's root node. */
static const char KRFFS_File_System_Root_Node_Name[] = "";

/** Mode of the file system's root node. */
static const uint16_t KRFFS_File_System_Root_Node_Mode = 0755;

/**
 * File system handle. Nodes point into the pages mapped from the backing file.
 */
struct krffs_file_system
{
    struct krffs_node *node;
    off_t size;
};

/**
 * Initializes the file system, writing a root metadata node followed by a
 * free-space node that spans the rest of the mapping.
 */
struct krffs_file_system *krffs_initialize_file_system(
                              struct krffs_file_system *file_system
                          );

#endif
