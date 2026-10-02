#ifndef _LOOP_H_
#define _LOOP_H_

#include "../lib/kint.h"
#include "block.h"

/*
 * A block device over a regular file.
 *
 * The file is reached through the file system holding it, so an image on any
 * mounted volume can carry a file system of its own. Four of them, attached
 * and detached through /dev/loop-control, by a line written to it or by
 * Linux's loop ioctls, which util-linux's losetup and mount make, and listed
 * in /proc/loop.
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

/** The device for `index` when its attachment is complete and its file still
 * there, or NULL: the device reads and writes reach. */
block_device_t *loop_attached_at(int index);

/**
 * The device a read (`write` 0) or a write of device `index`'s node reaches,
 * or NULL with the errno in `*err`: `ENXIO` when nothing is attached, and
 * for a write `EROFS` on a read-only attachment and `EBUSY` while a file
 * system is mounted on it, whose blocks the cache may hold.
 */
block_device_t *loop_node_device(int index, int write, int32_t *err);

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

/* Linux's loop ioctls: on a device's node, and on /dev/loop-control. */
#define LOOP_SET_FD         0x4C00u
#define LOOP_CLR_FD         0x4C01u
#define LOOP_SET_STATUS64   0x4C04u
#define LOOP_GET_STATUS64   0x4C05u
#define LOOP_SET_CAPACITY   0x4C07u
#define LOOP_SET_DIRECT_IO  0x4C08u
#define LOOP_SET_BLOCK_SIZE 0x4C09u
#define LOOP_CONFIGURE      0x4C0Au
#define LOOP_CTL_ADD        0x4C80u
#define LOOP_CTL_REMOVE     0x4C81u
#define LOOP_CTL_GET_FREE   0x4C82u

/* struct loop_config: the backing descriptor, the block size and a struct
 * loop_info64, 232 bytes, then 64 reserved. */
#define LOOP_CONFIG_SIZE 304u
#define LOOP_CONFIG_INFO 8u

/* The flags of a struct loop_info64 this driver acts on. */
#define LO_FLAGS_READ_ONLY 1u
#define LO_FLAGS_AUTOCLEAR 4u

/**
 * The flags of the struct loop_info64 at `info`, or `EINVAL` for one this
 * driver cannot do: an offset, a size limit or an encryption.
 */
int loop_info_flags(const uint8_t *info, uint32_t *flags);

/**
 * Attach `path`, the file a descriptor of the caller has open, to device
 * `index`, as LOOP_SET_FD and LOOP_CONFIGURE do: `writable` when both the
 * descriptor and the device's node are open for writing and `flags` does not
 * ask for LO_FLAGS_READ_ONLY, and cleared at the last close and unmount with
 * LO_FLAGS_AUTOCLEAR. 0, or what `loop_attach` answers.
 */
int loop_attach_file(int index, const char *path, int writable, uint32_t flags);

/**
 * A loop ioctl `request` on device `index`'s node other than the two that
 * attach: LOOP_CLR_FD, LOOP_SET_STATUS64, LOOP_GET_STATUS64 and the settings
 * a file-backed device has one answer for. `privileged` is the caller's
 * standing to change the device. `ENOTTY` for a request that is not one.
 */
int32_t loop_ioctl(int index, int privileged, uint32_t request, uint32_t arg);

/** LOOP_CTL_GET_FREE, LOOP_CTL_ADD and LOOP_CTL_REMOVE on /dev/loop-control;
 * `ENOTTY` for another request. */
int32_t loop_control_ioctl(uint32_t request, uint32_t arg);

/** A description of device `index`'s node was opened, or its last
 * reference closed; a close may clear a device set to clear itself. */
void loop_node_opened(int index);
void loop_node_closed(int index);

/** A mount of `device` went: when it is a loop device set to clear itself
 * that nothing else holds, detach it, as Linux does at the last release. */
void loop_unmounted(block_device_t *device);

#endif /* _LOOP_H_ */
