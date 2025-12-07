#ifndef KRFFS_FILE_SYSTEM_H
#define KRFFS_FILE_SYSTEM_H

#include <stdint.h>
#include <sys/types.h>

struct krffs_node;

static const uint16_t KRFFS_File_System_Magic = 0x4B52;
static const char KRFFS_File_System_Root_Node_Name[] = "";
static const uint16_t KRFFS_File_System_Root_Node_Mode = 0755;

struct krffs_file_system {
    struct krffs_node *node;
    off_t size;
};

struct krffs_file_system *krffs_initialize_file_system(struct krffs_file_system *file_system);

#endif
