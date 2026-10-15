/*
 * loop.h: block devices over regular files.
 *
 * The file is reached through the file system it is on, so an image on any
 * mounted volume can have a file system of its own. Four of them, attached
 * and detached through /dev/loop-control, by a line written to it or by
 * Linux's loop ioctls, which util-linux's losetup and mount make, and listed
 * in /proc/loop.
 */

#ifndef _LOOP_H_
#define _LOOP_H_

#include "../lib/kint.h"
#include "block.h"

/*
 * Policy: four devices. Each has a node of its own in /dev, made with the
 * device table at boot, and a program names one by that node, so the count
 * is part of the names the system offers rather than a table that runs
 * out.
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
 * access to the file. Syscall context; it sleeps.
 *
 * A file is named by its mount and its node and not by its path, so a hard
 * link to an attached file is the same file. The description the attachment
 * opens keeps the volume the file is on from being unmounted.
 *
 * Answers the index, or a negative errno: `EINVAL` for an index past the
 * devices; `EBUSY` when that device or every device is taken, the file is
 * already attached, open, or mapped shared, or its path moved to another
 * mount meanwhile; `EINVAL` when the file is not a regular file or its length
 * is not a positive whole number of sectors; `ELOOP` when the file is on a
 * chain of devices as deep as the buffer cache can serve; and whatever the
 * resolve, the open or the sync answered otherwise.
 */
int loop_attach(int index, const char *path, int writable);

/**
 * Detach loop device `index`. `EINVAL` for an index past the devices,
 * `ENOENT` when nothing is attached, and `EBUSY` while an attachment is still
 * being made, anything is mounted on the device, a buffer of it is held, or
 * a transfer is inside the file system the file is on; `EIO` when its dirty
 * buffers would not write. Otherwise the buffers are written and dropped and
 * the file is released. A device whose medium went has nothing to write and
 * is only released.
 */
int loop_detach(int index);

/** Whether the file at (`mnt`, `node`) backs a loop device, where `node` may
 * be the file's node or the handle its file system gave the open. */
int loop_backs(struct vfs_mount *mnt, void *node);

/** Follow a node the file system moved, for `vfs_repoint`, which owns that
 * sweep. */
void loop_repoint(struct vfs_mount *mnt, const void *from, void *to);

/**
 * Take down every loop device standing on `device`, which has gone: nothing
 * can reach its file again, so the loop device is marked `BLOCK_GONE` with
 * it, the buffer cache is told, and a mount above it can be taken down
 * without writing. The slot stays taken, since a mount may still stand on
 * the device; the file's description is released later, by `loop_detach` or
 * by the next control command, ioctl, or read of /proc/loop or
 * /sys/block/loopN/loop once nothing holds the device, since this may be
 * called where a mount's lock cannot be taken.
 */
void loop_device_gone(block_device_t *device);

/** One line of /proc/loop for `index`, written into `buffer` of `cap` bytes:
 * its size, `rw` or `ro`, its writes and syncs, and its path or `(gone)`.
 * Returns its length, or 0 when the slot is free. */
int loop_line(int index, char *buffer, uint32_t cap);

/** The path the file that backs loop device `index` was attached by, and
 * in `*autoclear` its autoclear flag, for /sys/block/loopN/loop; NULL when
 * no file backs it. */
const char *loop_backing(int index, int *autoclear);

/** The longest backing path a control line may name; a line is that plus
 * its command word, and a longer write is refused. */
#define LOOP_PATH_MAX 64

/**
 * The write handler behind /dev/loop-control: one command a write, `attach
 * N PATH`, `attach - PATH` for the first free device, either with `ro`
 * before the number, or `detach N`. PATH is absolute. Answers what
 * `loop_attach` or `loop_detach` answers, or `EINVAL` for a line that is not
 * a command.
 */
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
 * standing to change the device. LOOP_CLR_FD on a device still mounted or
 * open elsewhere sets it to clear itself when the last of them goes, as
 * Linux does. `ENXIO` when nothing is attached, `EPERM` for a change by a
 * caller without standing, `ENOTTY` for a request that is not one.
 */
int32_t loop_ioctl(int index, int privileged, uint32_t request, uintptr_t arg);

/** LOOP_CTL_GET_FREE, LOOP_CTL_ADD and LOOP_CTL_REMOVE on /dev/loop-control;
 * `ENOTTY` for another request. */
int32_t loop_control_ioctl(uint32_t request, uintptr_t arg);

/** A description of device `index`'s node was opened, or its last
 * reference closed; a close may clear a device set to clear itself. */
void loop_node_opened(int index);
void loop_node_closed(int index);

/** A mount of `device` went: when it is a loop device set to clear itself
 * that nothing else holds, detach it, as Linux does at the last release. */
void loop_unmounted(block_device_t *device);

#endif /* _LOOP_H_ */
