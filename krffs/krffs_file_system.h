/*
 * krffs_file_system.h: the handle of a mapped KRFFS region, the constants of
 * the format, and the layout of an empty file system.
 */
#ifndef KRFFS_FILE_SYSTEM_H
#define KRFFS_FILE_SYSTEM_H

#include <stdint.h>
#include <sys/types.h>

struct krffs_node;

/** The magic number every node begins with: the ASCII codes of `K` and `R`. */
static const uint16_t KRFFS_File_System_Magic = 0x4B52;

/** The root node's name, which is empty. */
static const char KRFFS_File_System_Root_Node_Name[] = "";

/** The mode `mkfs.krffs` gives the root: 0755, with no type bits. */
static const uint16_t KRFFS_File_System_Root_Node_Mode = 0755;

/**
 * A KRFFS region mapped into memory: `node` is the root, at the first byte of
 * the mapping, and `size` is the region's length in bytes. Every node a tool
 * holds is a pointer into the mapping, so a store into a node is a store
 * into the file.
 */
struct krffs_file_system {
    struct krffs_node *node;
    off_t size;
};

/**
 * Writes an empty file system over the region of `file_system`, which must be
 * at least two headers: a root node (`krffs_initialize_root_node`), whose
 * header is not synced, and one free node over the rest, whose header is.
 * Returns `file_system`.
 */
struct krffs_file_system *krffs_initialize_file_system(struct krffs_file_system *file_system);

#endif
