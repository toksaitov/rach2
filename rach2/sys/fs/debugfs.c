/*
 * debugfs.c: the debug file system's files and directories. A stub until
 * 76-18a writes it, which takes no file.
 */

#include "debugfs.h"

#include "../lib/kerrno.h"

int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops)
{
    (void)path;
    (void)mode;
    (void)ops;
    return -ENODEV;
}
