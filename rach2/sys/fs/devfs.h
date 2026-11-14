#ifndef _DEVFS_H_
#define _DEVFS_H_

#include "../lib/kint.h"

/*
 * devfs: the devices of /dev. A table maps a name ("null", "zero", ...) to a
 * device with file operations and a major and minor number, and the nodes
 * of the disks join it as the disks are registered, and the nodes drivers
 * make as they bind, through the registry below. An open of a device
 * binds its operations to the descriptor, as a pipe's are, and stat and the
 * listing of /dev are made from the table. A node in a directory of /dev
 * has the directory in its name, as the event devices of /dev/input are
 * "input/eventN" in the table, which the classifier gives a path below the
 * directory, a row of synthfs's table of them (synthfs.h). "console" and
 * "tty" open the
 * kernel console as FD_TYPE_CONSOLE, "tty" a pty instead when the caller's
 * session is on one, and "ptmx" opens a new pty pair.
 */

struct fd_entry;
struct block_device;
struct file_ops;

/*
 * The st_dev of /dev and its devices. The KRFFS root's files report 0 and
 * every other mount a number of its own (`procfs_mount_dev`), so a device's
 * (st_dev, st_ino), which ttyname compares, is no other file's.
 */
#define DEVFS_FSID 5u

/**
 * Open the device `name` of /dev with the Linux open flags `flags` and
 * return the new descriptor; the caller has checked the device's mode. Most
 * devices bind their operations to the descriptor (FD_TYPE_DEVICE);
 * "ptmx" makes a new pty pair; "console" is the kernel console
 * (FD_TYPE_CONSOLE), and so is "tty" when the caller is in the console's
 * session, else the slave of the pty that has the caller's session.
 *
 * A write-capable open of a raw window that a write may reach, a whole
 * disk, one of the two boot windows or in the recovery build a partition,
 * takes the disk's writer reservation, which the last close gives back.
 * Returns -ENOENT for no such device present, -EEXIST under O_CREAT|O_EXCL,
 * -ENOTDIR under O_DIRECTORY, -ENXIO for "tty" with no controlling
 * terminal, -EBUSY for a second open of "grubcore" and for a write-capable
 * open of a raw window while another writer has the reservation or a
 * mounted volume claims its range, or the error of the descriptor's
 * allocation or the pty's open.
 */
int devfs_open(const char *name, int flags);

/**
 * The mode, device number and inode of /dev, for an empty `name`, or of the
 * device `name` present now: its type, a block device for a disk node or a
 * loop device and a character device for the rest, with its permission
 * bits; its major and minor as major << 8 | minor; and the inode its place
 * in the table gives. Each pointer may be NULL. Returns 0, or -1 for no such
 * device.
 */
int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino);

/**
 * The name of the device present now whose number, packed as `devfs_stat`
 * reports it, is `rdev`: a block device's when `block` is nonzero and a
 * character device's otherwise; NULL when there is none. This is what a
 * device node on a disk file system opens.
 */
const char *devfs_name_of(int block, uint32_t rdev);

/** The device number, major << 8 | minor, of the device the descriptor `e`
 * is open on, for fstat; 0 for a descriptor that is not of a device. */
uint32_t devfs_rdev(struct fd_entry *e);

/** The inode of the device the descriptor `e` is open on, as `devfs_stat`
 * gives it; 0 for a descriptor that is not of a device. */
uint32_t devfs_fd_ino(struct fd_entry *e);

/** The mode of the device the descriptor `e` is open on, as `devfs_stat`
 * gives it; 0 for a descriptor that is not of a device. */
uint32_t devfs_fd_mode(struct fd_entry *e);

/**
 * The owner `*uid` and group `*gid` of the device `name`: root's, 0 and 0,
 * until `devfs_setattr` gives it others. Returns 0, or -1 for no such device.
 */
int devfs_owner(const char *name, uint32_t *uid, uint32_t *gid);

/** The owner and group of the device the descriptor `e` is open on, as
 * `devfs_owner` gives them; 0 and 0 for a descriptor not of a device. */
void devfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid);

/**
 * Give the device `name` the permission bits of `mode`, the owner `uid`
 * and the group `gid`, each left as it is when (uint32_t)-1, as udev sets
 * the nodes of Linux's devtmpfs; `devfs_stat`, `devfs_owner` and the check
 * of an open read them, and a node registered anew starts again as root's
 * with its driver's mode. The caller has checked the caller's right.
 * Returns 0, -ENOENT for no such device, or -ENOSPC when DEVFS_OWNED_MAX
 * devices have attributes already.
 */
int32_t devfs_setattr(const char *name, uint32_t mode, uint32_t uid, uint32_t gid);

struct disk;
struct part_table;

/**
 * Make the nodes of a newly registered disk `d`: /dev/diskN for the whole
 * disk and one for each partition slot, which `devfs_disk_part_nodes`
 * names. Each is listed while its window in the disk's record is present.
 * The nodes have major 240; disk 0 has minor 1 for the whole disk and 4 to
 * 7 for partitions 1 to 4, and every other node 64 + 16N + M, M being 0 for
 * the whole disk. A disk past the room of the table has no nodes, and a
 * warning says so.
 */
void devfs_disk_nodes(struct disk *d);

/**
 * Name the partition nodes of `d` from its table: slot `i` is
 * `/dev/diskNpM` with `M` the number of the table's entry `i`, or `i` + 1
 * past the table's entries, and has the minor that gives. Called after
 * every scan of the table.
 */
void devfs_disk_part_nodes(struct disk *d);

/** Whether a mount holds a claim on any range of `d`. */
int devfs_disk_claimed(const struct disk *d);

/**
 * Publish /dev/bootpart over the `sector_count` sectors of `backing` from
 * `start_lba`: the GRUB disk's FAT boot partition, which `mbr_bootpart_ok`
 * has judged. Until it is published the device is absent from lookup,
 * stat, open and the listing of /dev alike. A partition of 2 GiB or more is
 * not published, so that the 32-bit `lseek` names every byte of the window.
 */
void devfs_bootpart_publish(struct block_device *backing, uint32_t start_lba,
                            uint32_t sector_count);

/**
 * Publish /dev/grubcore over the post-MBR gap of `backing`.
 *
 * The window is LBA 1 through 2047, a constant of the disk layout rather than
 * anything the partition table reports, so a damaged table cannot widen it.
 * Absent when the device is too small to hold the gap.
 */
void devfs_grubcore_publish(struct block_device *backing);

/**
 * Publish /dev/grubcore over the BIOS boot partition of the GUID table `t`,
 * which the partition service read from `backing`.
 *
 * The window is that partition's sectors and nothing else; a table without
 * exactly one BIOS boot partition clear of sector 0, the headers and the
 * entry array has no window.
 */
void devfs_grubcore_publish_gpt(struct block_device *backing, const struct part_table *t);

/**
 * The window the /dev path `path` names, for a mount: the view a mount
 * reads through, the physical device under it (`block_physical`), and the
 * sector range on that device. /dev/bootpart and the disk nodes, whole or a
 * partition, are windows. A loop device's node gives its device as the
 * view, no physical device, and its whole size, or -ENXIO while no file is
 * attached. Returns 0, -EINVAL for a path outside /dev or a device that is
 * no window, or -ENOENT for no such device, which a window not published is
 * as well.
 */
int devfs_window_of(const char *path, struct block_device **view, struct block_device **backing,
                    uint64_t *first, uint64_t *count);

/**
 * Claim the sectors `first` to `last` of `backing` for a mount. Returns the
 * claim's index, -EBUSY while an overlapping claim or the writer
 * reservation of a raw descriptor over the range exists, or -ENOMEM when
 * every claim is taken.
 */
int devfs_claim(struct block_device *backing, uint64_t first, uint64_t last);

/** Release the claim of index `claim`; an index out of range is ignored. */
void devfs_release_claim(int claim);

/** Withdraw /dev/bootpart, and log nothing. Returns 1 when it was published,
 * 0 when there was nothing to withdraw. */
int devfs_bootpart_withdraw(void);

/** Withdraw /dev/grubcore, before a controller is reset; nothing when it is
 * not published. */
void devfs_grubcore_withdraw(void);

/* The most sectors one driver call of a raw window moves, a mebibyte, the
 * block layer's largest request; a device whose requests are smaller has
 * smaller batches. /proc/disk reports it beside the count of calls. */
#define DEVFS_RAW_BATCH_SECTORS        2048
/* The sectors of the static buffer, taken when the heap has no batch. */
#define DEVFS_RAW_BATCH_STATIC_SECTORS 64

/**
 * How many driver calls the raw disk windows have made since boot.
 *
 * Reported by `/proc/disk`, because batching is otherwise invisible from
 * outside: a transfer of any size returns the same answer whether it took one
 * driver command or two hundred. A test that requires the batching, and not
 * only the result, reads this before and after.
 */
uint64_t devfs_raw_calls(void);

/** Withdraw the whole-disk window of every registered disk, before a
 * controller is reset; the partition windows stay as they are. */
void devfs_disk_withdraw(void);

/**
 * Whether `e` is one of the raw disk windows (`/dev/bootpart`, `/dev/grubcore`
 * or a whole-disk or partition node), `/dev/mem` or a loop device's node.
 *
 * This asks what the descriptor is, by the operations its open bound, and
 * not whether its window is published now. The two differ while a
 * controller is probed again, and a disk window's descriptor that turned
 * into an unseekable device's for that time would be a worse answer than one
 * whose backing is gone.
 */
int devfs_fd_is_raw(struct fd_entry *e);

/** Whether `e` is open on `/dev/mem`, whose offsets are unsigned, as Linux's
 * FMODE_UNSIGNED_OFFSET makes them, so that no transfer is refused for
 * ending past 2^63-1: 1 or 0. */
int devfs_fd_is_mem(struct fd_entry *e);

/**
 * Whether `e` is a raw disk window whose backing is published, `/dev/mem`, or
 * a loop device's node with a file attached; if so, `*size` gets its byte
 * size.
 *
 * The size needs 64 bits because a whole drive can exceed 4 GiB. The
 * descriptors `devfs_fd_is_raw` names are the only ones that seek and
 * transfer through the open file description's `raw_offset`.
 */
int devfs_fd_raw_size(struct fd_entry *e, uint64_t *size);

/** Publish /dev/diskN, the window over the whole of disk `d`, which only a
 * recovery build writes; nothing, logged, for a disk of no sectors. */
void devfs_disk_publish(struct disk *d);

/**
 * Whether `e` is the open boot log; if so, `*size`, unless NULL, gets the
 * bytes the log holds now. The log seeks through the description's
 * `offset`, as a file does, and not through `raw_offset`.
 */
int devfs_fd_log_size(struct fd_entry *e, uint32_t *size);

/**
 * Whether a read or write of `e` sets the description's offset past what it
 * moved, as a raw window's (`devfs_fd_is_raw`), the boot log's and the
 * screen log's do. Such a transfer may sleep, in a disk request or in a
 * fault on the caller's buffer, so its caller holds the description for it
 * (`ops_offset_held`, syscall_fs.c).
 */
int devfs_fd_moves_offset(struct fd_entry *e);

/** The name of the device the descriptor `e` is open on, for the link
 * /proc/<pid>/fd shows, or "?" for a descriptor that is not of a device. */
const char *devfs_fd_name(struct fd_entry *e);

/** Whether an lseek of the device the descriptor `e` is open on returns 0,
 * as one of /dev/null, /dev/zero and /dev/full does on Linux, and not
 * -ESPIPE: 1 or 0. */
int devfs_lseek_zero(struct fd_entry *e);

/**
 * The `n`th device present now in /dev itself, from 0: its name and its
 * inode. A device that is not present is skipped and does not end the
 * walk, since bootpart, grubcore, the disk nodes and the registered nodes
 * come and go, and a machine can have a later one without an earlier. The
 * inode is the device's place in the table and not in the listing, so it is
 * the number `stat` reports whatever else is present. Returns 1 when `n`
 * names a device, 0 past the last. A device in a directory of /dev is not
 * among them (`devfs_subdir_dirent`).
 */
int devfs_dirent(int n, const char **name, uint32_t *ino);

/**
 * The `n`th device present now in the directory `dir` of /dev, "input",
 * "dri" or "cpu/0", from 0, as `devfs_dirent` gives those of /dev: its name
 * in the directory, such as eventN, and its inode. Returns 1, or 0 past
 * the last. A row in the directory is not among them.
 */
int devfs_subdir_dirent(const char *dir, int n, const char **name, uint32_t *ino);

/** Whether row `row` of /dev's directories (synthfs.h) is present now: 1
 * for a row marked `always`, one with a device present or one with a row
 * in it present, else 0. */
int devfs_subdir_present(int row);

/*
 * The registry of nodes made at run time: a driver's devices, which it
 * finds as it binds and may lose, such as /dev/dri/card0 and renderD128,
 * /dev/udmabuf, /dev/kvm and /dev/watchdog. The registrant keeps the
 * record while it is registered. `name` is the path below /dev: a name of
 * /dev itself, or "DIR/NAME" for a directory DIR of /dev's table of them
 * (synthfs.h), such as "cpu/0/msr". A registered node is looked up,
 * stat'ed, listed and opened as a node of the table is, a character device
 * owned by root with the
 * permission bits `mode`: an open binds `ops` to the descriptor, with
 * `priv` NULL, and then calls `open`, unless NULL, which may set the
 * descriptor's `priv` and refuse with -errno. A node with NULL `ops` opens
 * the console terminal, as /dev/console does, for the node of a device
 * that carries the console. Its inode is its slot's,
 * the same for the life of the kernel unless the slots run out. A node
 * unregistered leaves /dev at once; a descriptor open on it keeps its
 * operations and its `priv`, and fstat gives its name and numbers until
 * its slot is taken again, first by a node of the same name.
 */
/* The most nodes at once: 64 for the drivers and one msr for each of the
 * 64 processors (proc/percpu.h's NR_CPUS). */
#define DEVFS_NODES_MAX     128
/* The devices that may have an owner, group or mode of their own at once. */
#define DEVFS_OWNED_MAX     256
#define DEVFS_NODE_NAME_MAX 32
struct devfs_node {
    const char *name;
    uint16_t major, minor;
    uint16_t mode; /* the nine permission bits */
    const struct file_ops *ops;
    int32_t (*open)(struct devfs_node *node, struct fd_entry *e, int flags);
    void *priv; /* the registrant's */
};

/** Register `node`: 0, -EINVAL for a name no directory of /dev takes or
 * one too long, -EEXIST for a name the table, a disk
 * or a registered node has, or the numbers of a character device present,
 * or -ENOSPC when every slot is held. */
int32_t devfs_node_register(struct devfs_node *node);

/** Withdraw `node` from /dev; nothing for a node not registered. */
void devfs_node_unregister(struct devfs_node *node);

struct block_device;

/* A disk node as /sys/block shows it: its name and numbers, the index of its
 * disk's whole node, its partition's number, 0 for a whole disk, its first
 * sector and its size in sectors, whether its disk is a removable stick,
 * and that disk's model and serial number. */
struct devfs_disk_row {
    const char *name;
    uint32_t major, minor;
    int whole;
    int partno;
    uint64_t start, sectors;
    int removable;
    const char *model, *serial;
    uint64_t discard_bytes; /* the most bytes one BLKDISCARD gives the
                               device at once; 0 when it takes none */
};

/** The major and minor numbers of the node at `path`, /dev and its leaf:
 * 1, or 0 for a path that names no node of /dev. */
int devfs_numbers(const char *path, uint32_t *major, uint32_t *minor);

/** Disk node `i` as `devfs_disk_node_at` finds it, into `row`: 1, or 0 or
 * -1 as `devfs_disk_node_at` answers. */
int devfs_disk_row(int i, struct devfs_disk_row *row);

/**
 * Disk node `i` of the table, for /proc/diskstats: 1 with its name, its
 * device numbers, the device whose counts are its own, the disk's for a
 * whole disk and the partition view's for a partition, and the alias whose
 * counts are added to them, or NULL; 0 for a partition not published now,
 * or a disk that is gone; -1 past the table. A whole disk has its line
 * whether or not its node is published. After the disk nodes come the four
 * loop devices, each a whole disk with no partitions while a file is
 * attached to it, as Linux lists them.
 */
int devfs_disk_node_at(int i, const char **name, uint32_t *major, uint32_t *minor,
                       const struct block_device **dev, const struct block_device **alias);

/**
 * Count the transfers of `alias`, a device over the same sectors as the
 * partition view `view`, as the view's node's in /proc/diskstats: the
 * kernel mounts its boot root through a view of its own.
 */
void devfs_window_alias(const struct block_device *view, const struct block_device *alias);

/**
 * The wake alarm of /dev/rtc0, as /sys/class/rtc/rtc0/wakealarm reads and
 * writes it: `devfs_rtc_wakealarm` gives the seconds since 1970 it is set
 * for while it is enabled, -1 while it is not, as before it is set or once
 * it has come; `devfs_rtc_set_wakealarm` sets it to `at` and enables it,
 * -EINVAL for a time further than a day less a second ahead, which leaves
 * it disabled, or for one whose year passes 2^31 - 1, which leaves it as it
 * was, and -EIO when the clock cannot be read; it raises the alarm at once
 * for a time not after now, as Linux's rtc_timer_enqueue does, and for a
 * negative `at` disables it with a time five minutes ahead, as Linux's
 * wakealarm_store does.
 */
int64_t devfs_rtc_wakealarm(void);
int32_t devfs_rtc_set_wakealarm(int64_t at);

#endif /* _DEVFS_H_ */
