#ifndef _LOOP_H_
#define _LOOP_H_

#include "../lib/kint.h"
#include "block.h"

/*
 * A block device over a regular file.
 *
 * The file is reached through the file system holding it, so an image on any
 * mounted volume can carry a file system of its own. Four of them, attached
 * and detached through /dev/loop-control and listed in /proc/loop.
 *
 * Policy: four devices. Each has a node of its own in /dev, made with the
 * device table at boot, and a program names one by that node, so the count
 * is part of the names the system offers rather than a table that runs
 * out; a fifth would be a fifth name.
 */

#define LOOP_DEVICES 4

struct vfs_mount;

/** The device for `index`, or NULL when nothing is attached there. */
block_device_t *loop_device_at(int index);

/**
 * Attach `path` to loop device `index`, or to the first free one when `index`
 * is -1. `writable` asks for a device that takes writes, which needs write
 * access to the file.
 *
 * Answers the index, or a negative errno: `EBUSY` when that device is taken
 * or the file is already attached, `EINVAL` when the file is not a regular
 * file or its length is not whole sectors, `ELOOP` when the file lives on a
 * chain of devices as deep as the buffer cache can serve, and whatever the
 * resolve or the open answered otherwise.
 */
int loop_attach(int index, const char *path, int writable);

/**
 * Detach loop device `index`. `EBUSY` while anything is mounted on it or
 * holds one of its buffers; the buffers are dropped and the file released
 * otherwise.
 */
int loop_detach(int index);

/** Whether the file at (`mnt`, `node`) backs a loop device. */
int loop_backs(struct vfs_mount *mnt, void *node);

/** Follow a node the file system moved, for `vfs_repoint`, which owns that
 * sweep. */
void loop_repoint(struct vfs_mount *mnt, const void *from, void *to);

/** Whether `mnt` holds the file of any loop device. */
int loop_holds_mount(struct vfs_mount *mnt);

/**
 * Take down every loop device standing on `device`, which has gone: nothing
 * can reach its file again, so the loop device goes with it and the mount
 * above it can be taken down. What it held is not released, because the
 * mount it was held on is going too.
 */
void loop_device_gone(block_device_t *device);

/** One line of /proc/loop for `index`, or 0 when nothing is attached. */
int loop_line(int index, char *buffer, uint32_t cap);

/** The write handler behind /dev/loop-control. */
/** The longest backing path a control line may name; a line is that plus
    its command word, and a longer write is refused. */
#define LOOP_PATH_MAX 64

int loop_control_write(const char *buffer, uint32_t count);

#endif /* _LOOP_H_ */
