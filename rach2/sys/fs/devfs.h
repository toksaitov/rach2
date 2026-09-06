#ifndef _DEVFS_H_
#define _DEVFS_H_

#include "../lib/kint.h"

/*
 * devfs: a synthetic /dev. The device namespace is virtual (not KRFFS
 * nodes): a small registry maps a leaf name ("null", "zero", ...) to a
 * character device backed by a file_ops vtable and a {major,minor}. open()
 * of a /dev path binds the device's ops onto the descriptor (the ops
 * dispatch then drives read/write/close), exactly as pipes do; stat/getdents
 * synthesize from the registry. Two names ("console" and "tty") resolve to
 * the kernel console and so open as FD_TYPE_CONSOLE, reusing the existing
 * terminal path.
 */

struct fd_entry;
struct block_device;

/*
 * The synthetic device the namespace lives on. KRFFS files report st_dev 0,
 * so a distinct nonzero id keeps a device's identity from ever colliding
 * with a real file's; this matters because ttyname compares (st_dev,
 * st_ino).
 */
#define DEVFS_FSID 5u

/** Open a /dev leaf by name; the new fd, or a negative errno. */
int devfs_open(const char *name, int flags);

/** Synthesize stat fields for /dev or a device leaf; 0 ok, -1 if no such
 * leaf. */
int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino);

/*
 * The encoded rdev (major<<8 | minor) and the stable inode of an open
 * FD_TYPE_DEVICE descriptor, for fstat64. Both return 0 if e is not a device.
 */
uint32_t devfs_rdev(struct fd_entry *e);
uint32_t devfs_fd_ino(struct fd_entry *e);

/** The S_IFCHR-typed mode of an open device fd (fstat64); 0 if not a device. */
uint32_t devfs_fd_mode(struct fd_entry *e);

/**
 * Publish /dev/bootpart over the `sector_count`-sector slice of `backing` at
 * `start_lba`: the GRUB disk's FAT boot partition, judged by `mbr_bootpart_ok`
 * before this is called. Until it is published the device is absent from
 * lookup, stat, open, and /dev enumeration alike.
 *
 * The partition must stay under 2 GiB, which is not a limit of the offset any
 * more: the raw windows seek and transfer through a 64-bit one. It is kept so
 * the legacy 32-bit `lseek` can still name every byte of this particular
 * window, which the boot partition is small enough for on any disk this
 * project builds. A larger one is not published.
 */
struct disk;
/**
 * Make the nodes of a newly registered disk: `/dev/diskN` and one
 * `/dev/diskNpM` per possible partition. Each is listed while its window
 * in the disk's record is present.
 */
void devfs_disk_nodes(struct disk *d);
void devfs_bootpart_publish(struct block_device *backing, uint32_t start_lba,
                            uint32_t sector_count);

/**
 * Publish the stick numbered `n`: `/dev/usbN` over the whole medium and
 * `/dev/usbNp1` upward over the `count` partitions in `parts`.
 *
 * The nodes report themselves as block devices, which is what a label scan
 * looks for; they are read-only, because a stick is written through a file
 * system mounted on it.
 */
struct gpt_part;
void devfs_usb_publish(int n, struct block_device *whole, const struct gpt_part *parts, int count);

/** Take the stick's nodes out of `/dev`, keeping the windows a mount and an
 * open description still read through. */
void devfs_usb_hide(int n);

/** Take the stick's windows back, once nothing holds them. */
void devfs_usb_withdraw(int n);

/** Window `k` of the stick numbered `n`: 0 is the whole medium and 1 to 4 are
 * its partitions. Null when that window is not published. */
struct block_device *devfs_usb_view(int n, int k);

/** How many descriptions are open on the stick's nodes. */
int devfs_usb_opens(int n);

/**
 * Publish /dev/grubcore over the post-MBR gap of `backing`.
 *
 * The window is LBA 1 through 2047, a constant of the disk layout rather than
 * anything the partition table reports, so a damaged table cannot widen it.
 * Absent when the device is too small to hold the gap.
 */
void devfs_grubcore_publish(struct block_device *backing);

/**
 * The partition window `path` names (`/dev/bootpart` or `/dev/disk0pN`):
 * the view a mount reads through, the backing disk, and the sector range
 * on it. EINVAL for a device that is not a partition window, ENOENT for no
 * such device, which a window not published is as well.
 */
int devfs_window_of(const char *path, struct block_device **view,
                    struct block_device **backing, uint64_t *first,
                    uint64_t *count);

/**
 * Claim `[first, last]` on `backing` for a mount; the claim's index, or
 * -EBUSY while an overlapping claim or a write-capable raw descriptor over
 * the range exists. A claim is released by index.
 */
int devfs_claim(struct block_device *backing, uint64_t first, uint64_t last);
void devfs_release_claim(int claim);

/** Withdraw /dev/bootpart; a no-op when it was never published. */
void devfs_bootpart_withdraw(void);

/** Withdraw /dev/grubcore; a no-op when it was never published. */
void devfs_grubcore_withdraw(void);

/* Sectors carried by one driver call from a raw window. Reported by /proc/disk
 * beside the call count so the two can be compared. */
#define DEVFS_RAW_BATCH_SECTORS 64

/**
 * How many driver calls the raw disk windows have made since boot.
 *
 * Reported by `/proc/disk`, because batching is otherwise invisible from
 * outside: a transfer of any size returns the same answer whether it took one
 * driver command or two hundred. A test that wants to require the batching,
 * not the result reads this before and after.
 */
uint64_t devfs_raw_calls(void);

/** Withdraw /dev/disk0; a no-op when it was never published. */
void devfs_disk_withdraw(void);

/**
 * Whether `e` is one of the raw disk windows (`/dev/bootpart`, `/dev/grubcore`
 * or `/dev/disk0`).
 *
 * This asks what the descriptor IS, from the operations bound to it at open,
 * and not whether its window is currently published. The two differ while a
 * controller is being re-probed, and a descriptor that changed from a seekable
 * disk window into an unseekable generic device for the duration would be a
 * worse answer than one that reports its backing is gone.
 */
int devfs_fd_is_raw(struct fd_entry *e);

/**
 * Whether `e` is a raw disk window whose backing is published; if so, `*size`
 * gets its byte size.
 *
 * The size needs 64 bits because a whole drive can exceed 4 GiB. These are the
 * only descriptors that seek and transfer through the open-file description's
 * `raw_offset`.
 */
int devfs_fd_raw_size(struct fd_entry *e, uint64_t *size);

/**
 * Publish `/dev/diskN`, a read-only window over the whole of disk `d`.
 *
 * The window is `window_sectors` sectors, which is the disk's size clamped to
 * this window's chosen ceiling; `/proc/disk` prints both numbers so the clamp
 * is visible, not silent.
 */
void devfs_disk_publish(struct disk *d, uint64_t window_sectors);

/**
 * Whether `e` is the open boot log; if so, `*size` gets its current byte
 * length. It seeks through the ordinary 32-bit offset like any other file.
 */
int devfs_fd_log_size(struct fd_entry *e, uint32_t *size);

/* The leaf name ("null", ...) of an open device fd, for /proc/<pid>/fd
 * readlink. Returns "?" if e is not a devfs device. */
const char *devfs_fd_name(struct fd_entry *e);

/*
 * Whether lseek on this device returns offset 0 (the mem devices
 * null/zero/full), not -ESPIPE. Returns 1 for a seek-to-zero device,
 * 0 otherwise.
 */
int devfs_lseek_zero(struct fd_entry *e);

/*
 * Number of devfs entries, and the leaf name of entry i (for getdents on /dev).
 * Returns NULL past the end.
 */
/**
 * The `n`-th device that exists right now: its name and its stable inode.
 *
 * Absent entries are skipped instead of ending the walk, because two of them
 * are dynamic and a machine can have the later one without the earlier: the
 * metal profile never mounts, so it has `disk0` and no `bootpart`. The inode
 * is the registry index and not the listing position, so a device's identity
 * is the same number `stat` reports whatever else is present.
 *
 * @return 1 when `n` named a device, 0 past the end.
 */
int devfs_dirent(int n, const char **name, uint32_t *ino);
const char *devfs_name(int i);

#endif /* _DEVFS_H_ */
