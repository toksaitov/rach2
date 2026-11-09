/* debugfs.h: the debug file system at /sys/kernel/debug, whose files the
 * kernel's subsystems register, root's alone, as Linux mounts it. */
#ifndef _DEBUGFS_H_
#define _DEBUGFS_H_

#include "../lib/kint.h"
#include "file.h"

/**
 * Register the file `path`, relative to the mount's root ("sync/sw_sync",
 * "dri/0/name"), with permission bits `mode` and the operations an open
 * binds to its descriptor; the directories on the path come with it. 0, or
 * -errno: -ENODEV while debugfs has no files of its own.
 */
int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops);

#endif /* _DEBUGFS_H_ */
